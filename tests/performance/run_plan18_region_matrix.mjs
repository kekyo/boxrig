/* boxrig - Under MIT. */
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

const output = resolve(process.argv[2] ?? 'artifacts/plan18/region-matrix');
const variants = resolve(process.env.BOXRIG_PLAN18_VARIANTS ?? 'artifacts/plan18');
const cairo = process.env.BOXRIG_PLAN18_CAIRO === '1';
mkdirSync(output, { recursive: true });
const versions = ['baseline', 'step2', 'step3'];
const samples = [];
const median = (values) => {
  const sorted = [...values].sort((a, b) => a - b);
  const middle = sorted.length / 2;
  return sorted.length % 2 === 0 ? (sorted[middle - 1] + sorted[middle]) / 2 :
    sorted[Math.floor(middle)];
};
for (let round = 0; round < 3; round++) {
  const order = round % 2 === 0 ? versions : [...versions].reverse();
  for (const version of order) {
    const binary = join(variants, version, `${cairo ? 'cairo_' : ''}region_bench`);
    const path = join(output, `${round}-${version}.tsv`);
    const result = spawnSync(binary, [], { encoding: 'utf8' });
    if (result.status !== 0) throw new Error(`${version} round ${round}: ${result.stderr}`);
    writeFileSync(path, result.stdout);
    writeFileSync(join(output, `${round}-${version}.stderr.log`), result.stderr);
    const [header, ...lines] = result.stdout.trim().split('\n');
    const keys = header.split('\t');
    for (const line of lines) {
      const row = Object.fromEntries(line.split('\t').map((value, index) =>
        [keys[index], index < 2 ? value : Number(value)]));
      samples.push({ round, version, ...row });
    }
  }
}
const aggregate = {};
for (const version of versions) {
  aggregate[version] = {};
  for (const kind of [...new Set(samples.map((sample) => sample.case))]) {
    const rows = samples.filter((sample) => sample.version === version && sample.case === kind);
    const times = rows.map((row) => row.cpu_ns).sort((a, b) => a - b);
    const summary = {
      count: times.length, median_ns: median(times),
      p95_ns: times[Math.ceil(times.length * 0.95) - 1],
      minimum_ns: times[0], maximum_ns: times.at(-1),
      round_median_ns: [0, 1, 2].map((round) =>
        median(rows.filter((row) => row.round === round).map((row) => row.cpu_ns))),
      rows: [...new Set(rows.map((row) => row.rows))],
    };
    for (const key of ['state_queries', 'state_changes', 'state_restores',
      'attribute_pointers', 'uniform_lookups']) {
      if (rows[0][key] !== undefined) summary[key] = [...new Set(rows.map((row) => row[key]))];
    }
    aggregate[version][kind] = summary;
  }
}
writeFileSync(join(output, 'summary.json'), JSON.stringify({
  method: `3 alternating rounds; ${cairo ? 'Cairo' : 'OpenGL'}; 20 iterations per case; 193x45 font 23 DIP`,
  timing: 'CPU draw call; pixel readback outside timing', aggregate,
}, null, 2));
