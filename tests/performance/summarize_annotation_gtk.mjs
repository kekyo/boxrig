/* boxrig - Under MIT. */
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';

const [casesPath, outputPath] = process.argv.slice(2);
if (!casesPath || !outputPath) throw new Error('expected cases and summary paths');
const cases = JSON.parse(readFileSync(casesPath, 'utf8'));
const lines = (path) => readFileSync(path, 'utf8').trim().split('\n');
const quantile = (values, portion) => {
  const sorted = [...values].sort((left, right) => left - right);
  return sorted[Math.max(0, Math.ceil(sorted.length * portion) - 1)];
};
const median = (values) => {
  const sorted = [...values].sort((left, right) => left - right);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 === 0 ?
    (sorted[middle - 1] + sorted[middle]) / 2 : sorted[middle];
};
const statistics = (values) => ({
  count: values.length,
  median: median(values),
  p95: quantile(values, 0.95),
  maximum: Math.max(...values),
});
const trials = [];
const groups = new Map();
const countReferenceRoot = resolve(process.env.BOXRIG_ANNOTATION_COUNT_REFERENCE_DIR ??
  'artifacts/plan11');
const reference = (frontend, period, history, peerHash) => {
  if (history === 0) return { count: null, reason: 'no counted H0 companion trial' };
  const directory = `${countReferenceRoot}/${frontend === 'baseline' ?
    'gtk-count-baseline-100' : `gtk-count-${period}`}`;
  const countPath = `${directory}/count.tsv`;
  const configPath = `${directory}/configuration.txt`;
  if (!existsSync(countPath) || !existsSync(configPath)) {
    return { count: null, reason: 'counted companion trial unavailable' };
  }
  const companionHash = lines(configPath).find((line) => line.includes('/annotation_gtk_peer'))?.split(' ')[0];
  if (companionHash !== peerHash) {
    return { count: null, reason: 'counted companion peer hash differs', path: countPath };
  }
  const settled = lines(countPath).map((line) => line.split('\t').map(Number))
    .find((row) => row[1] === history - 23 && row[2] > 0 && row[7] === 0);
  if (!settled) return { count: null, reason: 'no complete matching history count', path: countPath };
  return { count: settled[2], history_rows: settled[1], path: countPath,
    settled_ns: settled[0], reference_renderer: 'cairo',
    scope: 'initial history before transcript output; renderer-specific count not sampled' };
};
for (const item of cases) {
  if (item.exit_code !== 0) throw new Error(`incomplete trial ${item.directory}`);
  const peerLines = lines(`${item.directory}/peer.tsv`);
  const probeLines = lines(`${item.directory}/probe.tsv`);
  const peer = peerLines.filter((line) => !line.startsWith('#')).slice(1)
    .map((line) => line.split('\t').map(Number));
  const probe = probeLines.filter((line) => !line.startsWith('#')).slice(1)
    .map((line) => line.split('\t').map(Number));
  if (peer.length !== item.keys || probe.length !== item.keys) {
    throw new Error(`missing keys in ${item.directory}`);
  }
  const header = Object.fromEntries(peerLines[0].slice(2).split(' ')
    .map((field) => field.split('=')));
  const resources = Object.fromEntries(lines(`${item.directory}/resources.txt`)[0]
    .split(' ').map((field) => field.split('=')));
  const peerHash = lines(`${item.directory}/configuration.txt`)
    .find((line) => line.includes('/annotation_gtk_peer'))?.split(' ')[0];
  const cpuSeconds = Number(resources.user_s) + Number(resources.system_s);
  const parsedKeys = probe.map((row, index) => {
    const [, received, written] = peer[index];
    const [key, sent, seen, phase] = row;
    const measurements = {
      total_ms: (seen - sent) / 1e6,
      pty_ms: (received - sent) / 1e6,
      write_ms: (written - received) / 1e6,
      paint_ms: (seen - written) / 1e6,
    };
    const phaseName = item.load === 'idle' ? 'idle' :
      ['output', 'stop_immediate', 'after_barrier'][phase];
    const groupKey = [item.frontend, item.renderer, item.history, item.load,
      item.capture, item.period, phaseName].join('/');
    if (!groups.has(groupKey)) groups.set(groupKey, []);
    groups.get(groupKey).push(measurements);
    return { key, phase: phaseName, ...measurements };
  });
  trials.push({ ...item,
    rows: Number(header.rows), columns: Number(header.columns),
    estimated_history_rows: Number(header.estimated_history_rows),
    emitted_lines: Number(header.emitted_lines),
    initial_annotation_reference: reference(item.frontend, item.period, item.history, peerHash),
    cpu_seconds: cpuSeconds, wall_seconds: Number(resources.elapsed_s),
    max_rss_kb: Number(resources.max_rss_kb), keys: parsedKeys,
  });
}
const aggregate = [];
for (const [key, values] of groups) {
  const [frontend, renderer, history, load, capture, period, phase] = key.split('/');
  aggregate.push({ frontend, renderer, history: Number(history), load, capture,
    period: Number(period), phase,
    trial_cpu_seconds: statistics(trials.filter((trial) =>
      trial.frontend === frontend && trial.renderer === renderer &&
      trial.history === Number(history) && trial.load === load &&
      trial.capture === capture && trial.period === Number(period))
      .map((trial) => trial.cpu_seconds)),
    total_ms: statistics(values.map((item) => item.total_ms)),
    pty_ms: statistics(values.map((item) => item.pty_ms)),
    write_ms: statistics(values.map((item) => item.write_ms)),
    paint_ms: statistics(values.map((item) => item.paint_ms)),
  });
}
writeFileSync(outputPath, JSON.stringify({
  method: 'Xvfb 1280x800x24; LIBGL_ALWAYS_SOFTWARE=1; frame timestamps are monotonic clock',
  trials, aggregate,
  exclusions: existsSync(join(dirname(casesPath), 'exclusions.json')) ?
    JSON.parse(readFileSync(join(dirname(casesPath), 'exclusions.json'), 'utf8')) : [],
}, null, 2));
console.log(`summarized ${trials.length} trials and ${aggregate.length} groups`);
