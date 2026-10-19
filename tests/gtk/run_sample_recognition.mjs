/* boxrig - Under MIT. */
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

if (process.argv[2] === '--child') {
  process.stdin.setRawMode(true);
  process.stdin.resume();
  process.stdin.on('data', (bytes) => {
    assert.equal(bytes.toString(), 'q');
    process.exit(0);
  });
  // A single finite write; the cursor is hidden and there are no blinking cells.
  // Scrollback requires many recognition budgets before the final visible URL.
  process.stdout.write('\x1b[?25l' +
    'unchanged row 0123456789012345678901234567890123456789\r\n'.repeat(250) +
    'https://example.org/final\x1b]2;boxrig-finite-recognition\x1b\\');
} else {
  const child = spawn(process.argv[2], ['--renderer=opengl', '--', process.execPath,
    fileURLToPath(import.meta.url), '--child'], {
    env: { ...process.env, BOXRIG_TEST_FINITE_RECOGNITION: '1', BOXRIG_AUTO_URLS: '1' },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let output = '';
  child.stdout.on('data', (chunk) => { output += chunk; });
  child.stderr.on('data', (chunk) => { output += chunk; });
  const timeout = setTimeout(() => {
    process.stderr.write(output);
    child.kill('SIGKILL');
  }, 60000);
  const [code, signal] = await new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('exit', (code, signal) => resolve([code, signal]));
  });
  clearTimeout(timeout);
  process.stdout.write(output);
  assert.equal(signal, null);
  assert.equal(code, 0);
  assert.match(output, /PASS finite PTY recognition resumes=\d+ annotations=1 events=0 presented=1/);
}
