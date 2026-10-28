/* Record native GTK4 zoom while a mouse selection remains visible. */
import assert from 'node:assert/strict';
import { execFileSync, spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { setTimeout } from 'node:timers/promises';

const [sample, helper, output, renderer] = process.argv.slice(2);
assert(sample && helper && output);
mkdirSync(output, { recursive: true });
const command = (name, args, options = {}) =>
  execFileSync(name, args, { encoding: 'utf8', timeout: 30000, ...options }).trim();
const pause = () => setTimeout(25);
let child, recorder;
let diagnostics = '';
try {
  child = spawn(resolve(sample), [
    ...(renderer ? ['--renderer=' + renderer] : []),
    resolve(helper), '--sample-scenario',
  ], {
    env: { ...process.env, BOXRIG_TEST_SAMPLE_SELECTION_STYLE_VIDEO: '1', NO_AT_BRIDGE: '1' },
    stdio: ['ignore', 'ignore', 'pipe'],
  });
  child.stderr.on('data', (data) => { diagnostics += data; });
  let id;
  const deadline = Date.now() + 30000;
  while (!id) {
    try {
      id = command('xdotool', ['search', '--onlyvisible', '--name', '^boxrig-selection-ready-']).split('\n')[0];
    } catch {
      assert(child.exitCode === null, diagnostics);
      assert(Date.now() < deadline, 'GTK4 sample did not open');
      await pause();
    }
  }
  command('xdotool', ['windowfocus', '--sync', id]);
  const title = command('xdotool', ['getwindowname', id]);
  const titleMatch = /^boxrig-selection-ready-(\d+)-(\d+)$/.exec(title);
  assert(titleMatch, title);
  const initialHeight = Number(titleMatch[1]);
  const initialWidth = Number(titleMatch[2]);
  const geometry = Object.fromEntries(command('xdotool', [
    'getwindowgeometry', '--shell', id,
  ]).split('\n').map((line) => line.split('=')));
  const originX = Number(geometry.X), originY = Number(geometry.Y);
  const width = Math.min(1000, 1280 - originX);
  const height = Math.min(700, 1024 - originY);
  assert(width >= 700 && height >= 450, 'capture surface covers zoomed window');
  const frameBytes = width * height * 3;
  const screenshot = () => execFileSync('ffmpeg', [
    '-nostdin', '-loglevel', 'error', '-f', 'x11grab', '-draw_mouse', '0',
    '-video_size', `${width}x${height}`, '-i', `${process.env.DISPLAY}+${originX},${originY}`,
    '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1',
  ], { maxBuffer: frameBytes * 2, timeout: 30000 });
  const pixel = (frame, x, y, color) => {
    const at = (y * width + x) * 3;
    return color.every((value, channel) => Math.abs(frame[at + channel] - value) <= 2);
  };
  const inspect = (frame) => {
    let left = width, top = height, right = -1, bottom = -1, blue = 0, background = 0;
    for (let y = 0; y < Math.min(height, 200); y++) {
      for (let x = 0; x < Math.min(width, 400); x++) {
        if (pixel(frame, x, y, [231, 43, 19])) {
          left = Math.min(left, x); top = Math.min(top, y);
          right = Math.max(right, x); bottom = Math.max(bottom, y);
        }
        if (pixel(frame, x, y, [46, 115, 242])) blue++;
        if (pixel(frame, x, y, [24, 42, 63])) background++;
      }
    }
    return { left, top, cellWidth: right - left + 1, cellHeight: bottom - top + 1, blue, background };
  };
  const until = async (name, predicate) => {
    const end = Date.now() + 15000;
    for (;;) {
      const frame = screenshot();
      const state = inspect(frame);
      if (predicate(state)) {
        console.log(name, state);
        return state;
      }
      assert(child.exitCode === null, diagnostics);
      if (Date.now() >= end) {
        writeFileSync(resolve(output, `${name}-failure.ppm`), Buffer.concat([
          Buffer.from(`P6\n${width} ${height}\n255\n`), frame,
        ]));
      }
      assert(Date.now() < end, `${name}: ${JSON.stringify(state)}\n${diagnostics}`);
      await pause();
    }
  };
  const initial = await until('initial', (s) =>
    s.cellWidth === initialWidth && s.cellHeight === initialHeight && s.background > 5000);
  const mouseY = originY + initial.top + initialHeight + Math.floor(initialHeight / 2);
  command('xdotool', [
    'mousemove', String(originX + initial.left + initialWidth * 2 + 1), String(mouseY),
    'mousedown', '1',
    'mousemove', String(originX + initial.left + initialWidth * 10 + 1), String(mouseY),
    'mouseup', '1',
    'mousemove', '1200', '900',
  ]);
  await until('selected', (s) => s.blue > 10);
  const movie = resolve(output, 'zoom-selection.mkv');
  recorder = spawn('ffmpeg', [
    '-y', '-loglevel', 'error', '-f', 'x11grab', '-framerate', '30',
    '-draw_mouse', '0', '-video_size', `${width}x${height}`,
    '-i', `${process.env.DISPLAY}+${originX},${originY}`,
    '-c:v', 'ffv1', movie,
  ], { stdio: ['pipe', 'ignore', 'pipe'] });
  let recordingError = '';
  recorder.stderr.on('data', (data) => { recordingError += data; });
  await setTimeout(150);
  const key = (name) => command('xdotool', ['key', '--clearmodifiers', name]);
  key('ctrl+plus');
  const zoom1 = await until('zoom-1', (s) => s.cellWidth > initialWidth && s.blue > 10 && s.background > 5000);
  await setTimeout(180);
  key('ctrl+plus');
  const zoom2 = await until('zoom-2', (s) => s.cellWidth > zoom1.cellWidth && s.blue > 10 && s.background > 5000);
  await setTimeout(180);
  key('ctrl+0');
  await until('reset', (s) => s.cellWidth === initialWidth && s.cellHeight === initialHeight && s.blue > 10);
  await setTimeout(180);
  const beforeInput = screenshot();
  key('e');
  const edgeDeadline = Date.now() + 15000;
  while (!/^boxrig-selection-edge-ready-\d+$/.test(command('xdotool', ['getwindowname', id]))) {
    assert(child.exitCode === null, diagnostics);
    assert(Date.now() < edgeDeadline, 'normal input was not processed');
    await pause();
  }
  const inputDeadline = Date.now() + 15000;
  for (;;) {
    const afterInput = screenshot();
    let changed = 0;
    for (let y = initial.top + 3 * initialHeight;
      y < initial.top + 4 * initialHeight; y++) {
      for (let x = initial.left; x < initial.left + 6 * initialWidth; x++) {
        const at = (y * width + x) * 3;
        if (Math.abs(afterInput[at] - beforeInput[at]) > 2 ||
            Math.abs(afterInput[at + 1] - beforeInput[at + 1]) > 2 ||
            Math.abs(afterInput[at + 2] - beforeInput[at + 2]) > 2) changed++;
      }
    }
    if (changed > 20) {
      console.log('input painted', { changed });
      break;
    }
    assert(Date.now() < inputDeadline, 'normal input did not change painted text');
    await pause();
  }
  await until('input', (s) => s.background > 5000 && s.blue > 10);
  await setTimeout(150);
  recorder.stdin.write('q');
  const [recorderStatus] = await once(recorder, 'close');
  assert.equal(recorderStatus, 0, recordingError);
  recorder = undefined;
  const recorded = execFileSync('ffmpeg', [
    '-nostdin', '-loglevel', 'error', '-i', movie,
    '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1',
  ], { maxBuffer: frameBytes * 150, timeout: 30000 });
  assert.equal(recorded.length % frameBytes, 0);
  assert(recorded.length / frameBytes >= 12, 'zoom video has too few frames');
  let phase = 0;
  const widths = [initialWidth, zoom1.cellWidth, zoom2.cellWidth, initialWidth];
  for (let at = 0; at < recorded.length; at += frameBytes) {
    const frame = inspect(recorded.subarray(at, at + frameBytes));
    assert(frame.background > 5000, `blank frame ${at / frameBytes}`);
    assert(frame.blue > 10, `selection disappeared in frame ${at / frameBytes}`);
    if (phase + 1 < widths.length && frame.cellWidth === widths[phase + 1]) phase++;
  }
  assert.equal(phase, widths.length - 1, 'video omitted a zoom or reset phase');
  console.log('video frames', { count: recorded.length / frameBytes, widths });
  key('q');
  while (child.exitCode === null) await pause();
  assert.equal(child.exitCode, 0, diagnostics);
  console.log('PASS GTK4 zoom selection and normal input video', movie);
} finally {
  if (recorder?.exitCode === null) recorder.kill('SIGTERM');
  if (child?.exitCode === null) child.kill('SIGTERM');
  if (diagnostics) console.log(diagnostics);
}
