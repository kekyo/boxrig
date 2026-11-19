/* boxrig - Under MIT. Validate saved pixels independently of the live probe. */
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import assert from 'node:assert/strict';

const [directory, renderer, encodedPath] = process.argv.slice(2);
assert(directory && ['cairo', 'opengl'].includes(renderer));
const isolated = Boolean(process.env.BOXRIG_LATENCY_ISOLATED_CELL);
const read = name => readFileSync(join(directory, name), 'utf8');
const table = name => read(name).trim().split('\n').filter(line => !line.startsWith('#'))
  .slice(1).map(line => line.split('\t').map(Number));
const peer = table('peer.tsv'), probe = table('probe.tsv'), frames = table('frames.tsv');
assert.equal(peer.length, 24);
assert.equal(probe.length, 24);
const grid = read('probe.tsv').match(/# grid=(\d+),(\d+),(\d+),(\d+) rows=(\d+) columns=(\d+)/);
assert(grid);
const [gx, gy, cw, ch, rows, columns] = grid.slice(1).map(Number);
const expectedColor = sequence => [0xff0000, 0x00ff00, 0x0000ff, 0][sequence % 4];
const seen = new Map();
let previous = -1;
const concat = ['ffconcat version 1.0'];
for (let i = 0; i < frames.length; i++) {
  const [index, timestamp, reported, reportedPixel, reportedInk] = frames[i];
  assert.equal(index, i);
  assert(i === 0 || timestamp > frames[i - 1][1]);
  const name = `frame-${String(index).padStart(5, '0')}.ppm`;
  const bytes = readFileSync(join(directory, name));
  let offset = 0;
  const line = () => { const end = bytes.indexOf(10, offset); assert(end >= 0);
    const value = bytes.toString('ascii', offset, end); offset = end + 1; return value; };
  assert.equal(line(), 'P6');
  const [width, height] = line().split(' ').map(Number);
  assert.equal(line(), '255');
  assert.equal(bytes.length - offset, width * height * 3);
  assert(gx + columns * cw <= width && gy + rows * ch <= height);
  const pixel = (x, y) => {
    const at = offset + (y * width + x) * 3;
    return bytes[at] * 65536 + bytes[at + 1] * 256 + bytes[at + 2];
  };
  const marker = pixel(gx + 3 * cw, gy + Math.floor(ch / 2));
  assert.equal(marker & 65535, 0x5050);
  const background = isolated ? 0 : pixel(gx + 30 * cw + Math.floor(cw / 2), gy + ch + Math.floor(ch / 2));
  assert.equal(background, reportedPixel);
  const inkX = isolated ? gx + (columns - 1) * cw : gx;
  let ink = 0;
  for (let y = gy + ch; y < gy + 2 * ch; y++)
    for (let x = inkX; x < inkX + cw; x++) if (pixel(x, y) !== background) ink++;
  assert.equal(ink, reportedInk);
  let sequence = ((marker >> 16) - 64) / 5;
  if (isolated) {
    assert.equal(marker, 0x405050);
    const next = previous + 1;
    sequence = (next % 2 === 0) === (ink > 0) ? next : previous;
  } else {
    assert(ink > 0);
    assert.equal(background, expectedColor(sequence), `image state ${sequence}, frame ${index}`);
  }
  assert(Number.isInteger(sequence) && sequence >= previous && sequence < 24);
  assert.equal(sequence, reported);
  if (sequence >= 0 && !seen.has(sequence)) seen.set(sequence, timestamp);
  previous = sequence;
  const duration = i + 1 < frames.length ? (frames[i + 1][1] - timestamp) / 1e9 : 0.017;
  concat.push(`file '${name}'`, 'option framerate 1000000', `duration ${duration.toFixed(9)}`);
}
assert.equal(seen.size, 24);
assert.equal(previous, 23);
writeFileSync(join(directory, 'video.ffconcat'), `${concat.join('\n')}\n`);
const captureInput = process.env.BOXRIG_CAPTURE_INPUT;
const syntheticTrace = !captureInput && !isolated;
const trace = [];
if (syntheticTrace) {
  const traceText = read('trace.tsv');
  assert(traceText.startsWith('# dropped=0\n'));
  for (const line of traceText.trim().split('\n').slice(2)) {
    const [name, ...values] = line.split('\t');
    const [start, end, cpu, units, revision, status, more] = values.map(Number);
    assert(start > 0 && end >= start && cpu >= 0 && (name === 'read' || status === 0));
    trace.push({ name, start, end, cpu, units, revision, more });
  }
}
const phases = ['read', 'feed', 'model', renderer === 'cairo' ? 'cairo' : 'gl_draw',
  renderer === 'cairo' ? 'paint' : 'gl_present'];
const inputToVisible = [], writeToVisible = [], intervals = [];
for (let i = 0; i < 24; i++) {
  const [sequence, emitted, written] = peer[i];
  assert.equal(sequence, i); assert.equal(probe[i][0], i);
  assert(emitted <= written && written <= seen.get(i));
  assert.equal(probe[i][2], seen.get(i));
  if (i === 0) continue; // Startup is separately visible but is not a steady-state sample.
  assert(probe[i][1] <= emitted);
  if (syntheticTrace) for (const name of phases)
    assert(trace.some(event => event.name === name && event.end >= emitted && event.end <= seen.get(i)),
      `missing ${name} for frame ${i}`);
  inputToVisible.push((seen.get(i) - emitted) / 1e6);
  writeToVisible.push((seen.get(i) - written) / 1e6);
  intervals.push((seen.get(i) - seen.get(i - 1)) / 1e6);
}
const statistics = values => {
  const sorted = values.toSorted((a, b) => a - b);
  assert(sorted.length > 0);
  const percentile = p => sorted[Math.max(0, Math.ceil(sorted.length * p) - 1)];
  return { count: sorted.length, median: percentile(0.5), p95: percentile(0.95),
    p99: percentile(0.99), max: sorted.at(-1), over_33_3_ms: values.filter(v => v > 33.3).length };
};
const rss = probe.slice(7).map(row => row[3]);
if (encodedPath) {
  const times = readFileSync(encodedPath, 'utf8').trim().split('\n').map(Number);
  assert.equal(times.length, frames.length);
  for (let i = 0; i < times.length; i++)
    assert(Math.abs(times[i] - (frames[i][1] - frames[0][1]) / 1e9) <= 0.0011);
}
const summary = { renderer, rows, columns, cell_width: cw, cell_height: ch,
  frame_count: frames.length, transitions: 24,
  input_to_visible_ms: statistics(inputToVisible), write_end_to_visible_ms: statistics(writeToVisible),
  observed_intervals_ms: statistics(intervals),
  warm_rss_min_kib: Math.min(...rss), warm_rss_max_kib: Math.max(...rss),
  encoded_video_verified: Boolean(encodedPath) };
if (captureInput) {
  summary.input_sha256 = JSON.parse(readFileSync(join(captureInput, 'manifest.json'), 'utf8')).warmupAndFramesSha256;
} else if (isolated) {
  summary.input_bytes_per_update = 1;
  if (process.env.BOXRIG_LATENCY_KEY_ECHO)
    summary.key_to_visible_ms = statistics(probe.slice(1).map(([, sent, visible]) => (visible - sent) / 1e6));
} else {
  const imageStates = trace.filter(event => event.name === 'image_state');
  assert(imageStates.length > 0 && imageStates.every(event => event.units <= 1 && event.revision <= 4));
  assert.equal(imageStates.at(-1).units, 0);
  assert.equal(imageStates.at(-1).revision, 0);
  const uploads = trace.filter(event => event.name === (renderer === 'cairo' ? 'cairo_uploads' : 'gl_uploads'));
  assert.equal(Math.max(...uploads.map(event => event.units)), 18);
  assert(Math.max(...rss) - Math.min(...rss) < 16384, 'RSS grows beyond the warm-up allowance');
  Object.assign(summary, { trace_dropped: 0,
    phases_cpu_ms: Object.fromEntries(phases.map(name => [name,
      statistics(trace.filter(event => event.name === name).map(event => event.cpu / 1e6))])),
    peak_images: Math.max(...imageStates.map(event => event.units)),
    peak_image_bytes: Math.max(...imageStates.map(event => event.revision)),
    image_uploads: Math.max(...uploads.map(event => event.units)),
    gpu_upload_bytes: renderer === 'opengl' ? Math.max(...uploads.map(event => event.revision)) : null });
}
if (existsSync(join(directory, 'terminal-cpu.tsv'))) {
  const samples = table('terminal-cpu.tsv');
  assert.equal(samples.length, 24);
  for (let i = 0; i < samples.length; i++)
    assert(samples[i][0] === i && (i === 0 || samples[i][1] > samples[i - 1][1]));
  summary.process_cpu_ms_per_update = (samples.at(-1)[1] - samples[0][1]) / 23e6;
}
if (existsSync(join(directory, 'host-cpu.tsv'))) {
  const [first, last] = table('host-cpu.tsv');
  assert(first[0] === 0 && last[0] === 23 && last[1] > first[1]);
  summary.host_busy_percent = 100 * (1 - (last[2] - first[2]) / (last[1] - first[1]));
}
writeFileSync(join(directory, 'summary.json'), `${JSON.stringify(summary, null, 2)}\n`);
console.log(`PASS ${renderer} ${columns}x${rows}: 24 visible transitions, ${frames.length} video frames`);
