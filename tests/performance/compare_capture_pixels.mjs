// Compare the whole terminal at each independently observed update.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const load = dir => {
  const probe = readFileSync(join(dir, 'probe.tsv'), 'utf8');
  const match = /# grid=(\d+),(\d+),(\d+),(\d+) rows=(\d+) columns=(\d+)/.exec(probe);
  assert(match);
  const [x, y, cw, ch, rows, columns] = match.slice(1).map(Number);
  const times = probe.split('\n').filter(line => /^\d+\t/.test(line)).map(line => line.split('\t')[2]);
  assert.equal(times.length, 24);
  const seen = new Map(readFileSync(join(dir, 'frames.tsv'), 'utf8').trim().split('\n').slice(1)
    .map(line => { const [index, time] = line.split('\t'); return [time, index]; }));
  return { width: cw * columns, height: ch * rows, frame: seq => {
    const index = seen.get(times[seq]);
    assert(index !== undefined);
    const image = readFileSync(join(dir, 'frame-' + index.padStart(5, '0') + '.ppm'));
    const header = /^P6\n(\d+) (\d+)\n255\n/.exec(image.toString('ascii', 0, 40));
    assert(header);
    const stride = Number(header[1]) * 3;
    return row => image.subarray(header[0].length + stride * (row + y) + x * 3,
      header[0].length + stride * (row + y) + (x + cw * columns) * 3);
  }};
};
const [before, after] = process.argv.slice(2).map(load);
assert(before && after && before.width === after.width && before.height === after.height);
for (let seq = 0; seq < 24; seq++) {
  const first = before.frame(seq), second = after.frame(seq);
  for (let row = 0; row < before.height; row++) {
    const a = first(row), b = second(row);
    if (!a.equals(b)) {
      const column = Math.floor(a.findIndex((v, i) => v !== b[i]) / 3);
      assert.fail('different pixel in update ' + seq + ' at ' + column + ',' + row + ' (' +
        a.readUIntBE(column * 3, 3).toString(16) + ' / ' + b.readUIntBE(column * 3, 3).toString(16) + ')');
    }
  }
}
console.log('PASS 24 identical terminal images at', before.width, 'x', before.height);
