/* boxrig - Under MIT. */
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {once} from 'node:events';
import {createConnection,createServer} from 'node:net';
import {mkdtemp,rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
import {createInterface} from 'node:readline';

if (process.argv[2] === '--child') {
  // The PTY child exits only after the parent has observed the actual route.
  const socket=createConnection(process.argv[3]);
  await once(socket,'connect');
  const [bytes]=await once(socket,'data');
  assert.equal(bytes.toString(),'quit\n');
  socket.end();
  await once(socket,'close');
} else {
  const [gtk3,gtk4]=process.argv.slice(2);
  assert(gtk3 && gtk4);
  const directory=await mkdtemp(join(tmpdir(),'boxrig-sample-ime-'));
  try {
    for (const [major,binary] of [[3,gtk3],[4,gtk4]]) for (const policy of ['auto','gtk','ibus'])
    for (const candidates of ['auto','system','client']) {
      const server=createServer();
      const connected=once(server,'connection');
      const socketPath=join(directory,'child.sock');
      server.listen(socketPath);
      await once(server,'listening');
      const env={...process.env,XDG_RUNTIME_DIR:directory,
        BOXRIG_GTK_IME:policy,BOXRIG_IME_CANDIDATES:candidates,GTK_A11Y:'none',NO_AT_BRIDGE:'1'};
      delete env.GTK_IM_MODULE;
      delete env.GDK_DEBUG;
      delete env.GSK_RENDERER;
      const sample=spawn(resolve(binary),['--',process.execPath,
        import.meta.filename,'--child',socketPath],{env,stdio:['ignore','pipe','pipe']});
      const closed=once(sample,'close');
      let errorOutput='';
      sample.stderr.on('data',bytes=>{errorOutput+=bytes.toString();});
      let timer,socket;
      const deadline=new Promise((_,reject)=>{timer=setTimeout(()=>reject(new Error('Sample route readiness did not complete')),60000);});
      try {
        const observe=async()=>{
          const lines=createInterface({input:sample.stdout});
          let imeReady=false,renderReady=false;
          for await (const line of lines) {
            console.log(line);
            if (line.startsWith('boxrig_term: graphics-api=')) {
              assert.match(line,major===4
                ? /^boxrig_term: graphics-api=OpenGL(?: ES)?$/
                : /^boxrig_term: graphics-api=OpenGL$/);
              renderReady=true;
              if (imeReady) return;
            }
            if (!line.includes('ime-route=') || line.includes('ime-route=pending')) continue;
            const fields=Object.fromEntries([...line.matchAll(/([\w-]+)=([^ ]+)/g)].map(match=>[match[1],match[2]]));
            assert.equal(fields.display,'GdkX11Display');
            const direct=major===3 && policy!=='gtk';
            const client=direct && candidates==='client';
            if (direct) {
              assert.equal(fields['ime-reason'],policy==='auto'?'x11-ibus':'explicit-ibus');
              if (fields['ime-route']!=='ibus') continue;
            } else {
              assert.equal(fields['ime-route'],'gtk');
              assert.equal(fields['ime-reason'],policy==='gtk'?'explicit-gtk':'non-wayland');
            }
            {
              assert.equal(fields['candidate-policy'],candidates);
              assert.equal(fields['candidate-owner'],client?'client':'system');
              assert.equal(fields['candidate-reason'],candidates==='system'?'system-request':client?'ready':'standard-input');
              assert.equal(fields['candidate-count'],'0');
              assert.equal(fields['candidate-visible'],'0');
            }
            imeReady=true;
            if (renderReady) return;
          }
          assert.fail('Sample exited before reporting its configured route: '+errorOutput);
        };
        const observing=observe();
        const outcome=await Promise.race([Promise.all([observing,connected]),closed,deadline]);
        assert(Array.isArray(outcome[1]),'Sample closed before its PTY child connected: '+errorOutput);
        [socket]=outcome[1];
        socket.end('quit\n');
        const [code,signal]=await Promise.race([closed,deadline]);
        assert.equal(signal,null,errorOutput);assert.equal(code,0,errorOutput);
        console.log(`PASS GTK ${major} sample policy=${policy} candidates=${candidates} startup and clean child shutdown`);
      } finally {
        clearTimeout(timer);
        if(sample.exitCode===null && sample.signalCode===null)sample.kill('SIGTERM');
        await closed;
        socket?.destroy();
        server.close();
      }
    }
  } finally { await rm(directory,{recursive:true,force:true}); }
}
