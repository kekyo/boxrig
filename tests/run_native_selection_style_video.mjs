/* Lossless native recordings assert observed phases and unchanged neighbors. */
import assert from 'node:assert/strict';
import { spawn, execFileSync } from 'node:child_process';
import {
  mkdirSync,
  writeFileSync,
  mkdtempSync,
  existsSync,
  rmSync,
  readFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { setTimeout } from 'node:timers/promises';
import { resolve } from 'node:path';

const [sample, helper, output, renderer] = process.argv.slice(2);
assert(
  sample && helper && output,
  'sample, PTY helper and output directory required'
);
mkdirSync(output, { recursive: true });
const command = (name, args) =>
  execFileSync(name, args, { encoding: 'utf8', timeout: 30000 }).trim();
const wayland = process.env.BOXRIG_TEST_BLINK_WAYLAND === '1';
let weston, runtime, child;
let diagnostics = '';
const results = [];
try {
  if (wayland) {
    runtime = mkdtempSync(resolve(tmpdir(), 'boxrig-selection-wayland-'));
    process.env.XDG_RUNTIME_DIR = runtime;
    process.env.WAYLAND_DISPLAY = 'boxrig-selection';
    weston = spawn(
      'weston',
      [
        '--debug',
        '--backend=x11',
        '--renderer=pixman',
        '--socket=boxrig-selection',
        '--width=1000',
        '--height=650',
        '--idle-time=0',
        '--no-config',
        '--log=' + runtime + '/weston.log',
      ],
      { env: process.env, stdio: 'ignore' }
    );
    const deadline = Date.now() + 15000;
    while (!existsSync(runtime + '/boxrig-selection')) {
      assert(Date.now() < deadline, 'Weston socket timeout');
      await setTimeout(20);
    }
  }
  const arguments_ = [
    ...(renderer ? ['--renderer=' + renderer] : []),
    resolve(helper),
    '--sample-scenario',
  ];
  child = spawn(resolve(sample), arguments_, {
    env: {
      ...process.env,
      BOXRIG_TEST_SAMPLE_SELECTION_STYLE_VIDEO: '1',
      BOXRIG_SELECTION_STYLE_DEMO: '1',
      BOXRIG_COLOR_FILTER_DEMO: '1',
      NO_AT_BRIDGE: '1',
    },
    stdio: ['ignore', 'ignore', 'pipe'],
  });
  child.stderr.on('data', (data) => {
    diagnostics += data;
  });
  const scene = () => command('weston-debug', ['scene-graph']);
  const readTitle = () =>
    wayland
      ? /top-level window '(boxrig-selection-[^']*)'/.exec(scene())?.[1]
      : command('xdotool', ['getwindowname', id]);
  let id, values, title;
  if (wayland) {
    const until = Date.now() + 30000;
    while (!(title = readTitle())?.startsWith('boxrig-selection-ready-')) {
      assert(Date.now() < until, 'Wayland child ready timeout');
      await setTimeout(20);
    }
    id = /window id (\d+)/.exec(
      readFileSync(runtime + '/weston.log', 'utf8')
    )?.[1];
    assert(id, 'Weston X11 window');
    const section = scene()
      .split(/(?=View \d+ \(role )/)
      .find(
        (part) =>
          part.includes('PID ' + child.pid + ',') &&
          part.startsWith('View ') &&
          part.includes('role xdg_toplevel')
      );
    const position = /position: \((\d+), (\d+)\) -> \((\d+), (\d+)\)/.exec(
      section ?? ''
    );
    assert(position, section);
    values = {
      X: position[1],
      Y: position[2],
      WIDTH: Number(position[3]) - Number(position[1]),
      HEIGHT: Number(position[4]) - Number(position[2]),
    };
    command('xdotool', ['windowfocus', '--sync', id]);
    command('xdotool', [
      'mousemove',
      '--window',
      id,
      String(Number(values.X) + 200),
      String(Number(values.Y) + 100),
      'click',
      '1',
    ]);
  } else {
    id = command('xdotool', [
      'search',
      '--sync',
      '--onlyvisible',
      '--name',
      '^boxrig-selection-ready-',
    ]).split('\n')[0];
    title = readTitle();
    values = Object.fromEntries(
      command('xdotool', ['getwindowgeometry', '--shell', id])
        .split('\n')
        .map((line) => line.split('='))
    );
  }
  // F12 remains available to disable the initial color filter while the
  // selection demonstration uses its independent F10 gesture.
  command('xdotool', ['windowfocus', '--sync', id]);
  command('xdotool', ['key', '--clearmodifiers', 'ctrl+shift+F12']);
  const match = /^boxrig-selection-ready-(\d+)-(\d+)$/.exec(title);
  assert(match, title);
  const cellHeight = Number(match[1]),
    cellWidth = Number(match[2]);
  command('xdotool', ['windowfocus', '--sync', id]);
  command('xdotool', ['mousemove', '1200', '1000']);
  const width = Math.min(Number(values.WIDTH), 500),
    height = Math.min(Number(values.HEIGHT), 200);
  const frameBytes = width * height * 3;
  let origin;
  const rawCapture = () =>
    execFileSync(
      'ffmpeg',
      [
        '-nostdin',
        '-loglevel',
        'error',
        '-f',
        'x11grab',
        '-draw_mouse',
        '0',
        '-video_size',
        width + 'x' + height,
        '-i',
        process.env.DISPLAY + '+' + values.X + ',' + values.Y,
        '-frames:v',
        '1',
        '-f',
        'rawvideo',
        '-pix_fmt',
        'rgb24',
        'pipe:1',
      ],
      { maxBuffer: frameBytes * 2, timeout: 30000 }
    );
  let left, top, right, bottom;
  const anchorDeadline = Date.now() + 15000;
  for (;;) {
    const anchor = rawCapture();
    left = width;
    top = height;
    right = -1;
    bottom = -1;
    for (let y = 0; y < height; y++)
      for (let x = 0; x < width; x++) {
        const at = (y * width + x) * 3;
        if (
          Math.abs(anchor[at] - 231) <= 1 &&
          Math.abs(anchor[at + 1] - 43) <= 1 &&
          Math.abs(anchor[at + 2] - 19) <= 1
        ) {
          left = Math.min(left, x);
          top = Math.min(top, y);
          right = Math.max(right, x);
          bottom = Math.max(bottom, y);
        }
      }
    if (right - left + 1 === cellWidth && bottom - top + 1 === cellHeight)
      break;
    assert(
      Date.now() < anchorDeadline,
      'first painted grid anchor not observed'
    );
    await setTimeout(20);
  }
  origin = { x: left, y: top };
  const x = left + cellWidth,
    y = top + cellHeight,
    selectionWidth = 11 * cellWidth;
  command('xdotool', [
    'mousemove',
    String(Number(values.X) + x + 1),
    String(Number(values.Y) + y + Math.floor(cellHeight / 2)),
    'mousedown',
    '1',
    'mousemove',
    String(Number(values.X) + x + selectionWidth - 1),
    String(Number(values.Y) + y + Math.floor(cellHeight / 2)),
    'mouseup',
    '1',
  ]);
  command('xdotool', ['mousemove', '1200', '1000']);
  const blend = (base, color, alpha) =>
    base.map((v, i) =>
      Math.round((v * (255 - alpha) + color[i] * alpha) / 255)
    );
  const background = [24, 42, 63];
  let reference;
  const record = (name, mode) => {
    const file = resolve(output, name + '.mkv');
    execFileSync(
      'ffmpeg',
      [
        '-y',
        '-nostdin',
        '-loglevel',
        'error',
        '-f',
        'x11grab',
        '-framerate',
        '30',
        '-draw_mouse',
        '0',
        '-video_size',
        width + 'x' + height,
        '-i',
        process.env.DISPLAY + '+' + values.X + ',' + values.Y,
        '-t',
        '2',
        '-c:v',
        'ffv1',
        file,
      ],
      { timeout: 30000 }
    );
    const raw = execFileSync(
      'ffmpeg',
      [
        '-nostdin',
        '-loglevel',
        'error',
        '-i',
        file,
        '-f',
        'rawvideo',
        '-pix_fmt',
        'rgb24',
        'pipe:1',
      ],
      { maxBuffer: frameBytes * 100, timeout: 30000 }
    );
    assert.equal(raw.length % frameBytes, 0);
    const frames = raw.length / frameBytes;
    assert(frames >= 45);
    let maximumOutsideError = 0,
      maximumPixelError = 0;
    const fill =
      mode === 0
        ? background
        : blend(background, mode === 1 ? [60, 150, 240] : [255, 190, 40], 96);
    const edge =
      mode === 0
        ? [46, 115, 242]
        : mode === 1
          ? fill
          : blend(fill, [255, 215, 0], 220);
    for (let frame = 15; frame < frames; frame++) {
      const pixels = raw.subarray(frame * frameBytes, (frame + 1) * frameBytes);
      if (!reference) reference = Buffer.from(pixels);
      for (const [px, py, expected] of [
        [x + Math.floor(cellWidth / 2), y + Math.floor(cellHeight / 2), fill],
        [x + Math.floor(cellWidth / 2), y, edge],
        [x, y, edge],
      ]) {
        const at = (py * width + px) * 3;
        for (let c = 0; c < 3; c++)
          maximumPixelError = Math.max(
            maximumPixelError,
            Math.abs(pixels[at + c] - expected[c])
          );
      }
      for (let py = top; py < Math.min(height, top + 6 * cellHeight); py++)
        for (let px = left; px < Math.min(width, left + 40 * cellWidth); px++) {
          if (
            px >= x &&
            px < x + selectionWidth &&
            py >= y &&
            py < y + cellHeight
          )
            continue;
          const at = (py * width + px) * 3;
          for (let c = 0; c < 3; c++)
            maximumOutsideError = Math.max(
              maximumOutsideError,
              Math.abs(pixels[at + c] - reference[at + c])
            );
        }
      if (name === 'outline-restored')
        for (let i = 0; i < frameBytes; i++)
          maximumOutsideError = Math.max(
            maximumOutsideError,
            Math.abs(pixels[i] - reference[i])
          );
    }
    assert(
      maximumPixelError <= 2,
      name + ': selection pixels differ ' + maximumPixelError
    );
    assert(
      maximumOutsideError <= (wayland ? 1 : 0),
      name + ': unselected pixels changed ' + maximumOutsideError
    );
    results.push({
      name,
      mode,
      frames,
      settledFrames: frames - 15,
      maximumPixelError,
      maximumOutsideError,
      origin,
      cellWidth,
      cellHeight,
    });
    console.log(JSON.stringify(results.at(-1)));
  };
  record('outline', 0);
  command('xdotool', ['key', '--clearmodifiers', 'ctrl+shift+F10']);
  record('fill', 1);
  command('xdotool', ['key', '--clearmodifiers', 'ctrl+shift+F10']);
  record('fill-outline', 2);
  command('xdotool', ['key', '--clearmodifiers', 'ctrl+shift+F10']);
  record('outline-restored', 0);
  command('xdotool', ['key', '--clearmodifiers', 'q']);
  const exitDeadline = Date.now() + 15000;
  while (child.exitCode === null && child.signalCode === null) {
    assert(Date.now() < exitDeadline, 'sample did not exit after q');
    await setTimeout(20);
  }
  assert.equal(child.exitCode, 0, diagnostics);
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
