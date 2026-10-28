/* boxrig - Under MIT. */
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';

const [peerPath, probePath, framesPath, encodedTimesPath] = process.argv.slice(2);
if (!peerPath || !probePath) throw new Error('expected peer and probe reports');
const parse = (path) => readFileSync(path, 'utf8').trim().split('\n')
  .filter((line) => line && !line.startsWith('#'))
  .slice(1).map((line) => line.split('\t').map(Number));
const peer = parse(peerPath);
const probe = parse(probePath);
const peerText = readFileSync(peerPath, 'utf8');
const probeText = readFileSync(probePath, 'utf8');
const transcript = peerText.split('\n')[0].includes('load=transcript');
const requestedHistory = Number(peerText.match(/requested_history=(\d+)/)?.[1]);
if (requestedHistory >= 10050 && !peerText.includes('history_full=1')) {
  throw new Error('preload did not reach the configured history capacity');
}
if (peer.length < 1 || probe.length !== peer.length) throw new Error('missing key responses');
const timings = [];
for (let index = 0; index < peer.length; index++) {
  const [key, received, written] = peer[index];
  const [probeKey, sent, seen, phase] = probe[index];
  const expectedPhase = !transcript || index < Math.floor(peer.length / 2) ? 0 :
    index === Math.floor(peer.length / 2) ? 1 : 2;
  if (key !== index + 1 || probeKey !== key ||
      phase !== expectedPhase ||
      !(sent <= received && received <= written && written <= seen)) {
    throw new Error(`invalid endpoint order for key ${index + 1}`);
  }
  console.log(`key=${key} phase=${phase} total_ms=${((seen - sent) / 1e6).toFixed(3)} ` +
    `pty_ms=${((received - sent) / 1e6).toFixed(3)} ` +
    `paint_ms=${((seen - written) / 1e6).toFixed(3)}`);
  timings.push({ key, phase, total_ms: (seen - sent) / 1e6,
    pty_ms: (received - sent) / 1e6, paint_ms: (seen - written) / 1e6 });
}
const pointers = [...probeText.matchAll(/# pointer phase=(\w+) mode=osc8 sent_ns=(\d+) seen_ns=(\d+)/g)]
  .map(([, phase, sent, seen]) => ({ phase, sent: Number(sent), seen: Number(seen),
    latency_ms: (Number(seen) - Number(sent)) / 1e6 }));
const regexPointers = [...probeText.matchAll(/# pointer phase=(\w+) mode=regex underline_preserved=([01])/g)]
  .map(([, phase, preserved]) => ({ phase, underline_preserved: preserved === '1' }));
for (const pointer of pointers) {
  if (!(pointer.seen >= pointer.sent)) throw new Error('invalid pointer endpoint order');
}
writeFileSync(join(dirname(probePath), 'latency.json'),
  `${JSON.stringify({ keys: timings, pointers, regex_pointers: regexPointers }, null, 2)}\n`);
if (framesPath) {
  const frames = readFileSync(framesPath, 'utf8').trim().split('\n').slice(1)
    .map((line) => {
      const [index, phase, time] = line.split('\t');
      return { index: Number(index), phase, time: Number(time) };
    });
  const grid = probeText.match(/# grid gx=(\d+) gy=(\d+) cw=(\d+) ch=(\d+) link_mode=(regex|osc8|both)/);
  if (!grid) throw new Error('video lacks grid calibration');
  const [, gxText, gyText, cwText, chText, linkMode] = grid;
  const [gx, gy, cw, ch] = [gxText, gyText, cwText, chText].map(Number);
  if (cw <= 0 || ch <= 0) throw new Error('invalid video grid');
  const observations = new Map();
  // Inspect the saved image bytes independently of the live probe's verdict.
  // Only compact observations are retained, so long videos do not accumulate
  // full-resolution frame buffers in the checker.
  const inspect = (frame) => {
    if (observations.has(frame.index)) return observations.get(frame.index);
    const path = join(dirname(framesPath), `frame-${String(frame.index).padStart(5, '0')}.ppm`);
    const bytes = readFileSync(path);
    let offset = 0;
    const headerLine = () => {
      const end = bytes.indexOf(10, offset);
      if (end < 0) throw new Error(`invalid PPM header: ${path}`);
      const line = bytes.toString('ascii', offset, end);
      offset = end + 1;
      return line;
    };
    if (headerLine() !== 'P6') throw new Error(`unexpected image format: ${path}`);
    const dimensions = headerLine().split(' ').map(Number);
    const [width, height] = dimensions;
    if (dimensions.length !== 2 || width <= 0 || height <= 0 ||
        headerLine() !== '255' || bytes.length - offset !== width * height * 3) {
      throw new Error(`invalid PPM dimensions: ${path}`);
    }
    const pixel = (x, y) => {
      if (x < 0 || y < 0 || x >= width || y >= height) throw new Error('pixel outside frame');
      const at = offset + (y * width + x) * 3;
      return (bytes[at] << 16) | (bytes[at + 1] << 8) | bytes[at + 2];
    };
    const region = (row, column, columns) => {
      const color = linkMode === 'osc8' || (linkMode === 'both' && row >= 4) ?
        0x1abfe6 : 0xffffff;
      let underline = 0;
      let bright = 0;
      for (let y = gy + (row - 1) * ch; y < gy + row * ch; y++) {
        let run = 0;
        for (let x = gx + column * cw; x < gx + (column + columns) * cw; x++) {
          const value = pixel(x, y);
          if (value !== 0) bright++;
          run = value === color ? run + 1 : 0;
          if (y >= gy + (row - 1) * ch + Math.floor(ch / 2)) underline = Math.max(underline, run);
        }
      }
      return { underline, bright };
    };
    const value = {
      key: pixel(gx + 3 * cw, gy + 23 * ch + Math.floor(ch / 2)),
      rows: Object.fromEntries([2, 3, 4, 5].map((row) => [row, region(row, 11, 37)])),
      footer: region(24, 27, 21),
    };
    observations.set(frame.index, value);
    return value;
  };
  const required = ['key', 'before', 'create', 'created', 'modify', 'modified',
    'scroll', 'scrolled', 'erase', 'erased'];
  for (const phase of required) {
    if (!frames.some((frame) => frame.phase === phase)) throw new Error(`missing video phase ${phase}`);
  }
  for (let index = 0; index < frames.length; index++) {
    if (frames[index].index !== index ||
        (index > 0 && frames[index].time <= frames[index - 1].time)) {
      throw new Error('video frame order invalid');
    }
  }
  for (const [, sent, seen] of probe) {
    if (!frames.some((frame) => frame.phase === 'key' &&
        frame.time >= sent && frame.time <= seen)) {
      throw new Error('key response lacks observed video frame');
    }
  }
  for (const [key, sent, seen] of probe) {
    const expected = ((64 + key * 5) << 16) | 0x5050;
    if (!frames.some((frame) => frame.phase === 'key' &&
        frame.time >= sent && frame.time <= seen && inspect(frame).key === expected)) {
      throw new Error(`key ${key} has no matching response pixels in its video interval`);
    }
  }
  for (const [phase, row, present] of [
    ['before', 3, false], ['created', 3, true], ['modified', 3, true],
    ['scrolled', 2, true], ['erased', 2, false],
  ]) {
    const frame = frames.findLast((entry) => entry.phase === phase);
    const region = inspect(frame).rows[row];
    if (present ? region.underline < cw * 2 : region.bright !== 0) {
      throw new Error(`video pixels do not show ${phase}`);
    }
  }
  if (linkMode === 'both') {
    for (const [phase, row, state] of [
      ['osc-created', 5, 'underlined'], ['osc-hover-off', 5, 'plain'],
      ['osc-scrolled', 4, 'underlined'], ['osc-erased', 4, 'blank'],
    ]) {
      const frame = frames.findLast((entry) => entry.phase === phase);
      if (!frame) throw new Error(`missing video phase ${phase}`);
      const region = inspect(frame).rows[row];
      if (state === 'underlined' ? region.underline < cw * 2 :
          state === 'plain' ? region.underline >= cw * 2 || region.bright === 0 :
          region.bright !== 0) throw new Error(`invalid OSC 8 pixels at ${phase}`);
    }
  }
  const pointerPhases = transcript ? ['during', 'after'] : ['idle'];
  if (linkMode !== 'regex') {
    if (pointers.length !== pointerPhases.length || pointers.some((pointer, index) =>
        pointer.phase !== pointerPhases[index])) throw new Error('missing pointer observations');
    for (const { sent, seen } of pointers) {
      const off = frames.findLast((frame) => frame.phase === 'pointer-off' && frame.time < sent);
      if (!off || inspect(off).footer.underline >= cw * 2 || inspect(off).footer.bright === 0) {
        throw new Error('OSC 8 hover did not start from visible text without an underline');
      }
      if (!frames.some((frame) => frame.phase === 'pointer-on' &&
          frame.time >= sent && frame.time <= seen && inspect(frame).footer.underline >= cw * 2)) {
        throw new Error('OSC 8 pointer response lacks matching video pixels');
      }
    }
  } else {
    // A regex annotation is always underlined. Check the final image of each
    // pointer move, without claiming a latency from an unchanged appearance.
    for (const phase of ['pointer-regex-off', 'pointer-regex-on']) {
      const ends = frames.filter((frame, index) => frame.phase === phase &&
        frames[index + 1]?.phase !== phase);
      if (ends.length !== pointerPhases.length ||
          ends.some((frame, index) => pointerPhases[index] !== 'during' &&
            inspect(frame).footer.underline < cw * 2)) {
        throw new Error('regex annotation disappeared during pointer movement');
      }
    }
    const recorded = regexPointers.find((entry) => entry.phase === 'during');
    if (transcript) {
      const observed = ['pointer-regex-off', 'pointer-regex-on'].every((phase) =>
        inspect(frames.find((frame) => frame.phase === phase)).footer.underline >= cw * 2);
      if (!recorded || recorded.underline_preserved !== observed) {
        throw new Error('regex pointer observation disagrees with video');
      }
    }
  }
  const entries = ['ffconcat version 1.0'];
  for (let index = 0; index < frames.length; index++) {
    entries.push(`file 'frame-${String(index).padStart(5, '0')}.ppm'`);
    entries.push('option framerate 1000');
    if (index + 1 < frames.length) {
      entries.push(`duration ${((frames[index + 1].time - frames[index].time) / 1e9).toFixed(9)}`);
    }
  }
  writeFileSync(join(dirname(framesPath), 'video.ffconcat'), `${entries.join('\n')}\n`);
  if (encodedTimesPath) {
    const encoded = readFileSync(encodedTimesPath, 'utf8').trim().split('\n').map(Number);
    if (encoded.length !== frames.length) throw new Error('encoded frame count changed');
    for (let index = 0; index < frames.length; index++) {
      const expected = (frames[index].time - frames[0].time) / 1e9;
      if (Math.abs(encoded[index] - expected) > 0.003) {
        throw new Error(`encoded frame ${index} lost its timestamp`);
      }
    }
  }
  console.log(`video_frames=${frames.length} duration_ms=${
    ((frames.at(-1).time - frames[0].time) / 1e6).toFixed(3)}`);
}
