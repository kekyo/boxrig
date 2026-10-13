/* Exercise the sample's font policy through a real child and native input. */
import assert from 'node:assert/strict';
import { spawn, execFileSync } from 'node:child_process';
import { once } from 'node:events';
import { mkdirSync, mkdtempSync, existsSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve } from 'node:path';
import { setTimeout } from 'node:timers/promises';

const [sample, output, renderer, backend] = process.argv.slice(2);
assert(sample && output && renderer && backend, 'sample, output, renderer, x11|wayland required');
const initiallyEnabled = process.env.BOXRIG_TEST_COLOR_GLYPHS !== '0';
mkdirSync(output, { recursive: true });
const command = (name, args) => execFileSync(name, args,
  { encoding: 'utf8', timeout: 30000 }).trim();
const waitFor = async (predicate, message) => {
  const deadline = Date.now() + 30000;
  for (;;) {
    const value = predicate();
    if (value) return value;
    assert(Date.now() < deadline, message);
    await setTimeout(20);
  }
};
const terminate = async (child) => {
  if (!child || child.exitCode !== null || child.signalCode !== null) return;
  const stopped = once(child, 'exit');
  child.kill('SIGTERM');
  await stopped;
};
let weston, child, recorder, runtime;
let diagnostics = '';
try {
  if (backend === 'wayland') {
    runtime = mkdtempSync(resolve(tmpdir(), 'boxrig-font-'));
    process.env.XDG_RUNTIME_DIR = runtime;
    process.env.WAYLAND_DISPLAY = 'boxrig-font';
    weston = spawn('weston', ['--debug', '--backend=x11', '--renderer=pixman',
      '--socket=boxrig-font', '--width=1000', '--height=650', '--idle-time=0',
      '--no-config', '--log=' + runtime + '/weston.log'], { stdio: 'ignore' });
    await waitFor(() => existsSync(runtime + '/boxrig-font'), 'Weston socket');
  }
  const title = 'boxrig-color-glyph-test';
  const text = '\x1b[?25l\x1b[2J\x1b[H\x1b[48;2;231;43;19m \x1b[0m' +
    '\x1b[2;1H\x1b[38;2;255;255;255;48;2;0;0;0m🧑🧑🧑🧑' +
    '\x1b[3;1HPlain text\x1b]2;' + title + '\x07';
  child = spawn(resolve(sample), ['--renderer=' + renderer, '/bin/sh', '-c',
    'printf %s "$1"; read -r reply', 'boxrig-font-child', text], {
    env: { ...process.env, GDK_BACKEND: backend, BOXRIG_COLOR_GLYPHS: initiallyEnabled ? '1' : '0',
      BOXRIG_SCROLLBAR: 'never', GTK_IM_MODULE: 'gtk-im-context-simple', NO_AT_BRIDGE: '1' },
    stdio: ['ignore', 'ignore', 'pipe'],
  });
  child.stderr.on('data', data => { diagnostics += data; });
  let id, x, y, width, height;
  if (backend === 'wayland') {
    const section = await waitFor(() => {
      assert(child.exitCode === null, diagnostics);
      return command('weston-debug', ['scene-graph']).split(/(?=View \d+ \(role )/)
        .find(part => part.includes("top-level window '" + title + "'") &&
          part.includes('role xdg_toplevel') && part.includes('position:'));
    }, 'Wayland sample ready');
    const position = /position: \((\d+), (\d+)\) -> \((\d+), (\d+)\)/.exec(section);
    assert(position, section);
    [x, y] = position.slice(1, 3).map(Number);
    width = Math.min(500, Number(position[3]) - x);
    height = Math.min(200, Number(position[4]) - y);
    id = /window id (\d+)/.exec(readFileSync(runtime + '/weston.log', 'utf8'))?.[1];
    assert(id);
    command('xdotool', ['windowfocus', '--sync', id]);
    command('xdotool', ['mousemove', '--window', id, String(x + 200), String(y + 100), 'click', '1']);
    // Translate the Weston window origin into root coordinates for recording.
    const geometry = Object.fromEntries(command('xdotool', ['getwindowgeometry', '--shell', id])
      .split('\n').map(line => line.split('=')));
    x += Number(geometry.X); y += Number(geometry.Y);
  } else {
    id = await waitFor(() => {
      assert(child.exitCode === null, diagnostics);
      try {
        return command('xdotool', ['search', '--onlyvisible', '--name', '^' + title + '$']).split('\n')[0];
      } catch (error) {
        if (error.status !== 1) throw error;
        return false;
      }
    }, 'X11 sample ready');
    const geometry = Object.fromEntries(command('xdotool', ['getwindowgeometry', '--shell', id])
      .split('\n').map(line => line.split('=')));
    x = Number(geometry.X); y = Number(geometry.Y);
    width = Math.min(500, Number(geometry.WIDTH)); height = Math.min(200, Number(geometry.HEIGHT));
    command('xdotool', ['windowfocus', '--sync', id]);
  }
  command('xdotool', ['mousemove', '1200', '1000']);
  const captureArgs = ['-f', 'x11grab', '-draw_mouse', '0', '-video_size', width + 'x' + height,
    '-i', process.env.DISPLAY + '+' + x + ',' + y];
  const frameBytes = width * height * 3;
  const capture = () => execFileSync('ffmpeg', ['-nostdin', '-loglevel', 'error', ...captureArgs,
    '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'],
    { maxBuffer: frameBytes * 2, timeout: 30000 });
  let anchor;
  await waitFor(() => {
    const frame = capture();
    const xs = [], ys = [];
    for (let yy = 0; yy < height; yy++) for (let xx = 0; xx < width; xx++) {
      const at = (yy * width + xx) * 3;
      if (Math.abs(frame[at] - 231) <= 1 && Math.abs(frame[at + 1] - 43) <= 1 &&
          Math.abs(frame[at + 2] - 19) <= 1) { xs.push(xx); ys.push(yy); }
    }
    if (xs.length < 30) return false;
    anchor = { x: Math.min(...xs), y: Math.min(...ys),
      width: Math.max(...xs) - Math.min(...xs) + 1, height: Math.max(...ys) - Math.min(...ys) + 1 };
    return xs.length === anchor.width * anchor.height;
  }, 'Opaque terminal anchor');
  const count = frame => {
    let ink = 0, colored = 0, plain = 0;
    for (let row = 1; row <= 2; row++)
      for (let yy = anchor.y + row * anchor.height; yy < anchor.y + (row + 1) * anchor.height; yy++)
        for (let xx = anchor.x; xx < anchor.x + 8 * anchor.width; xx++) {
          const at = (yy * width + xx) * 3;
          const [r, g, b] = frame.subarray(at, at + 3);
          if (row === 1) {
            ink += Math.max(r, g, b) > 24;
            colored += Math.max(r, g, b) - Math.min(r, g, b) > 4;
          } else plain += Math.max(r, g, b) > 24;
        }
    return { ink, colored, plain };
  };
  const phases = [];
  const video = resolve(output, 'color-glyphs.mkv');
  recorder = spawn('ffmpeg', ['-y', '-loglevel', 'error', '-framerate', '30',
    ...captureArgs, '-c:v', 'ffv1', video], { stdio: ['pipe', 'ignore', 'pipe'] });
  let recordingError = '';
  recorder.stderr.on('data', data => { recordingError += data; });
  for (const enabled of [initiallyEnabled, !initiallyEnabled, initiallyEnabled]) {
    if (phases.length) command('xdotool', ['key', '--clearmodifiers', 'ctrl+shift+F11']);
    const result = await waitFor(() => {
      const result = count(capture());
      return result.ink > 20 && result.plain > 10 &&
        (enabled ? result.colored > 20 : result.colored === 0) ? result : false;
    }, 'Glyph policy ' + enabled + ': ' + diagnostics);
    phases.push({ enabled, ...result });
  }
  const recorded = once(recorder, 'exit');
  recorder.stdin.end('q\n');
  const [code] = await recorded;
  assert.equal(code, 0, recordingError);
  const frames = execFileSync('ffmpeg', ['-nostdin', '-loglevel', 'error', '-i', video,
    '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'], { maxBuffer: frameBytes * 1800, timeout: 30000 });
  assert.equal(frames.length % frameBytes, 0);
  const observed = [];
  for (let offset = 0; offset < frames.length; offset += frameBytes) {
    const result = count(frames.subarray(offset, offset + frameBytes));
    assert(result.ink > 20 && result.plain > 10, 'Text disappeared during the font switch');
    const state = result.colored > 20 ? 'color' : result.colored === 0 ? 'mono' : 'transition';
    if (state !== 'transition' && observed.at(-1) !== state) observed.push(state);
  }
  assert.deepEqual(observed, initiallyEnabled ? ['color', 'mono', 'color'] : ['mono', 'color', 'mono']);
  writeFileSync(resolve(output, 'validation.json'), JSON.stringify({ sample, renderer, backend,
    anchor, phases, frames: frames.length / frameBytes, observed }, null, 2) + '\n');
  console.log('PASS native color glyph policy and video:', renderer, backend, phases);
} finally {
  await terminate(recorder);
  await terminate(child);
  await terminate(weston);
  writeFileSync(resolve(output, 'sample.log'), diagnostics);
  if (runtime) rmSync(runtime, { recursive: true, force: true });
}
