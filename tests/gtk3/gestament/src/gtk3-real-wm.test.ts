import { spawn } from 'node:child_process';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';

import { createDeferred } from 'async-primitives';
import { createGtkAppLauncher, type GtkCaptureBounds } from 'gestament';
import { toPass } from 'gestament/testing';
import { afterEach, describe, expect, it } from 'vitest';

interface TestConfiguration {
  readonly samplePath: string;
  readonly ptyHelperPath: string;
  readonly artifactDirectory: string | undefined;
}

interface ProcessResult {
  readonly code: number | null;
  readonly signal: NodeJS.Signals | null;
  readonly stdout: string;
  readonly stderr: string;
}

interface MonitorBounds extends GtkCaptureBounds {
  readonly name: string;
}

interface VideoRecorder {
  readonly frame: () => number;
  readonly stop: () => Promise<ProcessResult>;
}

interface PtyResize {
  readonly columns: number;
  readonly rows: number;
}

const requireEnvironment = (name: string): string => {
  const value = process.env[name];

  if (value === undefined || value.length === 0) {
    throw new Error(`${name} must be defined by Makefile.`);
  }
  return value;
};

const configuration: TestConfiguration = {
  samplePath: requireEnvironment('BOXRIG_GTK3_SAMPLE'),
  ptyHelperPath: requireEnvironment('BOXRIG_GTK3_PTY_HELPER'),
  artifactDirectory: process.env.BOXRIG_GTK3_REAL_WM_ARTIFACT_DIR,
};

const tracePath =
  configuration.artifactDirectory === undefined
    ? undefined
    : join(configuration.artifactDirectory, 'geometry.csv');

const launcher = createGtkAppLauncher({
  accessibilitySession: 'inherit',
  appPath: configuration.samplePath,
  args: [
    ...(process.env.BOXRIG_GTK_RENDERER === undefined
      ? []
      : [`--renderer=${process.env.BOXRIG_GTK_RENDERER}`]),
    '--',
    configuration.ptyHelperPath,
    '--report-resize',
  ],
  display: 'host',
  env: {
    BOXRIG_TERM_GTK3_GEOMETRY_TRACE: tracePath,
    GDK_BACKEND: 'x11',
    GTK_MODULES: undefined,
  },
  gsettings: null,
  outputBufferBytes: 64 * 1024,
  theme: null,
  timeoutMs: 15_000,
});

afterEach(async () => {
  await launcher.release();
}, 60_000);

const runProcess = async (
  command: string,
  args: readonly string[],
  environment: NodeJS.ProcessEnv
): Promise<ProcessResult> => {
  const completed = createDeferred<ProcessResult>();
  const child = spawn(command, args, {
    env: environment,
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let stdout = '';
  let stderr = '';

  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stdout.on('data', (chunk: string) => {
    stdout += chunk;
  });
  child.stderr.on('data', (chunk: string) => {
    stderr += chunk;
  });
  child.once('error', (error) => {
    completed.reject(error);
  });
  child.once('close', (code, signal) => {
    completed.resolve({ code, signal, stderr, stdout });
  });
  return await completed.promise;
};

const connectedMonitors = async (
  environment: NodeJS.ProcessEnv
): Promise<readonly MonitorBounds[]> => {
  const result = await runProcess(
    'xrandr',
    ['--listactivemonitors'],
    environment
  );

  expect(result.code, result.stderr).toBe(0);
  return result.stdout
    .split('\n')
    .map((line): MonitorBounds | undefined => {
      const match = line.match(
        /^\s*\d+:\s+\S+\s+(\d+)\/\d+x(\d+)\/\d+\+(-?\d+)\+(-?\d+)\s+(\S+)$/
      );

      if (match === null) {
        return undefined;
      }
      return {
        height: Number(match[2]),
        name: match[5],
        width: Number(match[1]),
        x: Number(match[3]),
        y: Number(match[4]),
      };
    })
    .filter((monitor): monitor is MonitorBounds => monitor !== undefined);
};

const readWindowBounds = async (
  windowId: string,
  environment: NodeJS.ProcessEnv
): Promise<GtkCaptureBounds> => {
  const result = await runProcess(
    'xdotool',
    ['getwindowgeometry', '--shell', windowId],
    environment
  );

  expect(result.code, result.stderr).toBe(0);
  const values = Object.fromEntries(
    result.stdout
      .trim()
      .split('\n')
      .map((line) => line.split('=', 2))
  );
  const bounds = {
    height: Number(values.HEIGHT),
    width: Number(values.WIDTH),
    x: Number(values.X),
    y: Number(values.Y),
  };

  expect(Object.values(bounds).every(Number.isFinite)).toBe(true);
  return bounds;
};

const readWindowTitle = async (
  windowId: string,
  environment: NodeJS.ProcessEnv
): Promise<string> => {
  const result = await runProcess(
    'xdotool',
    ['getwindowname', windowId],
    environment
  );

  expect(result.code, result.stderr).toBe(0);
  return result.stdout.trimEnd();
};

const visibleSampleWindows = async (
  environment: NodeJS.ProcessEnv
): Promise<readonly string[]> => {
  const result = await runProcess(
    'xdotool',
    ['search', '--onlyvisible', '--class', '^Boxrig_term$'],
    environment
  );

  expect([0, 1], result.stderr).toContain(result.code);
  if (result.code === 1) {
    return [];
  }
  const candidates = result.stdout.trim().split('\n').filter(Boolean);
  const windows: string[] = [];

  for (const windowId of candidates) {
    const bounds = await readWindowBounds(windowId, environment);

    if (bounds.width > 100 && bounds.height > 100) {
      windows.push(windowId);
    }
  }
  return windows;
};

const startVideoRecorder = (
  display: string,
  bounds: GtkCaptureBounds,
  outputPath: string,
  environment: NodeJS.ProcessEnv
): VideoRecorder => {
  const completed = createDeferred<ProcessResult>();
  const child = spawn(
    'ffmpeg',
    [
      '-hide_banner',
      '-loglevel',
      'error',
      '-f',
      'x11grab',
      '-framerate',
      '60',
      '-video_size',
      `${bounds.width}x${bounds.height}`,
      '-i',
      `${display}+${bounds.x},${bounds.y}`,
      '-an',
      '-c:v',
      'libx264',
      '-preset',
      'ultrafast',
      '-crf',
      '18',
      '-pix_fmt',
      'yuv420p',
      '-progress',
      'pipe:1',
      '-y',
      outputPath,
    ],
    { env: environment, stdio: ['pipe', 'pipe', 'pipe'] }
  );
  let frame = 0;
  let progress = '';
  let stdout = '';
  let stderr = '';

  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stdout.on('data', (chunk: string) => {
    stdout += chunk;
    progress += chunk;
    const lines = progress.split('\n');

    progress = lines.pop() ?? '';
    for (const line of lines) {
      const match = line.match(/^frame=(\d+)$/);

      if (match !== null) {
        frame = Number(match[1]);
      }
    }
  });
  child.stderr.on('data', (chunk: string) => {
    stderr += chunk;
  });
  child.once('error', (error) => {
    completed.reject(error);
  });
  child.once('close', (code, signal) => {
    completed.resolve({ code, signal, stderr, stdout });
  });
  return {
    frame: () => frame,
    stop: async () => {
      child.stdin.end('q\n');
      return await completed.promise;
    },
  };
};

const sendActiveWindowKey = async (
  windowId: string,
  key: string,
  environment: NodeJS.ProcessEnv
): Promise<void> => {
  const result = await runProcess(
    'xdotool',
    ['windowactivate', '--sync', windowId, 'key', '--clearmodifiers', key],
    environment
  );

  expect(result.code, result.stderr).toBe(0);
};

const monotonicMicroseconds = (): string =>
  (process.hrtime.bigint() / 1000n).toString();

const readTransitionResizes = async (
  path: string
): Promise<{
  readonly maximized: readonly PtyResize[];
  readonly restored: readonly PtyResize[];
}> => {
  const lines = (await readFile(path, 'utf8')).trimEnd().split('\n');
  const maximized: PtyResize[] = [];
  const restored: PtyResize[] = [];
  let sawMaximized = false;
  let sawRestored = false;

  expect(lines[0]).toBe(
    'sequence,monotonic_us,event,state,native_width,native_height,' +
      'window_width,window_height,content_width,content_height,' +
      'terminal_width,terminal_height,rows,columns'
  );
  for (const line of lines.slice(1)) {
    const fields = line.split(',');
    const event = fields[2];
    const state = fields[3];

    expect(fields).toHaveLength(14);
    if (event === 'state' && state === 'maximized') {
      sawMaximized = true;
    } else if (event === 'state' && state === 'normal' && sawMaximized) {
      sawRestored = true;
    } else if (event === 'pty-resize') {
      const resize = {
        columns: Number(fields[13]),
        rows: Number(fields[12]),
      };

      expect(Number.isInteger(resize.rows)).toBe(true);
      expect(Number.isInteger(resize.columns)).toBe(true);
      if (state === 'maximized' && sawMaximized && !sawRestored) {
        maximized.push(resize);
      } else if (state === 'normal' && sawRestored) {
        restored.push(resize);
      }
    }
  }
  return { maximized, restored };
};

describe('GTK 3 terminal sample under a real window manager', () => {
  it.runIf(configuration.artifactDirectory !== undefined)(
    'records the real CSD maximize and restore timeline at 60 fps',
    async () => {
      const artifactDirectory = configuration.artifactDirectory;

      if (artifactDirectory === undefined || tracePath === undefined) {
        throw new Error('real-WM artifact directory was not configured');
      }
      await mkdir(artifactDirectory, { recursive: true });
      const launcherEnvironment =
        (await launcher.environment()) as NodeJS.ProcessEnv;
      const previousWindows = new Set(
        await visibleSampleWindows(launcherEnvironment)
      );
      const app = await launcher.launch();
      const environment = (await app.environment()) as NodeJS.ProcessEnv;
      const display = environment.DISPLAY;
      let windowId = '';

      if (display === undefined) {
        throw new Error('real X11 window was not available');
      }
      await toPass(async () => {
        const windows = (await visibleSampleWindows(environment)).filter(
          (candidate) => !previousWindows.has(candidate)
        );

        expect(windows).toHaveLength(1);
        windowId = windows[0] ?? '';
      });
      await sendActiveWindowKey(windowId, 'g', environment);
      const monitors = await connectedMonitors(environment);
      const monitor = monitors.toSorted(
        (left, right) => right.width * right.height - left.width * left.height
      )[0];

      if (monitor === undefined) {
        throw new Error('xrandr did not report an active monitor');
      }
      const normalWindow = await readWindowBounds(windowId, environment);
      const centerX = normalWindow.x + normalWindow.width / 2;
      const centerY = normalWindow.y + normalWindow.height / 2;

      expect(centerX).toBeGreaterThanOrEqual(monitor.x);
      expect(centerX).toBeLessThan(monitor.x + monitor.width);
      expect(centerY).toBeGreaterThanOrEqual(monitor.y);
      expect(centerY).toBeLessThan(monitor.y + monitor.height);
      const captureWidth = Math.min(1600, monitor.width);
      const captureHeight = Math.min(1000, monitor.height);
      const captureBounds: GtkCaptureBounds = {
        height: captureHeight,
        width: captureWidth,
        x: Math.max(
          monitor.x,
          Math.min(
            Math.floor(centerX - captureWidth / 2),
            monitor.x + monitor.width - captureWidth
          )
        ),
        y: Math.max(
          monitor.y,
          Math.min(
            Math.floor(centerY - captureHeight / 2),
            monitor.y + monitor.height - captureHeight
          )
        ),
      };
      const videoPath = join(artifactDirectory, 'maximize-restore.mkv');
      const recorder = startVideoRecorder(
        display,
        captureBounds,
        videoPath,
        environment
      );
      const timeline: Array<Record<string, unknown>> = [
        {
          event: 'normal',
          monotonicUs: monotonicMicroseconds(),
          window: normalWindow,
        },
      ];

      try {
        await toPass(() => {
          expect(recorder.frame()).toBeGreaterThanOrEqual(6);
        });
        const maximizeFrame = recorder.frame();

        await sendActiveWindowKey(windowId, 'alt+F10', environment);
        await toPass(async () => {
          const bounds = await readWindowBounds(windowId, environment);

          expect(bounds.width).toBeGreaterThan(normalWindow.width);
          expect(bounds.height).toBeGreaterThan(normalWindow.height);
          expect(bounds.width).toBeGreaterThanOrEqual(monitor.width * 0.9);
        });
        const maximizedWindow = await readWindowBounds(windowId, environment);

        await sendActiveWindowKey(windowId, 'g', environment);
        await toPass(async () => {
          const title = await readWindowTitle(windowId, environment);

          expect(title).toMatch(/^grid-\d+-\d+$/);
          expect(title).not.toBe('grid-24-80');
        });
        await toPass(() => {
          expect(recorder.frame()).toBeGreaterThanOrEqual(maximizeFrame + 6);
        });
        timeline.push({
          event: 'maximized',
          frame: recorder.frame(),
          monotonicUs: monotonicMicroseconds(),
          window: maximizedWindow,
        });
        const restoreFrame = recorder.frame();

        await sendActiveWindowKey(windowId, 'alt+F10', environment);
        await toPass(async () => {
          const bounds = await readWindowBounds(windowId, environment);

          expect(bounds.width).toBe(normalWindow.width);
          expect(bounds.height).toBe(normalWindow.height);
        });
        await sendActiveWindowKey(windowId, 'g', environment);
        await toPass(async () => {
          expect(await readWindowTitle(windowId, environment)).toBe(
            'grid-24-80'
          );
        });
        await toPass(() => {
          expect(recorder.frame()).toBeGreaterThanOrEqual(restoreFrame + 6);
        });
        timeline.push({
          event: 'restored',
          frame: recorder.frame(),
          monotonicUs: monotonicMicroseconds(),
          window: await readWindowBounds(windowId, environment),
        });
        await sendActiveWindowKey(windowId, 'q', environment);
        await toPass(async () => {
          expect((await app.output()).exitCode).toBe(0);
        });
      } finally {
        const recording = await recorder.stop();

        expect(recording.code, recording.stderr).toBe(0);
      }
      const probe = await runProcess(
        'ffprobe',
        [
          '-v',
          'error',
          '-count_frames',
          '-select_streams',
          'v:0',
          '-show_entries',
          'stream=avg_frame_rate,nb_read_frames,width,height',
          '-of',
          'json',
          videoPath,
        ],
        environment
      );

      expect(probe.code, probe.stderr).toBe(0);
      const video = JSON.parse(probe.stdout) as {
        streams: Array<{
          avg_frame_rate: string;
          height: number;
          nb_read_frames: string;
          width: number;
        }>;
      };

      expect(video.streams).toHaveLength(1);
      expect(video.streams[0]?.avg_frame_rate).toBe('60/1');
      expect(Number(video.streams[0]?.nb_read_frames)).toBeGreaterThan(12);
      expect(video.streams[0]?.width).toBe(captureBounds.width);
      expect(video.streams[0]?.height).toBe(captureBounds.height);
      const transitionResizes = await readTransitionResizes(tracePath);

      expect(transitionResizes.maximized).toHaveLength(1);
      expect(transitionResizes.restored).toEqual([{ columns: 80, rows: 24 }]);
      await writeFile(
        join(artifactDirectory, 'timeline.json'),
        `${JSON.stringify(
          {
            capture: captureBounds,
            monitor,
            timeline,
            video: video.streams[0],
          },
          undefined,
          2
        )}\n`,
        'utf8'
      );
    },
    90_000
  );
});
