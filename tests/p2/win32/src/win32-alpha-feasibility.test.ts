import { execFileSync, spawn } from 'node:child_process';
import { mkdir, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

import { connectRemoteAgent } from './agent-connection.js';
import { delay } from 'async-primitives';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';

const modes = [
  'gdi-top',
  'gdi-child',
  'opengl-top-control',
  'opengl-top',
  'opengl-child',
  'opengl-nested',
  'd3d11-top',
  'd3d11-child',
  'gdi-window',
  'opengl-window',
  'd3d11-hwnd',
  'd3d11-window',
  'gdi-retained-window',
  'gdi-removed-window',
  'gdi-dwm-child',
  'd3d11-dwm-child',
  'd3d11-dwm-redirected-child',
];

interface State {
  ready: boolean;
  stage: string;
  result: string;
  window: string;
  x: number;
  y: number;
  width: number;
  height: number;
  backdrop: number;
  presents: number;
  remote: boolean;
  restored: boolean;
  restoreAttempted: boolean;
  layered: boolean;
  inputCount: number;
  mouseCount: number;
  keyCount: number;
}

const backgroundExpected = (backdrop: number): number[][] => {
  const color = backdrop === 0 ? [20, 80, 180] : [180, 40, 20];
  return [
    color,
    color.map((channel, i) => [96, 0, 16][i] + (channel * 127) / 255),
    [32, 208, 80],
    [255, 255, 255],
  ];
};

const expected = (
  backdrop: number,
  mode: string,
  restored: boolean
): number[][] => {
  if (!mode.endsWith('-window') && mode !== 'd3d11-hwnd')
    return backgroundExpected(backdrop);
  const pixels = [
    [0, 0, 0],
    [192, 0, 32],
    [32, 208, 80],
    [255, 255, 255],
  ];
  if (restored || mode === 'd3d11-hwnd') return pixels;
  const behind = backgroundExpected(backdrop)[0];
  return pixels.map((pixel) =>
    pixel.map((channel, index) => (channel * 128 + behind[index] * 127) / 255)
  );
};

const samples = (bytes: Buffer): number[][] => {
  const png = PNG.sync.read(bytes);
  expect([png.width, png.height]).toEqual([300, 180]);
  return [
    [50, 90],
    [150, 90],
    [250, 90],
    [150, 40],
  ].map(([x, y]) => {
    const offset = (y * png.width + x) * 4;
    return [...png.data.subarray(offset, offset + 3)];
  });
};

const matches = (
  actual: number[][],
  target: number[][],
  tolerance: number
): boolean =>
  actual.length === target.length &&
  actual.every(
    (pixel, i) =>
      pixel.length === 3 &&
      pixel.every((channel, j) => Math.abs(channel - target[i][j]) <= tolerance)
  );

// Readers never lock a filename that the native publisher needs to replace.
const readReport = async (
  entries: (directory: string) => Promise<readonly { name: string }[]>,
  read: (path: string) => Promise<Buffer>,
  path: string
): Promise<Buffer> => {
  const boundary = Math.max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
  const directory = boundary < 0 ? '.' : path.slice(0, boundary);
  const prefix = path.slice(boundary + 1) + '.';
  const latest = (await entries(directory))
    .map((entry) => entry.name)
    .filter(
      (name) =>
        name.startsWith(prefix) && /^\d{10}$/.test(name.slice(prefix.length))
    )
    .sort()
    .at(-1);
  if (!latest) throw new Error('No completed native observation.');
  return await read(directory + '/' + latest);
};

// The deadline bounds an observation; completion is driven by the report or pixels.
const waitState = async (
  read: () => Promise<Buffer>,
  accept: (state: State) => boolean
): Promise<State> => {
  const deadline = Date.now() + 30_000;
  let lastError: unknown;
  let lastState: State | undefined;
  while (Date.now() < deadline) {
    try {
      const state: State = JSON.parse((await read()).toString());
      lastState = state;
      if (!state.ready || accept(state)) return state;
    } catch (error) {
      lastError = error;
    }
    await delay(50);
  }
  throw new Error(
    `Native report did not complete: ${JSON.stringify(lastState)}; ${String(lastError)}`
  );
};

const observe = async (
  capture: () => Promise<Buffer>,
  target: number[][],
  path: string
): Promise<{ pixels: number[][]; match: boolean }> => {
  const deadline = Date.now() + 4000;
  let bytes: Buffer;
  let pixels: number[][];
  do {
    bytes = await capture();
    pixels = samples(bytes);
    if (matches(pixels, target, 2)) break;
    await delay(100);
  } while (Date.now() < deadline);
  await writeFile(path, bytes);
  return { pixels, match: matches(pixels, target, 2) };
};

it.each(['amd64', 'i686'])(
  'measures native alpha on physical Windows (%s)',
  async (architecture) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    if (!source) throw new Error('BOXRIG_P2_WIN32_SOURCE_DIR is required.');
    execFileSync('make', ['-f', 'tests/p2/win32_alpha_feasibility.mk'], {
      cwd: source,
    });
    const output = resolve(
      source,
      'build/test-results/step20-14-2/fallback-feasibility'
    );
    await mkdir(output, { recursive: true });
    using agent = await connectRemoteAgent({
      timeoutMs: 30_000,
    });
    const directory = await agent.files.mkdtemp('boxrig-alpha-feasibility-');
    const executable = directory + '/probe.exe';
    const results: Record<string, unknown>[] = [];
    try {
      await agent.files.writeFile(
        executable,
        await readFile(
          resolve(
            source,
            `build/tests/win32-alpha-feasibility/${architecture}.exe`
          )
        )
      );
      await agent.files.writeFile(
        directory + '/report-check.exe',
        await readFile(
          resolve(
            source,
            'build/win32',
            architecture,
            'tests/test_win32_alpha_report.exe'
          )
        )
      );
      const check = await agent.processes.launchManaged({
        path: directory + '/report-check.exe',
        workingDirectory: directory,
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      try {
        const ended = await check.waitForExit({ timeoutMs: 30000 });
        const log = (await check.stdoutText()) + (await check.stderrText());
        await writeFile(
          resolve(output, 'report-publication-' + architecture + '.log'),
          log
        );
        expect(ended.root.exitCode, log).toBe(0);
      } finally {
        if ((await check.snapshot()).running) await check.kill();
        await check.releaseAsync();
      }
      await writeFile(
        resolve(output, `desktop-${architecture}.json`),
        JSON.stringify(await agent.desktop(), null, 2)
      );
      for (const mode of modes) {
        const stem = resolve(output, `windows-${architecture}-${mode}`);
        // The recorder rejects an existing destination; these artifacts belong
        // to this probe and must be replaceable on a complete rerun.
        await rm(stem + '.mp4', { force: true });
        const report = directory + '/' + mode + '.json';
        const process = await agent.processes.launchManaged({
          path: executable,
          arguments: [mode, mode + '.json'],
          workingDirectory: directory,
          captureStdout: true,
          captureStderr: true,
          createNoWindow: true,
        });
        let state: State | undefined;
        try {
          state = await waitState(
            async () =>
              await readReport(
                agent.files.readdir,
                agent.files.readFile,
                report
              ),
            (candidate) => candidate.backdrop === 0
          );
          expect(state.remote).toBe(false);
          const result: Record<string, unknown> = { mode, state };
          results.push(result);
          if (state.ready) {
            const rect = {
              x: state.x,
              y: state.y,
              width: state.width,
              height: state.height,
            };
            const capture = async (): Promise<Buffer> =>
              (await agent.screenshot({ rect })).image;
            const control = async (value: string): Promise<void> => {
              const child = await agent.processes.launchManaged({
                path: executable,
                arguments: ['--control', state!.window, value],
                createNoWindow: true,
              });
              try {
                expect(
                  (await child.waitForExit({ timeoutMs: 30_000 })).root.exitCode
                ).toBe(0);
              } finally {
                await child.releaseAsync();
              }
            };
            result.initial = await observe(
              capture,
              expected(0, mode, false),
              stem + '-a.png'
            );
            await control('1');
            const changed = await waitState(
              async () =>
                await readReport(
                  agent.files.readdir,
                  agent.files.readFile,
                  report
                ),
              (candidate) => candidate.backdrop === 1
            );
            expect(changed.presents).toBe(state.presents);
            result.changed = await observe(
              capture,
              expected(1, mode, false),
              stem + '-b.png'
            );
            const supported =
              (result.initial as { match: boolean }).match &&
              (result.changed as { match: boolean }).match;
            result.supported = supported;
            if (supported && mode !== 'd3d11-hwnd') {
              const [recording, changes] = await Promise.allSettled([
                agent.recordVideo(4000, stem + '.mp4', { fps: 10, rect }),
                (async () => {
                  for (let i = 0; i < 6; i++) {
                    await control(String(i % 2));
                    await waitState(
                      async () =>
                        await readReport(
                          agent.files.readdir,
                          agent.files.readFile,
                          report
                        ),
                      (candidate) => candidate.backdrop === i % 2
                    );
                    // Hold each stimulus long enough to be represented in the recording.
                    await delay(350);
                  }
                })(),
              ]);
              if (recording.status === 'rejected') throw recording.reason;
              if (changes.status === 'rejected') throw changes.reason;
              const bytes = execFileSync(
                'ffmpeg',
                [
                  '-v',
                  'error',
                  '-i',
                  stem + '.mp4',
                  '-f',
                  'rawvideo',
                  '-pix_fmt',
                  'rgb24',
                  '-fps_mode',
                  'passthrough',
                  'pipe:1',
                ],
                { maxBuffer: 32 * 1024 * 1024 }
              );
              const stride = 300 * 180 * 3;
              const counts = [0, 0];
              expect(bytes.length).toBe(recording.value.frameCount * stride);
              for (let offset = 0; offset < bytes.length; offset += stride) {
                const pixels = [
                  [50, 90],
                  [150, 90],
                  [250, 90],
                  [150, 40],
                ].map(([x, y]) => [
                  ...bytes.subarray(
                    offset + (y * 300 + x) * 3,
                    offset + (y * 300 + x) * 3 + 3
                  ),
                ]);
                const a = matches(pixels, expected(0, mode, false), 7),
                  b = matches(pixels, expected(1, mode, false), 7);
                expect(a || b).toBe(true);
                if (a) counts[0]++;
                if (b) counts[1]++;
              }
              expect(counts[0]).toBeGreaterThan(0);
              expect(counts[1]).toBeGreaterThan(0);
              result.video = {
                frames: recording.value.frameCount,
                backdropFrames: counts,
              };
            }
            if (mode.endsWith('-window')) {
              if (supported) {
                await agent.mouse.click({ x: rect.x + 250, y: rect.y + 90 });
                // SendInput completion does not imply that WM_LBUTTONDOWN has
                // established keyboard focus in the target application.
                await waitState(
                  async () =>
                    await readReport(
                      agent.files.readdir,
                      agent.files.readFile,
                      report
                    ),
                  (candidate) => candidate.mouseCount >= 1
                );
                await agent.keyboard.press('a');
                result.inputBeforeRestore = await waitState(
                  async () =>
                    await readReport(
                      agent.files.readdir,
                      agent.files.readFile,
                      report
                    ),
                  (candidate) => candidate.keyCount >= 1
                );
              }
              await control('r');
              const restored = await waitState(
                async () =>
                  await readReport(
                    agent.files.readdir,
                    agent.files.readFile,
                    report
                  ),
                (candidate) => candidate.restoreAttempted
              );
              expect(restored.restored).toBe(true);
              expect(restored.layered).toBe(false);
              result.restored = await observe(
                capture,
                expected(1, mode, true),
                stem + '-restored.png'
              );
              if (supported) {
                expect((result.restored as { match: boolean }).match).toBe(
                  true
                );
                await agent.mouse.click({ x: rect.x + 250, y: rect.y + 90 });
                await waitState(
                  async () =>
                    await readReport(
                      agent.files.readdir,
                      agent.files.readFile,
                      report
                    ),
                  (candidate) => candidate.mouseCount >= 2
                );
                await agent.keyboard.press('a');
                result.inputAfterRestore = await waitState(
                  async () =>
                    await readReport(
                      agent.files.readdir,
                      agent.files.readFile,
                      report
                    ),
                  (candidate) => candidate.keyCount >= 2
                );
              }
            }
            await control('q');
          }
          const exit = await process.waitForExit({ timeoutMs: 30_000 });
          result.exitCode = exit.root.exitCode;
          expect(exit.root.exitCode).toBe(state.ready ? 0 : 2);
          console.log(`${architecture} ${mode}: ${JSON.stringify(result)}`);
        } finally {
          if ((await process.snapshot()).running) await process.kill();
          await writeFile(stem + '.log', await process.stdoutText());
          await process.releaseAsync();
          await writeFile(
            resolve(output, `windows-${architecture}.json`),
            JSON.stringify(results, null, 2)
          );
        }
      }
      // The child variants are measurements for the implementation plan. The
      // top-level cases establish feasibility independently of boxrig's topology.
      for (const mode of [
        'gdi-top',
        'opengl-top',
        'd3d11-top',
        'gdi-window',
        'd3d11-hwnd',
      ])
        expect(results.find((result) => result.mode === mode)?.supported).toBe(
          true
        );
      expect(
        results.find((result) => result.mode === 'opengl-top-control')
          ?.supported
      ).toBe(false);
    } finally {
      await agent.files.remove(directory, { recursive: true });
    }
  }
);

it.each(['amd64', 'i686'])(
  'measures the same native alpha paths under Wine (%s)',
  async (architecture) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    if (!source || !process.env.DISPLAY)
      throw new Error('A composited X11 desktop is required.');
    const output = resolve(
      source,
      'build/test-results/step20-14-2/fallback-feasibility'
    );
    await mkdir(output, { recursive: true });
    const executable = resolve(
      source,
      `build/tests/win32-alpha-feasibility/${architecture}.exe`
    );
    const environment = {
      ...process.env,
      WINEDEBUG: '-all',
      WINEDLLOVERRIDES: 'mscoree,mshtml=',
      WINEPREFIX: resolve(source, 'build/wine-alpha-feasibility'),
    };
    const results: Record<string, unknown>[] = [];
    for (const mode of modes) {
      const stem = resolve(output, `wine-${architecture}-${mode}`);
      const report = stem + '-state.json';
      for (const name of await readdir(output)) {
        if (name.startsWith(report.slice(output.length + 1)))
          await rm(resolve(output, name), { force: true });
      }
      const child = spawn('wine', [executable, mode, 'Z:' + report], {
        env: environment,
      });
      let log = '';
      child.stdout.on('data', (data: Buffer) => {
        log += data.toString();
      });
      child.stderr.on('data', (data: Buffer) => {
        log += data.toString();
      });
      let exited = false;
      const completion = new Promise<number | null>((resolveExit, reject) => {
        child.on('error', reject);
        child.on('exit', (code) => {
          exited = true;
          resolveExit(code);
        });
      });
      try {
        const state = await waitState(
          async () =>
            await readReport(
              async (directory) =>
                await readdir(directory, { withFileTypes: true }),
              readFile,
              report
            ),
          (candidate) => candidate.backdrop === 0
        );
        const result: Record<string, unknown> = { mode, state };
        results.push(result);
        if (state.ready) {
          // Wine's primary-monitor origin can differ from the X11 root origin.
          // Locate our actual native window instead of treating Win32 coordinates
          // as X11 coordinates; independently verify the backdrop is in view.
          const geometry = execFileSync('xwininfo', ['-name', mode], {
            encoding: 'utf8',
          });
          const x = Number(
            /Absolute upper-left X:\s+(-?\d+)/.exec(geometry)?.[1]
          );
          const y = Number(
            /Absolute upper-left Y:\s+(-?\d+)/.exec(geometry)?.[1]
          );
          expect(Number.isFinite(x) && Number.isFinite(y)).toBe(true);
          expect(Number(/Width:\s+(\d+)/.exec(geometry)?.[1])).toBe(
            state.width
          );
          expect(Number(/Height:\s+(\d+)/.exec(geometry)?.[1])).toBe(
            state.height
          );
          result.captureOrigin = { x, y };
          const captureRoot = (crop: string): Buffer =>
            execFileSync(
              'import',
              ['-window', 'root', '-crop', crop, 'png:-'],
              {
                maxBuffer: 4 * 1024 * 1024,
              }
            );
          const verifyBackdrop = async (index: number): Promise<number[]> => {
            const deadline = Date.now() + 4000;
            let pixel: number[];
            do {
              const png = PNG.sync.read(captureRoot(`1x1+${x - 20}+${y - 20}`));
              expect([png.width, png.height]).toEqual([1, 1]);
              pixel = [...png.data.subarray(0, 3)];
              if (matches([pixel], [backgroundExpected(index)[0]], 2))
                return pixel;
              await delay(100);
            } while (Date.now() < deadline);
            throw new Error(`Backdrop ${index} is not visible: ${pixel}`);
          };
          const capture = async (): Promise<Buffer> =>
            captureRoot(`${state.width}x${state.height}+${x}+${y}`);
          result.initialBackdrop = await verifyBackdrop(0);
          result.initial = await observe(
            capture,
            expected(0, mode, false),
            stem + '-a.png'
          );
          execFileSync('wine', [executable, '--control', state.window, '1'], {
            env: environment,
          });
          const changed = await waitState(
            async () =>
              await readReport(
                async (directory) =>
                  await readdir(directory, { withFileTypes: true }),
                readFile,
                report
              ),
            (candidate) => candidate.backdrop === 1
          );
          expect(changed.presents).toBe(state.presents);
          result.changedBackdrop = await verifyBackdrop(1);
          result.changed = await observe(
            capture,
            expected(1, mode, false),
            stem + '-b.png'
          );
          result.supported =
            (result.initial as { match: boolean }).match &&
            (result.changed as { match: boolean }).match;
          if (mode === 'opengl-top-control') {
            // Establish that our foreground is visible even if native alpha is
            // unsupported. A fully occluded probe is not evidence of that limit.
            for (const observation of [result.initial, result.changed])
              expect(
                matches(
                  (observation as { pixels: number[][] }).pixels.slice(2),
                  expected(0, mode, false).slice(2),
                  2
                )
              ).toBe(true);
          }
          if (mode.endsWith('-window')) {
            execFileSync('wine', [executable, '--control', state.window, 'r'], {
              env: environment,
            });
            result.restoredState = await waitState(
              async () =>
                await readReport(
                  async (directory) =>
                    await readdir(directory, { withFileTypes: true }),
                  readFile,
                  report
                ),
              (candidate) => candidate.restoreAttempted
            );
            result.restored = await observe(
              capture,
              expected(1, mode, true),
              stem + '-restored.png'
            );
          }
          execFileSync('wine', [executable, '--control', state.window, 'q'], {
            env: environment,
          });
        }
        result.exitCode = await completion;
        expect(result.exitCode).toBe(state.ready ? 0 : 2);
        console.log(`Wine ${architecture} ${mode}: ${JSON.stringify(result)}`);
      } finally {
        if (!exited) child.kill();
        await writeFile(stem + '.log', log);
        await writeFile(
          resolve(output, `wine-${architecture}.json`),
          JSON.stringify(results, null, 2)
        );
      }
    }
  },
  360_000
);
