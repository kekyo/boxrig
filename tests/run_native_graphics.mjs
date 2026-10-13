/* Validate image transport through the real sample, PTY and rendered pixels. */
import assert from 'node:assert/strict';
import { spawn, execFileSync } from 'node:child_process';
import { once } from 'node:events';
import {
  existsSync,
  readFileSync,
  writeFileSync,
  mkdirSync,
  mkdtempSync,
  rmSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve } from 'node:path';
import { setTimeout } from 'node:timers/promises';

const [sample, helper, outputBase, renderer, backend, policy = 'allow'] =
  process.argv.slice(2);
const animation = process.env.BOXRIG_TEST_FRAME_DELETE === '1';
const output = animation ? outputBase + '-animation' : outputBase;
assert(sample && helper && output && renderer && backend);
mkdirSync(output, { recursive: true });
const report = resolve(output, 'child.log');
rmSync(report, { force: true });
const command = (name, args) =>
  execFileSync(name, args, { encoding: 'utf8', timeout: 30000 }).trim();
const waitFor = async (predicate, description) => {
  const deadline = Date.now() + 30000;
  for (;;) {
    const result = predicate();
    if (result) return result;
    assert(Date.now() < deadline, description);
    await setTimeout(20);
  }
};
const terminate = async (child) => {
  if (!child || child.exitCode !== null || child.signalCode !== null) return;
  const stopped = once(child, 'exit');
  child.kill();
  await stopped;
};
let weston, child, recorder, runtime;
let diagnostics = '';
try {
  if (backend === 'wayland') {
    runtime = mkdtempSync(resolve(tmpdir(), 'boxrig-graphics-'));
    process.env.XDG_RUNTIME_DIR = runtime;
    process.env.WAYLAND_DISPLAY = 'boxrig-graphics';
    weston = spawn(
      'weston',
      [
        '--debug',
        '--backend=x11',
        '--renderer=pixman',
        '--socket=boxrig-graphics',
        '--width=1000',
        '--height=650',
        '--idle-time=0',
        '--no-config',
        '--log=' + runtime + '/weston.log',
      ],
      { stdio: 'ignore' }
    );
    await waitFor(
      () => existsSync(runtime + '/boxrig-graphics'),
      'Weston ready'
    );
  }
  child = spawn(
    resolve(sample),
    [
      '--renderer=' + renderer,
      resolve(helper),
      report,
      animation ? 'animation' : policy,
    ],
    {
      env: {
        ...process.env,
        GDK_BACKEND: backend,
        BOXRIG_ALLOW_GRAPHICS_EXTERNAL: policy === 'allow' ? '1' : '0',
        BOXRIG_SCROLLBAR: 'never',
        GTK_IM_MODULE: 'gtk-im-context-simple',
        NO_AT_BRIDGE: '1',
        LIBGL_ALWAYS_SOFTWARE: '1',
      },
      stdio: ['ignore', 'ignore', 'pipe'],
    }
  );
  child.stderr.on('data', (data) => {
    diagnostics += data;
  });
  await waitFor(() => {
    const text = existsSync(report) ? readFileSync(report, 'utf8') : '';
    assert(!text.includes('FAIL'), text);
    assert(child.exitCode === null, diagnostics + text);
    return text.includes('PASS all');
  }, 'External transfer completion');
  let id, x, y, width, height;
  if (backend === 'wayland') {
    const part = await waitFor(
      () =>
        command('weston-debug', ['scene-graph'])
          .split(/(?=View \d+ \(role )/)
          .find(
            (part) =>
              part.includes("top-level window 'boxrig-graphics-ready'") &&
              part.includes('role xdg_toplevel') &&
              part.includes('position:')
          ),
      'Wayland title'
    );
    const position = /position: \((\d+), (\d+)\) -> \((\d+), (\d+)\)/.exec(
      part
    );
    assert(position, part);
    [x, y] = position.slice(1, 3).map(Number);
    width = Math.min(600, Number(position[3]) - x);
    height = Math.min(250, Number(position[4]) - y);
    id = /window id (\d+)/.exec(
      readFileSync(runtime + '/weston.log', 'utf8')
    )?.[1];
    assert(id);
    command('xdotool', ['windowfocus', '--sync', id]);
    command('xdotool', [
      'mousemove',
      '--window',
      id,
      String(x + 200),
      String(y + 100),
      'click',
      '1',
    ]);
    const geometry = Object.fromEntries(
      command('xdotool', ['getwindowgeometry', '--shell', id])
        .split('\n')
        .map((line) => line.split('='))
    );
    x += Number(geometry.X);
    y += Number(geometry.Y);
  } else {
    id = command('xdotool', [
      'search',
      '--sync',
      '--onlyvisible',
      '--name',
      '^boxrig-graphics-ready$',
    ]).split('\n')[0];
    const geometry = Object.fromEntries(
      command('xdotool', ['getwindowgeometry', '--shell', id])
        .split('\n')
        .map((line) => line.split('='))
    );
    x = Number(geometry.X);
    y = Number(geometry.Y);
    width = Math.min(600, Number(geometry.WIDTH));
    height = Math.min(250, Number(geometry.HEIGHT));
    command('xdotool', ['windowfocus', '--sync', id]);
  }
  command('xdotool', ['mousemove', '1200', '1000']);
  if (animation) {
    width = Math.min(width, 240);
    height = Math.min(height, 200);
  }
  const colors = [
    [211, 37, 83],
    [23, 191, 71],
    [31, 79, 233],
  ];
  let result;
  if (animation) {
    const frameBytes = width * height * 3;
    const video = resolve(output, 'frame-deletion.mkv');
    const observations = [];
    const analyze = (frame) => {
      const counts = [0, 0, 0],
        markers = [0, 0, 0, 0, 0];
      for (let at = 0; at < frame.length; at += 3) {
        for (let color = 0; color < colors.length; color++) {
          if (
            Math.abs(frame[at] - colors[color][0]) <= 1 &&
            Math.abs(frame[at + 1] - colors[color][1]) <= 1 &&
            Math.abs(frame[at + 2] - colors[color][2]) <= 1
          )
            counts[color]++;
        }
        if (
          Math.abs(frame[at + 1] - 43) <= 1 &&
          Math.abs(frame[at + 2] - 173) <= 1
        ) {
          const phase = Math.round((frame[at] - 71) / 31);
          if (
            phase >= 0 &&
            phase < markers.length &&
            Math.abs(frame[at] - (71 + phase * 31)) <= 1
          )
            markers[phase]++;
        }
      }
      const phase = markers.findIndex((count) => count >= 20);
      const color = counts.findIndex((count) => count >= 200);
      return { phase, color, counts };
    };
    let pending = Buffer.alloc(0),
      recordingError = '';
    // Weston fades newly mapped windows. Wait for the authored color before
    // measuring frame deletion so that this compositor transition is excluded.
    await waitFor(() => {
      const frame = execFileSync(
        'ffmpeg',
        [
          '-v',
          'error',
          '-f',
          'x11grab',
          '-draw_mouse',
          '0',
          '-video_size',
          width + 'x' + height,
          '-i',
          process.env.DISPLAY + '+' + x + ',' + y,
          '-frames:v',
          '1',
          '-f',
          'rawvideo',
          '-pix_fmt',
          'rgb24',
          'pipe:1',
        ],
        { maxBuffer: frameBytes * 2 }
      );
      let exact = 0;
      for (let at = 0; at < frame.length; at += 3) {
        if (
          frame[at] === colors[0][0] &&
          frame[at + 1] === colors[0][1] &&
          frame[at + 2] === colors[0][2]
        )
          exact++;
      }
      return exact >= 200;
    }, 'Initial image after compositor fade');
    recorder = spawn(
      'ffmpeg',
      [
        '-y',
        '-loglevel',
        'error',
        '-framerate',
        '30',
        '-f',
        'x11grab',
        '-draw_mouse',
        '0',
        '-video_size',
        width + 'x' + height,
        '-i',
        process.env.DISPLAY + '+' + x + ',' + y,
        '-map',
        '0:v',
        '-c:v',
        'ffv1',
        video,
        '-map',
        '0:v',
        '-f',
        'rawvideo',
        '-pix_fmt',
        'rgb24',
        'pipe:1',
      ],
      { stdio: ['pipe', 'pipe', 'pipe'] }
    );
    recorder.stderr.on('data', (data) => {
      recordingError += data;
    });
    recorder.stdout.on('data', (data) => {
      pending = Buffer.concat([pending, data]);
      while (pending.length >= frameBytes) {
        observations.push(analyze(pending.subarray(0, frameBytes)));
        pending = pending.subarray(frameBytes);
      }
    });
    const phaseFrames = (phase) =>
      observations.filter((frame) => frame.phase === phase);
    const transitions = (frames) =>
      frames
        .map((frame) => frame.color)
        .filter(
          (color, index, colors) => index === 0 || color !== colors[index - 1]
        );
    const waitPhase = async (phase, minimumFrames, minimumTransitions) => {
      await waitFor(() => {
        assert(recorder.exitCode === null, recordingError);
        assert(child.exitCode === null, diagnostics);
        assert(
          !readFileSync(report, 'utf8').includes('FAIL'),
          readFileSync(report, 'utf8')
        );
        const frames = phaseFrames(phase);
        return (
          frames.length >= minimumFrames &&
          transitions(frames).length >= minimumTransitions
        );
      }, 'Animation phase ' + phase);
    };
    await waitPhase(0, 3, 1);
    for (const [key, phase, frames, changes] of [
      ['s', 1, 12, 7],
      ['d', 2, 12, 5],
      ['f', 3, 15, 1],
      ['x', 4, 5, 1],
    ]) {
      command('xdotool', ['key', '--clearmodifiers', key]);
      await waitPhase(phase, frames, changes);
    }
    const stopped = once(recorder, 'exit');
    recorder.stdin.end('q\n');
    const [code] = await stopped;
    assert.equal(code, 0, recordingError);
    const recorded = execFileSync(
      'ffmpeg',
      [
        '-v',
        'error',
        '-i',
        video,
        '-f',
        'rawvideo',
        '-pix_fmt',
        'rgb24',
        'pipe:1',
      ],
      { maxBuffer: frameBytes * 1800, timeout: 30000 }
    );
    assert.equal(recorded.length % frameBytes, 0);
    const decoded = [];
    const allowed = [[0], [0, 1, 2], [0, 2], [2], [-1]];
    let lastPhase = -1;
    for (let at = 0; at < recorded.length; at += frameBytes) {
      const frame = analyze(recorded.subarray(at, at + frameBytes));
      if (lastPhase === -1 && frame.phase === -1) continue;
      assert(
        frame.phase >= lastPhase,
        'Animation marker disappeared or moved backward'
      );
      lastPhase = frame.phase;
      assert(allowed[frame.phase].includes(frame.color), JSON.stringify(frame));
      assert(
        frame.counts.filter((count) => count >= 200).length <= 1,
        JSON.stringify(frame)
      );
      for (let color = 0; color < colors.length; color++) {
        if (!allowed[frame.phase].includes(color))
          assert.equal(frame.counts[color], 0, JSON.stringify(frame));
      }
      decoded.push(frame);
    }
    const phases = allowed.map((_, phase) => {
      const frames = decoded.filter((frame) => frame.phase === phase);
      const changes = transitions(frames);
      assert(frames.length >= [3, 12, 12, 15, 5][phase]);
      assert(changes.length >= [1, 7, 5, 1, 1][phase]);
      assert.deepEqual([...new Set(changes)].sort(), allowed[phase]);
      return { phase, frames: frames.length, transitions: changes };
    });
    result = {
      scenario: 'animation',
      renderer,
      backend,
      phases,
      frames: decoded.length,
      video: 'frame-deletion.mkv',
    };
  } else {
    let frame, counts;
    await waitFor(() => {
      frame = execFileSync(
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
          `${process.env.DISPLAY}+${x},${y}`,
          '-frames:v',
          '1',
          '-f',
          'rawvideo',
          '-pix_fmt',
          'rgb24',
          'pipe:1',
        ],
        { maxBuffer: width * height * 6 }
      );
      counts = colors.map((color) => {
        let count = 0;
        for (let at = 0; at < frame.length; at += 3)
          if (
            color.every(
              (value, channel) => Math.abs(frame[at + channel] - value) <= 1
            )
          )
            count++;
        return count;
      });
      return policy === 'allow'
        ? counts.every((count) => count >= 200)
        : counts.every((count) => count === 0);
    }, 'All three images displayed with correct colors');
    execFileSync(
      'ffmpeg',
      [
        '-y',
        '-v',
        'error',
        '-f',
        'rawvideo',
        '-pix_fmt',
        'rgb24',
        '-video_size',
        `${width}x${height}`,
        '-i',
        'pipe:0',
        '-frames:v',
        '1',
        resolve(output, 'frame.png'),
      ],
      { input: frame }
    );
    result = { policy, renderer, backend, colors, counts };
  }
  const stopped = once(child, 'exit');
  command('xdotool', ['key', '--clearmodifiers', 'q']);
  const [status] = await stopped;
  assert.equal(status, 0, diagnostics);
  writeFileSync(
    resolve(output, 'results.json'),
    JSON.stringify({ ...result, status }, null, 2) + '\n'
  );
  console.log(JSON.stringify({ ...result, status }));
} finally {
  await terminate(recorder);
  await terminate(child);
  await terminate(weston);
  writeFileSync(resolve(output, 'sample.log'), diagnostics);
  if (runtime) rmSync(runtime, { recursive: true, force: true });
}
