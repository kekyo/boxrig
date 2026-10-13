/* Read back native clipboard contents after common right-button gestures. */
import assert from 'node:assert/strict';
import { spawn, execFileSync } from 'node:child_process';
import {
  mkdirSync,
  mkdtempSync,
  existsSync,
  readFileSync,
  writeFileSync,
  rmSync,
} from 'node:fs';
import { resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { setTimeout } from 'node:timers/promises';
const [sample, helper, peerPath, output, renderer] = process.argv.slice(2);
assert(sample && helper && peerPath && output);
mkdirSync(output, { recursive: true });
const wayland = process.env.BOXRIG_TEST_COPY_WAYLAND === '1';
const command = (name, args) =>
  execFileSync(name, args, { encoding: 'utf8', timeout: 15000 }).trim();
const until = async (predicate, label) => {
  const deadline = Date.now() + 15000;
  while (!predicate()) {
    assert(Date.now() < deadline, label);
    await setTimeout(10);
  }
};
let runtime,
  weston,
  child,
  id,
  origin,
  state,
  geometry,
  diagnostics = '';
const readers = [],
  results = [];
const scene = () => command('weston-debug', ['scene-graph']);
const geometryOf = (pid) => {
  const part = scene()
    .split(/(?=View \d+ \(role )/)
    .find(
      (p) => p.includes('PID ' + pid + ',') && p.includes('role xdg_toplevel')
    );
  const p = /position: \((\d+), (\d+)\) -> \((\d+), (\d+)\)/.exec(part ?? '');
  if (!p) return undefined;
  return {
    x: Number(p[1]),
    y: Number(p[2]),
    width: Number(p[3]) - Number(p[1]),
    height: Number(p[4]) - Number(p[2]),
  };
};
try {
  if (wayland) {
    runtime = mkdtempSync(resolve(tmpdir(), 'boxrig-copy-wayland-'));
    process.env.XDG_RUNTIME_DIR = runtime;
    process.env.WAYLAND_DISPLAY = 'boxrig-copy';
    weston = spawn(
      'weston',
      [
        '--debug',
        '--backend=x11',
        '--renderer=pixman',
        '--socket=boxrig-copy',
        '--width=1600',
        '--height=1000',
        '--idle-time=0',
        '--no-config',
        '--log=' + runtime + '/weston.log',
      ],
      { env: process.env, stdio: 'ignore' }
    );
    await until(() => existsSync(runtime + '/boxrig-copy'), 'Weston ready');
  }
  child = spawn(
    resolve(sample),
    [
      ...(renderer ? ['--renderer=' + renderer] : []),
      resolve(helper),
      '--sample-scenario',
    ],
    {
      env: {
        ...process.env,
        BOXRIG_TEST_SAMPLE_COPY_POLICY: '1',
        NO_AT_BRIDGE: '1',
      },
      stdio: ['ignore', 'ignore', 'pipe'],
    }
  );
  child.stderr.on('data', (data) => {
    diagnostics += data;
  });
  const title = () =>
    wayland
      ? /top-level window '(boxrig-copy-[^']*)'/.exec(scene())?.[1]
      : command('xdotool', ['getwindowname', id]);
  if (wayland) {
    await until(() => title()?.startsWith('boxrig-copy-0-R-'), 'sample ready');
    id = /window id (\d+)/.exec(
      readFileSync(runtime + '/weston.log', 'utf8')
    )?.[1];
    assert(id);
    geometry = geometryOf(child.pid);
  } else {
    id = command('xdotool', [
      'search',
      '--sync',
      '--onlyvisible',
      '--name',
      '^boxrig-copy-0-R-',
    ]).split('\n')[0];
    const g = Object.fromEntries(
      command('xdotool', ['getwindowgeometry', '--shell', id])
        .split('\n')
        .map((l) => l.split('='))
    );
    geometry = {
      x: Number(g.X),
      y: Number(g.Y),
      width: Number(g.WIDTH),
      height: Number(g.HEIGHT),
    };
  }
  assert(geometry);
  const readState = () => {
    const m = /^boxrig-copy-(\d+)-([a-zMR])-(\d+)-(\d+)-(\d+)-(\d+)$/.exec(
      title() ?? ''
    );
    assert(m);
    return {
      sequence: Number(m[1]),
      tag: m[2],
      height: Number(m[3]),
      width: Number(m[4]),
      reports: Number(m[5]),
      buttonReports: Number(m[6]),
    };
  };
  state = readState();
  command('xdotool', ['windowfocus', '--sync', id]);
  const send = async (key) => {
    const sequence = readState().sequence;
    command('xdotool', ['key', '--clearmodifiers', key]);
    await until(
      () => readState().sequence > sequence && readState().tag === key,
      'PTY acknowledgement ' + key
    );
    state = readState();
  };
  const capture = () => {
    const w = Math.min(geometry.width, 600),
      h = Math.min(geometry.height, 240);
    const bytes = execFileSync(
      'ffmpeg',
      [
        '-v',
        'error',
        '-f',
        'x11grab',
        '-draw_mouse',
        '0',
        '-video_size',
        `${w}x${h}`,
        '-i',
        `${process.env.DISPLAY}+${geometry.x},${geometry.y}`,
        '-frames:v',
        '1',
        '-f',
        'rawvideo',
        '-pix_fmt',
        'rgb24',
        'pipe:1',
      ],
      { maxBuffer: w * h * 3 + 10000, timeout: 15000 }
    );
    return { bytes, w, h };
  };
  const background = (bytes, at) =>
    [24, 42, 63].every((c, i) => bytes[at + i] === c);
  await until(() => {
    const { bytes, w, h } = capture();
    const x = Math.floor(w / 2);
    let y = 0;
    while (y < h && !background(bytes, (y * w + x) * 3)) y++;
    if (y === h) return false;
    let left = 0;
    while (left < w && !background(bytes, (y * w + left) * 3)) left++;
    origin = { x: left, y };
    return left < w;
  }, 'initial background presentation');
  const point = (row, column, half = 0.25) => ({
    x: geometry.x + origin.x + Math.floor(state.width * (column + half)),
    y: geometry.y + origin.y + Math.floor(state.height * (row + 0.5)),
  });
  const move = (row, column, half = 0.25) => {
    const p = point(row, column, half);
    command('xdotool', ['mousemove', String(p.x), String(p.y)]);
  };
  if (wayland) {
    move(5, 20);
    command('xdotool', ['click', '1']);
  }
  const select = async (row1, col1, row2, col2, mods = []) => {
    for (const m of mods) command('xdotool', ['keydown', m]);
    try {
      move(row1, col1);
      command('xdotool', ['mousedown', '1']);
      move(row2, col2, 0.75);
      command('xdotool', ['mouseup', '1']);
    } finally {
      for (const m of [...mods].reverse()) command('xdotool', ['keyup', m]);
    }
    await send('z');
  };
  const right = async () => {
    move(4, 10);
    command('xdotool', ['click', '3']);
    await send('z');
  };
  const readClipboard = async (target = 'clipboard') => {
    if (!wayland)
      return execFileSync(resolve(peerPath), ['get', target], {
        encoding: 'utf8',
        env: { ...process.env, GDK_BACKEND: 'x11' },
        timeout: 15000,
      });
    const peer = spawn(resolve(peerPath), ['get-active', target], {
      env: { ...process.env, GDK_BACKEND: 'wayland' },
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    readers.push(peer);
    let bytes = '',
      error = '';
    peer.stdout.on('data', (data) => {
      bytes += data;
    });
    peer.stderr.on('data', (data) => {
      error += data;
    });
    // The reader requests text when its focused client receives the offer.
    await until(() => peer.exitCode !== null, 'clipboard read complete');
    assert.equal(peer.exitCode, 0, error);
    // Title clicks can become a maximize gesture in GTK or libdecor. Wait for
    // the reader's surface to retire, then use Weston's documented mod+Tab
    // binding to activate the sole remaining application without clicking it.
    // weston-bindings(7); weston.ini(5) defaults binding-modifier to super.
    await until(() => {
      const views = scene()
        .split(/(?=View \d+ \(role )/)
        .filter((part) => part.includes('role xdg_toplevel'));
      return views.length === 1 && views[0].includes('PID ' + child.pid + ',');
    }, 'clipboard reader surface retired');
    command('xdotool', ['key', '--clearmodifiers', 'Super_L+Tab']);
    await send('z');
    assert.deepEqual(
      geometryOf(child.pid),
      geometry,
      'Refocusing preserves geometry'
    );
    return bytes;
  };
  await select(2, 0, 2, 10);
  command('xdotool', ['key', '--clearmodifiers', 'ctrl+shift+c']);
  await send('z');
  assert.equal(await readClipboard(), 'copy-target');
  results.push({ name: 'keyboard-copy-baseline', text: 'copy-target' });
  await select(4, 1, 4, 1);
  await right();
  assert.equal(await readClipboard(), 'copy-target');
  results.push({ name: 'empty-keeps-clipboard', text: 'copy-target' });
  await select(0, 1, 0, 3);
  const expected = '\u754ce\u0301';
  if (!wayland) assert.equal(await readClipboard('primary'), expected);
  await right();
  assert.equal(await readClipboard(), expected);
  results.push({ name: 'linear-right-copy', text: expected });
  command('xdotool', ['key', '--clearmodifiers', 'ctrl+shift+c']);
  await send('z');
  assert.equal(await readClipboard(), expected);
  results.push({ name: 'selection-retained', text: expected });
  await send('r');
  await right();
  assert.equal(await readClipboard(), expected);
  assert.equal(readState().reports, 0);
  results.push({ name: 'reporting-copy-exclusion', reports: 0 });
  await select(0, 1, 1, 3, ['Control_L']);
  const rectangle = '\u754ce\u0301\n\u754co\u0308';
  await right();
  assert.equal(await readClipboard(), rectangle);
  assert.equal(readState().reports, 0);
  results.push({ name: 'rectangle-right-copy', text: rectangle });
  await send('x');
  await right();
  assert.equal(await readClipboard(), rectangle);
  await until(
    () => diagnostics.includes('could not copy selection'),
    'copy failure diagnostic'
  );
  assert.equal(readState().reports, 0);
  results.push({ name: 'stale-keeps-clipboard-and-input', reports: 0 });
  await select(4, 1, 4, 1, ['Shift_L']);
  await right();
  assert.equal(await readClipboard(), rectangle);
  assert.equal(readState().buttonReports, 2);
  results.push({ name: 'empty-reporting-fallback', reports: 2 });
  await send('n');
  command('xdotool', ['key', '--clearmodifiers', 'q']);
  console.log(JSON.stringify({ results }));
} finally {
  for (const reader of readers)
    if (reader.exitCode === null) reader.kill('SIGTERM');
  if (child) child.kill('SIGTERM');
  if (weston) weston.kill('SIGTERM');
  writeFileSync(
    resolve(output, 'results.json'),
    JSON.stringify({ results, diagnostics }, null, 2) + '\n'
  );
  if (runtime) {
    if (existsSync(runtime + '/weston.log'))
      writeFileSync(
        resolve(output, 'weston.log'),
        readFileSync(runtime + '/weston.log')
      );
    rmSync(runtime, { recursive: true, force: true });
  }
}
