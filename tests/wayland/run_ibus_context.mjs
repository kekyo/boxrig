/* boxrig - Under MIT. */
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {once} from 'node:events';
import {mkdtemp, mkdir, rm, open} from 'node:fs/promises';
import {watch, readFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {resolve, join} from 'node:path';
import {createInterface} from 'node:readline';

const [binary, font, engineBinary, resultDirectory] = process.argv.slice(2);
assert(binary && font && engineBinary && resultDirectory);
const root = await mkdtemp(join(tmpdir(), 'boxrig-wayland-ibus-'));
await mkdir(resultDirectory, {recursive: true});
const environment = {...process.env, IBUS_ADDRESS: `unix:path=${root}/ibus.sock`,
  IBUS_ADDRESS_FILE: `${root}/ibus.address`,
  XDG_CONFIG_HOME: `${root}/config`, XDG_CACHE_HOME: `${root}/cache`,
  XDG_DATA_HOME: `${root}/data`, GSETTINGS_BACKEND: 'memory', GIO_USE_VFS: 'local',
  GTK_IM_MODULE: 'ibus', XMODIFIERS: '@im=ibus'};
delete environment.IBUS_USE_PORTAL;
if (environment.BOXRIG_TEST_IBUS_WAYLAND !== '1') delete environment.WAYLAND_DISPLAY;
let daemon, engine, test;
let generation = 0;

const stop = async child => {
  if (!child || child.exitCode !== null || child.signalCode !== null) return;
  const closed = once(child, 'close');
  child.kill('SIGCONT');
  child.kill('SIGTERM');
  await closed;
};

const start = async () => {
  generation++;
  await rm(environment.IBUS_ADDRESS_FILE, {force: true});
  const daemonLog = await open(join(resultDirectory, `ibus-${generation}.log`), 'w');
  const engineLog = await open(join(resultDirectory, `engine-${generation}.log`), 'w');
  let watcher;
  const addressReady = new Promise((resolveReady, reject) => {
    // The daemon writes this record after g_dbus_server_start(). Socket-file
    // creation alone precedes listen readiness and is not a readiness signal.
    const observe = () => {
      try {
        const record = readFileSync(environment.IBUS_ADDRESS_FILE, 'utf8');
        if (record.includes(`IBUS_DAEMON_PID=${daemon.pid}\n`) &&
            record.includes(`IBUS_ADDRESS=${environment.IBUS_ADDRESS}`)) resolveReady();
      } catch (error) {
        if (error.code !== 'ENOENT') reject(error);
      }
    };
    watcher = watch(root, observe);
    watcher.on('error', reject);
  });
  try {
    daemon = spawn('ibus-daemon', ['--single', '--panel=disable', '--config=disable',
      '--emoji-extension=disable', `--address=${environment.IBUS_ADDRESS}`],
      {env: environment, stdio: ['ignore', daemonLog.fd, daemonLog.fd]});
    const exited = once(daemon, 'exit');
    const outcome = await Promise.race([addressReady, exited]);
    assert(outcome === undefined, `IBus exited before address publication: ${outcome}`);
    watcher.close();
    engine = spawn(resolve(engineBinary), [],
      {env: environment, stdio: ['ignore', 'pipe', engineLog.fd]});
    const lines = createInterface({input: engine.stdout});
    let ready = false;
    for await (const line of lines) {
      await engineLog.write(`${line}\n`);
      if (line === 'READY') { ready = true; break; }
    }
    assert(ready, 'Probe engine exited before registration and selection completed');
  } finally {
    watcher.close();
    await daemonLog.close();
    await engineLog.close();
  }
};

try {
  await start();
  test = spawn(resolve(binary), [font, ...process.argv.slice(6)], {env: {...environment,
    BOXRIG_TEST_IBUS_ENGINE_PID: String(engine.pid), BOXRIG_TEST_IBUS_DAEMON_PID: String(daemon.pid)},
    stdio: ['ignore', 'inherit', 'inherit', 'pipe', 'pipe']});
  const finished = once(test, 'close');
  const requests = createInterface({input: test.stdio[3]});
  const handleRequests = async () => {
    for await (const request of requests) {
      assert.equal(request, 'restart');
      await stop(engine);
      await stop(daemon);
      await rm(`${root}/ibus.sock`, {force: true});
      await start();
      test.stdio[4].write(`ready ${engine.pid} ${daemon.pid}\n`);
    }
  };
  const handling = handleRequests();
  const [[code, signal]] = await Promise.all([finished, handling]);
  assert.equal(signal, null, `Test terminated by ${signal}`);
  process.exitCode = code;
} finally {
  await stop(test);
  await stop(engine);
  await stop(daemon);
  await rm(root, {recursive: true, force: true});
}
