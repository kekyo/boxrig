/* boxrig - Under MIT. */
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

const root = process.cwd();
const output = resolve(process.argv[2] ?? 'artifacts/plan18/baseline-xvfb');
const rounds = Number(process.argv[3] ?? 3);
if (!Number.isInteger(rounds) || rounds < 2) throw new Error('rounds must be at least 2');
const binary = resolve(process.env.BOXRIG_PLAN18_BINARY ??
  join(root, 'build/boxrig_term/gtk3/boxrig_term'));
const peer = join(root, 'build/tests/performance/annotation_gtk_peer');
const probe = join(root, 'build/tests/performance/annotation_gtk_probe');
const script = join(root, 'tests/performance/run_annotation_gtk.sh');
mkdirSync(output, { recursive: true });
const cases = [];
for (let round = 0; round < rounds; round++) {
  for (const [load, history] of [['idle', 0], ['transcript', 10000]]) {
    const order = round % 2 === 0 ? ['cairo', 'opengl'] : ['opengl', 'cairo'];
    for (const renderer of order) {
      const name = `${round}-${load}-h${history}-${renderer}`;
      const directory = join(output, name);
      const result = spawnSync('bash', [script, binary, peer, probe,
        renderer, String(history), load, 'none', directory], {
        cwd: root,
        env: { ...process.env, BOXRIG_ANNOTATION_GTK_KEYS: '20',
          BOXRIG_ANNOTATION_BINARY_REVISION:
            process.env.BOXRIG_PLAN18_BINARY_REVISION ?? 'unverified' },
        stdio: 'inherit',
      });
      cases.push({ frontend: 'current', renderer, history, load,
        capture: 'none', period: 100, round, keys: 20, directory,
        exit_code: result.status });
      writeFileSync(join(output, 'cases.json'), JSON.stringify(cases, null, 2));
      if (result.status !== 0) throw new Error(`${name} failed: ${result.status}`);
    }
  }
}
const summary = spawnSync('node', [join(root, 'tests/performance/summarize_annotation_gtk.mjs'),
  join(output, 'cases.json'), join(output, 'summary.json')], {
  cwd: root, stdio: 'inherit',
});
if (summary.status !== 0) throw new Error('summary failed');
const data = JSON.parse(readFileSync(join(output, 'summary.json'), 'utf8'));
const result = data.aggregate.map(({ renderer, history, load, phase, total_ms, paint_ms }) =>
  ({ renderer, history, load, phase, total_ms, paint_ms }));
writeFileSync(join(output, 'latency.json'), JSON.stringify(result, null, 2));
console.log(`plan18 baseline: ${join(output, 'summary.json')}`);
