/* Lossless native recordings assert observed phases and unchanged neighbors. */
import assert from 'node:assert/strict';
import { once } from 'node:events';
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
    // x11grab hides the X11 cursor, but Weston composites the Wayland cursor
    // into its window. Use a transparent cursor so it cannot cover a one-cell
    // selection. The Xcursor format is documented in Xcursor(3) and Xcursor.h.
    const cursorWords = [
      0x72756358,
      16,
      0x10000,
      1, // File header, one table-of-contents entry.
      0xfffd0002,
      24,
      28, // Image chunk type, nominal size and file offset.
      36,
      0xfffd0002,
      24,
      1, // Image header and version.
      1,
      1,
      0,
      0,
      0,
      0, // Width, height, hotspot, delay and transparent ARGB.
    ];
    const cursor = Buffer.alloc(cursorWords.length * 4);
    cursorWords.forEach((word, index) => cursor.writeUInt32LE(word, index * 4));
    const cursorThemes = new Set(['boxrig-invisible', 'default']);
    if (process.env.GDK_BACKEND === 'wayland') {
      // GTK 4 reads the desktop theme below XDG_DATA_HOME, bypassing XCURSOR_PATH.
      const theme = command('gsettings', [
        'get',
        'org.gnome.desktop.interface',
        'cursor-theme',
      ]).replace(/^'|'$/g, '');
      assert(theme && !theme.includes('/') && theme !== '.' && theme !== '..');
      cursorThemes.add(theme);
    }
    process.env.XCURSOR_PATH = resolve(runtime, 'icons');
    process.env.XCURSOR_THEME = 'boxrig-invisible';
    process.env.XDG_DATA_HOME = runtime;
    for (const theme of cursorThemes) {
      const directory = resolve(runtime, 'icons', theme, 'cursors');
      mkdirSync(directory, { recursive: true });
      for (const name of ['default', 'left_ptr', 'text', 'xterm'])
        writeFileSync(resolve(directory, name), cursor);
    }
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
  const width = Math.min(Number(values.WIDTH), 1000),
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
    if (Date.now() >= anchorDeadline) {
      writeFileSync(
        resolve(output, 'anchor-failure.ppm'),
        Buffer.concat([
          Buffer.from('P6\n' + width + ' ' + height + '\n255\n'),
          anchor,
        ])
      );
      console.error(
        JSON.stringify({ left, top, right, bottom, cellWidth, cellHeight })
      );
    }
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

  // Capture every frame while the rectangle crosses zero width. Advance only
  // after the requested geometry is visible in consecutive captured frames.
  const gestureFrames = [];
  let pending = Buffer.alloc(0);
  const capture = spawn(
    'ffmpeg',
    [
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
      '-f',
      'rawvideo',
      '-pix_fmt',
      'rgb24',
      'pipe:1',
    ],
    { stdio: ['pipe', 'pipe', 'pipe'] }
  );
  const captureClosed = once(capture, 'close');
  let captureDiagnostics = '';
  capture.stderr.on('data', (bytes) => {
    captureDiagnostics += bytes;
  });
  capture.stdout.on('data', (bytes) => {
    pending = Buffer.concat([pending, bytes]);
    while (pending.length >= frameBytes) {
      gestureFrames.push(Buffer.from(pending.subarray(0, frameBytes)));
      pending = pending.subarray(frameBytes);
    }
  });
  const ax = left + 2 * cellWidth,
    ay = top + 5 * cellHeight;
  assert(ay + 2 * cellHeight < height, 'gesture rows fit recorded surface');
  const blue = (frame, px, py) => {
    const at = (py * width + px) * 3;
    return [46, 115, 242].every(
      (value, channel) => Math.abs(frame[at + channel] - value) <= 2
    );
  };
  const phases = [];
  const waitGeometry = async (name, selectedColumns, selectedRows, after) => {
    const deadline = Date.now() + 15000;
    let examined = after,
      consecutive = 0;
    while (consecutive < 3) {
      assert(Date.now() < deadline, name + ': geometry was not painted');
      assert(capture.exitCode === null, captureDiagnostics);
      while (examined < gestureFrames.length && consecutive < 3) {
        const frame = gestureFrames[examined++];
        let valid = true;
        for (let py = ay; py < ay + 2 * cellHeight; py++) {
          for (let px = ax; px < ax + 4 * cellWidth; px++) {
            const edge =
              selectedColumns > 0 &&
              px < ax + selectedColumns * cellWidth &&
              py < ay + selectedRows * cellHeight &&
              (px === ax ||
                px === ax + selectedColumns * cellWidth - 1 ||
                py === ay ||
                py === ay + selectedRows * cellHeight - 1);
            if (edge && !blue(frame, px, py)) valid = false;
            if (selectedColumns === 0 && blue(frame, px, py)) valid = false;
          }
        }
        consecutive = valid ? consecutive + 1 : 0;
      }
      if (consecutive < 3) await setTimeout(20);
    }
    phases.push({
      name,
      firstSettledFrame: examined - consecutive,
      lastSettledFrame: examined - 1,
    });
  };
  const waitFill = async (name, selected, fullViewport, after) => {
    const deadline = Date.now() + 15000;
    let examined = after,
      consecutive = 0;
    const filled = blend(background, [60, 150, 240], 96);
    while (consecutive < 3) {
      assert(Date.now() < deadline, name + ': fill was not painted');
      assert(capture.exitCode === null, captureDiagnostics);
      while (examined < gestureFrames.length && consecutive < 3) {
        const frame = gestureFrames[examined++];
        let valid = true;
        // GTK scroll containers shade the very top edge when history exists.
        const firstY = fullViewport ? top + cellHeight : ay;
        const lastY = fullViewport ? top + 8 * cellHeight : ay + 2 * cellHeight;
        for (let py = firstY; py < lastY; py++) {
          for (let px = left + cellWidth; px < left + 6 * cellWidth; px++) {
            const expected =
              selected && px >= ax && px < ax + 3 * cellWidth
                ? filled
                : background;
            const at = (py * width + px) * 3;
            if (
              !expected.every(
                (value, channel) => Math.abs(frame[at + channel] - value) <= 2
              )
            )
              valid = false;
          }
        }
        consecutive = valid ? consecutive + 1 : 0;
      }
      if (consecutive < 3) await setTimeout(20);
    }
    phases.push({
      name,
      firstSettledFrame: examined - consecutive,
      lastSettledFrame: examined - 1,
    });
  };
  const move = (column, row) =>
    command('xdotool', [
      'mousemove',
      String(Number(values.X) + ax + column * cellWidth + 1),
      String(Number(values.Y) + ay + row * cellHeight + 1),
    ]);
  try {
    await waitGeometry('before-rectangle', 0, 0, 0);
    move(0, 0);
    command('xdotool', ['keydown', 'Control_L', 'mousedown', '1']);
    await waitGeometry('rectangle-start', 1, 1, gestureFrames.length);
    move(0, 1);
    await waitGeometry('zero-width', 0, 0, gestureFrames.length);
    move(3, 1);
    await waitGeometry('rectangle-expanded', 3, 2, gestureFrames.length);
    move(0, 1);
    await waitGeometry('zero-width-restored', 0, 0, gestureFrames.length);
    command('xdotool', ['mouseup', '1', 'keyup', 'Control_L']);
    await waitGeometry('empty-released', 0, 0, gestureFrames.length);
    command('xdotool', ['key', '--clearmodifiers', 'e']);
    const edgeDeadline = Date.now() + 15000;
    let edgeReady;
    while (
      !(edgeReady = /^boxrig-selection-edge-ready-(\d+)$/.exec(readTitle()))
    ) {
      assert(Date.now() < edgeDeadline, 'full row preparation timeout');
      await setTimeout(20);
    }
    const columns = Number(edgeReady[1]);
    const gridRight = left + columns * cellWidth;
    assert(gridRight <= width, 'full row fits recorded surface');
    const edgeY = top + cellHeight * 2;
    const markerWidth = Math.ceil(cellWidth / 4);
    const pointer = (px, py) =>
      command('xdotool', [
        'mousemove',
        String(Number(values.X) + px),
        String(Number(values.Y) + py),
      ]);
    const waitEdge = async (name, selected, after) => {
      const deadline = Date.now() + 15000;
      let examined = after,
        consecutive = 0;
      while (consecutive < 3) {
        assert(Date.now() < deadline, name + ': LF marker was not painted');
        assert(capture.exitCode === null, captureDiagnostics);
        while (examined < gestureFrames.length && consecutive < 3) {
          const frame = gestureFrames[examined++];
          let valid = true;
          for (let py = edgeY + 2; py < edgeY + cellHeight - 2; py++) {
            for (let px = gridRight - markerWidth; px < gridRight - 1; px++) {
              const at = (py * width + px) * 3;
              const expected = selected ? [46, 115, 242] : background;
              if (!expected.every((v, c) => Math.abs(frame[at + c] - v) <= 2))
                valid = false;
            }
          }
          consecutive = valid ? consecutive + 1 : 0;
        }
        if (consecutive < 3) await setTimeout(20);
      }
      phases.push({
        name,
        firstSettledFrame: examined - consecutive,
        lastSettledFrame: examined - 1,
      });
    };
    pointer(gridRight - Math.ceil(cellWidth / 2), edgeY + cellHeight / 2);
    command('xdotool', ['mousedown', '1']);
    await waitEdge('last-character-only', false, gestureFrames.length);
    pointer(gridRight - 1, edgeY + cellHeight / 2);
    await waitEdge('through-edge-newline', true, gestureFrames.length);
    pointer(left + 1, edgeY + cellHeight * 1.5);
    await waitEdge('through-next-line', true, gestureFrames.length);
    pointer(gridRight - Math.ceil(cellWidth / 2), edgeY + cellHeight / 2);
    await waitEdge('edge-newline-retracted', false, gestureFrames.length);
    command('xdotool', ['mouseup', '1']);
    await waitEdge('edge-released', false, gestureFrames.length);
    command('xdotool', ['key', '--clearmodifiers', 'h']);
    const historyDeadline = Date.now() + 15000;
    while (readTitle() !== 'boxrig-selection-history-ready') {
      assert(Date.now() < historyDeadline, 'history preparation timeout');
      await setTimeout(20);
    }
    command('xdotool', ['key', '--clearmodifiers', 'ctrl+shift+F10']);
    command('xdotool', ['key', '--clearmodifiers', 'ctrl+shift+F9']);
    await waitFill('history-anchor', true, false, gestureFrames.length);
    command('xdotool', ['key', '--clearmodifiers', 'ctrl+shift+F9']);
    await waitFill('history-expanded', true, true, gestureFrames.length);
    command('xdotool', ['key', '--clearmodifiers', 'ctrl+shift+F9']);
    await waitFill('history-cleared', false, true, gestureFrames.length);
  } finally {
    capture.stdin.end('q');
    await captureClosed;
    execFileSync(
      'ffmpeg',
      [
        '-y',
        '-loglevel',
        'error',
        '-f',
        'rawvideo',
        '-pixel_format',
        'rgb24',
        '-video_size',
        width + 'x' + height,
        '-framerate',
        '30',
        '-i',
        'pipe:0',
        '-c:v',
        'ffv1',
        resolve(output, 'rectangle-gesture.mkv'),
      ],
      { input: Buffer.concat(gestureFrames), timeout: 30000 }
    );
    results.push({
      name: 'rectangle-gesture',
      frames: gestureFrames.length,
      phases,
    });
  }
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
