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
const transcript = readFileSync(peerPath, 'utf8').split('\n')[0].includes('load=transcript');
if (peer.length < 1 || probe.length !== peer.length) throw new Error('missing key responses');
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
}
if (framesPath) {
  const frames = readFileSync(framesPath, 'utf8').trim().split('\n').slice(1)
    .map((line) => {
      const [index, phase, time] = line.split('\t');
      return { index: Number(index), phase, time: Number(time) };
    });
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
