import { execFileSync } from 'node:child_process';
import { mkdir, writeFile } from 'node:fs/promises';
import { createHash, randomUUID } from 'node:crypto';
import { resolve } from 'node:path';

import {
  saveDiagnostics,
  type AppWindow,
  type RemoteAgent,
  type RemoteApplicationProcess,
  type RemoteInteractionSession,
  type RemoteManagedProcess,
  type ScreenRect,
} from 'agent-rover';
import { connectRemoteAgent } from './agent-connection.js';
import { delay } from 'async-primitives';
import { PNG } from 'pngjs';
import { describe, expect, it } from 'vitest';

interface TestConfiguration {
  readonly sourceDirectory: string;
  readonly diagnosticsDirectory: string;
}

interface GridSize {
  readonly rows: number;
  readonly columns: number;
}

interface WindowGeometry {
  readonly dpi: number;
  readonly inputBounds: ScreenRect;
  readonly surface: ScreenRect;
}

interface RenderingSurface {
  readonly bounds: ScreenRect;
  readonly className: string;
  readonly refresh: () => Promise<RenderingSurface>;
}

const readWindowGeometry = async (
  window: AppWindow,
  surface: RenderingSurface
): Promise<WindowGeometry> => {
  // Agent 0.5 reports both input and rendering rectangles in physical pixels,
  // without changing the target window's DPI awareness.
  const host = await window.refresh();
  const child = await surface.refresh();
  if (host.dpi === null) throw new Error('Window DPI is unavailable.');
  expect(host.dpi).toBeGreaterThanOrEqual(96);
  expect(child.bounds.width).toBeGreaterThan(0);
  expect(child.bounds.height).toBeGreaterThan(0);
  return { dpi: host.dpi, inputBounds: host.bounds, surface: child.bounds };
};

interface BandRectangles {
  readonly right: ScreenRect;
  readonly bottom: ScreenRect;
}

interface BandHashes {
  readonly right: string;
  readonly bottom: string;
}

interface ResizeObservation {
  readonly phase: string;
  readonly pointer: Readonly<{ x: number; y: number }>;
  readonly outer: ScreenRect;
  readonly surface: ScreenRect;
  readonly grid: GridSize;
  readonly hold: BandHashes;
  readonly released: BandHashes;
}

interface ResizeRequest {
  readonly phase: string;
  readonly deltaColumns: number;
  readonly deltaRows: number;
}

interface ResizeContext {
  readonly agent: RemoteAgent;
  readonly process: RemoteManagedProcess;
  readonly interaction: RemoteInteractionSession;
  readonly window: AppWindow;
  readonly cellWidth: number;
  readonly cellHeight: number;
  readonly resizeGripOffsetX: number;
  readonly resizeGripOffsetY: number;
  readonly diagnosticsDirectory: string;
}

const requireEnvironment = (name: string): string => {
  const value = process.env[name];

  if (value === undefined || value.length === 0) {
    throw new Error(`${name} must be defined by the Makefile gate.`);
  }
  return value;
};

const createConfiguration = (): TestConfiguration => ({
  sourceDirectory: requireEnvironment('BOXRIG_P2_WIN32_SOURCE_DIR'),
  diagnosticsDirectory: resolve(
    '../../../build/test-results/p2-win32-interactive-resize'
  ),
});

const remoteJoin = (base: string, relative: string): string =>
  `${base}\\${relative.replaceAll('/', '\\')}`;

const rectanglesEqual = (left: ScreenRect, right: ScreenRect): boolean =>
  left.x === right.x &&
  left.y === right.y &&
  left.width === right.width &&
  left.height === right.height;

const waitForGridTitle = async (
  process: RemoteManagedProcess
): Promise<GridSize> => {
  const window = await process.waitForWindow(
    {
      className: 'boxrig_win32_sample',
      titleRegex: /^grid-\d+-\d+$/u,
      visible: true,
    },
    {
      message: 'Timed out waiting for the PTY grid-title response.',
      timeoutMs: 15_000,
    }
  );
  const match = /^grid-(\d+)-(\d+)$/u.exec(window.title);

  if (match === null) {
    throw new Error(`Invalid PTY grid title: ${window.title}`);
  }
  return { columns: Number(match[2]), rows: Number(match[1]) };
};

const waitForPresentationTitle = async (
  process: RemoteManagedProcess
): Promise<void> => {
  await process.waitForWindow(
    {
      className: 'boxrig_win32_sample',
      title: 'presentation-ready',
      visible: true,
    },
    {
      message: 'Timed out waiting for the static presentation frame.',
      timeoutMs: 15_000,
    }
  );
};

// Every standard surface includes a composited scrollbar. Query the OS metric in the
// same per-monitor awareness context as the driver; keep terminal-grid assertions
// independent of the renderer's window topology and the machine's system DPI.
const scrollbarWidths = new Map<string, number>();
const readScrollbarWidth = async (agent: RemoteAgent): Promise<number> => {
  const query = await agent.processes.launchManaged({
    path: 'powershell.exe',
    arguments: [
      '-NoProfile',
      '-NonInteractive',
      '-Command',
      `Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class Metrics { [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context); [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index); }'; $old = [Metrics]::SetThreadDpiAwarenessContext([IntPtr](-4)); if ($old -eq [IntPtr]::Zero) { throw 'DPI context unavailable' }; try { [Metrics]::GetSystemMetrics(2) } finally { [void][Metrics]::SetThreadDpiAwarenessContext($old) }`,
    ],
    captureStdout: true,
    captureStderr: true,
    createNoWindow: true,
  });
  try {
    const result = await query.waitForExit({ timeoutMs: 30000 });
    expect(result.root.exitCode, await query.stderrText()).toBe(0);
    const width = Number((await query.stdoutText()).trim());
    expect(Number.isInteger(width)).toBe(true);
    expect(width).toBeGreaterThan(0);
    return width;
  } finally {
    await query.releaseAsync();
  }
};

const findRenderingSurface = async (
  window: AppWindow
): Promise<RenderingSurface> => {
  const width = scrollbarWidths.get(window.id);
  if (width === undefined)
    throw new Error('Scrollbar metric was not measured.');
  const gpuSurfaces = [
    ...new Map(
      (await window.descendants())
        .filter(
          (child) =>
            child.visible &&
            (child.className.startsWith('boxrig_wgl_') ||
              child.className.startsWith('boxrig_d3d11_'))
        )
        .map((child) => [child.id, child])
    ).values(),
  ];
  if (gpuSurfaces.length !== 0) {
    expect(gpuSurfaces).toHaveLength(1);
    const surface = gpuSurfaces[0];
    return {
      bounds: { ...surface.bounds, width: surface.bounds.width - width },
      className: surface.className,
      refresh: async () => await findRenderingSurface(await window.refresh()),
    };
  }
  const surfaces = await window.findDescendants({
    className: 'boxrig_win32_transparency_surface',
    strict: true,
    visible: true,
  });

  expect(surfaces).toHaveLength(1);
  return {
    bounds: { ...surfaces[0].bounds, width: surfaces[0].bounds.width - width },
    className: surfaces[0].className,
    refresh: async () => await findRenderingSurface(await window.refresh()),
  };
};

const assertBorrowedAlphaTopology = async (
  process: RemoteManagedProcess,
  window: AppWindow
): Promise<Readonly<Record<string, unknown>>> => {
  const surface = await findRenderingSurface(window);
  const ownedComponents = await window.findDescendants({
    className: 'boxrig_terminal_component',
    visible: true,
  });
  const relatedWindows = await process.windows({ visible: true });
  const scrollbars = relatedWindows.filter(
    (candidate) => candidate.className === 'boxrig_win32_transparency_scrollbar'
  );

  expect(ownedComponents).toHaveLength(0);
  expect(scrollbars).toHaveLength(0);
  return {
    borrowedTarget: true,
    hostClass: window.className,
    layeredRenderingChild: surface.className,
    scrollbarPresentation: 'composited',
  };
};

const assertProcessRunning = async (
  process: RemoteManagedProcess,
  message: string
): Promise<void> => {
  const snapshot = await process.snapshot();

  if (!snapshot.running) {
    throw new Error(
      `${message}; exit=${snapshot.root.exitCode}; stdout=${JSON.stringify(await process.stdoutText())}; stderr=${JSON.stringify(await process.stderrText())}`
    );
  }
};

const focusWindow = async (
  agent: RemoteAgent,
  window: AppWindow
): Promise<AppWindow> => {
  const refreshed = await window.refresh();

  await agent.mouse.click({
    x: refreshed.bounds.x + Math.floor(refreshed.bounds.width / 2),
    y: refreshed.bounds.y + Math.floor(refreshed.bounds.height / 2),
  });
  const deadline = performance.now() + 5_000;

  while (performance.now() < deadline) {
    const focused = await refreshed.refresh();

    if (focused.active) {
      return focused;
    }
    await delay(75);
  }
  throw new Error('The sample did not become active after a real mouse click.');
};

const waitForChangedStableBounds = async (
  process: RemoteManagedProcess,
  window: AppWindow,
  initial: ScreenRect,
  widthDirection: number,
  heightDirection: number
): Promise<AppWindow> => {
  const deadline = performance.now() + 15_000;
  let previous: ScreenRect | undefined;
  let stableIterations = 0;

  while (performance.now() < deadline) {
    const current = await window.refresh();
    const widthChanged =
      Math.sign(current.bounds.width - initial.width) === widthDirection;
    const heightChanged =
      Math.sign(current.bounds.height - initial.height) === heightDirection;

    if (widthChanged && heightChanged) {
      if (previous !== undefined && rectanglesEqual(previous, current.bounds)) {
        stableIterations += 1;
      } else {
        stableIterations = 1;
      }
      previous = current.bounds;
      if (stableIterations >= 3) {
        return current;
      }
    } else {
      previous = undefined;
      stableIterations = 0;
    }
    await assertProcessRunning(
      process,
      'The sample exited while waiting for interactive resize bounds'
    );
    await delay(75);
  }
  throw new Error(
    `Interactive resize did not reach stable changed bounds from ${JSON.stringify(initial)}.`
  );
};

const waitForExactStableBounds = async (
  process: RemoteManagedProcess,
  window: AppWindow,
  expected: ScreenRect
): Promise<AppWindow> => {
  const deadline = performance.now() + 10_000;
  let stableIterations = 0;

  while (performance.now() < deadline) {
    const current = await window.refresh();

    if (rectanglesEqual(current.bounds, expected)) {
      stableIterations += 1;
      if (stableIterations >= 3) {
        return current;
      }
    } else {
      stableIterations = 0;
    }
    await assertProcessRunning(
      process,
      'The sample exited while waiting for release geometry'
    );
    await delay(75);
  }
  throw new Error(
    `Button release changed the resize geometry from ${JSON.stringify(expected)}.`
  );
};

const waitForSurfaceBounds = async (
  process: RemoteManagedProcess,
  window: AppWindow,
  expected: ScreenRect
): Promise<ScreenRect> => {
  const deadline = performance.now() + 10_000;

  while (performance.now() < deadline) {
    const surface = await findRenderingSurface(window);

    if (rectanglesEqual(surface.bounds, expected)) {
      return surface.bounds;
    }
    await assertProcessRunning(
      process,
      'The sample exited while synchronizing its rendering child bounds'
    );
    await delay(75);
  }
  throw new Error(`Rendering child did not reach ${JSON.stringify(expected)}.`);
};

const createBandRectangles = (
  previous: ScreenRect,
  current: ScreenRect,
  cellWidth: number,
  cellHeight: number
): BandRectangles => {
  const previousRight = previous.x + previous.width;
  const previousBottom = previous.y + previous.height;
  const currentRight = current.x + current.width;
  const currentBottom = current.y + current.height;
  const expandedWidth = currentRight - previousRight;
  const expandedHeight = currentBottom - previousBottom;
  const rightWidth =
    expandedWidth > 6
      ? expandedWidth - 4
      : Math.min(cellWidth * 2, current.width - 4);
  const bottomHeight =
    expandedHeight > 6
      ? expandedHeight - 4
      : Math.min(cellHeight * 2, current.height - 4);
  const right = {
    height: Math.max(cellHeight, current.height - cellHeight * 2),
    width: rightWidth,
    x: expandedWidth > 6 ? previousRight + 2 : currentRight - rightWidth - 2,
    y: current.y + cellHeight,
  };
  const bottom = {
    height: bottomHeight,
    width: Math.max(cellWidth, current.width - cellWidth * 2),
    x: current.x + cellWidth,
    y:
      expandedHeight > 6
        ? previousBottom + 2
        : currentBottom - bottomHeight - 2,
  };

  if (
    right.width <= 0 ||
    right.height <= 0 ||
    bottom.width <= 0 ||
    bottom.height <= 0
  ) {
    throw new Error(
      `Could not form resize comparison bands from ${JSON.stringify({ previous, current })}.`
    );
  }
  return { bottom, right };
};

const capturePixelHash = async (
  agent: RemoteAgent,
  rectangle: ScreenRect,
  imagePath: string
): Promise<string> => {
  const capture = await agent.screenshot({ rect: rectangle });
  const image = PNG.sync.read(capture.image);

  expect(capture.clipped).toBe(false);
  expect(image.width).toBe(rectangle.width);
  expect(image.height).toBe(rectangle.height);
  await writeFile(imagePath, capture.image);
  return createHash('sha256').update(image.data).digest('hex');
};

const captureBandHashes = async (
  agent: RemoteAgent,
  rectangles: BandRectangles,
  imagePrefix: string
): Promise<BandHashes> => ({
  bottom: await capturePixelHash(
    agent,
    rectangles.bottom,
    imagePrefix + '-bottom.png'
  ),
  right: await capturePixelHash(
    agent,
    rectangles.right,
    imagePrefix + '-right.png'
  ),
});

const waitForStableBandHashes = async (
  agent: RemoteAgent,
  process: RemoteManagedProcess,
  window: AppWindow,
  outer: ScreenRect,
  rectangles: BandRectangles,
  imagePrefix: string
): Promise<BandHashes> => {
  const deadline = performance.now() + 15_000;
  let previous: BandHashes | undefined;
  let stableIterations = 0;

  while (performance.now() < deadline) {
    const currentWindow = await window.refresh();

    if (!rectanglesEqual(currentWindow.bounds, outer)) {
      stableIterations = 0;
      previous = undefined;
      await delay(75);
      continue;
    }
    const current = await captureBandHashes(agent, rectangles, imagePrefix);

    if (
      previous !== undefined &&
      current.right === previous.right &&
      current.bottom === previous.bottom
    ) {
      stableIterations += 1;
    } else {
      stableIterations = 1;
    }
    previous = current;
    if (stableIterations >= 3) {
      return current;
    }
    await assertProcessRunning(
      process,
      'The sample exited while stabilizing resize presentation hashes'
    );
    await delay(75);
  }
  throw new Error('Resize presentation bands did not reach stable hashes.');
};

const prepareStaticPattern = async (
  agent: RemoteAgent,
  process: RemoteManagedProcess
): Promise<void> => {
  await agent.keyboard.press('p');
  await waitForPresentationTitle(process);
};

const performInteractiveResize = async (
  context: ResizeContext,
  request: ResizeRequest
): Promise<ResizeObservation> => {
  const initialWindow = await context.window.refresh();
  const initialSurface = (await findRenderingSurface(initialWindow)).bounds;
  const startPoint = {
    x:
      initialWindow.frameBounds.x +
      initialWindow.frameBounds.width +
      context.resizeGripOffsetX,
    y:
      initialWindow.frameBounds.y +
      initialWindow.frameBounds.height +
      context.resizeGripOffsetY,
  };
  const targetPoint = {
    x: startPoint.x + request.deltaColumns * context.cellWidth,
    y: startPoint.y + request.deltaRows * context.cellHeight,
  };

  await context.interaction.mouse.move(startPoint);
  await context.interaction.mouse.down({ button: 'left' });
  await context.interaction.mouse.move(targetPoint);
  const heldWindow = await waitForChangedStableBounds(
    context.process,
    context.window,
    initialWindow.bounds,
    Math.sign(request.deltaColumns),
    Math.sign(request.deltaRows)
  );
  const widthDelta = heldWindow.bounds.width - initialWindow.bounds.width;
  const heightDelta = heldWindow.bounds.height - initialWindow.bounds.height;

  expect(heldWindow.bounds.x).toBe(initialWindow.bounds.x);
  expect(heldWindow.bounds.y).toBe(initialWindow.bounds.y);
  expect(Math.abs(widthDelta) % context.cellWidth).toBe(0);
  expect(Math.abs(heightDelta) % context.cellHeight).toBe(0);
  const expectedSurface = {
    height: initialSurface.height + heightDelta,
    width: initialSurface.width + widthDelta,
    x: initialSurface.x,
    y: initialSurface.y,
  };
  const heldSurface = await waitForSurfaceBounds(
    context.process,
    heldWindow,
    expectedSurface
  );
  const bands = createBandRectangles(
    initialSurface,
    heldSurface,
    context.cellWidth,
    context.cellHeight
  );
  const hold = await waitForStableBandHashes(
    context.agent,
    context.process,
    heldWindow,
    heldWindow.bounds,
    bands,
    resolve(context.diagnosticsDirectory, request.phase + '-held')
  );

  await context.interaction.mouse.up({ button: 'left' });
  const releasedWindow = await waitForExactStableBounds(
    context.process,
    heldWindow,
    heldWindow.bounds
  );
  await context.agent.keyboard.press('g');
  const grid = await waitForGridTitle(context.process);

  expect(grid.columns * context.cellWidth).toBe(heldSurface.width);
  expect(grid.rows * context.cellHeight).toBe(heldSurface.height);
  const released = await waitForStableBandHashes(
    context.agent,
    context.process,
    releasedWindow,
    releasedWindow.bounds,
    bands,
    resolve(context.diagnosticsDirectory, request.phase + '-released')
  );

  await writeFile(
    resolve(context.diagnosticsDirectory, request.phase + '.json'),
    JSON.stringify({ bands, hold, released, grid }, null, 2)
  );
  expect(released.right, request.phase + ' right band after release').toBe(
    hold.right
  );
  expect(released.bottom, request.phase + ' bottom band after release').toBe(
    hold.bottom
  );

  return {
    grid,
    hold,
    outer: releasedWindow.bounds,
    phase: request.phase,
    pointer: targetPoint,
    released,
    surface: heldSurface,
  };
};

const runOpacityCase = async (
  opacity: number,
  architecture: string,
  executable: string,
  renderer: string | undefined
): Promise<void> => {
  const configuration = createConfiguration();
  const agent = await connectRemoteAgent({
    timeoutMs: 30_000,
  });
  let remoteDirectory: string | undefined;
  let managedProcess: RemoteManagedProcess | undefined;
  let watchdog: RemoteApplicationProcess | undefined;
  let interaction: RemoteInteractionSession | undefined;
  let stage = 'initialize remote sample';

  try {
    const capabilities = await agent.capabilities();

    expect(capabilities.platform).toBe('windows');
    expect(capabilities.features).toContain('agent.native-windows');
    remoteDirectory = await agent.files.mkdtemp(
      `boxrig-resize-${opacity}-${randomUUID()}-`
    );
    const resolver = await agent.processes.launchManaged({
      path: 'cmd.exe',
      arguments: ['/d', '/c', 'cd'],
      workingDirectory: remoteDirectory,
      captureStdout: true,
      createNoWindow: true,
    });
    try {
      expect(
        (await resolver.waitForExit({ timeoutMs: 30000 })).root.exitCode
      ).toBe(0);
      remoteDirectory = (await resolver.stdoutText()).trim();
      expect(remoteDirectory).toMatch(/^[a-z]:\\/i);
    } finally {
      await resolver.releaseAsync();
    }
    await agent.files.syncDirectory({
      checksum: 'sha256',
      deleteExtraneous: true,
      exclude: ['build/wine*', 'build/tests/wine*', 'build/test-results'],
      include: [
        `build/boxrig_term/win32/${architecture}/${executable}`,
        `build/boxrig_term/win32/${architecture}/**`,
        'build/boxrig_term/win32/amd64/conpty.dll',
        'build/boxrig_term/win32/amd64/x64/OpenConsole.exe',
        'build/boxrig_term/win32/amd64/arm64/OpenConsole.exe',
        'build/win32/amd64/tests/pty_helper.exe',
        'build/win32/amd64/tests/p2/win32_ime_control.exe',
      ],
      localPath: configuration.sourceDirectory,
      mode: 'mirror',
      onLockedFile: 'killRelatedProcessesAndRetry',
      relatedProcessPaths: [remoteDirectory],
      remotePath: remoteDirectory,
    });
    const applicationPath = remoteJoin(
      remoteDirectory,
      `build/boxrig_term/win32/${architecture}/${executable}`
    );

    managedProcess = await agent.processes.launchManaged({
      arguments: [
        ...(renderer === undefined ? [] : [`--renderer=${renderer}`]),
        '--',
        'build\\win32\\amd64\\tests\\pty_helper.exe',
      ],
      // Terminal output is observed through the ConPTY surface.
      captureStdout: true,
      captureStderr: true,
      environment: {
        BOXRIG_BACKGROUND_OPACITY: String(opacity),
        // Exact held/released pixels require one unchanged VT scene. System
        // ConPTY regenerates that scene on resize; keep its geometry coverage
        // in the SDK and scrollbar tests, and use passthrough for this fixture.
        BOXRIG_CONPTY: 'packaged-required',
        BOXRIG_CONPTY_DIRECTORY: remoteJoin(
          remoteDirectory,
          `build/boxrig_term/win32/${architecture}`
        ),
      },
      path: applicationPath,
      workingDirectory: remoteDirectory,
    });
    if (executable.includes('_d3d11') || renderer === 'd3d11') {
      // Managed launch enumerates windows first; this guard must start before
      // any query can wait on the sample's UI thread.
      watchdog = await agent.applications.launch({
        path: remoteJoin(
          remoteDirectory,
          'build/win32/amd64/tests/p2/win32_ime_control.exe'
        ),
        arguments: ['watchdog', String(managedProcess.id)],
        createNoWindow: true,
      });
    }
    let window = await managedProcess.waitForWindow(
      {
        className: 'boxrig_win32_sample',
        title: 'helper-ready',
        visible: true,
      },
      {
        message: 'Timed out waiting for the real boxrig Win32 sample.',
        timeoutMs: 20_000,
      }
    );

    stage = 'verify the actual runtime renderer before resizing';
    const expectedRenderer =
      renderer ?? (executable.includes('_d3d11') ? 'd3d11' : 'opengl');
    expect(await managedProcess.stdoutText()).toContain(
      'boxrig_term: renderer=' + expectedRenderer
    );

    if (executable.includes('_d3d11') || renderer === 'd3d11') {
      const control = await agent.processes.launchManaged({
        path: remoteJoin(
          remoteDirectory,
          'build/win32/amd64/tests/p2/win32_ime_control.exe'
        ),
        arguments: ['responsive', String(managedProcess.id)],
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      try {
        const exited = await control.waitForExit({ timeoutMs: 10000 });
        if (exited.root.exitCode !== 0) {
          await managedProcess.kill();
          throw new Error(await control.stderrText());
        }
      } finally {
        await control.releaseAsync();
      }
    }
    scrollbarWidths.set(window.id, await readScrollbarWidth(agent));
    stage = 'normalize the baseline window and inspect topology';
    const requestedBounds = {
      height: 400,
      width: 560,
      x: 120,
      y: 100,
    };
    window = await window.setBounds(requestedBounds);
    window = await window.waitForStableBounds({
      stableIterations: 3,
      timeoutMs: 10_000,
    });
    window = await focusWindow(agent, window);

    await agent.keyboard.press('g');
    let initialGrid = await waitForGridTitle(managedProcess);
    let initialSurface = (await findRenderingSurface(window)).bounds;
    const cellWidth = Math.floor(initialSurface.width / initialGrid.columns);
    const cellHeight = Math.floor(initialSurface.height / initialGrid.rows);
    const widthRemainder =
      initialSurface.width - initialGrid.columns * cellWidth;
    const heightRemainder =
      initialSurface.height - initialGrid.rows * cellHeight;
    const correctedRequestedBounds = {
      ...requestedBounds,
      height: requestedBounds.height - heightRemainder,
      width: requestedBounds.width - widthRemainder,
    };

    expect(cellWidth).toBeGreaterThan(1);
    expect(cellHeight).toBeGreaterThan(1);
    window = await window.setBounds(correctedRequestedBounds);
    window = await window.waitForStableBounds({
      stableIterations: 3,
      timeoutMs: 10_000,
    });
    window = await focusWindow(agent, window);
    await agent.keyboard.press('g');
    initialGrid = await waitForGridTitle(managedProcess);
    initialSurface = (await findRenderingSurface(window)).bounds;
    expect(initialSurface.width % initialGrid.columns).toBe(0);
    expect(initialSurface.height % initialGrid.rows).toBe(0);
    expect(initialSurface.width / initialGrid.columns).toBe(cellWidth);
    expect(initialSurface.height / initialGrid.rows).toBe(cellHeight);
    const topology =
      expectedRenderer !== 'win32'
        ? { graphicsChild: (await findRenderingSurface(window)).className }
        : await assertBorrowedAlphaTopology(managedProcess, window);
    const resizeGripOffsetX = Math.floor(
      (correctedRequestedBounds.x +
        correctedRequestedBounds.width -
        (window.frameBounds.x + window.frameBounds.width)) /
        2
    );
    const resizeGripOffsetY = Math.floor(
      (correctedRequestedBounds.y +
        correctedRequestedBounds.height -
        (window.frameBounds.y + window.frameBounds.height)) /
        2
    );

    expect(resizeGripOffsetX).toBeGreaterThan(0);
    expect(resizeGripOffsetY).toBeGreaterThan(0);
    interaction = await agent.interaction.start({ restoreCursor: true });
    const bandDirectory = resolve(
      configuration.diagnosticsDirectory,
      `bands-${architecture}-${expectedRenderer}-${opacity}-${randomUUID()}`
    );
    await mkdir(bandDirectory, { recursive: true });
    const resizeContext: ResizeContext = {
      agent,
      cellHeight,
      cellWidth,
      interaction,
      process: managedProcess,
      resizeGripOffsetX,
      resizeGripOffsetY,
      window,
      diagnosticsDirectory: bandDirectory,
    };
    const observations: ResizeObservation[] = [];

    await prepareStaticPattern(agent, managedProcess);
    // Keep one static pattern throughout the recording. Asking the child to
    // clear and rewrite it would include intentional partial output frames.
    const resizeSequence = async (): Promise<void> => {
      stage = 'hold an enlarged bottom-right resize before release';
      observations.push(
        await performInteractiveResize(resizeContext, {
          deltaColumns: 8,
          deltaRows: 4,
          phase: 'expand',
        })
      );
      stage = 'hold a shrunken bottom-right resize before release';
      observations.push(
        await performInteractiveResize(resizeContext, {
          deltaColumns: -5,
          deltaRows: -3,
          phase: 'shrink',
        })
      );
      stage = 'hold a repeated expansion before release';
      observations.push(
        await performInteractiveResize(resizeContext, {
          deltaColumns: 5,
          deltaRows: 3,
          phase: 'reexpand',
        })
      );
    };
    if (expectedRenderer !== 'win32') {
      await mkdir(configuration.diagnosticsDirectory, { recursive: true });
      const rect = {
        x: initialSurface.x + 8,
        y: initialSurface.y + 6,
        width: 240,
        height: 100,
      };
      const videoPath = resolve(
        configuration.diagnosticsDirectory,
        `resize-${executable}-${architecture}-${randomUUID()}.mp4`
      );
      // The transparent product exposed an intermittent missing composition
      // frame. Exercise repeated exchanges in one continuous recording.
      const sequences = renderer === 'd3d11' ? 3 : 1;
      const [recording, resizing] = await Promise.allSettled([
        agent.recordVideo(12_000 * sequences, videoPath, { fps: 15, rect }),
        (async () => {
          for (let sequence = 0; sequence < sequences; sequence++) {
            await resizeSequence();
          }
        })(),
      ]);
      if (recording.status === 'rejected') throw recording.reason;
      if (resizing.status === 'rejected') throw resizing.reason;
      const video = recording.value;
      expect(video.frameCount).toBeGreaterThan(1);
      const frames = execFileSync(
        'ffmpeg',
        [
          '-v',
          'error',
          '-i',
          videoPath,
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
      const frameBytes = rect.width * rect.height * 3;
      expect(frames.length % frameBytes).toBe(0);
      expect(frames.length / frameBytes).toBe(video.frameCount);
      // Every recorded frame must retain the pattern's blue background and light glyphs.
      for (let offset = 0; offset < frames.length; offset += frameBytes) {
        let background = 0;
        let glyph = 0;
        for (let pixel = offset; pixel < offset + frameBytes; pixel += 3) {
          const red = frames[pixel];
          const green = frames[pixel + 1];
          const blue = frames[pixel + 2];
          if (blue > red + 15 && blue > 55 && red < 110) background++;
          if (red > 140 && green > 140 && blue > 140) glyph++;
        }
        expect(
          background,
          `background in video frame ${offset / frameBytes}`
        ).toBeGreaterThan(rect.width * rect.height * 0.35);
        expect(
          glyph,
          `glyphs in video frame ${offset / frameBytes}`
        ).toBeGreaterThan(rect.width * rect.height * 0.005);
      }
      window = await window.maximize();
      await window.waitForStableBounds({
        stableIterations: 3,
        timeoutMs: 10_000,
      });
      await assertProcessRunning(
        managedProcess,
        'Accelerated sample exited on maximize'
      );
      window = await window.restore();
      await window.waitForStableBounds({
        stableIterations: 3,
        timeoutMs: 10_000,
      });
      await assertProcessRunning(
        managedProcess,
        'Accelerated sample exited on restore'
      );
      await prepareStaticPattern(agent, managedProcess);
      // Only the instrumented host implements per-monitor DPI policy. The
      // actual CLI still records resize/maximize/restore with its own policy.
      if (executable.includes('_ogl') || executable.includes('_d3d11')) {
        const { monitors } = await agent.desktop();
        const monitorEvidence: {
          monitor: string;
          reportedScale: number | null;
          dpi: number;
          surface: ScreenRect;
          grid: GridSize;
          cell: { width: number; height: number };
        }[] = [];
        const dragInteraction = interaction;
        const dragProcess = managedProcess;
        let dragDurationMs = 0;
        const dragSequence = async (): Promise<void> => {
          const started = performance.now();
          // Return to the first display to exercise both increasing and decreasing DPI.
          for (const monitor of [
            ...monitors,
            ...monitors.slice(0, monitors.length > 1 ? 1 : 0),
          ]) {
            stage = 'drag the accelerated title bar to ' + monitor.name;
            window = await window.refresh();
            const before = await readWindowGeometry(
              window,
              await findRenderingSurface(window)
            );
            const start = {
              x:
                before.inputBounds.x + Math.floor(before.inputBounds.width / 2),
              y: before.inputBounds.y + 22,
            };
            const end = {
              // Keep the sampled terminal region inside the display.
              x: monitor.workArea.x + Math.floor(monitor.workArea.width / 3),
              y: monitor.workArea.y + 120,
            };
            await dragInteraction.mouse.move(start);
            await dragInteraction.mouse.down({ button: 'left' });
            for (let step = 1; step <= 8; step++) {
              await dragInteraction.mouse.move({
                x: Math.round(start.x + ((end.x - start.x) * step) / 8),
                y: Math.round(start.y + ((end.y - start.y) * step) / 8),
              });
              await window.waitForStableBounds({
                stableIterations: 2,
                timeoutMs: 10_000,
              });
            }
            window = await window.waitForStableBounds({
              stableIterations: 3,
              timeoutMs: 10_000,
            });
            const held = await readWindowGeometry(
              window,
              await findRenderingSurface(window)
            );
            const center = held.inputBounds.x + held.inputBounds.width / 2;
            expect(center).toBeGreaterThan(monitor.workArea.x);
            expect(center).toBeLessThan(
              monitor.workArea.x + monitor.workArea.width
            );
            const middle = held.inputBounds.y + held.inputBounds.height / 2;
            expect(middle).toBeGreaterThan(monitor.workArea.y);
            expect(middle).toBeLessThan(
              monitor.workArea.y + monitor.workArea.height
            );
            await assertProcessRunning(
              dragProcess,
              'Accelerated sample exited during monitor drag'
            );
            const capture = await agent.screenshot({
              rect: {
                x: held.surface.x + 8,
                y: held.surface.y + 6,
                width: 240,
                height: 100,
              },
            });
            expect(capture.clipped).toBe(false);
            const image = PNG.sync.read(capture.image);
            let background = 0;
            let glyph = 0;
            for (let pixel = 0; pixel < image.data.length; pixel += 4) {
              const red = image.data[pixel];
              const green = image.data[pixel + 1];
              const blue = image.data[pixel + 2];
              if (blue > red + 15 && blue > 55 && red < 110) background++;
              if (red > 140 && green > 140 && blue > 140) glyph++;
            }
            expect(background).toBeGreaterThan(
              image.width * image.height * 0.35
            );
            expect(glyph).toBeGreaterThan(image.width * image.height * 0.005);
            await writeFile(
              resolve(
                configuration.diagnosticsDirectory,
                `monitor-${architecture}-${monitorEvidence.length}.png`
              ),
              capture.image
            );
            await dragInteraction.mouse.up({ button: 'left' });
            window = await window.waitForStableBounds({
              stableIterations: 3,
              timeoutMs: 10_000,
            });
            const released = await readWindowGeometry(
              window,
              await findRenderingSurface(window)
            );
            expect(released.dpi).toBe(held.dpi);
            await agent.keyboard.press('g');
            const grid = await waitForGridTitle(dragProcess);
            const cell = {
              width: Math.floor(released.surface.width / grid.columns),
              height: Math.floor(released.surface.height / grid.rows),
            };
            const first = monitorEvidence[0];
            if (first !== undefined) {
              expect(grid).toEqual(first.grid);
              expect(
                Math.abs(
                  cell.width - (first.cell.width * released.dpi) / first.dpi
                )
              ).toBeLessThanOrEqual(1);
              expect(
                Math.abs(
                  cell.height - (first.cell.height * released.dpi) / first.dpi
                )
              ).toBeLessThanOrEqual(1);
            }
            monitorEvidence.push({
              monitor: monitor.name,
              reportedScale: monitor.scaleFactor,
              dpi: released.dpi,
              surface: released.surface,
              grid,
              cell,
            });
          }
          dragDurationMs = performance.now() - started;
        };
        const dragVideoPath = resolve(
          configuration.diagnosticsDirectory,
          'monitor-drag-' +
            executable +
            '-' +
            architecture +
            '-' +
            randomUUID() +
            '.mp4'
        );
        const [dragRecording, dragging] = await Promise.allSettled([
          window.recordVideo(60_000, dragVideoPath, {
            fps: 10,
            tracking: 'followWindow',
          }),
          dragSequence(),
        ]);
        if (dragRecording.status === 'rejected') throw dragRecording.reason;
        if (dragging.status === 'rejected') throw dragging.reason;
        const dragVideo = dragRecording.value;
        await writeFile(
          resolve(
            configuration.diagnosticsDirectory,
            `monitors-${architecture}.json`
          ),
          JSON.stringify(
            { monitors: monitorEvidence, video: dragVideo, dragDurationMs },
            null,
            2
          )
        );
        expect(dragDurationMs).toBeLessThan(dragVideo.durationMs);
        expect(dragVideo.frameCount).toBeGreaterThan(1);
        // Window growth is cropped to the initial encoded size by this API.
        // Validate an interior region that fits both DPIs, rather than that flag.
        expect(dragVideo.initialBounds.width).toBeGreaterThanOrEqual(320);
        expect(dragVideo.initialBounds.height).toBeGreaterThanOrEqual(180);
        expect(dragVideo.droppedFrames).toBe(0);
        const dragFrames = execFileSync(
          'ffmpeg',
          [
            '-v',
            'error',
            '-i',
            dragVideoPath,
            '-vf',
            'crop=240:100:80:80',
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
        const dragFrameBytes = 240 * 100 * 3;
        expect(dragFrames.length).toBe(dragVideo.frameCount * dragFrameBytes);
        for (
          let offset = 0;
          offset < dragFrames.length;
          offset += dragFrameBytes
        ) {
          let background = 0;
          let glyph = 0;
          for (
            let pixel = offset;
            pixel < offset + dragFrameBytes;
            pixel += 3
          ) {
            const red = dragFrames[pixel];
            const green = dragFrames[pixel + 1];
            const blue = dragFrames[pixel + 2];
            if (blue > red + 15 && blue > 55 && red < 110) background++;
            if (red > 140 && green > 140 && blue > 140) glyph++;
          }
          expect(
            background,
            'background in drag frame ' + offset / dragFrameBytes
          ).toBeGreaterThan(240 * 100 * 0.35);
          expect(
            glyph,
            'glyphs in drag frame ' + offset / dragFrameBytes
          ).toBeGreaterThan(240 * 100 * 0.005);
        }
        if (process.env.BOXRIG_P2_REQUIRE_MIXED_DPI === '1') {
          expect(
            new Set(monitorEvidence.map((entry) => entry.dpi)).size,
            'Mixed-DPI acceptance requires different effective window DPIs'
          ).toBeGreaterThan(1);
          expect(monitorEvidence.at(-1)?.dpi).toBe(monitorEvidence[0].dpi);
          expect(monitorEvidence.at(-1)?.cell).toEqual(monitorEvidence[0].cell);
        }
      }
    } else {
      await resizeSequence();
    }
    await interaction.releaseAsync();
    interaction = undefined;

    stage = 'close the helper and verify the process exit';
    await agent.keyboard.press('q');
    const exited = await managedProcess.waitForExit({
      message: 'The resized Win32 sample did not exit after the helper quit.',
      timeoutMs: 15_000,
    });

    expect(exited.root.exitCode).toBe(0);
    // Redirected stdio can buffer the PTY report until process exit.
    expect(await managedProcess.stdoutText()).toContain(
      'boxrig_term: pty-backend=packaged fallback=0'
    );
    if (renderer !== undefined)
      expect(await managedProcess.stdoutText()).toContain(
        `boxrig_term: renderer=${renderer}`
      );
    console.log(
      JSON.stringify({
        cell: { height: cellHeight, width: cellWidth },
        observations,
        opacity,
        topology,
      })
    );
  } catch (error) {
    console.error(
      'Win32 resize/monitor drag failure',
      { architecture, stage },
      error
    );
    try {
      await saveDiagnostics(
        `${configuration.diagnosticsDirectory}-${opacity}-${randomUUID()}`,
        {
          agent,
          attachments:
            managedProcess === undefined
              ? []
              : [
                  {
                    kind: 'managedProcess',
                    name: `boxrig-resize-${opacity}`,
                    process: managedProcess,
                  },
                ],
          captureOptions: {
            eventLogs: { maxEntries: 50 },
            includeDescendants: true,
            maxDescendantDepth: 4,
          },
        }
      );
    } catch (diagnosticsError) {
      console.error('Failed to save resize diagnostics.', diagnosticsError);
    }
    throw new Error(
      `Win32 interactive resize P2 failed at ${stage} for opacity ${opacity}.`,
      { cause: error }
    );
  } finally {
    try {
      if (interaction !== undefined) {
        await interaction.releaseAsync();
      }
      if (managedProcess !== undefined) {
        try {
          if (executable.includes('_d3d11') || renderer === 'd3d11') {
            await mkdir(configuration.diagnosticsDirectory, {
              recursive: true,
            });
            await writeFile(
              resolve(
                configuration.diagnosticsDirectory,
                architecture + '-' + executable + '-native.log'
              ),
              await managedProcess.stderrText()
            );
          }
        } finally {
          await managedProcess.releaseAsync();
        }

        if (watchdog !== undefined) {
          await agent.processes.waitForExit(watchdog.id, { timeoutMs: 10000 });
        }
      }
      if (remoteDirectory !== undefined) {
        const deadline = Date.now() + 10_000;
        for (;;) {
          try {
            await agent.files.remove(remoteDirectory, { recursive: true });
            break;
          } catch (error) {
            // Exited images can still have transient mappings. Require actual
            // deletion after process release, and retain permanent failures.
            if (
              !/DeleteFileW failed\..*win32Error=(5|32)\b/u.test(
                String(error)
              ) ||
              Date.now() >= deadline
            ) {
              throw error;
            }
            await delay(100);
          }
        }
      }
    } finally {
      agent.release();
    }
  }
};

describe('Win32 interactive resize presentation through agent-rover', () => {
  it.each(['amd64', 'i686'])(
    'keeps automatic OpenGL %s frames complete during physical resize',
    async (architecture) => {
      await runOpacityCase(100, architecture, 'boxrig_term.exe', undefined);
    }
  );
  it.each(['amd64', 'i686'])(
    'resizes the actual transparent D3D11 CLI (%s)',
    async (architecture) => {
      await runOpacityCase(80, architecture, 'boxrig_term.exe', 'd3d11');
    }
  );
  it.each(['amd64', 'i686'])(
    'keeps OpenGL %s frames complete during physical resize',
    async (architecture) => {
      await runOpacityCase(100, architecture, 'boxrig_term_ogl.exe', undefined);
    }
  );
  it.each(['amd64', 'i686'])(
    'keeps D3D11 %s frames complete during physical resize',
    async (architecture) => {
      await runOpacityCase(
        100,
        architecture,
        'boxrig_term_d3d11.exe',
        undefined
      );
    }
  );
  it.each([100, 80])(
    'keeps opacity %i terminal bands complete while Button1 remains held',
    async (opacity) => {
      await runOpacityCase(opacity, 'amd64', 'boxrig_term.exe', 'win32');
    }
  );
});
