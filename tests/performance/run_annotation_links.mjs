/* boxrig - Under MIT. */
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { spawnSync } from 'node:child_process';

const [beforePath, afterPath, outputPath] = process.argv.slice(2);
if (!beforePath || !afterPath || !outputPath) {
  throw new Error('expected BEFORE_BINARY AFTER_BINARY OUTPUT');
}
const binaries = { before: resolve(beforePath), after: resolve(afterPath) };
const output = resolve(outputPath);
mkdirSync(output, { recursive: true });
const cases = [];
const manifest = join(output, 'cases.json');
const run = (version, renderer, mode, history, capture, round) => {
  const name = `${round}-${version}-${renderer}-${mode}-h${history}-${capture}`;
  const directory = join(output, name);
  console.log(`START ${name}`);
  const result = spawnSync('bash', ['tests/performance/run_annotation_gtk.sh',
    binaries[version], 'build/tests/performance/annotation_gtk_peer',
    'build/tests/performance/annotation_gtk_probe', renderer, String(history),
    'transcript', capture, directory], {
    env: { ...process.env, BOXRIG_ANNOTATION_LINK_MODE: mode,
      BOXRIG_ANNOTATION_URL_PERIOD: '10', BOXRIG_ANNOTATION_GTK_KEYS: '12' },
    stdio: 'inherit',
  });
  const item = { version, renderer, mode, history, capture, round, directory,
    exit_code: result.status };
  if (result.status === 0) {
    item.latency = JSON.parse(readFileSync(join(directory, 'latency.json'), 'utf8'));
  }
  cases.push(item);
  writeFileSync(manifest, `${JSON.stringify(cases, null, 2)}\n`);
  if (result.status !== 0) throw new Error(`failed ${name}: ${result.status}`);
  console.log(`DONE ${name}`);
};

// Compare uninstrumented binaries serially. Capture changes the sampling cost,
// so video trials stay separate from the three repeated timing trials.
for (const round of [0, 1, 2]) {
  for (const renderer of ['cairo', 'opengl']) {
    for (const mode of ['regex', 'osc8', 'both']) {
      for (const version of round === 1 ? ['after', 'before'] : ['before', 'after']) {
        run(version, renderer, mode, 10050, 'none', round);
      }
    }
  }
}
for (const renderer of ['cairo', 'opengl']) {
  for (const mode of ['regex', 'osc8', 'both']) {
    for (const version of ['before', 'after']) {
      run(version, renderer, mode, 0, 'none', 'control');
      run(version, renderer, mode, 10050, 'video', 'visual');
    }
  }
}

const stats = (values) => {
  const sorted = values.toSorted((left, right) => left - right);
  const mid = Math.floor(sorted.length / 2);
  return { count: sorted.length,
    median_ms: sorted.length % 2 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2,
    p95_ms: sorted[Math.ceil(sorted.length * 0.95) - 1], max_ms: sorted.at(-1) };
};
const summary = [];
for (const renderer of ['cairo', 'opengl']) {
  for (const mode of ['regex', 'osc8', 'both']) {
    for (const version of ['before', 'after']) {
      const trials = cases.filter((item) => item.renderer === renderer &&
        item.mode === mode && item.version === version && typeof item.round === 'number');
      summary.push({ renderer, mode, version, trials: trials.length,
        keys: Object.fromEntries([0, 1, 2].map((phase) => [phase,
          stats(trials.flatMap((item) => item.latency.keys)
            .filter((key) => key.phase === phase).map((key) => key.total_ms))])),
        pointers: Object.fromEntries((mode === 'regex' ? [] : ['during', 'after'])
          .map((phase) => [phase, stats(trials.flatMap((item) => item.latency.pointers)
            .filter((pointer) => pointer.phase === phase).map((pointer) => pointer.latency_ms))])),
      });
    }
  }
}
writeFileSync(join(output, 'summary.json'), `${JSON.stringify(summary, null, 2)}\n`);
console.log(`SUMMARY ${join(output, 'summary.json')}`);
