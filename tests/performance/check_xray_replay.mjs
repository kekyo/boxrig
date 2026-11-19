// Check core RGBA and placements against the capture and independently read the visible image.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const [input, directory, phaseFile] = process.argv.slice(2);
assert(input && directory, 'usage: node check_xray_replay.mjs CAPTURE-DIRECTORY READBACK-DIRECTORY [PHASE-TSV]');
const read = name => readFileSync(join(directory, name), 'utf8');
const table = name => read(name).trimEnd().split('\n').filter(line => !line.startsWith('#'))
  .slice(1).map(line => line.split('\t').map(Number));
const peer = table('peer.tsv'), probe = table('probe.tsv'), frames = table('frames.tsv');
assert.equal(peer.length, 24);
assert.equal(probe.length, 24);
const [gx, gy, cw, ch, gridRows, gridColumns] =
  /# grid=(\d+),(\d+),(\d+),(\d+) rows=(\d+) columns=(\d+)/.exec(read('probe.tsv')).slice(1).map(Number);
const records = readFileSync(phaseFile ?? join(directory, 'image-phases.tsv'), 'utf8')
  .trimEnd().split('\n').map(line => line.split('\t'));
const publications = records.filter(row => row[0] === 'resource');
if (process.env.BOXRIG_VERIFY_ALL_IMAGES) assert(publications.length > 0, 'missing image publication trace');
if (publications.length) {
  const placements = records.filter(row => row[0] === 'placement');
  const intro = Buffer.from('\x1b_G'), stop = Buffer.from('\x1b\\');
  let resource, resourceIndex = 0, placementIndex = 0;
  const files = ['warmup.bin', ...Array.from({ length: 23 }, (_, i) => 'frame-' + i + '.bin')];
  for (const file of files) {
    const bytes = readFileSync(join(input, file));
    for (let at = bytes.indexOf(intro); at >= 0;) {
      const end = bytes.indexOf(stop, at + intro.length);
      assert(end >= 0);
      const command = bytes.subarray(at + intro.length, end);
      const semicolon = command.indexOf(59);
      const header = command.toString('ascii', 0, semicolon < 0 ? command.length : semicolon);
      const fields = Object.fromEntries(header.split(',').map(field => field.split('=')));
      if (fields.a === 't') {
        assert(!resource && fields.f === '32');
        resource = { id: fields.i, width: fields.s, height: fields.v, digest: createHash('sha256') };
      }
      if (resource && semicolon >= 0) {
        const payload = command.toString('ascii', semicolon + 1);
        assert.equal(payload.length % 4, 0);
        resource.digest.update(Buffer.from(payload, 'base64'));
        if (fields.m !== '1') {
          assert.deepEqual(publications[resourceIndex++].slice(2),
            [resource.id, resource.width, resource.height, resource.digest.digest('hex')]);
          resource = undefined;
        }
      }
      if (fields.a === 'p') {
        const cursor = /\x1b\[(\d+);(\d+)H$/.exec(bytes.toString('latin1', Math.max(0, at - 20), at));
        assert(cursor);
        const published = publications[resourceIndex - 1];
        assert.equal(fields.i, published[2]);
        assert.deepEqual(placements[placementIndex++].slice(2),
          [fields.i, fields.p, String(Number(cursor[1]) - 1), String(Number(cursor[2]) - 1), published[3], published[4]]);
      }
      at = bytes.indexOf(intro, end + stop.length);
    }
  }
  const manifest = JSON.parse(readFileSync(join(input, 'manifest.json'), 'utf8'));
  assert.equal(resourceIndex, manifest.synchronizedUpdates);
  assert.equal(resourceIndex, publications.length);
  assert.equal(placementIndex, placements.length);
  assert.equal(resourceIndex, placementIndex);
}
const pct = (list, p) => [...list].sort((a, b) => a - b)[Math.ceil(list.length * p) - 1];
const samples = [];
let previousSnapshot, lastOpaque = [];
const submissions = records.filter(row => row[0] === 'framebuffer');
if (process.env.BOXRIG_VERIFY_GL_PRESENTATION) assert(submissions.length > 0, 'missing GPU readback');
const verifiedFrames = [];
for (let seq = 0; seq < 24; seq++) {
  const file = 'frame-' + seq + '.bin';
  const source = readFileSync(join(input, file), 'latin1');
  const [i, emitted] = peer[seq], [j, , seen] = probe[seq];
  assert(i === seq && j === seq && seen >= emitted);
  const hit = frames.find(frame => frame[1] === seen && frame[2] === seq);
  assert(hit);
  const framePath = join(directory, 'frame-' + String(hit[0]).padStart(5, '0') + '.ppm');
  verifiedFrames.push(framePath);
  const ppm = readFileSync(framePath);
  const header = /^P6\n(\d+) (\d+)\n255\n/.exec(ppm.toString('ascii', 0, 40));
  assert(header && ppm.length === header[0].length + 3 * Number(header[1]) * Number(header[2]));
  const pixel = (x, y) => ppm.readUIntBE(header[0].length + 3 * (y * Number(header[1]) + x), 3);
  const rows = records.filter(row => Number(row[1]) === seq);
  if (submissions.length) {
    const surfaces = rows.filter(row => row[0] === 'framebuffer');
    assert(surfaces.length > 0, 'missing GPU readback for ' + seq);
    for (const [, , width, height, retained, back] of surfaces) {
      assert.equal(retained, back, 'retained and EGL back buffer differ at ' + seq);
      const digest = createHash('sha256');
      for (let y = gy; y < gy + Number(height); y++) {
        const at = header[0].length + 3 * (y * Number(header[1]) + gx);
        digest.update(ppm.subarray(at, at + 3 * Number(width)));
      }
      assert.equal(back, digest.digest('hex'), 'GPU and confirmed X11 pixels differ at ' + seq);
    }
  }
  const state = rows.find(row => row[0] === 'state');
  const committed = Number(rows.find(row => row[0] === 'committed')[2]);
  assert(committed >= emitted && committed <= seen);
  const request = rows.find(row => row[0] === 'request' && Number(row[2]) >= committed);
  assert(request && Number(request[2]) <= seen, 'missing presentation request for update ' + seq);
  const submit = rows.findIndex(row => row[0] === 'submit');
  assert(submit >= 0, 'missing submitted update ' + seq);
  const snapshot = {};
  for (const row of rows.slice(submit + 1, submit + 9)) {
    assert.equal(row[0], 'phase');
    snapshot[row[2]] = row.slice(3).map(Number);
  }
  assert.equal(Object.keys(snapshot).length, 8);
  const phases = {};
  if (previousSnapshot) for (const [name, [calls, units, cpu]] of Object.entries(snapshot)) {
    const previous = previousSnapshot[name];
    phases[name] = { calls: calls - previous[0], units: units - previous[1], cpuMs: (cpu - previous[2]) / 1e6 };
  }
  previousSnapshot = snapshot;
  const sample = { seq, inputToPixelsMs: (seen - emitted) / 1e6, requestToPixelsMs: (seen - Number(request[2])) / 1e6,
    submissionToPixelsMs: (seen - Number(rows[submit][2])) / 1e6, phases };
  if (seq < 23) {
    assert.deepEqual(state.slice(2), ['1', '1']);
    const payloads = [...source.matchAll(/\x1b_G([^;\x1b]*);([^\x1b]*)\x1b\\/g)];
    const first = payloads[0][1].split(',').map(field => field.split('='));
    const fields = Object.fromEntries(first);
    assert.equal(fields.f, '32');
    const w = Number(fields.s), h = Number(fields.v);
    const payload = payloads.map(match => match[2]).join('');
    const rgba = Buffer.from(payload, 'base64');
    assert.equal(rgba.length, w * h * 4);
    const sha = createHash('sha256').update(rgba).digest('hex');
    assert.deepEqual(rows.find(row => row[0] === 'image').slice(2), [fields.i, String(w), String(h), String(rgba.length), sha]);
    const place = /\x1b\[(\d+);(\d+)H\x1b_Ga=p,i=(\d+),p=(\d+)/.exec(source);
    assert(place && place[3] === fields.i);
    const geometry = rows.find(row => row[0] === 'place').slice(2).map(Number);
    const [id, placement, row, column, dy, dx, sx, sy, sw, sh, dw, dh, cx, cy, clipW, clipH, z] = geometry;
    assert.deepEqual([id, placement, row, column, dy, dx, sx, sy, sw, sh, cx, cy, z],
      [Number(fields.i), Number(place[4]), Number(place[1]) - 1, Number(place[2]) - 1, 0, 0, 0, 0, w, h, 0, 0, 0]);
    assert(dw > 0 && dh > 0 && clipW <= dw && clipH <= dh);
    let checked = 0;
    lastOpaque = [];
    for (let y = 1; y < clipH - 1; y++) for (let x = 1; x < clipW - 1; x++) {
      const srcX = (x + 0.5) * w / dw - 0.5, srcY = (y + 0.5) * h / dh - 0.5;
      const at = 4 * (Math.floor(srcY) * w + Math.floor(srcX));
      const color = rgba.readUInt32BE(at);
      if ((color & 255) !== 255) continue;
      // Where resampling crosses a texel boundary, require the same opaque
      // color on each contributing texel. This is independent of GL rounding.
      if ((srcX % 1 && rgba.readUInt32BE(at + 4) !== color) ||
          (srcY % 1 && rgba.readUInt32BE(at + w * 4) !== color) ||
          (srcX % 1 && srcY % 1 && rgba.readUInt32BE(at + (w + 1) * 4) !== color)) continue;
      const px = gx + column * cw + dx + x, py = gy + row * ch + dy + y;
      assert.equal(pixel(px, py), color >>> 8, 'image ' + seq + ' at ' + x + ',' + y);
      checked++;
      if (seq === 22 && (color >>> 8) !== 0) lastOpaque.push([px, py, color >>> 8]);
    }
    assert(checked > 100, 'too few independently observable image pixels');
    if (seq > 0) assert.equal(phases.decode.units, payload.length);
    Object.assign(sample, { sha, geometry, checked });
  } else {
    assert.deepEqual(state.slice(2), ['0', '0']);
    assert(!rows.some(row => row[0] === 'image' || row[0] === 'place'));
    assert.equal(phases.decode.units, 0);
    sample.clearedPixels = lastOpaque.filter(([x, y, color]) => pixel(x, y) !== color).length;
    assert(sample.clearedPixels > 100);
  }
  samples.push(sample);
}
// Check the raw samples which changed between two X11 reads, too. A single
// transition can contain an old prefix and a new suffix (or the converse).
// Any unrelated pixel or second boundary is a verification failure.
let rawReads = 0, splitReads = 0;
if (existsSync(join(directory, 'unstable.tsv'))) {
  for (const [index, firstTime, secondTime] of table('unstable.tsv')) {
    assert(firstTime < secondTime);
    const seq = peer.findLastIndex(row => row[1] <= secondTime);
    assert(seq > 0 && secondTime < probe[seq][2]);
    const old = readFileSync(verifiedFrames[seq - 1]);
    const next = readFileSync(verifiedFrames[seq]);
    const header = /^P6\n(\d+) (\d+)\n255\n/.exec(next.toString('ascii', 0, 40));
    assert(header && old.subarray(0, header[0].length).equals(next.subarray(0, header[0].length)));
    for (const side of ['first', 'second']) {
      const sample = readFileSync(join(directory, 'unstable-' + String(index).padStart(5, '0') + '-' + side + '.ppm'));
      assert(sample.length === next.length && sample.subarray(0, header[0].length).equals(next.subarray(0, header[0].length)));
      let last, boundaries = 0;
      for (let y = gy; y < gy + gridRows * ch; y++) {
        const line = header[0].length + y * Number(header[1]) * 3;
        for (let x = gx; x < gx + gridColumns * cw; x++) {
          const at = line + x * 3, value = sample.readUIntBE(at, 3);
          const a = old.readUIntBE(at, 3), b = next.readUIntBE(at, 3);
          assert(value === a || value === b, 'unexpected raw X11 pixel for update ' + seq);
          if (a === b) continue;
          const part = value === b;
          if (last !== undefined && last !== part) boundaries++;
          last = part;
        }
      }
      assert(boundaries <= 1, 'multiple boundaries in raw X11 read for update ' + seq);
      rawReads++;
      if (boundaries) splitReads++;
    }
  }
}
const steady = samples.slice(1), images = samples.slice(1, 23);
const phases = {};
for (const name of Object.keys(images[0].phases)) {
  phases[name] = { cpuMsPerImage: images.reduce((sum, s) => sum + s.phases[name].cpuMs, 0) / images.length,
    calls: images.reduce((sum, s) => sum + s.phases[name].calls, 0),
    units: images.reduce((sum, s) => sum + s.phases[name].units, 0) };
}
const summary = { observed: samples.length, independentlyVerifiedResources: publications.length,
  verifiedNativeFramebuffers: submissions.length, rawReads, splitReads,
  steadyImages: images.length, phases,
  inputToPixelsP95Ms: pct(steady.map(s => s.inputToPixelsMs), 0.95),
  requestToPixelsP95Ms: pct(steady.map(s => s.requestToPixelsMs), 0.95), samples };
writeFileSync(join(directory, 'xray.json'), JSON.stringify(summary, null, 2) + '\n');
console.log('PASS xray replay', summary.observed, 'updates, request-to-pixels p95', summary.requestToPixelsP95Ms, 'ms');
