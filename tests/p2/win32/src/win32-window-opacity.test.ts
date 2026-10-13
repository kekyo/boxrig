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

it.each(['amd64', 'i686'])(
  'composites GDI CLI glyphs once and restores the same session (%s)',
  async (architecture) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    if (!source) throw new Error('BOXRIG_P2_WIN32_SOURCE_DIR is required.');
    using agent = await connectRemoteAgent({
      timeoutMs: 30000,
    });
    const directory = await agent.files.mkdtemp('boxrig-gdi-window-opacity-');
    const output = resolve(
      source,
      'build/test-results/step20-14-2/gdi-window-opacity',
      architecture
    );
    try {
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
        path: directory + '/boxrig_term.exe',
        // The child resolves its path from the working directory below.
        arguments: ['--renderer=win32', '--', '.\\pty_helper.exe'],
        workingDirectory: directory,
        environment: {
          BOXRIG_TRANSPARENCY_MODE: 'window',
          BOXRIG_WINDOW_OPACITY: '100',
          BOXRIG_BACKGROUND_OPACITY: '0',
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
            'The actual GDI terminal did not reach helper-ready.'
          );
        window = await window.setBounds({ ...window.bounds, x: 80, y: 80 });
        window = await activate(agent, window);
        const helper = (await process.processes()).find(
          (value) => value.name.toLowerCase() === 'pty_helper.exe'
        );
        expect(helper).toBeDefined();
        const rect = {
          x: window.clientBounds.x + 4,
          y: window.clientBounds.y + 2,
          width: 350,
          height: Math.floor(window.clientBounds.height / 24) - 4,
        };
        // Capture the same complete foreground and backdrop, then verify the
        // whole-window operation rather than accepting an alpha API return code.
        const opaque = await stableCapture(agent, rect);
        await writeFile(resolve(output, 'opaque.png'), opaque);
        requireInk(opaque);
        await window.minimize();
        const behind = await stableCapture(agent, rect);
        expect(difference(opaque, behind)).toBeGreaterThan(30);
        await writeFile(resolve(output, 'backdrop.png'), behind);
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
        const half = await observe(
          agent,
          rect,
          composite(opaque, behind),
          resolve(output, 'whole-50-failure.png')
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

it.each(['amd64', 'i686'])(
  'applies one host alpha to two external GDI components (%s)',
  async (architecture) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    if (!source) throw new Error('BOXRIG_P2_WIN32_SOURCE_DIR is required.');
    using agent = await connectRemoteAgent({
      timeoutMs: 30000,
    });
    const directory = await agent.files.mkdtemp(
      'boxrig-external-window-opacity-'
    );
    const output = resolve(
      source,
      'build/test-results/step20-14-2/external-window-opacity',
      architecture
    );
    const opaque: Buffer[] = [];
    const backdrop: Buffer[] = [];
    try {
      await mkdir(output, { recursive: true });
      await agent.files.writeFile(
        directory + '/host-opacity.exe',
        await readFile(
          resolve(
            source,
            'build/tests/win32-sdk-' + architecture,
            'software/host-opacity.exe'
          )
        )
      );
      for (const mode of ['opaque', 'window']) {
        const child = await agent.processes.launchManaged({
          path: directory + '/host-opacity.exe',
          arguments: [
            '--helper',
            '--apply',
            '--two',
            ...(mode === 'opaque' ? ['--opaque'] : []),
          ],
          workingDirectory: directory,
          captureStdout: true,
          captureStderr: true,
          createNoWindow: true,
        });
        try {
          const id = (await child.snapshot()).root.id;
          let window: AppWindow | undefined;
          const deadline = Date.now() + 30000;
          while (Date.now() < deadline) {
            const values = await agent.findWindows({
              processId: id,
              className: 'boxrig_host_opacity_example',
              visible: true,
            });
            if (values.length === 1 && values[0].title.includes('applied=1')) {
              window = values[0];
              break;
            }
            await delay(50);
          }
          if (!window)
            throw new Error(
              'The external host did not commit both component frames.'
            );
          window = await window.setBounds({ ...window.bounds, x: 80, y: 80 });
          window = await activate(agent, window);
          const rows = [0, 6].map((row) => ({
            x: window!.clientBounds.x + 4,
            y:
              window!.clientBounds.y +
              Math.floor((window!.clientBounds.height * row) / 12) +
              2,
            width: 300,
            height: Math.floor(window!.clientBounds.height / 12) - 4,
          }));
          for (let index = 0; index < rows.length; index++) {
            const bytes =
              mode === 'opaque'
                ? await stableCapture(agent, rows[index])
                : await observe(
                    agent,
                    rows[index],
                    composite(opaque[index], backdrop[index]),
                    resolve(output, 'component-' + index + '-failure.png')
                  );
            if (mode === 'opaque') {
              opaque.push(bytes);
            }
            await writeFile(
              resolve(output, mode + '-component-' + index + '.png'),
              bytes
            );
            if (mode === 'opaque') requireInk(bytes);
          }
          if (mode === 'opaque') {
            await window.minimize();
            for (let index = 0; index < rows.length; index++) {
              const bytes = await stableCapture(agent, rows[index]);
              expect(difference(opaque[index], bytes)).toBeGreaterThan(30);
              backdrop.push(bytes);
              await writeFile(
                resolve(output, 'backdrop-' + index + '.png'),
                bytes
              );
            }
          }
          await window.close();
          expect(
            (await child.waitForExit({ timeoutMs: 30000 })).root.exitCode
          ).toBe(0);
          const log = (await child.stdoutText()) + (await child.stderrText());
          expect(log).toContain('background=255');
          expect(log).toContain(
            'window=' + (mode === 'opaque' ? '255' : '128')
          );
          await writeFile(resolve(output, mode + '.log'), log);
        } finally {
          if ((await child.snapshot()).running) await child.kill();
          await child.releaseAsync();
        }
      }
    } finally {
      await agent.files.remove(directory, { recursive: true });
    }
  }
);
