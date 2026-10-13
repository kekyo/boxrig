/* boxrig - Under MIT. */
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {once} from 'node:events';
import {createConnection, createServer} from 'node:net';
import {mkdtemp, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {createInterface} from 'node:readline';

if (process.argv[2] === '--child') {
  const socket = createConnection(process.argv[3]);
  await once(socket, 'connect');
  const [bytes] = await once(socket, 'data');
  assert.equal(bytes.toString(), 'quit\n');
  socket.end();
  await once(socket, 'close');
} else {
  const binary = resolve(process.argv[2]);
  const directory = await mkdtemp(join(tmpdir(), 'boxrig-native-candidates-'));
  try {
    for (const renderer of ['wayland', 'opengl'])
    for (const policy of ['auto', 'wayland', 'ibus'])
    for (const candidates of ['auto', 'system', 'client']) {
      const server = createServer();
      const connected = once(server, 'connection');
      const socketPath = join(directory, 'child.sock');
      server.listen(socketPath);
      await once(server, 'listening');
      const sample = spawn(binary, ['--renderer=' + renderer, '--', process.execPath,
        import.meta.filename, '--child', socketPath], {env: {...process.env,
        BOXRIG_WAYLAND_IME: policy, BOXRIG_IME_CANDIDATES: candidates},
        stdio: ['ignore', 'pipe', 'pipe']});
      const closed = once(sample, 'close');
      let stderr = '', socket, timer;
      sample.stderr.on('data', bytes => { stderr += bytes.toString(); });
      const deadline = new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error('Native sample route readiness: ' + stderr)), 60000);
      });
      try {
        const observe = async () => {
          for await (const line of createInterface({input: sample.stdout})) {
            console.log(line);
            if (!line.includes('candidate-owner=') || line.includes('candidate-owner=none')) continue;
            const fields = Object.fromEntries([...line.matchAll(/([\w-]+)=([^ ]+)/g)].map(match => [match[1], match[2]]));
            assert.equal(fields['candidate-policy'], candidates);
            const direct = policy === 'ibus' || (policy === 'auto' &&
              (candidates === 'client' || Number(fields['text-input-version']) < 2));
            assert.equal(fields['ime-route'], direct ? 'ibus' : 'wayland');
            assert.equal(fields['candidate-owner'], direct && candidates !== 'system' ? 'client' : 'system');
            assert.equal(fields['candidate-reason'], candidates === 'system' ? 'system-request' : direct ? 'ready' : 'standard-input');
            assert.equal(fields['candidate-count'], '0');
            assert.equal(fields['candidate-visible'], '0');
            return;
          }
          assert.fail('Native sample exited without a candidate diagnostic: ' + stderr);
        };
        const observing = observe();
        const outcome = await Promise.race([Promise.all([observing, connected]), closed, deadline]);
        assert(Array.isArray(outcome[1]), stderr);
        [socket] = outcome[1];
        socket.end('quit\n');
        const [code, signal] = await Promise.race([closed, deadline]);
        assert.equal(signal, null, stderr);
        assert.equal(code, 0, stderr);
        console.log(`PASS native sample ${renderer} ${policy} ${candidates} and clean child shutdown`);
      } finally {
        clearTimeout(timer);
        if (sample.exitCode === null && sample.signalCode === null) sample.kill('SIGTERM');
        await closed;
        socket?.destroy();
        server.close();
      }
    }
  } finally { await rm(directory, {recursive: true, force: true}); }
}
