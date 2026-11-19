// Send the recorded application's original bytes, with one rendered update outstanding.
import assert from 'node:assert/strict';
import { closeSync, existsSync, openSync, readFileSync, watch, writeFileSync, writeSync } from 'node:fs';
import { join } from 'node:path';

const [input, directory] = process.argv.slice(2);
const report = join(directory, 'work.tsv');
const tty = openSync('/dev/tty', 'w');
const end = Buffer.from('\x1b[?2026l');
let completed = 0;
let wake;
const watcher = watch(directory, (_event, name) => {
  if (name === 'work.tsv' && wake) wake();
});

const send = async (file) => {
  const bytes = readFileSync(join(input, file));
  for (let index = bytes.indexOf(end); index >= 0; index = bytes.indexOf(end, index + end.length)) completed++;
  let sent = 0;
  while (sent < bytes.length) sent += writeSync(tty, bytes, sent, bytes.length - sent);
  const deadline = Date.now() + 30000;
  for (;;) {
    if (existsSync(report)) {
      const [header, ...lines] = readFileSync(report, 'utf8').trimEnd().split('\n');
      const keys = header.split('\t');
      for (const line of lines) {
        const values = line.split('\t');
        if (values.length === keys.length && Number(values[1]) === completed) {
          const row = Object.fromEntries(keys.map((key, i) => [key, key === 'background' ? values[i] : Number(values[i])]));
          assert.equal(row.rows, 45);
          assert.equal(row.columns, 118);
          return row;
        }
      }
    }
    const remaining = deadline - Date.now();
    assert(remaining > 0, 'no renderer report after synchronized update ' + completed);
    // Check and install the wakeup in the same JavaScript turn.
    await new Promise((resolve) => {
      const timer = setTimeout(resolve, remaining);
      wake = () => { clearTimeout(timer); resolve(); };
    });
    wake = undefined;
  }
};

try {
  const manifest = JSON.parse(readFileSync(join(input, 'manifest.json'), 'utf8'));
  await send('warmup.bin');
  assert.equal(completed, manifest.warmFrames);
  const frames = [];
  for (const { sequence } of manifest.sourceRanges) frames.push(await send('frame-' + sequence + '.bin'));
  assert.equal(frames.length, manifest.sourceRanges.length);
  assert(frames.every((frame, i) => frame.sync_ends === manifest.warmFrames + i + 1));
  const totals = {};
  for (const key of Object.keys(frames[0]).slice(5))
    totals[key] = frames.reduce((sum, frame) => sum + frame[key], 0);
  writeFileSync(join(directory, 'updates.json'), JSON.stringify({ source: manifest.warmupAndFramesSha256, frames, totals }, null, 2) + '\n');
} finally {
  watcher.close();
  closeSync(tty);
}
