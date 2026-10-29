/* boxrig - Under MIT. */
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

const root = process.cwd();
const output = resolve(process.argv[2] ?? 'artifacts/plan18/trace-gpu');
const variants = resolve(process.env.BOXRIG_PLAN18_VARIANTS ?? 'artifacts/plan18');
const versions = ['baseline', 'step2', 'step3'];
const revisions = { baseline: '89548bf', step2: 'e1909c5', step3: 'aae9d33' };
const peer = join(root, 'build/tests/performance/annotation_gtk_peer');
const probe = join(root, 'build/tests/performance/annotation_gtk_probe');
const script = join(root, 'tests/performance/run_annotation_gtk.sh');
mkdirSync(output, { recursive: true });
const cases = [];
for (let round = 0; round < 2; round++) {
  for (const [load, history] of [['idle', 0], ['transcript', 10000]]) {
    for (const renderer of ['cairo', 'opengl']) {
      for (const frontend of round === 0 ? versions : [...versions].reverse()) {
        const name = `${round}-${load}-h${history}-${renderer}-${frontend}`;
        const directory = join(output, name);
        const binary = join(variants, frontend, 'boxrig_term_traced');
        const result = spawnSync('bash', [script, binary, peer, probe,
          renderer, String(history), load, 'none', directory], {
          cwd: root,
          env: { ...process.env, BOXRIG_PLAN18_TRACE: '1',
            BOXRIG_ANNOTATION_GTK_KEYS: '20',
            BOXRIG_ANNOTATION_BINARY_REVISION: revisions[frontend] },
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
for (const [scriptName, outputName] of [
  ['summarize_annotation_gtk.mjs', 'summary.json'],
  ['summarize_plan18_trace.mjs', 'trace-summary.json'],
]) {
  const args = scriptName === 'summarize_annotation_gtk.mjs' ?
    [join(output, 'cases.json'), join(output, outputName)] : [output];
  const result = spawnSync('node', [join(root, `tests/performance/${scriptName}`), ...args], {
    cwd: root, stdio: 'inherit',
  });
  if (result.status !== 0) throw new Error(`${scriptName} failed`);
}
