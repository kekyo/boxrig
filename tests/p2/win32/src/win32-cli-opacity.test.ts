import { execFileSync } from 'node:child_process';
import { mkdir, readFile, rm, writeFile } from 'node:fs/promises';
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

const observeBackground = async (
  agent: RemoteAgent,
  rect: ScreenRect,
  opaque: Buffer,
  behind: Buffer,
  failure: string
): Promise<Buffer> => {
  const foreground = PNG.sync.read(opaque),
    backdrop = PNG.sync.read(behind);
  let solid = 0;
  const colors = new Map<string, number>();
  for (let at = 0; at < foreground.data.length; at += 4) {
    const color = foreground.data.subarray(at, at + 3).join(',');
    colors.set(color, (colors.get(color) ?? 0) + 1);
    if (
      foreground.data[at] === foreground.data[at + 1] &&
      foreground.data[at] === foreground.data[at + 2]
    )
      solid = Math.max(solid, foreground.data[at]);
  }
  expect(solid).toBeGreaterThan(160);
  const backgroundColor = [...colors].sort((a, b) => b[1] - a[1])[0];
  expect(backgroundColor[1]).toBeGreaterThan(100);
  const deadline = Date.now() + 15000;
  let image: Buffer,
    differences = 0,
    glyphs = 0,
    background = 0;
  do {
    image = await capture(agent, rect);
    const actual = PNG.sync.read(image);
    differences = glyphs = background = 0;
    for (let at = 0; at < foreground.data.length; at += 4) {
      const glyph =
        foreground.data[at] === solid &&
        foreground.data[at + 1] === solid &&
        foreground.data[at + 2] === solid;
      const blank =
        foreground.data.subarray(at, at + 3).join(',') === backgroundColor[0];
      if (!glyph && !blank) continue;
      if (glyph) glyphs++;
      else background++;
      for (let channel = 0; channel < 3; channel++) {
        const wanted = glyph
          ? solid
          : Math.round(
              (foreground.data[at + channel] * 128 +
                backdrop.data[at + channel] * 127) /
                255
            );
        if (Math.abs(actual.data[at + channel] - wanted) > 2) differences++;
      }
    }
    if (differences === 0) break;
    await delay(80);
  } while (Date.now() < deadline);
  await writeFile(failure, image!);
  expect(glyphs).toBeGreaterThan(20);
  expect(background).toBeGreaterThan(100);
  expect(differences).toBe(0);
  return image!;
};

it.each(
  ['amd64', 'i686'].flatMap((architecture) => [
    ...['win32', 'opengl', 'd3d11'].flatMap((renderer) =>
      ['background', 'window', 'opaque'].flatMap((mode) =>
        (mode === 'background' ? [0, 50, 100] : [100]).map((startup) => ({
          architecture,
          renderer,
          mode,
          startup,
          modeExchange: false,
          missingComposition: false,
          fallback: false,
        }))
      )
    ),
    {
      architecture,
      renderer: 'd3d11',
      mode: 'background',
      startup: 100,
      modeExchange: true,
      missingComposition: false,
      fallback: false,
    },
    ...[false, true].flatMap((fallback) =>
      (fallback ? [0, 100] : [100]).map((startup) => ({
        architecture,
        renderer: 'd3d11',
        mode: 'background',
        startup,
        modeExchange: false,
        missingComposition: true,
        fallback,
      }))
    ),
  ])
)(
  'composites $renderer CLI $mode opacity starting at $startup% mode exchange=$modeExchange with missing composition=$missingComposition fallback=$fallback ($architecture)',
  async ({
    architecture,
    renderer,
    mode,
    startup,
    missingComposition,
    modeExchange,
    fallback,
  }) => {
    const effective = missingComposition
      ? fallback
        ? 'window'
        : 'opaque'
      : mode;
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    if (!source) throw new Error('BOXRIG_P2_WIN32_SOURCE_DIR is required.');
    using agent = await connectRemoteAgent({
      timeoutMs: 30000,
    });
    const directory = await agent.files.mkdtemp('boxrig-cli-opacity-');
    const output = resolve(
      source,
      'build/test-results/step20-14-2/gpu-cli-opacity',
      renderer,
      architecture,
      mode +
        '-' +
        startup +
        (modeExchange ? '-exchange' : '') +
        (missingComposition ? '-missing-' + fallback : '')
    );
    try {
      // Each case owns these generated artifacts. A previous recording must
      // not prevent this run from observing a fresh mode exchange.
      await rm(output, { recursive: true, force: true });
      await mkdir(output, { recursive: true });
      await agent.files.syncDirectory({
        localPath: resolve(source, 'build/boxrig_term/win32', architecture),
        remotePath: directory,
        checksum: 'sha256',
      });
      await agent.files.writeFile(
        directory + '/pty_helper.exe',
        await readFile(
          resolve(source, 'build/win32', architecture, 'tests/pty_helper.exe')
        )
      );
      const process = await agent.processes.launchManaged({
        path:
          directory +
          (modeExchange
            ? '/boxrig_term_opacity_probe.exe'
            : missingComposition
              ? '/boxrig_term_no_dcomp.exe'
              : '/boxrig_term.exe'),
        // The child resolves its path from the working directory below.
        arguments: [`--renderer=${renderer}`, '--', '.\\pty_helper.exe'],
        workingDirectory: directory,
        environment: {
          BOXRIG_TRANSPARENCY_MODE: mode,
          BOXRIG_TRANSPARENCY_FALLBACK: fallback ? 'window' : 'opaque',
          BOXRIG_WINDOW_OPACITY: '100',
          BOXRIG_BACKGROUND_OPACITY:
            mode === 'background' ? String(startup) : '0',
        },
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      try {
        const id = (await process.snapshot()).root.id;
        let window: AppWindow | undefined;
        const deadline = Date.now() + 30000;
        while (Date.now() < deadline) {
          const values = await agent.findWindows({
            processId: id,
            className: 'boxrig_win32_sample',
            title: 'helper-ready',
            visible: true,
          });
          if (values.length === 1) {
            window = values[0];
            break;
          }
          await delay(50);
        }
        if (!window)
          throw new Error(
            'The actual Win32 terminal did not reach helper-ready.'
          );
        window = await window.setBounds({ ...window.bounds, x: 80, y: 80 });
        window = await activate(agent, window);
        const helper = (await process.processes()).find(
          (value) => value.name.toLowerCase() === 'pty_helper.exe'
        );
        expect(helper).toBeDefined();
        // Establish real history and two static text rows before recording the
        // enabled control. READY alone has too few fully covered glyph pixels.
        await agent.keyboard.press('h');
        await process.waitForWindow(
          {
            className: 'boxrig_win32_sample',
            title: 'history-ready',
            visible: true,
          },
          { timeoutMs: 30000 }
        );
        await agent.keyboard.press('x');
        window = await process.waitForWindow(
          {
            className: 'boxrig_win32_sample',
            title: 'host-input-ready',
            visible: true,
          },
          { timeoutMs: 30000 }
        );
        // Keep the control's hover state identical before and after restore.
        await agent.mouse.move({
          x: window.frameBounds.x + 80,
          y: window.frameBounds.y + 12,
        });
        const rect = {
          x: window.clientBounds.x + 4,
          y: window.clientBounds.y + 2,
          width: 350,
          height: Math.floor(window.clientBounds.height / 24) * 2 - 4,
        };
        // Capture the same complete foreground and backdrop, then verify the
        // whole-window operation rather than accepting an alpha API return code.
        // Compare the visible control region regardless of whether the host
        // uses a native child or GPU composition for its scrollbar.
        const scrollbarRect = {
          x: window.clientBounds.x + window.clientBounds.width - 10,
          y: window.clientBounds.y + 2,
          width: 8,
          height: window.clientBounds.height - 4,
        };
        // Record the actual startup before requesting an opaque reference.
        // Input has already reached the same child even with a zero-alpha background.
        const startupImage =
          startup === 100 ? undefined : await stableCapture(agent, rect);
        const startupScrollbar =
          startup === 100
            ? undefined
            : await stableCapture(agent, scrollbarRect);
        if (startupImage !== undefined) {
          await agent.keyboard.press('o', { modifiers: ['Control', 'Shift'] });
          if (!missingComposition) {
            const deadline = Date.now() + 15000;
            let opaqueReady = await capture(agent, rect);
            while (
              difference(startupImage, opaqueReady) < 30 &&
              Date.now() < deadline
            ) {
              await delay(80);
              opaqueReady = await capture(agent, rect);
            }
            expect(
              difference(startupImage, opaqueReady)
            ).toBeGreaterThanOrEqual(30);
          }
        }
        const opaqueScrollbar = await stableCapture(agent, scrollbarRect);
        const opaque = await stableCapture(agent, rect);
        await writeFile(resolve(output, 'opaque.png'), opaque);
        await writeFile(
          resolve(output, 'scrollbar-opaque.png'),
          opaqueScrollbar
        );
        requireInk(opaque);
        const behind = await minimizeBackdrop(agent, window, rect, opaque);
        const behindScrollbar = await stableCapture(agent, scrollbarRect);
        // A transparent hole must not become the reference for an opaque control.
        expect(difference(opaqueScrollbar, behindScrollbar)).toBeGreaterThan(
          30
        );
        expect(difference(opaque, behind)).toBeGreaterThan(30);
        await writeFile(resolve(output, 'backdrop.png'), behind);
        await writeFile(
          resolve(output, 'scrollbar-backdrop.png'),
          behindScrollbar
        );
        if (startupImage !== undefined && startupScrollbar !== undefined) {
          await writeFile(resolve(output, 'startup.png'), startupImage);
          const actual = PNG.sync.read(startupImage);
          const foreground = PNG.sync.read(opaque);
          const backdrop = PNG.sync.read(behind);
          const colors = new Map<string, number>();
          let solid = 0;
          for (let at = 0; at < foreground.data.length; at += 4) {
            const color = foreground.data.subarray(at, at + 3).join(',');
            colors.set(color, (colors.get(color) ?? 0) + 1);
            if (
              foreground.data[at] === foreground.data[at + 1] &&
              foreground.data[at] === foreground.data[at + 2]
            )
              solid = Math.max(solid, foreground.data[at]);
          }
          const blank = [...colors].sort((a, b) => b[1] - a[1])[0][0];
          const alpha = missingComposition
            ? 255
            : Math.round((startup * 255) / 100);
          let glyphs = 0,
            blanks = 0,
            errors = 0;
          for (let at = 0; at < foreground.data.length; at += 4) {
            const glyph =
              foreground.data[at] === solid &&
              foreground.data[at + 1] === solid &&
              foreground.data[at + 2] === solid;
            const background =
              foreground.data.subarray(at, at + 3).join(',') === blank;
            if (!glyph && !background) continue;
            if (glyph) glyphs++;
            else blanks++;
            for (let channel = 0; channel < 3; channel++) {
              const expected = glyph
                ? solid
                : Math.round(
                    (foreground.data[at + channel] * alpha +
                      backdrop.data[at + channel] * (255 - alpha)) /
                      255
                  );
              if (Math.abs(actual.data[at + channel] - expected) > 2) errors++;
            }
          }
          expect(glyphs).toBeGreaterThan(20);
          expect(blanks).toBeGreaterThan(100);
          expect(errors).toBe(0);
          expect(
            difference(startupScrollbar, opaqueScrollbar)
          ).toBeLessThanOrEqual(2);
        }
        await window.restore();
        await activate(agent, window);
        await observe(
          agent,
          rect,
          opaque,
          resolve(output, 'initial-restore-failure.png')
        );
        for (let step = 0; step < 5; step++)
          await agent.keyboard.press(',', { modifiers: ['Control', 'Shift'] });
        const half =
          effective === 'background'
            ? await observeBackground(
                agent,
                rect,
                opaque,
                behind,
                resolve(output, 'background-50.png')
              )
            : await observe(
                agent,
                rect,
                effective === 'window' ? composite(opaque, behind) : opaque,
                resolve(output, 'whole-50-failure.png')
              );
        await writeFile(
          resolve(output, 'scrollbar-half.png'),
          await observe(
            agent,
            scrollbarRect,
            effective === 'window'
              ? composite(opaqueScrollbar, behindScrollbar)
              : opaqueScrollbar,
            resolve(output, 'scrollbar-half-failure.png')
          )
        );
        // The same child redraws its fixed text after reallocation, which may
        // reveal older scrollback. Alpha must remain unchanged across resize.
        window = await window.setBounds({
          ...window.bounds,
          width: window.bounds.width + 112,
          height: window.bounds.height + 56,
        });
        await agent.keyboard.press('x');
        window = await process.waitForWindow(
          {
            className: 'boxrig_win32_sample',
            title: 'host-input-ready',
            visible: true,
          },
          { timeoutMs: 30000 }
        );
        if (effective === 'background')
          await observeBackground(
            agent,
            rect,
            opaque,
            behind,
            resolve(output, 'background-resized.png')
          );
        else
          await observe(
            agent,
            rect,
            effective === 'window' ? composite(opaque, behind) : opaque,
            resolve(output, 'whole-resized.png')
          );
        await writeFile(resolve(output, 'whole-50.png'), half);
        await agent.keyboard.press('o', { modifiers: ['Control', 'Shift'] });
        await writeFile(
          resolve(output, 'restored.png'),
          await observe(
            agent,
            rect,
            opaque,
            resolve(output, 'restore-failure.png')
          )
        );
        if (modeExchange) {
          const videoPath = resolve(output, 'mode-exchange.mp4');
          const exchange = async (): Promise<void> => {
            for (let cycle = 0; cycle < 3; cycle++) {
              await agent.keyboard.press('F9');
              await observe(
                agent,
                rect,
                composite(opaque, behind),
                resolve(output, 'window-exchange-failure.png')
              );
              if (cycle === 0) {
                await agent.keyboard.press('F11');
                const deadline = Date.now() + 15000;
                while (
                  !(await process.stdoutText()).includes(
                    'PROBE rejected candidate frame'
                  ) &&
                  Date.now() < deadline
                )
                  await delay(80);
                expect(await process.stdoutText()).toContain(
                  'PROBE rejected candidate frame after owner preparation'
                );
                await observe(
                  agent,
                  rect,
                  composite(opaque, behind),
                  resolve(output, 'window-rollback-failure.png')
                );
              }
              await agent.keyboard.press('F8');
              await observeBackground(
                agent,
                rect,
                opaque,
                behind,
                resolve(output, 'background-exchange.png')
              );
              await agent.keyboard.press('F10');
              await observe(
                agent,
                rect,
                opaque,
                resolve(output, 'disabled-exchange-failure.png')
              );
            }
          };
          // Encode an ordinary video extent and inspect the same semantic text
          // region after decoding. The 26-pixel-high strip is not an encoder input.
          const videoRect = { ...rect, width: 640, height: 360 };
          expect(window.clientBounds.width).toBeGreaterThanOrEqual(644);
          expect(window.clientBounds.height).toBeGreaterThanOrEqual(362);
          const [recorded, changed] = await Promise.allSettled([
            agent.recordVideo(30000, videoPath, { fps: 30, rect: videoRect }),
            exchange(),
          ]);
          if (changed.status === 'rejected') throw changed.reason;
          if (recorded.status === 'rejected') throw recorded.reason;
          expect(recorded.value.clipped).toBe(false);
          expect(recorded.value.frameCount).toBeGreaterThan(0);
          const frames = execFileSync(
            'ffmpeg',
            [
              '-v',
              'error',
              '-i',
              videoPath,
              '-vf',
              'crop=' + rect.width + ':' + rect.height + ':0:0',
              '-f',
              'rawvideo',
              '-pix_fmt',
              'rgb24',
              '-fps_mode',
              'passthrough',
              'pipe:1',
            ],
            { maxBuffer: 64 * 1024 * 1024 }
          );
          const foreground = PNG.sync.read(opaque);
          const frameBytes = rect.width * rect.height * 3;
          expect(frames.length % frameBytes).toBe(0);
          expect(frames.length / frameBytes).toBe(recorded.value.frameCount);
          expect(recorded.value.frameCount).toBeGreaterThan(30);
          // Intermediate frames may change opacity, but the same glyph pattern
          // must remain visible; a desktop-only frame loses this contrast.
          for (let offset = 0; offset < frames.length; offset += frameBytes) {
            let ink = 0,
              inkCount = 0,
              blank = 0,
              blankCount = 0;
            for (
              let at = 0, pixel = offset;
              at < foreground.data.length;
              at += 4, pixel += 3
            ) {
              const sum = frames[pixel] + frames[pixel + 1] + frames[pixel + 2];
              if (
                foreground.data[at] > 200 &&
                foreground.data[at + 1] > 200 &&
                foreground.data[at + 2] > 200
              ) {
                ink += sum;
                inkCount++;
              } else if (
                foreground.data[at] < 50 &&
                foreground.data[at + 1] < 50 &&
                foreground.data[at + 2] < 50
              ) {
                blank += sum;
                blankCount++;
              }
            }
            expect(inkCount).toBeGreaterThan(20);
            expect(blankCount).toBeGreaterThan(100);
            expect(
              ink / inkCount - blank / blankCount,
              'glyph contrast in mode frame ' + offset / frameBytes
            ).toBeGreaterThan(90);
          }
          // Return to the original policy for the remaining scrollbar actions.
          await agent.keyboard.press('F8');
          await agent.keyboard.press('o', { modifiers: ['Control', 'Shift'] });
          await observe(
            agent,
            rect,
            opaque,
            resolve(output, 'mode-restored-failure.png')
          );
        }
        expect((await process.snapshot()).root.id).toBe(id);
        expect((await process.snapshot()).root.running).toBe(true);
        expect(
          (await process.processes()).some(
            (value) =>
              value.id === helper!.id &&
              value.createdAt === helper!.createdAt &&
              value.running
          )
        ).toBe(true);
        expect(await process.stdoutText()).toContain(
          `boxrig_term: renderer=${renderer}`
        );
        // Exercise the enabled scrollbar while the same host is translucent.
        for (let step = 0; step < 5; step++)
          await agent.keyboard.press(',', { modifiers: ['Control', 'Shift'] });
        await agent.keyboard.press('h');
        window = await process.waitForWindow(
          {
            className: 'boxrig_win32_sample',
            title: 'history-ready',
            visible: true,
          },
          { timeoutMs: 30000 }
        );
        const history = await stableCapture(agent, rect);
        await agent.mouse.click({
          x: window.clientBounds.x + window.clientBounds.width - 6,
          y: window.clientBounds.y + Math.floor(window.clientBounds.height / 2),
        });
        const scrollDeadline = Date.now() + 15000;
        let scrolled = await capture(agent, rect);
        while (
          difference(scrolled, history) < 30 &&
          Date.now() < scrollDeadline
        ) {
          await delay(80);
          scrolled = await capture(agent, rect);
        }
        await writeFile(resolve(output, 'history-before.png'), history);
        await writeFile(resolve(output, 'history-scrolled.png'), scrolled);
        expect(difference(scrolled, history)).toBeGreaterThanOrEqual(30);
        await agent.mouse.click({ x: rect.x + 10, y: rect.y + 5 });
        await agent.keyboard.press('q');
        expect(
          (await process.waitForExit({ timeoutMs: 30000 })).root.exitCode
        ).toBe(0);
        await writeFile(
          resolve(output, 'runtime.log'),
          (await process.stdoutText()) + (await process.stderrText())
        );
      } finally {
        await writeFile(
          resolve(output, 'process.json'),
          JSON.stringify(await process.snapshot(), null, 2)
        );
        await writeFile(
          resolve(output, 'windows.json'),
          JSON.stringify(
            (await process.windows()).map((value) => ({
              id: value.id,
              title: value.title,
              className: value.className,
              visible: value.visible,
              bounds: value.bounds,
            })),
            null,
            2
          )
        );
        if ((await process.snapshot()).running) await process.kill();
        await writeFile(
          resolve(output, 'runtime.log'),
          (await process.stdoutText()) + (await process.stderrText())
        );
        await process.releaseAsync();
      }
    } finally {
      await agent.files.remove(directory, { recursive: true });
    }
  }
);
