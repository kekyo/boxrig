/* boxrig - Under MIT. */
import assert from 'node:assert/strict';
import {spawn,spawnSync} from 'node:child_process';
import {once} from 'node:events';
import {mkdtemp, rm, mkdir, writeFile, readFile, open} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {resolve, join} from 'node:path';
import {createHash} from 'node:crypto';

const [gtk3, gtk4, font, engine, output] = process.argv.slice(2);
assert(gtk3 && gtk4 && font && engine && output);
const binaries = {3:resolve(gtk3), 4:resolve(gtk4)};
const runtimes = [
  {name:'system-gtk3', gtk:3, libraries:undefined, hints:false},
  {name:'system-gtk4', gtk:4, libraries:undefined, hints:false},
];
if (process.env.BOXRIG_TEST_GTK_EARLY_LIBS) runtimes.push({
  name:'gtk4-early-hints', gtk:4, libraries:process.env.BOXRIG_TEST_GTK_EARLY_LIBS, hints:false, modules:false,
  version:'4.23.0'});
if (process.env.BOXRIG_TEST_GTK_SUPPORTED_LIBS) runtimes.push({
  name:'gtk4-supported-hints', gtk:4, libraries:process.env.BOXRIG_TEST_GTK_SUPPORTED_LIBS, hints:true,
  version:'4.23.4'});
else console.log('SKIP real newer GTK: set BOXRIG_TEST_GTK_SUPPORTED_LIBS to the verified GTK library search path');
const records=[];
const runRoot=resolve(output,new Date().toISOString().replaceAll(':','-'));
await mkdir(runRoot,{recursive:true});
await writeFile(resolve(output,'last-run.json'),JSON.stringify({runRoot})+'\n');
for (const runtime of runtimes) {
  if(runtime.libraries && runtime.modules !== false){
    const libdir=spawnSync('pkg-config',['--variable=libdir','gtk4'],{encoding:'utf8'});
    assert.equal(libdir.status,0,libdir.stderr);
    runtime.modulePath=join(libdir.stdout.trim(),'gtk-4.0');
  }
  const environment={...process.env,ASAN_OPTIONS:'detect_leaks=0:halt_on_error=1'};
  if(runtime.libraries)environment.LD_LIBRARY_PATH=runtime.libraries;
  const version=spawnSync(binaries[runtime.gtk],['--runtime'],{env:environment,encoding:'utf8'});
  assert.equal(version.status,0,version.stderr);
  runtime.loadedVersion=version.stdout.trim();
  assert(/^\d+\.\d+\.\d+$/.test(runtime.loadedVersion));
  const [major,minor,micro]=runtime.loadedVersion.split('.').map(Number);
  // This determines the contract to exercise. Selection alone is insufficient:
  // qualifying runtimes must also parse real hints and render the chosen clause.
  runtime.hints=major===4 && (minor>23 || minor===23 && micro>=4);
  if(runtime.version)assert.equal(runtime.loadedVersion,runtime.version);
  const cases=[];
  for (const version of [1,2]) for (const scale of [1,2]) for (const renderer of ['cairo','opengl'])
    cases.push({version,scale,renderer,policy:'auto',expected:runtime.hints && version===2?'standard':'direct',module:'default'});
  // Explicit choices override AUTO; unavailable IBus remains an observable fallback.
  for (const [policy,expected,module] of [
    ['ibus','direct','gtk-im-context-simple'],
    ['auto','simple','gtk-im-context-simple'],
    ['auto','simple','missing-boxrig-module'],
    ['auto','gtk-wayland','wayland'],
    ['gtk','gtk-wayland','wayland'],
    ['gtk','simple','gtk-im-context-simple'],
    ['auto','direct',''],
    ['auto','gtk-ibus','ibus'],
    ['gtk','gtk-ibus','ibus'],
    ['auto','unavailable','default'],
    ['auto','discovered','default'],
    ['auto','reconnect','default'],
  ]) cases.push({version:1,scale:1,renderer:'cairo',policy,expected,module});
  if(runtime.hints) cases.push({version:2,scale:1,renderer:'cairo',policy:'gtk',expected:'standard',module:'wayland'});
  if(runtime.hints) for(const renderer of ['cairo','opengl']) cases.push({version:2,scale:1,renderer,policy:'auto',expected:'standard',module:'default',defaultTheme:true});
  for (const base of cases) for (const candidates of ['auto','system','client']) {
    const test={...base,candidates};
    if(candidates==='client' && test.policy==='auto' &&
       ['default',''].includes(test.module) && test.expected==='standard') test.expected='direct';
    if (runtime.modules === false && test.expected === 'gtk-ibus') {
      console.log('SKIP '+runtime.name+' external IBus module: comparison GTK was built without X11');
      continue;
    }
    const label = [runtime.name, test.version, test.scale, test.renderer, test.policy,
      test.expected, test.module || 'empty',candidates, ...(test.defaultTheme?['default-theme']:[])].join('-');
    const directory=resolve(runRoot,label);
    await mkdir(directory,{recursive:true});
    const privateRuntime=await mkdtemp(join(tmpdir(),'boxrig-gtk-ime-'));
    const log=await open(join(directory,'test.log'),'w');
    const environment={...process.env,XDG_RUNTIME_DIR:privateRuntime,BOXRIG_TEST_IBUS_WAYLAND:'1',
      ASAN_OPTIONS:'detect_leaks=0:halt_on_error=1',UBSAN_OPTIONS:'halt_on_error=1',
      NO_AT_BRIDGE:'1',GTK_A11Y:'none',LIBGL_ALWAYS_SOFTWARE:'1'};
    if(test.defaultTheme) environment.GTK_THEME='Default';
    if(test.expected==='standard'){
      environment.BOXRIG_TEST_IME_CAPTURE=join(directory,'frames');
      await mkdir(environment.BOXRIG_TEST_IME_CAPTURE,{recursive:true});
    }
    delete environment.DISPLAY;
    delete environment.WAYLAND_DISPLAY;
    delete environment.GDK_SCALE;
    if(runtime.libraries)environment.LD_LIBRARY_PATH=runtime.libraries;
    if(runtime.version)environment.BOXRIG_TEST_GTK_RUNTIME=runtime.version;
    if(runtime.modulePath)environment.BOXRIG_TEST_GTK_MODULE_PATH=runtime.modulePath;
    else delete environment.BOXRIG_TEST_GTK_MODULE_PATH;
    const args=['tests/wayland/run_ibus_context.mjs',binaries[runtime.gtk],resolve(font),resolve(engine),
      directory, String(test.version),String(test.scale),test.renderer,test.policy,test.expected,test.module,candidates];
    console.log('GTK IME routing: '+label);
    try {
      const child=spawn('dbus-run-session',['--',process.execPath,...args],{
        env:environment,stdio:['ignore',log.fd,log.fd]});
      const [code,signal]=await once(child,'close');
      const text=await readFile(join(directory,'test.log'),'utf8');
      process.stdout.write(text);
      records.push({label,runtime,test,exitCode:code,signal,
        binary:binaries[runtime.gtk],sha256:createHash('sha256').update(await readFile(binaries[runtime.gtk])).digest('hex')});
      await writeFile(resolve(runRoot,'routing.json'),JSON.stringify(records,null,2)+'\n');
      if(test.expected==='standard' && code===0 && signal===null){
        const rows=(await readFile(join(directory,'frames','frames.jsonl'),'utf8')).trim().split('\n').map(JSON.parse);
        let concat='ffconcat version 1.0\n';
        rows.forEach((item,index)=>{
          assert.equal(item.index,index+1);
          concat+=`file '${item.index}.ppm'\noption framerate 1000\n`;
          if(index+1<rows.length){
            const seconds=Number(BigInt(rows[index+1].monotonicNs)-BigInt(item.monotonicNs))/1e9;
            assert(seconds>0);concat+=`duration ${seconds.toFixed(6)}\n`;
          }
        });
        await writeFile(join(directory,'frames','frames.ffconcat'),concat);
        const encoded=spawnSync('ffmpeg',['-v','error','-f','concat','-safe','0','-i',join(directory,'frames','frames.ffconcat'),
          '-fps_mode','passthrough','-enc_time_base','1:1000','-c:v','libx264','-qp','0','-pix_fmt','yuv444p',
          '-video_track_timescale','1000','-y',join(directory,'clauses.mp4')],{encoding:'utf8'});
        assert.equal(encoded.status,0,encoded.stderr);
      }
      assert.equal(signal,null,label);
      assert.equal(code,0,label);
    } finally { await log.close(); await rm(privateRuntime,{recursive:true,force:true}); }
  }
}
console.log(`PASS GTK IME routing matrix: ${records.length} real runtime cases`);
