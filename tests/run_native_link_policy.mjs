/* Observe native pointer shapes after link entry and stationary content updates. */
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
const [sample, helper, probePath, output, renderer] = process.argv.slice(2);
assert(sample && helper && probePath && output);
mkdirSync(output, { recursive: true });
const wayland = process.env.BOXRIG_TEST_LINK_WAYLAND === '1';
const command = (name, args) =>
  execFileSync(name, args, { encoding: 'utf8', timeout: 15000 }).trim();
if (process.env.BOXRIG_TEST_LINK_GTK === '1')
  process.env.BOXRIG_TEST_LINK_CURSOR_THEME = command('gsettings', [
    'get', 'org.gnome.desktop.interface', 'cursor-theme',
  ]).replace(/^'|'$/g, '');
const until = async (predicate, label) => {
  const deadline = Date.now() + 15000;
  while (!predicate()) {
    assert(child?.exitCode == null, 'sample exited: ' + diagnostics);
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
    runtime = mkdtempSync(resolve(tmpdir(), 'boxrig-link-wayland-'));
    process.env.XDG_RUNTIME_DIR = runtime;
    process.env.WAYLAND_DISPLAY = 'boxrig-link';
    weston = spawn(
      'weston',
      [
        '--debug',
        '--backend=x11',
        '--renderer=pixman',
        '--socket=boxrig-link',
        '--width=1600',
        '--height=1000',
        '--idle-time=0',
        '--no-config',
        '--log=' + runtime + '/weston.log',
      ],
      { env: process.env, stdio: 'ignore' }
    );
    await until(() => existsSync(runtime + '/boxrig-link'), 'Weston ready');
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
        BOXRIG_TEST_SAMPLE_LINK_POLICY: '1',
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
      ? /top-level window '(boxrig-link-[^']*)'/.exec(scene())?.[1]
      : command('xdotool', ['getwindowname', id]);
  if (wayland) {
    await until(() => title()?.startsWith('boxrig-link-0-R-'), 'sample ready');
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
      '^boxrig-link-0-R-',
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
    const m = /^boxrig-link-(\d+)-([a-zR])-(\d+)-(\d+)$/.exec(title() ?? '');
    assert(m);
    return {
      sequence: Number(m[1]),
      tag: m[2],
      height: Number(m[3]),
      width: Number(m[4]),
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
  const resize = async (width) => {
    if (wayland) {
      /* Ending the previous drag can apply another compositor configure.
       * Calculate this drag from the current size, not its cached allocation. */
      geometry = geometryOf(child.pid);
      assert(geometry);
      const x = geometry.x + geometry.width - 20;
      const y = geometry.y + Math.floor(geometry.height / 2);
      command('xdotool', ['mousemove', String(x), String(y)]);
      command('xdotool', ['keydown', 'Super_L']);
      command('xdotool', ['mousedown', '3']);
      command('xdotool', ['mousemove', String(x + width + 1 - geometry.width), String(y)]);
      /* GDK retains the preceding interactive allocation for a subsequent
       * compositor configure(0, 0). Give the drag two acknowledged motions
       * instead of teleporting directly from its initial size to release. */
      await until(() => geometryOf(child.pid)?.width === width + 1,
        'Wayland intermediate resize allocation');
      command('xdotool', ['mousemove', String(x + width - geometry.width), String(y)]);
      /* Keep the compositor resize grab until its allocation is presented;
       * releasing first can legitimately restore the last committed size. */
      await until(() => {
        const next = geometryOf(child.pid);
        return next && next.width === width;
      }, 'Wayland resize allocation presented');
      command('xdotool', ['mouseup', '3']);
      command('xdotool', ['keyup', 'Super_L']);
      await until(() => {
        const next = geometryOf(child.pid);
        if (!next || Math.abs(next.width - width) > state.width * 2) return false;
        geometry = next;
        return true;
      }, 'native Wayland resize');
    } else {
      command('xdotool', ['windowsize', '--sync', id, String(width), String(geometry.height)]);
      const g = Object.fromEntries(command('xdotool', ['getwindowgeometry', '--shell', id]).split('\n').map(l => l.split('=')));
      geometry = { x: Number(g.X), y: Number(g.Y), width: Number(g.WIDTH), height: Number(g.HEIGHT) };
    }
    move(2, 5);
    await send('z');
  };
  if (wayland) {
    move(5, 20);
    command('xdotool', ['click', '1']);
  }
  const readCursor = () => JSON.parse(command(resolve(probePath), wayland ? ['--pixels'] : []));
  const expectCursor = async (name, hand) => {
    let cursor;
    await until(() => {
      cursor = readCursor();
      return (
        hand ? ['hand2', 'hand', 'pointer'] : ['xterm', 'text', 'ibeam']
      ).includes(cursor.name);
    }, name + ' native cursor');
    results.push({ name, hand, cursor });
  };
  move(0, 2);
  await send('z');
  await expectCursor('explicit-osc8', true);
  move(1, 5);
  await send('z');
  await expectCursor('inferred-url', process.env.BOXRIG_AUTO_URLS !== '0');
  move(2, 5);
  await send('z');
  await expectCursor('rejected-osc8-blocks-visible-url', false);
  const initialWidth = geometry.width;
  await resize(initialWidth - state.width * 40);
  await expectCursor('reflow-url', process.env.BOXRIG_AUTO_URLS !== '0');
  await resize(initialWidth);
  await expectCursor('reflow-rejected', false);
  move(3, 5);
  await send('z');
  await expectCursor('plain-text', false);
  move(0, 2);
  await send('z');
  await expectCursor('return-to-link', true);
  await send('x');
  await expectCursor('stationary-replaced-text', false);
  await send('o');
  await expectCursor('stationary-new-link', true);
  const focusPeer = spawn(resolve(probePath), ['--focus-peer'], {
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  readers.push(focusPeer);
  let focusId = '';
  focusPeer.stdout.on('data', (data) => {
    focusId += data;
  });
  await until(() => focusId.includes('\n'), 'focus peer mapped');
  command('xdotool', ['windowfocus', '--sync', focusId.trim()]);
  await expectCursor('unfocused', false);
  command('xdotool', ['windowfocus', '--sync', id]);
  await expectCursor('refocused', true);
  command('xdotool', ['mousemove', '1800', '1100']);
  await until(
    () => !['hand2', 'hand', 'pointer'].includes(readCursor().name),
    'pointer left sample'
  );
  move(0, 2);
  await send('z');
  await expectCursor('reentered', true);
  await send('s');
  await expectCursor(
    'stationary-scroll-url',
    process.env.BOXRIG_AUTO_URLS !== '0'
  );
  await send('s');
  await expectCursor('stationary-scroll-rejected', false);
  command('xdotool', ['key', '--clearmodifiers', 'q']);
  await until(() => child.exitCode !== null, 'sample exited');
  assert.equal(child.exitCode, 0, diagnostics);
  console.log(JSON.stringify({ results }));
} finally {
  for (const reader of readers)
    if (reader.exitCode === null) reader.kill('SIGTERM');
  if (child) child.kill('SIGTERM');
  if (weston) weston.kill('SIGTERM');
  writeFileSync(
    resolve(output, 'results.json'),
    JSON.stringify({ results, diagnostics, sampleExitCode: child?.exitCode }, null, 2) + '\n'
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
