// Extract the original PTY data from a util-linux script recording.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const [workload, source, destination] = process.argv.slice(2);
assert(['grid', 'btop', 'xray'].includes(workload) && source && destination,
  'usage: node extract_capture_updates.mjs grid|btop|xray SCRIPT-OUTPUT NEW-DIRECTORY');
const capture = readFileSync(source);
const first = capture.indexOf(10) + 1;
const footer = capture.lastIndexOf(Buffer.from('\nScript done on '));
assert(first > 0 && footer > first && capture.toString('ascii', 0, 15) === 'Script started ');
const raw = capture.subarray(first, footer);
const begin = Buffer.from('\x1b[?2026h'), end = Buffer.from('\x1b[?2026l');
const ranges = [];
let offset = 0;
for (;;) {
  const start = raw.indexOf(begin, offset);
  if (start < 0) break;
  const stop = raw.indexOf(end, start + begin.length);
  assert(stop >= 0, 'incomplete synchronized update');
  ranges.push({ offset, end: stop + end.length });
  offset = stop + end.length;
}
const imageReplay = workload === 'xray';
const warmFrames = imageReplay ? ranges.length - 23 : workload === 'btop' ? 42 : 8;
const selected = ranges.slice(warmFrames, warmFrames + 24);
assert(warmFrames > 0 && selected.length === (imageReplay ? 23 : 24));
const warmup = raw.subarray(0, selected[0].offset);
mkdirSync(destination, { mode: 0o700 });
writeFileSync(join(destination, 'warmup.bin'), warmup);
const digest = createHash('sha256').update(warmup);
const sourceRanges = selected.map(({ offset, end }, sequence) => {
  const bytes = raw.subarray(offset, end);
  digest.update(bytes);
  writeFileSync(join(destination, 'frame-' + sequence + '.bin'), bytes);
  return { sequence, offset: first + offset, length: bytes.length };
});
if (imageReplay) {
  const tail = raw.subarray(offset);
  assert(tail.length > 0);
  // Retain the demo's deletion and shell screen in a final observable transaction.
  const bytes = Buffer.concat([begin, tail, end]);
  digest.update(bytes);
  writeFileSync(join(destination, 'frame-23.bin'), bytes);
  sourceRanges.push({ sequence: 23, offset: first + offset, length: tail.length, addedSyncBytes: begin.length + end.length });
}
const manifest = { workload, source, rawBytes: raw.length, rawSha256: createHash('sha256').update(raw).digest('hex'),
  synchronizedUpdates: ranges.length, warmFrames, warmupAndFramesSha256: digest.digest('hex'), sourceRanges };
writeFileSync(join(destination, 'manifest.json'), JSON.stringify(manifest, null, 2) + '\n');
