/* Observe exact external URI requests from native pointer gestures. */
import net from 'node:net';
import { symlinkSync } from 'node:fs';
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
const uriDirectory = mkdtempSync(resolve(tmpdir(), 'boxrig-uri-native-'));
symlinkSync(resolve(peerPath), resolve(uriDirectory, 'xdg-open'));
const uris = [];
let failNext = false;
const server = net.createServer(socket => {
  let data = Buffer.alloc(0);
  socket.on('data', chunk => {
    data = Buffer.concat([data, chunk]);
    const nul = data.indexOf(0);
    if (nul >= 0) {
      uris.push(data.subarray(0, nul).toString('utf8'));
      socket.end(failNext ? '4' : '0');
      failNext = false;
    }
  });
});
await new Promise(resolveListen => server.listen(resolve(uriDirectory, 'peer'), resolveListen));
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
        PATH: uriDirectory + ':' + process.env.PATH,
        BOXRIG_TEST_URI_SOCKET: resolve(uriDirectory, 'peer'),
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
    await until(() => {
      try {
        id = command('xdotool', ['search', '--onlyvisible', '--name', '^boxrig-link-0-R-']).split('\n')[0];
        return Boolean(id);
      } catch { return false; }
    }, 'sample mapped');
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
  const expected = [];
  const regexUri = 'https://example.test/regex/long/path/segment-one/segment-two/end';
  const explicitUri = 'https://example.test/osc?x=1&y=2';
  const positive = async (name, row, column, uri) => {
    move(row, column);
    await send('z');
    command('xdotool', ['click', '1']);
    expected.push(uri);
    await until(() => uris.length >= expected.length, name + ' external URI');
    assert.deepEqual(uris, expected);
    results.push({name, uris: [...uris]});
  };
  await positive('OSC 8 exact URI once', 0, 2, explicitUri);
  await positive('regex full URI once', 1, 5, regexUri);
  move(0, 1);
  command('xdotool', ['mousedown', '1']);
  move(0, 8);
  command('xdotool', ['mouseup', '1']);
  await positive('drag does not launch', 1, 5, regexUri);
  move(0, 2);
  command('xdotool', ['click', '--repeat', '2', '--delay', '90', '1']);
  await positive('double click does not launch', 1, 5, regexUri);
  move(0, 2);
  command('xdotool', ['click', '--repeat', '3', '--delay', '90', '1']);
  await positive('triple click does not launch', 1, 5, regexUri);
  move(2, 3);
  command('xdotool', ['click', '1']);
  await positive('rejected OSC 8 does not fall back', 0, 2, explicitUri);
  move(0, 4);
  command('xdotool', ['keydown', 'Shift_L']);
  command('xdotool', ['click', '1']);
  command('xdotool', ['keyup', 'Shift_L']);
  await positive('Shift selection does not launch', 1, 5, regexUri);
  move(0, 2);
  command('xdotool', ['click', '1']);
  command('xdotool', ['mousemove', '1800', '1100']);
  await send('z');
  await positive('leave cancels pending launch', 1, 5, regexUri);
  move(0, 2);
  command('xdotool', ['click', '1']);
  await send('x');
  await positive('text replacement cancels pending launch', 1, 5, regexUri);
  await send('o');
  failNext = true;
  await positive('launch failure is observed once', 0, 2, 'https://example.test/new');
  await until(() => diagnostics.includes('could not open URL:'), 'asynchronous failure notification');
  await send('z');
  await positive('input continues after launch failure', 1, 5, regexUri);
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
    JSON.stringify({ results, uris, diagnostics, sampleExitCode: child?.exitCode }, null, 2) + '\n'
  );
  await new Promise(resolveClose => server.close(resolveClose));
  rmSync(uriDirectory, {recursive: true, force: true});
  if (runtime) {
    if (existsSync(runtime + '/weston.log'))
      writeFileSync(
        resolve(output, 'weston.log'),
        readFileSync(runtime + '/weston.log')
      );
    rmSync(runtime, { recursive: true, force: true });
  }
}
