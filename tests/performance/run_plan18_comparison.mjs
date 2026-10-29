/* boxrig - Under MIT. */
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

const [baselineArg, candidateArg, outputArg, roundsArg] = process.argv.slice(2);
if (!baselineArg || !candidateArg || !outputArg) {
  throw new Error('usage: run_plan18_comparison.mjs BASELINE CANDIDATE OUTPUT [ROUNDS]');
}
const rounds = Number(roundsArg ?? 2);
if (!Number.isInteger(rounds) || rounds < 2) throw new Error('rounds must be at least 2');
const root = process.cwd();
const binaries = { baseline: resolve(baselineArg), candidate: resolve(candidateArg) };
const output = resolve(outputArg);
const peer = join(root, 'build/tests/performance/annotation_gtk_peer');
const probe = join(root, 'build/tests/performance/annotation_gtk_probe');
const script = join(root, 'tests/performance/run_annotation_gtk.sh');
mkdirSync(output, { recursive: true });
const cases = [];
for (let round = 0; round < rounds; round++) {
  for (const [load, history] of [['idle', 0], ['transcript', 10000]]) {
    for (const renderer of ['cairo', 'opengl']) {
      const order = round % 2 === 0 ? ['baseline', 'candidate'] : ['candidate', 'baseline'];
      for (const frontend of order) {
        const name = `${round}-${load}-h${history}-${renderer}-${frontend}`;
        const directory = join(output, name);
        const result = spawnSync('bash', [script, binaries[frontend], peer, probe,
          renderer, String(history), load, 'none', directory], {
          cwd: root,
          env: { ...process.env, BOXRIG_ANNOTATION_GTK_KEYS: '20' },
          stdio: 'inherit',
        });
        cases.push({ frontend, renderer, history, load, capture: 'none', period: 100,
          round, keys: 20, directory, exit_code: result.status });
        writeFileSync(join(output, 'cases.json'), JSON.stringify(cases, null, 2));
        if (result.status !== 0) throw new Error(`${name} failed: ${result.status}`);
      }
    }
  }
}
const summary = spawnSync('node', [join(root, 'tests/performance/summarize_annotation_gtk.mjs'),
  join(output, 'cases.json'), join(output, 'summary.json')], { cwd: root, stdio: 'inherit' });
if (summary.status !== 0) throw new Error('summary failed');
console.log(`plan18 comparison: ${join(output, 'summary.json')}`);
