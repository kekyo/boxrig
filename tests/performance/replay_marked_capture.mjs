// Preserve terminal state around ten observation cells in each synchronized update.
import assert from 'node:assert/strict';
import { closeSync, openSync, readFileSync, readSync, writeSync } from 'node:fs';
import { join } from 'node:path';

const [input, directory] = process.argv.slice(2);
const terminal = openSync('/dev/tty', 'w');
const emit = bytes => {
  let offset = 0;
  while (offset < bytes.length) {
    const written = writeSync(terminal, bytes, offset, bytes.length - offset);
    assert(written > 0);
    offset += written;
  }
};
const end = Buffer.from('\x1b[?2026l');
const backgrounds = ['255;0;0', '0;255;0', '0;0;255', '0;0;0'];
const updates = Array.from({ length: 24 }, (_, i) => {
  const bytes = readFileSync(join(input, 'frame-' + i + '.bin'));
  assert(bytes.subarray(-end.length).equals(end));
  const marker = Buffer.from('\x1b7\x1b[0m\x1b[1;1H\x1b[48;2;' + (64 + 5 * i) + ';80;80m        ' +
    '\x1b[2;1H\x1b[48;2;' + backgrounds[i % 4] + 'm\x1b[38;2;255;255;255mM\x1b[2;31H \x1b8');
  return Buffer.concat([bytes.subarray(0, -end.length), marker, end]);
});
const signal = Buffer.alloc(1);
let control;
const waitFor = expected => {
  assert.equal(readSync(control, signal, 0, 1, null), 1);
  assert.equal(signal.toString(), expected);
};
const report = openSync(join(directory, 'peer.tsv'), 'w');
writeSync(report, 'sequence\temitted_ns\twritten_ns\n');
emit(readFileSync(join(input, 'warmup.bin')));
for (const [index, bytes] of updates.entries()) {
  if (index > 0) waitFor('n');
  const start = process.hrtime.bigint();
  emit(bytes);
  writeSync(report, [index, start, process.hrtime.bigint()].join('\t') + '\n');
  if (index === 0) control = openSync(join(directory, 'control.fifo'), 'r');
}
waitFor('q');
closeSync(control);
closeSync(report);
closeSync(terminal);
