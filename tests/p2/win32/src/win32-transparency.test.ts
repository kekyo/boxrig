import { readFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';

import {
  saveDiagnostics,
  type AppWindow,
  type ScreenRect,
  type RemoteAgent,
  type RemoteManagedProcess,
} from 'agent-rover';
import { connectRemoteAgent } from './agent-connection.js';
import { delay } from 'async-primitives';
import { PNG } from 'pngjs';
import { describe, expect, it } from 'vitest';

type NativePixels = readonly [number, number, number];

interface ReadyState {
  readonly revision: number;
  readonly effective: number;
  readonly presentCount: number;
  readonly mouseCount: number;
  readonly keyCount: number;
  readonly scrollCount: number;
  readonly imeCount: number;
  readonly backdrop: number;
  readonly frameStyle: boolean;
  readonly systemMenu: boolean;
  readonly imeContext: boolean;
}

const requireEnvironment = (name: string): string => {
  const value = process.env[name];

  if (value === undefined || value.length === 0) {
    throw new Error(`${name} must be defined by the Makefile gate.`);
  }
  return value;
};

const remoteJoin = (base: string, relative: string): string =>
  `${base}\\${relative.replaceAll('/', '\\')}`;

const parseReadyState = (text: string): ReadyState => {
  const value: unknown = JSON.parse(text);

  if (
    typeof value !== 'object' ||
    value === null ||
    !('revision' in value) ||
    typeof value.revision !== 'number' ||
    !('effective' in value) ||
    typeof value.effective !== 'number' ||
    !('presentCount' in value) ||
    typeof value.presentCount !== 'number' ||
    !('mouseCount' in value) ||
    typeof value.mouseCount !== 'number' ||
    !('keyCount' in value) ||
    typeof value.keyCount !== 'number' ||
    !('scrollCount' in value) ||
    typeof value.scrollCount !== 'number' ||
    !('imeCount' in value) ||
    typeof value.imeCount !== 'number' ||
    !('backdrop' in value) ||
    typeof value.backdrop !== 'number' ||
    !('frameStyle' in value) ||
    typeof value.frameStyle !== 'boolean' ||
    !('systemMenu' in value) ||
    typeof value.systemMenu !== 'boolean' ||
    !('imeContext' in value) ||
    typeof value.imeContext !== 'boolean'
  ) {
    throw new Error(`Invalid transparency state: ${text}`);
  }
  return value as ReadyState;
};

const readState = async (
  agent: RemoteAgent,
  path: string
): Promise<ReadyState> => {
  const boundary = Math.max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
  const directory = path.slice(0, boundary);
  const prefix = path.slice(boundary + 1) + '.';
  // Completed snapshots are immutable, so retaining one cannot block input.
  const latest = (await agent.files.readdir(directory))
    .map((entry) => entry.name)
    .filter(
      (name) =>
        name.startsWith(prefix) && /^\d{10}$/.test(name.slice(prefix.length))
    )
    .sort()
    .at(-1);
  if (latest === undefined)
    throw new Error('No completed transparency observation.');
  const text = new TextDecoder('utf-8', { fatal: true }).decode(
    await agent.files.readFile(remoteJoin(directory, latest))
  );

  return parseReadyState(text);
};

const waitForState = async (
  agent: RemoteAgent,
  path: string,
  process: RemoteManagedProcess,
  predicate: (state: ReadyState) => boolean,
  message: string
): Promise<ReadyState> => {
  const deadline = Date.now() + 15_000;
  let lastState: ReadyState | undefined;
  let lastReadError: unknown;

  while (Date.now() < deadline) {
    try {
      lastState = await readState(agent, path);
      lastReadError = undefined;
      if (predicate(lastState)) {
        return lastState;
      }
    } catch (error) {
      lastReadError = error;
    }
    const snapshot = await process.snapshot();

    if (!snapshot.running) {
      throw new Error(
        `Transparency host exited with ${snapshot.root.exitCode}; stdout=${JSON.stringify(await process.stdoutText())}; stderr=${JSON.stringify(await process.stderrText())}`,
        { cause: lastReadError }
      );
    }
    await delay(100);
  }
  throw new Error(`${message}; lastState=${JSON.stringify(lastState)}`, {
    cause: lastReadError,
  });
};

const rgb = (red: number, green: number, blue: number): number =>
  red | (green << 8) | (blue << 16);

const compositeChannel = (source: number, backdrop: number): number =>
  source + Math.floor((backdrop * 127 + 127) / 255);

const expectedPixels = (backdrop: number): NativePixels => {
  const red = backdrop & 0xff;
  const green = (backdrop >> 8) & 0xff;
  const blue = (backdrop >> 16) & 0xff;

  return [
    backdrop,
    rgb(
      compositeChannel(96, red),
      compositeChannel(0, green),
      compositeChannel(16, blue)
    ),
    rgb(32, 208, 80),
  ];
};

const expectPixels = (actual: NativePixels, expected: NativePixels): void => {
  for (let index = 0; index < actual.length; index += 1) {
    for (let shift = 0; shift <= 16; shift += 8) {
      const actualChannel = (actual[index] >> shift) & 0xff;
      const expectedChannel = (expected[index] >> shift) & 0xff;

      expect(Math.abs(actualChannel - expectedChannel)).toBeLessThanOrEqual(1);
    }
  }
};

const pixelsMatch = (actual: NativePixels, expected: NativePixels): boolean => {
  for (let index = 0; index < actual.length; index += 1) {
    for (let shift = 0; shift <= 16; shift += 8) {
      const actualChannel = (actual[index] >> shift) & 0xff;
      const expectedChannel = (expected[index] >> shift) & 0xff;

      if (Math.abs(actualChannel - expectedChannel) > 1) {
        return false;
      }
    }
  }
  return true;
};

const capturePixels = async (
  agent: RemoteAgent,
  bounds: ScreenRect
): Promise<NativePixels> => {
  const { width, height } = bounds;
  const capture = await agent.screenshot({
    rect: bounds,
  });
  const image = PNG.sync.read(capture.image);
  const readPixel = (sixths: number): number => {
    const x = Math.floor((width * sixths) / 6);
    const y = Math.floor(height / 2);
    const offset = (y * image.width + x) * 4;

    return rgb(
      image.data[offset],
      image.data[offset + 1],
      image.data[offset + 2]
    );
  };

  expect(capture.clipped).toBe(false);
  expect(image.width).toBe(width);
  expect(image.height).toBe(height);
  return [readPixel(1), readPixel(3), readPixel(5)];
};

const waitForCapturedPixels = async (
  agent: RemoteAgent,
  bounds: ScreenRect,
  expected: NativePixels,
  message: string
): Promise<NativePixels> => {
  const deadline = Date.now() + 10_000;
  let actual: NativePixels | undefined;

  while (Date.now() < deadline) {
    actual = await capturePixels(agent, bounds);
    if (pixelsMatch(actual, expected)) {
      return actual;
    }
    await delay(100);
  }
  throw new Error(
    `${message}; actual=${JSON.stringify(actual)} expected=${JSON.stringify(expected)}`
  );
};

const expectSynchronizedBounds = async (
  window: AppWindow,
  visualWindow: AppWindow,
  scrollbarWindow: AppWindow
): Promise<{ visual: ScreenRect; scrollbar: ScreenRect }> => {
  const [host, visual, scrollbar] = await Promise.all([
    window.refresh(),
    visualWindow.refresh(),
    scrollbarWindow.refresh(),
  ]);
  const client = host.clientBounds;
  expect(host.dpi).not.toBeNull();
  expect(host.dpi).toBeGreaterThanOrEqual(96);
  expect(visual.bounds.x).toBe(client.x);
  expect(visual.bounds.y).toBe(client.y);
  expect(visual.bounds.height).toBe(client.height);
  expect(scrollbar.bounds.x).toBe(visual.bounds.x + visual.bounds.width);
  expect(scrollbar.bounds.y).toBe(client.y);
  expect(scrollbar.bounds.x + scrollbar.bounds.width).toBe(
    client.x + client.width
  );
  expect(scrollbar.bounds.height).toBe(client.height);
  expect(visual.bounds.width).toBeGreaterThan(0);
  expect(scrollbar.bounds.width).toBeGreaterThan(0);
  return { visual: visual.bounds, scrollbar: scrollbar.bounds };
};

describe('Win32 layered client surface through agent-rover', () => {
  it('composites pixels and preserves the native host controls', async () => {
    const sourceDirectory = requireEnvironment('BOXRIG_P2_WIN32_SOURCE_DIR');
    using agent = await connectRemoteAgent({
      timeoutMs: 30_000,
    });
    let remoteDirectory: string | undefined;
    let process: RemoteManagedProcess | undefined;

    try {
      const capabilities = await agent.capabilities();

      expect(capabilities.platform).toBe('windows');
      expect(capabilities.features).toContain('agent.native-windows');
      remoteDirectory = await agent.files.mkdtemp(
        `boxrig-alpha-${Date.now()}-`
      );
      await agent.files.syncDirectory({
        checksum: 'sha256',
        deleteExtraneous: true,
        include: ['win32_transparency_host.exe'],
        localPath: dirname(
          resolve(
            sourceDirectory,
            'build/win32/amd64/tests/p2/win32_transparency_host.exe'
          )
        ),
        mode: 'mirror',
        onLockedFile: 'killRelatedProcessesAndRetry',
        relatedProcessPaths: [remoteDirectory],
        remotePath: remoteDirectory,
      });
      const executable = remoteJoin(
        remoteDirectory,
        'win32_transparency_host.exe'
      );
      const readyPath = remoteJoin(remoteDirectory, 'ready.json');

      const reportCheckPath = remoteJoin(remoteDirectory, 'report-check.exe');
      await agent.files.writeFile(
        reportCheckPath,
        await readFile(
          resolve(
            sourceDirectory,
            'build/win32/amd64/tests/test_win32_transparency_report.exe'
          )
        )
      );
      const reportCheck = await agent.processes.launchManaged({
        path: reportCheckPath,
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      try {
        const ended = await reportCheck.waitForExit({ timeoutMs: 30_000 });
        const output =
          (await reportCheck.stdoutText()) + (await reportCheck.stderrText());
        expect(ended.root.exitCode, output).toBe(0);
        expect(output).toContain('PASS: transparency observations advance');
      } finally {
        await reportCheck.releaseAsync();
      }

      process = await agent.processes.launchManaged({
        arguments: ['ready.json'],
        captureStderr: true,
        captureStdout: true,
        path: executable,
        workingDirectory: remoteDirectory,
      });
      let state = await waitForState(
        agent,
        readyPath,
        process,
        (candidate) => candidate.presentCount >= 1,
        'Timed out waiting for the initial alpha presentation'
      );

      let window = await process.waitForWindow(
        {
          className: 'boxrig_win32_transparency_p2_host',
          strict: true,
          visible: true,
        },
        { timeoutMs: 20_000 }
      );
      const [visualWindow] = await window.findDescendants({
        className: 'boxrig_win32_transparency_surface',
        strict: true,
        visible: true,
      });
      const scrollbarContainer = await process.waitForWindow(
        {
          className: 'boxrig_win32_transparency_scrollbar',
          strict: true,
          visible: true,
        },
        { timeoutMs: 20_000 }
      );
      const [scrollbarWindow] = await scrollbarContainer.findDescendants({
        className: 'ScrollBar',
        strict: true,
        visible: true,
      });
      expect(window.process.id).toBe(process.id);
      expect(state.effective).toBe(1);
      expect(state.frameStyle).toBe(true);
      expect(state.systemMenu).toBe(true);
      expect(state.imeContext).toBe(true);
      let geometry = await expectSynchronizedBounds(
        window,
        visualWindow,
        scrollbarWindow
      );
      expect(state.backdrop).toBe(rgb(20, 80, 180));
      const initialExpectedPixels = expectedPixels(state.backdrop);
      expectPixels(
        await waitForCapturedPixels(
          agent,
          geometry.visual,
          initialExpectedPixels,
          'The desktop capture did not observe the initial alpha presentation'
        ),
        initialExpectedPixels
      );
      const initialCapture = await window.screenshot();

      expect(initialCapture.image.length).toBeGreaterThan(0);
      const initialRevision = state.revision;
      await agent.mouse.click({
        x: geometry.visual.x + Math.floor(geometry.visual.width / 6),
        y: geometry.visual.y + Math.floor(geometry.visual.height / 2),
      });
      state = await waitForState(
        agent,
        readyPath,
        process,
        (candidate) =>
          candidate.revision > initialRevision && candidate.mouseCount === 1,
        'The alpha-zero client region did not route its click to the host'
      );
      expect((await window.refresh()).active).toBe(true);

      const presentationBeforeBackdropChange = state.presentCount;
      const backdropRevision = state.revision;
      await agent.keyboard.press('b');
      state = await waitForState(
        agent,
        readyPath,
        process,
        (candidate) =>
          candidate.revision > backdropRevision &&
          candidate.backdrop === rgb(180, 40, 20),
        'The independent backdrop did not repaint'
      );
      expect(state.keyCount).toBeGreaterThan(0);
      expect(state.presentCount).toBe(presentationBeforeBackdropChange);
      const changedExpectedPixels = expectedPixels(state.backdrop);
      expectPixels(
        await waitForCapturedPixels(
          agent,
          geometry.visual,
          changedExpectedPixels,
          'The desktop capture did not observe the changed backdrop'
        ),
        changedExpectedPixels
      );

      const maximizeRevision = state.revision;
      const presentationBeforeMaximize = state.presentCount;
      window = await window.maximize();
      await window.waitForStableBounds({
        stableIterations: 3,
        timeoutMs: 10_000,
      });
      state = await waitForState(
        agent,
        readyPath,
        process,
        (candidate) =>
          candidate.revision > maximizeRevision &&
          candidate.presentCount > presentationBeforeMaximize,
        'The layered visual did not resize after maximize'
      );
      expect((await window.refresh()).maximized).toBe(true);
      geometry = await expectSynchronizedBounds(
        window,
        visualWindow,
        scrollbarWindow
      );

      const resizeRevision = state.revision;
      const presentationBeforeResize = state.presentCount;
      window = await window.restore();
      window = await window.setBounds({
        height: 360,
        width: 560,
        x: 120,
        y: 100,
      });
      await window.waitForStableBounds({
        stableIterations: 3,
        timeoutMs: 10_000,
      });
      state = await waitForState(
        agent,
        readyPath,
        process,
        (candidate) =>
          candidate.revision > resizeRevision &&
          candidate.presentCount > presentationBeforeResize,
        'The auxiliary windows did not follow restore and resize'
      );
      geometry = await expectSynchronizedBounds(
        window,
        visualWindow,
        scrollbarWindow
      );

      expect((await window.refresh()).clientBounds.width).toBeGreaterThan(400);

      const scrollRevision = state.revision;
      await agent.mouse.click({
        x: geometry.scrollbar.x + Math.floor(geometry.scrollbar.width / 2),
        y: geometry.scrollbar.y + geometry.scrollbar.height - 8,
      });
      state = await waitForState(
        agent,
        readyPath,
        process,
        (candidate) =>
          candidate.revision > scrollRevision && candidate.scrollCount > 0,
        'The owned standard scrollbar did not send WM_VSCROLL to the host'
      );
      expect((await window.refresh()).active).toBe(true);
      const relatedWindows = await process.windows({ visible: true });

      expect(
        relatedWindows.some(
          (candidate) =>
            candidate.className === 'boxrig_win32_transparency_scrollbar'
        )
      ).toBe(true);
      await window.close();
      const exited = await process.waitForExit({ timeoutMs: 10_000 });

      expect(exited.root.exitCode).toBe(0);
      console.log(
        'PASS: physical Win32 alpha pixels, live backdrop, DWM frame, hit testing, resize, focus, IME context, and owned scrollbar'
      );
    } catch (error) {
      try {
        await saveDiagnostics(
          resolve(
            '../../../build/test-results/p2-win32-transparency',
            String(Date.now())
          ),
          {
            agent,
            attachments:
              process === undefined
                ? []
                : [{ kind: 'managedProcess', name: 'alpha-host', process }],
            captureOptions: { includeDescendants: true },
          }
        );
      } catch (diagnosticsError) {
        console.error(
          'Failed to save transparency diagnostics.',
          diagnosticsError
        );
      }
      throw error;
    } finally {
      if (process !== undefined) {
        await process.releaseAsync();
      }
      if (remoteDirectory !== undefined) {
        await agent.files.remove(remoteDirectory, { recursive: true });
      }
    }
  });
});
