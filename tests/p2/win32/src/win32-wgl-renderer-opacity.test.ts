import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

import { type AppWindow, type RemoteAgent, type ScreenRect } from 'agent-rover';
import { connectRemoteAgent } from './agent-connection.js';
import { delay } from 'async-primitives';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';

const capture = async (
  agent: RemoteAgent,
  rect: ScreenRect
): Promise<Buffer> => {
  const value = await agent.screenshot({ rect });
  expect(value.clipped).toBe(false);
  return value.image;
};

const difference = (left: Buffer, right: Buffer): number => {
  const a = PNG.sync.read(left),
    b = PNG.sync.read(right);
  if (a.width !== b.width || a.height !== b.height) return 255;
  let maximum = 0;
  for (let index = 0; index < a.data.length; index++) {
    if (index % 4 !== 3)
      maximum = Math.max(maximum, Math.abs(a.data[index] - b.data[index]));
  }
  return maximum;
};

const stableCapture = async (
  agent: RemoteAgent,
  rect: ScreenRect
): Promise<Buffer> => {
  const deadline = Date.now() + 15000;
  let previous = await capture(agent, rect);
  while (Date.now() < deadline) {
    await delay(80);
    const next = await capture(agent, rect);
    if (difference(previous, next) === 0) return next;
    previous = next;
  }
  throw new Error(
    'The opacity sample did not settle to a complete static frame.'
  );
};

// Minimize returns before desktop composition necessarily changes. Require the
// requested visual transition before considering a repeated screenshot stable.
const minimizeBackdrop = async (
  agent: RemoteAgent,
  window: AppWindow,
  rect: ScreenRect,
  foreground: Buffer
): Promise<Buffer> => {
  await window.minimize();
  const deadline = Date.now() + 15000;
  let observed = await window.refresh();
  let previous = await capture(agent, rect);
  while (Date.now() < deadline) {
    await delay(80);
    observed = await window.refresh();
    const next = await capture(agent, rect);
    if (
      observed.minimized &&
      difference(foreground, next) > 30 &&
      difference(previous, next) === 0
    )
      return next;
    previous = next;
  }
  throw new Error('Minimization did not expose a stable backdrop.');
};

const activate = async (
  agent: RemoteAgent,
  window: AppWindow
): Promise<AppWindow> => {
  let observed = await window.refresh();
  if (!observed.active) {
    // Real caption input grants activation even when SetForegroundWindow is
    // denied. Stay away from the previous cursor location so restoring a window
    // cannot turn two activation clicks into a maximize double-click.
    const cursor = await agent.cursor();
    const left = observed.frameBounds.x + 80;
    const right = observed.frameBounds.x + observed.frameBounds.width - 180;
    expect(right - left).toBeGreaterThan(32);
    await agent.mouse.click({
      x:
        Math.abs(cursor.point.x - left) > Math.abs(cursor.point.x - right)
          ? left
          : right,
      y: observed.frameBounds.y + 12,
    });
    observed = await window.refresh();
  }
  const deadline = Date.now() + 15000;
  while (!observed.active && Date.now() < deadline) {
    await delay(50);
    observed = await window.refresh();
  }
  expect(observed.active).toBe(true);
  expect(observed.maximized).toBe(false);
  return observed;
};

const composite = (foreground: Buffer, background: Buffer): Buffer => {
  const a = PNG.sync.read(foreground),
    b = PNG.sync.read(background);
  expect([a.width, a.height]).toEqual([b.width, b.height]);
  for (let index = 0; index < a.data.length; index++) {
    if (index % 4 !== 3)
      a.data[index] = Math.round(
        (a.data[index] * 128 + b.data[index] * 127) / 255
      );
  }
  return PNG.sync.write(a);
};

const observe = async (
  agent: RemoteAgent,
  rect: ScreenRect,
  expected: Buffer,
  failurePath: string
): Promise<Buffer> => {
  const deadline = Date.now() + 15000;
  let actual = await capture(agent, rect);
  while (difference(actual, expected) > 2 && Date.now() < deadline) {
    await delay(80);
    actual = await capture(agent, rect);
  }
  if (difference(actual, expected) > 2) {
    await writeFile(failurePath, actual);
    await writeFile(failurePath + '-expected.png', expected);
  }
  expect(difference(actual, expected)).toBeLessThanOrEqual(2);
  return actual;
};

const requireInk = (bytes: Buffer): void => {
  const image = PNG.sync.read(bytes);
  let ink = 0,
    background = 0;
  for (let offset = 0; offset < image.data.length; offset += 4) {
    if (
      image.data[offset] > 160 &&
      image.data[offset + 1] > 160 &&
      image.data[offset + 2] > 160
    )
      ink++;
    if (
      image.data[offset] + image.data[offset + 1] + image.data[offset + 2] <
      384
    )
      background++;
  }
  expect(ink).toBeGreaterThan(40);
  expect(background).toBeGreaterThan(100);
};

// Exercise the installed renderer and the application-owned native presenter.
it.each(['amd64', 'i686'])(
  'keeps WGL glyphs opaque while the prepared host composites the background (%s)',
  async (architecture) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    if (!source) throw new Error('BOXRIG_P2_WIN32_SOURCE_DIR is required.');
    using agent = await connectRemoteAgent({
      timeoutMs: 30000,
    });
    const directory = await agent.files.mkdtemp('boxrig-wgl-opacity-');
    const output = resolve(
      source,
      'build/test-results/step20-14-2/wgl-renderer-opacity',
      architecture
    );
    let opaque: Buffer | undefined;
    let backdrop: Buffer | undefined;
    try {
      await mkdir(output, { recursive: true });
      await agent.files.writeFile(
        directory + '/renderer.exe',
        await readFile(
          resolve(
            source,
            'build/tests/win32-sdk-' + architecture,
            'ogl/renderer-static.exe'
          )
        )
      );
      for (const mode of ['opaque', 'background', 'window']) {
        const child = await agent.processes.launchManaged({
          path: directory + '/renderer.exe',
          arguments: ['--' + mode],
          workingDirectory: directory,
          captureStdout: true,
          captureStderr: true,
          createNoWindow: true,
        });
        try {
          let window = await child.waitForWindow(
            { className: 'boxrig_renderer_example', visible: true },
            { timeoutMs: 30000 }
          );
          window = await window.setBounds({ ...window.bounds, x: 80, y: 80 });
          window = await activate(agent, window);
          const rect = {
            x: window.clientBounds.x + 4,
            y: window.clientBounds.y + 2,
            width: 300,
            height: 12,
          };
          if (mode === 'opaque') {
            opaque = await stableCapture(agent, rect);
            requireInk(opaque);
            await writeFile(resolve(output, 'opaque.png'), opaque);
            backdrop = await minimizeBackdrop(agent, window, rect, opaque);
            expect(difference(opaque, backdrop)).toBeGreaterThan(30);
            await writeFile(resolve(output, 'backdrop.png'), backdrop);
          } else {
            expect(opaque).toBeDefined();
            expect(backdrop).toBeDefined();
            if (!opaque || !backdrop)
              throw new Error('Opaque reference frame is required.');
            if (mode === 'window') {
              const half = await observe(
                agent,
                rect,
                composite(opaque, backdrop),
                resolve(output, 'whole-failure.png')
              );
              await writeFile(resolve(output, 'whole.png'), half);
            } else {
              const reference = PNG.sync.read(opaque),
                behind = PNG.sync.read(backdrop);
              // The example uses two large background fields from the active
              // terminal palette. They are not necessarily pure black or blue.
              const colors = new Map<string, number>();
              let glyphLevel = 0;
              for (let index = 0; index < reference.data.length; index += 4) {
                const key = reference.data.subarray(index, index + 3).join(',');
                colors.set(key, (colors.get(key) ?? 0) + 1);
                if (
                  reference.data[index] === reference.data[index + 1] &&
                  reference.data[index] === reference.data[index + 2]
                )
                  glyphLevel = Math.max(glyphLevel, reference.data[index]);
              }
              const backgroundColors = [...colors]
                .sort((left, right) => right[1] - left[1])
                .slice(0, 2);
              expect(backgroundColors.every((entry) => entry[1] > 100)).toBe(
                true
              );
              expect(glyphLevel).toBeGreaterThan(180);
              const deadline = Date.now() + 15000;
              let image: Buffer;
              let background = 0,
                glyph = 0,
                failures = 0;
              do {
                image = await capture(agent, rect);
                const actual = PNG.sync.read(image);
                background = glyph = failures = 0;
                for (let index = 0; index < reference.data.length; index += 4) {
                  const red = reference.data[index],
                    green = reference.data[index + 1],
                    blue = reference.data[index + 2];
                  const isBackground = backgroundColors.some(
                    (entry) => entry[0] === [red, green, blue].join(',')
                  );
                  const isGlyph =
                    red === glyphLevel && red === green && green === blue;
                  if (!isBackground && !isGlyph) continue;
                  if (isBackground) background++;
                  else glyph++;
                  for (let channel = 0; channel < 3; channel++) {
                    const wanted = isGlyph
                      ? reference.data[index + channel]
                      : Math.round(
                          (reference.data[index + channel] * 128 +
                            behind.data[index + channel] * 127) /
                            255
                        );
                    if (Math.abs(actual.data[index + channel] - wanted) > 2)
                      failures++;
                  }
                }
                if (failures === 0) break;
                await delay(80);
              } while (Date.now() < deadline);
              await writeFile(resolve(output, 'background.png'), image!);
              expect(background).toBeGreaterThan(100);
              expect(glyph).toBeGreaterThan(20);
              expect(failures).toBe(0);
            }
          }
          await window.close();
          const ended = await child.waitForExit({ timeoutMs: 30000 });
          const log = (await child.stdoutText()) + (await child.stderrText());
          await writeFile(resolve(output, mode + '.log'), log);
          expect(ended.root.exitCode, log).toBe(0);
          expect(log).toContain(
            'background=' + (mode === 'background' ? '128' : '255')
          );
          expect(log).toContain(
            'window=' + (mode === 'window' ? '128' : '255')
          );
        } finally {
          await writeFile(
            resolve(output, mode + '.log'),
            (await child.stdoutText()) + (await child.stderrText())
          );
          await writeFile(
            resolve(output, mode + '-process.json'),
            JSON.stringify(await child.snapshot(), null, 2)
          );
          if ((await child.snapshot()).running) await child.kill();
          await writeFile(
            resolve(output, mode + '-stopped.log'),
            (await child.stdoutText()) + (await child.stderrText())
          );
          await child.releaseAsync();
        }
      }
    } finally {
      await agent.files.remove(directory, { recursive: true });
    }
  }
);
