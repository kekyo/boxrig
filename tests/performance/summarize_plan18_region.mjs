/* boxrig - Under MIT. */
import { readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const [baseline, step2, step3, output] = process.argv.slice(2);
if (!baseline || !step2 || !step3 || !output) throw new Error('expected 3 TSV files and JSON output');
const names = { baseline, step2, step3 };
const result = {};
const stats = (values) => {
  const sorted = values.sort((a, b) => a - b);
  return { count: sorted.length,
    median: sorted.length % 2 === 0 ? (sorted[sorted.length / 2 - 1] +
      sorted[sorted.length / 2]) / 2 : sorted[Math.floor(sorted.length / 2)],
    p95: sorted[Math.ceil(sorted.length * 0.95) - 1],
    min: sorted[0], max: sorted.at(-1) };
};
for (const [version, path] of Object.entries(names)) {
  const [header, ...lines] = readFileSync(resolve(path), 'utf8').trim().split('\n');
  const keys = header.split('\t');
  result[version] = {};
  for (const line of lines) {
    const row = Object.fromEntries(line.split('\t').map((value, index) =>
      [keys[index], index < 2 ? value : Number(value)]));
    (result[version][row.case] ??= []).push(row);
  }
  for (const [name, rows] of Object.entries(result[version])) {
    result[version][name] = Object.fromEntries(keys.slice(2).map((key) =>
      [key, stats(rows.map((row) => row[key]))]));
  }
}
writeFileSync(resolve(output), JSON.stringify({
  workload: '193 columns x 45 rows, font 23 DIP; 20 iterations per case',
  timing: 'CPU draw call only; GL pixel readback outside timed interval waits for completion',
  cases: result,
}, null, 2));
