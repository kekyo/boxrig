/* Observe shared wheel policy through PTY replies and rendered history rows. */
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
import { tmpdir } from 'node:os';
import { resolve } from 'node:path';
import { setTimeout } from 'node:timers/promises';
const [sample, helper, outputArgument, renderer] = process.argv.slice(2);
const pixelOnly = process.env.BOXRIG_TEST_PIXEL_MOUSE === '1';
const output = outputArgument + (pixelOnly ? '-pixels' : '');
assert(sample && helper && output);
mkdirSync(output, { recursive: true });
const command = (name, args) =>
  execFileSync(name, args, { encoding: 'utf8', timeout: 30000 }).trim();
const wayland = process.env.BOXRIG_TEST_WHEEL_WAYLAND === '1';
const outputScale = wayland
  ? Number(process.env.BOXRIG_TEST_WHEEL_SCALE ?? '1')
  : 1;
assert(Number.isInteger(outputScale) && outputScale > 0);
let weston,
  runtime,
  child,
  id,
  geometry,
  origin,
  diagnostics = '',
  sequence = -1;
const results = [];
try {
  if (wayland) {
    runtime = mkdtempSync(resolve(tmpdir(), 'boxrig-wheel-wayland-'));
    process.env.XDG_RUNTIME_DIR = runtime;
    process.env.WAYLAND_DISPLAY = 'boxrig-wheel';
    // Keep compositor startup fading out of exact application pixel checks.
    // weston.ini(5) documents shell.startup-animation = none.
    const configuration = resolve(runtime, 'weston.ini');
    writeFileSync(configuration, '[shell]\nstartup-animation=none\n');
    weston = spawn(
      'weston',
      [
        '--debug',
        '--backend=x11',
        '--renderer=pixman',
        '--socket=boxrig-wheel',
        '--width=' + Math.floor(1600 / outputScale),
        '--height=' + Math.floor(1000 / outputScale),
        '--scale=' + outputScale,
        '--idle-time=0',
        '--config=' + configuration,
        '--log=' + runtime + '/weston.log',
      ],
      { env: process.env, stdio: 'ignore' }
    );
    const until = Date.now() + 15000;
    while (!existsSync(runtime + '/boxrig-wheel')) {
      assert(Date.now() < until, 'Weston ready');
      await setTimeout(20);
    }
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
        BOXRIG_TEST_SAMPLE_WHEEL_POLICY: '1',
        NO_AT_BRIDGE: '1',
      },
      stdio: ['ignore', 'ignore', 'pipe'],
    }
  );
  child.stderr.on('data', (data) => {
    diagnostics += data;
  });
  const scene = () => command('weston-debug', ['scene-graph']);
  const title = () =>
    wayland
      ? /top-level window '(boxrig-wheel-[^']*)'/.exec(scene())?.[1]
      : command('xdotool', ['getwindowname', id]);
  if (wayland) {
    const until = Date.now() + 30000;
    while (!title()?.startsWith('boxrig-wheel-0-R-')) {
      assert(Date.now() < until, 'sample ready');
      await setTimeout(20);
    }
    id = /window id (\d+)/.exec(
      readFileSync(runtime + '/weston.log', 'utf8')
    )?.[1];
    assert(id);
  } else
    id = command('xdotool', [
      'search',
      '--sync',
      '--onlyvisible',
      '--name',
      '^boxrig-wheel-0-R-',
    ]).split('\n')[0];
  const readState = () => {
    const text = title(),
      match =
        /^boxrig-wheel-(\d+)-([a-zMRUD])-(\d+)-(\d+)-(\d+)-(\d+)-(\d+)(?:-pixels-(-?\d+)-(-?\d+))?$/.exec(
          text ?? ''
        );
    assert(match, text);
    return {
      sequence: Number(match[1]),
      tag: match[2],
      height: Number(match[3]),
      width: Number(match[4]),
      rows: Number(match[5]),
      columns: Number(match[6]),
      reports: Number(match[7]),
      mouseX: match[8] === undefined ? undefined : Number(match[8]),
      mouseY: match[9] === undefined ? undefined : Number(match[9]),
    };
  };
  const refreshGeometry = () => {
    if (wayland) {
      const part = scene()
        .split(/(?=View \d+ \(role )/)
        .find(
          (part) =>
            part.includes('PID ' + child.pid + ',') &&
            part.includes('role xdg_toplevel')
        );
      const p = /position: \((\d+), (\d+)\) -> \((\d+), (\d+)\)/.exec(
        part ?? ''
      );
      assert(p, part);
      const host = Object.fromEntries(
        command('xdotool', ['getwindowgeometry', '--shell', id])
          .split('\n')
          .map((l) => l.split('='))
      );
      geometry = {
        x: Number(host.X) + Number(p[1]) * outputScale,
        y: Number(host.Y) + Number(p[2]) * outputScale,
        width: (Number(p[3]) - Number(p[1])) * outputScale,
        height: (Number(p[4]) - Number(p[2])) * outputScale,
      };
    } else {
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
  };
  let state = readState();
  sequence = state.sequence;
  const initial = { ...state };
  refreshGeometry();
  command('xdotool', ['windowfocus', '--sync', id]);
  if (wayland)
    command('xdotool', [
      'mousemove',
      String(geometry.x + 200),
      String(geometry.y + 100),
      'click',
      '1',
    ]);
  const send = async (key) => {
    command('xdotool', ['key', '--clearmodifiers', key]);
    const until = Date.now() + 15000;
    while (true) {
      const next = readState();
      if (next.sequence > sequence && next.tag === key) {
        sequence = next.sequence;
        state = next;
        return next;
      }
      assert(Date.now() < until, 'PTY acknowledgement ' + key);
      await setTimeout(10);
    }
  };
  const capture = () => {
    refreshGeometry();
    const width = Math.min(geometry.width, 600),
      height = Math.min(geometry.height, 240);
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
        `${width}x${height}`,
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
      { maxBuffer: width * height * 3 + 10000, timeout: 15000 }
    );
    return { bytes, width, height };
  };
  const rowColor = (bytes, at) => {
    for (let row = 0; row < 72; row++)
      if (
        [16 + row * 3, 190 - row * 2, 50 + row * 2].every(
          (c, i) => Math.abs(bytes[at + i] - c) <= 1
        )
      )
        return row;
    return -1;
  };
  const observeRow = ({ bytes, width, height }) => {
    if (!origin) {
      const x = Math.floor(width / 2);
      let y = 0;
      while (y < height && rowColor(bytes, (y * width + x) * 3) < 0) y++;
      if (y >= height) return -1;
      if (pixelOnly) {
        // A scrolled-window shadow can cover the first pixels of row 0.
        // The next colored row has an unshadowed, exact cell boundary.
        const firstRow = rowColor(bytes, (y * width + x) * 3);
        let nextY = y + 1;
        while (
          nextY < height &&
          rowColor(bytes, (nextY * width + x) * 3) !== firstRow + 1
        )
          nextY++;
        // The PTY can acknowledge new cell metrics before GTK presents them.
        // Require the next complete row to have that height before deriving
        // the content origin from the image.
        let thirdY = nextY + 1;
        while (
          thirdY < height &&
          rowColor(bytes, (thirdY * width + x) * 3) !== firstRow + 2
        )
          thirdY++;
        if (thirdY === height || thirdY - nextY !== state.height) return -1;
        assert(
          nextY <= y + state.height && nextY >= state.height,
          JSON.stringify({ y, nextY, cellHeight: state.height, firstRow })
        );
        y = nextY - state.height;
      }
      let left = 0;
      while (
        left < width &&
        rowColor(
          bytes,
          ((y + Math.floor(state.height / 2)) * width + left) * 3
        ) < 0
      )
        left++;
      origin = { x: left, y };
    }
    return rowColor(
      bytes,
      ((origin.y + Math.floor(state.height / 2)) * width +
        origin.x +
        Math.floor(state.width * 10.5)) *
        3
    );
  };
  const expectRow = async (expected, name) => {
    const until = Date.now() + 15000;
    let actual;
    while ((actual = observeRow(capture())) !== expected) {
      assert(
        Date.now() < until,
        `${name}: expected row ${expected}, got ${actual}`
      );
      await setTimeout(10);
    }
    results.push({ name, row: actual, ...state });
  };
  const move = () =>
    command('xdotool', [
      'mousemove',
      String(geometry.x + origin.x + Math.floor(state.width * 2.5)),
      String(geometry.y + origin.y + Math.floor(state.height * 2.5)),
    ]);
  const wheel = (button, modifiers = [], repeat = 1) => {
    refreshGeometry();
    move();
    for (const m of modifiers) command('xdotool', ['keydown', m]);
    try {
      command('xdotool', [
        'click',
        '--repeat',
        String(repeat),
        '--delay',
        '15',
        String(button),
      ]);
    } finally {
      for (const m of [...modifiers].reverse())
        command('xdotool', ['keyup', m]);
    }
  };
  const record = (name, expected) => {
    refreshGeometry();
    const width = Math.min(geometry.width, 600),
      height = Math.min(geometry.height, 240),
      file = resolve(output, name + '.mkv');
    execFileSync(
      'ffmpeg',
      [
        '-y',
        '-nostdin',
        '-v',
        'error',
        '-f',
        'x11grab',
        '-draw_mouse',
        '0',
        '-framerate',
        '30',
        '-video_size',
        `${width}x${height}`,
        '-i',
        `${process.env.DISPLAY}+${geometry.x},${geometry.y}`,
        '-t',
        '2',
        '-c:v',
        'ffv1',
        file,
      ],
      { timeout: 15000 }
    );
    const bytes = execFileSync(
      'ffmpeg',
      [
        '-v',
        'error',
        '-i',
        file,
        '-f',
        'rawvideo',
        '-pix_fmt',
        'rgb24',
        'pipe:1',
      ],
      { maxBuffer: width * height * 3 * 100 }
    );
    const stride = width * height * 3;
    assert.equal(bytes.length % stride, 0);
    assert(bytes.length / stride >= 45);
    for (let f = 15; f < bytes.length / stride; f++)
      assert.equal(
        rowColor(
          bytes,
          f * stride +
            ((origin.y + Math.floor(state.height / 2)) * width +
              origin.x +
              Math.floor(state.width * 10.5)) *
              3
        ),
        expected,
        name + ' stable visible row'
      );
    results.push({
      name,
      frames: bytes.length / stride,
      row: expected,
      ...state,
    });
  };
  let live = -1;
  const initialPresentationDeadline = Date.now() + 15000;
  while ((live = observeRow(capture())) < 0) {
    assert(
      Date.now() < initialPresentationDeadline,
      'initial colored history presentation'
    );
    await setTimeout(10);
  }
  assert(live >= 3);
  if (pixelOnly) {
    results.push({ name: 'content-origin', geometry, origin, ...state });
    await send('i');
    wheel(4);
    await send('z');
    assert.equal(state.reports, 1);
    assert.equal(state.mouseX, Math.floor(state.width * 2.5) + 1);
    assert.equal(
      state.mouseY,
      Math.floor(state.height * 2.5) + 1,
      JSON.stringify(results)
    );
    results.push({ name: 'physical-pixels', ...state });
    command('xdotool', [
      'mousemove',
      String(geometry.x + origin.x + Math.floor(state.width * 2.5) + 1),
      String(geometry.y + origin.y + Math.floor(state.height * 2.5) + 1),
      'click',
      '4',
    ]);
    await send('z');
    assert.equal(state.reports, 2);
    assert.equal(state.mouseX, Math.floor(state.width * 2.5) + 2);
    assert.equal(state.mouseY, Math.floor(state.height * 2.5) + 2);
    results.push({ name: 'one-pixel-within-cell', ...state });
    const previousFrame = capture();
    const previousHeight = state.height;
    wheel(4, ['Control_L']);
    await send('z');
    assert.equal(state.reports, 2);
    assert(state.height > initial.height || state.width > initial.width);
    origin = undefined;
    if (state.height !== previousHeight)
      assert.equal(
        observeRow(previousFrame),
        -1,
        'A frame with the previous cell height is not a presented zoom frame'
      );
    await expectRow(live, 'pixel-zoom');
    wheel(4);
    await send('z');
    assert.equal(state.reports, 3);
    assert.equal(state.mouseX, Math.floor(state.width * 2.5) + 1);
    assert.equal(state.mouseY, Math.floor(state.height * 2.5) + 1);
    results.push({ name: 'pixels-after-zoom', ...state });
    await send('r');
    wheel(4);
    await send('z');
    assert.equal(state.reports, 4);
    assert.equal(state.mouseX, undefined);
    results.push({ name: 'cell-encoding-restored', ...state });
  } else {
    record('live', live);
    wheel(4);
    await expectRow(live - 3, 'up-three');
    wheel(5);
    await expectRow(live, 'down-three');
    wheel(4, [], 40);
    await expectRow(0, 'oldest-clamp');
    wheel(4, [], 4);
    await expectRow(0, 'oldest-stays');
    wheel(5, [], 40);
    await expectRow(live, 'live-clamp');
    await send('r');
    wheel(4);
    const until = Date.now() + 15000;
    while (readState().reports !== 1) {
      assert(Date.now() < until, 'mouse report');
      await setTimeout(10);
    }
    state = readState();
    sequence = state.sequence;
    await expectRow(live, 'reporting-no-history');
    wheel(4, ['Shift_L']);
    await expectRow(live - 3, 'shift-override');
    wheel(5, ['Shift_L']);
    await expectRow(live, 'shift-restore');
    wheel(4, ['Control_L']);
    await send('z');
    assert(
      state.height > initial.height || state.width > initial.width,
      'Ctrl-wheel metrics grow'
    );
    assert.equal(state.rows, initial.rows);
    assert.equal(state.columns, initial.columns);
    assert.equal(state.reports, 1);
    origin = undefined;
    await expectRow(live, 'zoom');
    record('zoomed', live);
    command('xdotool', ['key', '--clearmodifiers', 'ctrl+0']);
    await send('z');
    assert.equal(state.width, initial.width);
    assert.equal(state.height, initial.height);
    origin = undefined;
    await expectRow(live, 'reset');
    record('reset', live);
    wheel(5, ['Control_L', 'Shift_L']);
    await send('z');
    assert(
      state.height < initial.height || state.width < initial.width,
      'Ctrl-Shift-wheel zoom wins'
    );
    assert.equal(state.reports, 1);
    command('xdotool', ['key', '--clearmodifiers', 'ctrl+0']);
    await send('z');
    origin = undefined;
    await expectRow(live, 'second-reset');
    await send('n');
    wheel(4);
    await expectRow(live - 3, 'primary-before-alternate');
    await send('a');
    wheel(4, [], 10);
    await send('z');
    assert.equal(state.reports, 1);
    await send('s');
    await expectRow(30, 'alternate-scroll-initial');
    record('alternate-scroll-initial', 30);
    const expectReport = async (tag, reports) => {
      const deadline = Date.now() + 15000;
      for (;;) {
        const next = readState();
        if (next.tag === tag && next.reports === reports) {
          state = next;
          sequence = next.sequence;
          results.push({ name: 'alternate-report-' + tag, ...state });
          return;
        }
        assert(
          Date.now() < deadline,
          `Expected ${tag} report ${reports}: ${JSON.stringify(next)}`
        );
        await setTimeout(10);
      }
    };
    wheel(4);
    await expectReport('U', 2);
    await expectRow(29, 'alternate-scroll-up');
    record('alternate-scroll-up', 29);
    wheel(5);
    await expectReport('D', 3);
    await expectRow(30, 'alternate-scroll-down');
    await send('c');
    wheel(4);
    await expectReport('U', 4);
    await expectRow(29, 'alternate-application-up');
    wheel(5);
    await expectReport('D', 5);
    await expectRow(30, 'alternate-application-down');
    await send('b');
    wheel(4, ['Shift_L']);
    await send('z');
    assert.equal(state.reports, 5, 'Shift overrides alternate scroll');
    await send('r');
    wheel(4);
    await expectReport('M', 6);
    await expectRow(30, 'alternate-mouse-reporting-wins');
    wheel(4, ['Shift_L']);
    await send('z');
    assert.equal(state.reports, 6, 'Shift overrides mouse reporting');
    await send('n');
    wheel(4, ['Control_L']);
    await send('z');
    assert.equal(state.reports, 6, 'Ctrl zoom overrides alternate scroll');
    assert(state.height > initial.height || state.width > initial.width);
    command('xdotool', ['key', '--clearmodifiers', 'ctrl+0']);
    await send('z');
    origin = undefined;
    await expectRow(30, 'alternate-zoom-reset');
    await send('t');
    wheel(4, [], 3);
    await send('z');
    assert.equal(
      state.reports,
      6,
      'Disabled alternate scroll produces no bytes'
    );
    record('alternate-scroll-disabled', 30);
    await send('p');
    await expectRow(live - 3, 'alternate-preserves-primary');
  }
  command('xdotool', ['key', '--clearmodifiers', 'q']);
  console.log(JSON.stringify({ results }));
} finally {
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
