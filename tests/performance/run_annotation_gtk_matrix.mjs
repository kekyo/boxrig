/* boxrig - Under MIT. */
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';

const output = resolve(process.argv[2] ?? 'artifacts/plan11/gtk-matrix');
const root = process.cwd();
const binaries = {
  baseline: join(root, 'artifacts/plan11/baseline/boxrig_term'),
  patched: join(root, 'build/boxrig_term/gtk3/boxrig_term'),
};
const peer = join(root, 'build/tests/performance/annotation_gtk_peer');
const probe = join(root, 'build/tests/performance/annotation_gtk_probe');
const counted = join(root, 'build/tests/performance/annotation_gtk_counted');
const baselineCounted = join(root, 'build/tests/performance/annotation_gtk_counted_baseline');
const script = join(root, 'tests/performance/run_annotation_gtk.sh');
mkdirSync(output, { recursive: true });
const manifest = join(output, 'cases.json');
const cases = [];
const countReferenceRoot = dirname(output);
for (const [frontend, period] of [['baseline', 100], ['patched', 100], ['patched', 10]]) {
  const directory = join(countReferenceRoot,
    frontend === 'baseline' ? 'gtk-count-baseline-100' : `gtk-count-${period}`);
  const result = spawnSync('bash', [script, frontend === 'baseline' ? baselineCounted : counted, peer, probe,
    'cairo', '10000', 'idle', 'none', directory], {
    cwd: root,
    env: { ...process.env, BOXRIG_ANNOTATION_URL_PERIOD: String(period),
      BOXRIG_ANNOTATION_GTK_KEYS: '3' },
    stdio: 'inherit',
  });
  if (result.status !== 0) throw new Error(`count companion ${frontend} a${period} failed`);
}
const run = (frontend, renderer, history, load, capture, period, round, keys) => {
  const name = `${round}-${frontend}-${renderer}-h${history}-${load}-${capture}-a${period}`;
  const directory = join(output, name);
  console.log(`START ${name}`);
  const result = spawnSync('bash', [script, binaries[frontend], peer, probe,
    renderer, String(history), load, capture, directory], {
    cwd: root,
    env: { ...process.env, BOXRIG_ANNOTATION_URL_PERIOD: String(period),
      BOXRIG_ANNOTATION_GTK_KEYS: String(keys) },
    stdio: 'inherit',
  });
  cases.push({ frontend, renderer, history, load, capture, period, round, keys,
    directory, exit_code: result.status });
  writeFileSync(manifest, JSON.stringify(cases, null, 2));
  if (result.status !== 0) throw new Error(`${name} failed: ${result.status}`);
  console.log(`DONE ${name}`);
};

for (const round of [0, 1]) {
  for (const renderer of ['cairo', 'opengl']) {
    for (const history of [0, 10000]) {
      for (const load of ['idle', 'transcript']) {
        const frontends = round === 0 ? ['baseline', 'patched'] : ['patched', 'baseline'];
        for (const frontend of frontends) run(frontend, renderer, history, load,
          'none', 100, round, 20);
      }
    }
  }
}
for (const round of [0, 1]) {
  run('patched', 'cairo', 10000, 'idle', 'none', 10, round, 20);
}
for (const [history, load] of [[0, 'transcript'], [10000, 'idle']]) {
  for (const frontend of ['baseline', 'patched']) run(frontend, 'cairo', history,
    load, 'video', 100, 2, 3);
}
const summary = spawnSync('node', [join(root, 'tests/performance/summarize_annotation_gtk.mjs'),
  join(output, 'cases.json'), join(output, 'summary.json')], {
    cwd: root, stdio: 'inherit',
    env: { ...process.env, BOXRIG_ANNOTATION_COUNT_REFERENCE_DIR: countReferenceRoot },
  });
if (summary.status !== 0) throw new Error('summary failed');
console.log(`SUMMARY ${join(output, 'summary.json')}`);
