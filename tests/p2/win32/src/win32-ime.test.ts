import { execFileSync } from 'node:child_process';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

import {
  saveDiagnostics,
  type AppWindow,
  type RemoteAgent,
  type RemoteApplicationProcess,
  type RemoteInteractionSession,
  type RemoteManagedProcess,
  type RemoteScreenshot,
  type ScreenPoint,
  type ScreenRect,
} from 'agent-rover';
import { connectRemoteAgent } from './agent-connection.js';
import { delay } from 'async-primitives';
import { PNG } from 'pngjs';
import { describe, expect, it } from 'vitest';
import { cursorVideoStaticError } from './cursor-video-error.js';

interface TestConfiguration {
  readonly sourceDirectory: string;
  readonly diagnosticsDirectory: string;
}

interface ImeState {
  readonly languageId: number;
  readonly imeOpen: boolean;
  readonly hasContext: boolean;
  readonly conversion: number;
  readonly sentence: number;
}

interface ImePlacementReport {
  readonly schema: number;
  readonly sequence: number;
  readonly kind: string;
  readonly status: number;
  readonly active: boolean;
  readonly visible: boolean;
  readonly revision: number;
  readonly clientSpot: readonly [number, number];
  readonly screenSpot: readonly [number, number];
  readonly screenOrigin: readonly [number, number];
  readonly screenExclude: readonly [number, number, number, number];
  readonly clientSize: readonly [number, number];
}

interface PixelComponent {
  readonly bounds: ScreenRect;
  readonly pixels: number;
}

interface ReconversionReport {
  readonly schema: number;
  readonly ready: boolean;
  readonly documentIdentity: number;
  readonly documentRevision: number;
  readonly contextOffset: number;
  readonly context: string;
  readonly selectionOffset: number;
  readonly selectionLength: number;
  readonly selectedText: string;
  readonly replacementCount: number;
  readonly lastRequestId: number;
  readonly lastBaseRevision: number;
  readonly lastRangeOffset: number;
  readonly lastRangeLength: number;
  readonly lastReplacement: string;
  readonly lastOutcome: string;
  readonly lastAction: string;
  readonly reconversionCapable: boolean;
  readonly reconversionState: string;
  readonly terminalOutput: string;
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
    '../../../build/test-results/p2-win32-agent-rover'
  ),
});

const remoteJoin = (base: string, relative: string): string =>
  `${base}\\${relative.replaceAll('/', '\\')}`;

const waitForRemoteFile = async (
  agent: RemoteAgent,
  path: string,
  timeoutMs: number
): Promise<void> => {
  const deadline = Date.now() + timeoutMs;

  while (Date.now() < deadline) {
    if (await agent.files.exists(path)) {
      return;
    }
    await delay(100);
  }
  throw new Error(`Timed out waiting for remote file ${path}.`);
};

const readStableBytes = async (
  agent: RemoteAgent,
  path: string,
  minimumLength: number,
  timeoutMs: number
): Promise<Buffer> => {
  const deadline = Date.now() + timeoutMs;
  let previous: Buffer = Buffer.alloc(0);
  let stableIterations = 0;
  let lastReadError: unknown;

  while (Date.now() < deadline) {
    let current: Buffer;

    try {
      current = await agent.files.readFile(path);
      lastReadError = undefined;
    } catch (error) {
      lastReadError = error;
      await delay(150);
      continue;
    }

    if (current.length >= minimumLength && current.equals(previous)) {
      stableIterations += 1;
      if (stableIterations >= 2) {
        return current;
      }
    } else {
      stableIterations = 0;
    }
    previous = current;
    await delay(150);
  }
  throw new Error(
    `Timed out waiting for stable remote bytes in ${path} (minimum ${minimumLength}).`,
    { cause: lastReadError }
  );
};

const runControl = async (
  agent: RemoteAgent,
  controlPath: string,
  workingDirectory: string,
  command: string,
  processId: number
): Promise<string> => {
  const control = await agent.processes.launchManaged({
    arguments: [command, String(processId)],
    captureStderr: true,
    captureStdout: true,
    createNoWindow: true,
    path: controlPath,
    workingDirectory,
  });

  try {
    const snapshot = await control.waitForExit({
      message: `Timed out running Win32 IME control command ${command}.`,
      timeoutMs: 10_000,
    });
    const stdout = await control.stdoutText();
    const stderr = await control.stderrText();

    if (snapshot.root.exitCode !== 0) {
      throw new Error(
        `Win32 IME control command ${command} exited with ${snapshot.root.exitCode}; stdout=${JSON.stringify(stdout)}; stderr=${JSON.stringify(stderr)}`
      );
    }
    return stdout.trim();
  } finally {
    await control.releaseAsync();
  }
};

const parseImeState = (text: string): ImeState => {
  const value: unknown = JSON.parse(text);

  if (
    typeof value !== 'object' ||
    value === null ||
    !('languageId' in value) ||
    typeof value.languageId !== 'number' ||
    !('imeOpen' in value) ||
    typeof value.imeOpen !== 'boolean' ||
    !('hasContext' in value) ||
    typeof value.hasContext !== 'boolean' ||
    !('conversion' in value) ||
    typeof value.conversion !== 'number' ||
    !('sentence' in value) ||
    typeof value.sentence !== 'number'
  ) {
    throw new Error(`Invalid Win32 IME state: ${text}`);
  }
  return value as ImeState;
};

const queryImeState = async (
  agent: RemoteAgent,
  controlPath: string,
  workingDirectory: string,
  processId: number
): Promise<ImeState> =>
  parseImeState(
    await runControl(agent, controlPath, workingDirectory, 'query', processId)
  );

const focusWindow = async (
  agent: RemoteAgent,
  window: AppWindow
): Promise<AppWindow> => {
  const refreshed = await window.refresh();

  await agent.mouse.click({
    x: refreshed.bounds.x + Math.floor(refreshed.bounds.width / 2),
    y: refreshed.bounds.y + Math.floor(refreshed.bounds.height / 2),
  });
  await delay(200);
  const focused = await refreshed.refresh();

  expect(focused.active).toBe(true);
  return focused;
};

const selectJapaneseInputLanguage = async (
  agent: RemoteAgent,
  window: AppWindow,
  controlPath: string,
  workingDirectory: string,
  processId: number
): Promise<ImeState> => {
  for (let attempt = 0; attempt < 4; attempt += 1) {
    const state = await queryImeState(
      agent,
      controlPath,
      workingDirectory,
      processId
    );

    if (state.languageId === 0x0411) {
      return state;
    }
    await focusWindow(agent, window);
    await agent.keyboard.press('Space', { modifiers: ['Meta'] });
    await delay(700);
  }
  throw new Error(
    'Win+Space did not switch the boxrig window from ENG to JPN.'
  );
};

const activateJapaneseIme = async (
  agent: RemoteAgent,
  window: AppWindow,
  controlPath: string,
  workingDirectory: string,
  processId: number
): Promise<boolean> => {
  let state = await selectJapaneseInputLanguage(
    agent,
    window,
    controlPath,
    workingDirectory,
    processId
  );
  let openedByTest = false;

  expect(state.hasContext).toBe(true);
  if (!state.imeOpen) {
    await focusWindow(agent, window);
    await runControl(agent, controlPath, workingDirectory, 'toggle', processId);
    openedByTest = true;
    await delay(500);
    state = await queryImeState(
      agent,
      controlPath,
      workingDirectory,
      processId
    );
  }
  expect(state.languageId).toBe(0x0411);
  return openedByTest;
};

const pressPhysicalAscii = async (
  agent: RemoteAgent,
  text: string
): Promise<void> => {
  for (const character of text) {
    await agent.keyboard.press(character);
    await delay(25);
  }
};

const decodeUtf8 = (bytes: Buffer): string =>
  new TextDecoder('utf-8', { fatal: true }).decode(bytes);

const expectJapaneseCommit = (
  bytes: Buffer,
  previousLength: number
): string => {
  const committed = decodeUtf8(bytes.subarray(previousLength));

  expect(committed).toMatch(/[\u3040-\u30ff\u3400-\u9fff]/u);
  expect(committed).not.toMatch(/[A-Za-z]{3}/u);
  expect(committed).not.toContain('\u001b');
  return committed;
};

const isNumberTuple = (value: unknown, length: number): boolean =>
  Array.isArray(value) &&
  value.length === length &&
  value.every((item) => typeof item === 'number' && Number.isFinite(item));

const parseImePlacementReport = (bytes: Buffer): ImePlacementReport => {
  const text = decodeUtf8(bytes);
  const value: unknown = JSON.parse(text);

  if (
    typeof value !== 'object' ||
    value === null ||
    !('schema' in value) ||
    value.schema !== 1 ||
    !('sequence' in value) ||
    typeof value.sequence !== 'number' ||
    !('kind' in value) ||
    typeof value.kind !== 'string' ||
    !('status' in value) ||
    typeof value.status !== 'number' ||
    !('active' in value) ||
    typeof value.active !== 'boolean' ||
    !('visible' in value) ||
    typeof value.visible !== 'boolean' ||
    !('revision' in value) ||
    typeof value.revision !== 'number' ||
    !('clientSpot' in value) ||
    !isNumberTuple(value.clientSpot, 2) ||
    !('screenSpot' in value) ||
    !isNumberTuple(value.screenSpot, 2) ||
    !('screenOrigin' in value) ||
    !isNumberTuple(value.screenOrigin, 2) ||
    !('screenExclude' in value) ||
    !isNumberTuple(value.screenExclude, 4) ||
    !('clientSize' in value) ||
    !isNumberTuple(value.clientSize, 2)
  ) {
    throw new Error(`Invalid Win32 IME placement report: ${text}`);
  }
  return value as unknown as ImePlacementReport;
};

const waitForImePlacementReport = async (
  agent: RemoteAgent,
  path: string,
  predicate: (report: ImePlacementReport) => boolean,
  timeoutMs: number
): Promise<ImePlacementReport> => {
  const deadline = performance.now() + timeoutMs;
  let lastReport: ImePlacementReport | undefined;
  let lastReadError: unknown;

  while (performance.now() < deadline) {
    try {
      lastReport = parseImePlacementReport(await agent.files.readFile(path));
      lastReadError = undefined;
      if (predicate(lastReport)) {
        return lastReport;
      }
    } catch (error) {
      lastReadError = error;
    }
    await delay(75);
  }
  throw new Error(
    `Timed out waiting for Win32 IME placement report ${path}; last=${JSON.stringify(lastReport)}`,
    { cause: lastReadError }
  );
};

const rectangleContainsPoint = (
  rectangle: ScreenRect,
  point: ScreenPoint
): boolean =>
  point.x >= rectangle.x &&
  point.y >= rectangle.y &&
  point.x < rectangle.x + rectangle.width &&
  point.y < rectangle.y + rectangle.height;

const findNativeImePresentationWindow = async (
  agent: RemoteAgent,
  applicationBounds: ScreenRect
): Promise<AppWindow> => {
  const center = {
    x: applicationBounds.x + Math.floor(applicationBounds.width / 2),
    y: applicationBounds.y + Math.floor(applicationBounds.height / 2),
  };
  const candidates = (
    await agent.findWindows({
      processName: 'TextInputHost.exe',
      visible: true,
    })
  )
    .filter(
      (candidate) =>
        candidate.bounds.width > 0 &&
        candidate.bounds.height > 0 &&
        rectangleContainsPoint(candidate.bounds, center)
    )
    .sort(
      (left, right) =>
        right.bounds.width * right.bounds.height -
        left.bounds.width * left.bounds.height
    );

  if (candidates.length === 0) {
    throw new Error(
      'MS-IME did not expose a visible TextInputHost presentation window.'
    );
  }
  return candidates[0];
};

const captureNativeImeSurface = async (
  agent: RemoteAgent,
  presentationWindow: AppWindow
): Promise<RemoteScreenshot> => {
  const refreshed = await presentationWindow.refresh();
  const capture = await agent.screenshot({ rect: refreshed.bounds });
  const image = PNG.sync.read(capture.image);

  expect(capture.clipped).toBe(false);
  expect(image.width).toBe(refreshed.bounds.width);
  expect(image.height).toBe(refreshed.bounds.height);
  return capture;
};

const intersectRectangles = (
  left: ScreenRect,
  right: ScreenRect
): ScreenRect => {
  const x = Math.max(left.x, right.x);
  const y = Math.max(left.y, right.y);
  const rightEdge = Math.min(left.x + left.width, right.x + right.width);
  const bottomEdge = Math.min(left.y + left.height, right.y + right.height);

  if (rightEdge <= x || bottomEdge <= y) {
    throw new Error(
      'IME candidate search bounds do not intersect the capture.'
    );
  }
  return { height: bottomEdge - y, width: rightEdge - x, x, y };
};

const detectCandidatePopup = (
  candidateCapture: RemoteScreenshot,
  baselineCapture: RemoteScreenshot,
  firstSpot: readonly [number, number],
  currentSpot: readonly [number, number]
): PixelComponent => {
  const candidate = PNG.sync.read(candidateCapture.image);
  const baseline = PNG.sync.read(baselineCapture.image);

  if (
    candidate.width !== baseline.width ||
    candidate.height !== baseline.height ||
    candidateCapture.bounds.x !== baselineCapture.bounds.x ||
    candidateCapture.bounds.y !== baselineCapture.bounds.y ||
    candidateCapture.bounds.width !== baselineCapture.bounds.width ||
    candidateCapture.bounds.height !== baselineCapture.bounds.height
  ) {
    throw new Error(
      'IME candidate and baseline captures have different bounds.'
    );
  }
  const requestedSearch = {
    height:
      Math.max(firstSpot[1], currentSpot[1]) -
      Math.min(firstSpot[1], currentSpot[1]) +
      640,
    width:
      Math.max(firstSpot[0], currentSpot[0]) -
      Math.min(firstSpot[0], currentSpot[0]) +
      768,
    x: Math.min(firstSpot[0], currentSpot[0]) - 128,
    y: Math.min(firstSpot[1], currentSpot[1]) - 96,
  };
  const search = intersectRectangles(candidateCapture.bounds, requestedSearch);
  const changed = new Uint8Array(candidate.width * candidate.height);
  const visited = new Uint8Array(candidate.width * candidate.height);
  const searchLeft = search.x - candidateCapture.bounds.x;
  const searchTop = search.y - candidateCapture.bounds.y;
  const searchRight = searchLeft + search.width;
  const searchBottom = searchTop + search.height;

  for (let y = searchTop; y < searchBottom; y += 1) {
    for (let x = searchLeft; x < searchRight; x += 1) {
      const pixel = y * candidate.width + x;
      const offset = pixel * 4;
      const difference = Math.max(
        Math.abs(candidate.data[offset] - baseline.data[offset]),
        Math.abs(candidate.data[offset + 1] - baseline.data[offset + 1]),
        Math.abs(candidate.data[offset + 2] - baseline.data[offset + 2])
      );

      if (difference >= 24) {
        changed[pixel] = 1;
      }
    }
  }

  const components: PixelComponent[] = [];
  for (let y = searchTop; y < searchBottom; y += 1) {
    for (let x = searchLeft; x < searchRight; x += 1) {
      const start = y * candidate.width + x;

      if (changed[start] === 0 || visited[start] !== 0) {
        continue;
      }
      const pending = [start];
      let minimumX = candidate.width;
      let minimumY = candidate.height;
      let maximumX = -1;
      let maximumY = -1;
      let pixels = 0;

      visited[start] = 1;
      while (pending.length > 0) {
        const pixel = pending.pop();

        if (pixel === undefined) {
          break;
        }
        const pixelX = pixel % candidate.width;
        const pixelY = Math.floor(pixel / candidate.width);

        pixels += 1;
        minimumX = Math.min(minimumX, pixelX);
        minimumY = Math.min(minimumY, pixelY);
        maximumX = Math.max(maximumX, pixelX);
        maximumY = Math.max(maximumY, pixelY);
        for (let offsetY = -1; offsetY <= 1; offsetY += 1) {
          for (let offsetX = -1; offsetX <= 1; offsetX += 1) {
            const nextX = pixelX + offsetX;
            const nextY = pixelY + offsetY;

            if (
              (offsetX === 0 && offsetY === 0) ||
              nextX < searchLeft ||
              nextX >= searchRight ||
              nextY < searchTop ||
              nextY >= searchBottom
            ) {
              continue;
            }
            const next = nextY * candidate.width + nextX;
            if (changed[next] !== 0 && visited[next] === 0) {
              visited[next] = 1;
              pending.push(next);
            }
          }
        }
      }
      const bounds = {
        height: maximumY - minimumY + 1,
        width: maximumX - minimumX + 1,
        x: minimumX + candidateCapture.bounds.x,
        y: minimumY + candidateCapture.bounds.y,
      };

      if (
        pixels >= 1_000 &&
        bounds.width >= 50 &&
        bounds.height >= 40 &&
        bounds.width <= 480 &&
        bounds.height <= 480
      ) {
        components.push({ bounds, pixels });
      }
    }
  }
  components.sort((left, right) => right.pixels - left.pixels);
  if (components.length === 0) {
    throw new Error(
      `No candidate-sized changed pixel region was found in ${JSON.stringify(search)}.`
    );
  }
  return components[0];
};

const waitForExactWindowBounds = async (
  process: RemoteManagedProcess,
  window: AppWindow,
  expected: ScreenRect
): Promise<AppWindow> => {
  const deadline = performance.now() + 15_000;
  let stableIterations = 0;

  while (performance.now() < deadline) {
    const current = await window.refresh();
    const equal =
      current.bounds.x === expected.x &&
      current.bounds.y === expected.y &&
      current.bounds.width === expected.width &&
      current.bounds.height === expected.height;

    stableIterations = equal ? stableIterations + 1 : 0;
    if (stableIterations >= 3) {
      return current;
    }
    const snapshot = await process.snapshot();
    if (!snapshot.running) {
      throw new Error(
        `The sample exited while waiting for dragged bounds: ${JSON.stringify(snapshot)}.`
      );
    }
    await delay(75);
  }
  throw new Error(
    `Physical title-bar drag did not reach ${JSON.stringify(expected)}.`
  );
};

const expectNear = (
  actual: number,
  expected: number,
  tolerance: number
): void => {
  expect(Math.abs(actual - expected)).toBeLessThanOrEqual(tolerance);
};

const parseReconversionReport = (bytes: Buffer): ReconversionReport => {
  const text = decodeUtf8(bytes);
  const value: unknown = JSON.parse(text);

  if (
    typeof value !== 'object' ||
    value === null ||
    !('schema' in value) ||
    typeof value.schema !== 'number' ||
    !('ready' in value) ||
    typeof value.ready !== 'boolean' ||
    !('documentIdentity' in value) ||
    typeof value.documentIdentity !== 'number' ||
    !('documentRevision' in value) ||
    typeof value.documentRevision !== 'number' ||
    !('contextOffset' in value) ||
    typeof value.contextOffset !== 'number' ||
    !('context' in value) ||
    typeof value.context !== 'string' ||
    !('selectionOffset' in value) ||
    typeof value.selectionOffset !== 'number' ||
    !('selectionLength' in value) ||
    typeof value.selectionLength !== 'number' ||
    !('selectedText' in value) ||
    typeof value.selectedText !== 'string' ||
    !('replacementCount' in value) ||
    typeof value.replacementCount !== 'number' ||
    !('lastRequestId' in value) ||
    typeof value.lastRequestId !== 'number' ||
    !('lastBaseRevision' in value) ||
    typeof value.lastBaseRevision !== 'number' ||
    !('lastRangeOffset' in value) ||
    typeof value.lastRangeOffset !== 'number' ||
    !('lastRangeLength' in value) ||
    typeof value.lastRangeLength !== 'number' ||
    !('lastReplacement' in value) ||
    typeof value.lastReplacement !== 'string' ||
    !('lastOutcome' in value) ||
    typeof value.lastOutcome !== 'string' ||
    !('lastAction' in value) ||
    typeof value.lastAction !== 'string' ||
    !('reconversionCapable' in value) ||
    typeof value.reconversionCapable !== 'boolean' ||
    !('reconversionState' in value) ||
    typeof value.reconversionState !== 'string' ||
    !('terminalOutput' in value) ||
    typeof value.terminalOutput !== 'string'
  ) {
    throw new Error(`Invalid Win32 reconversion report: ${text}`);
  }
  return value as ReconversionReport;
};

const latestReconversionReportPath = async (
  agent: RemoteAgent,
  path: string
): Promise<string | undefined> => {
  const separator = Math.max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
  const directory = path.slice(0, separator);
  const prefix = path.slice(separator + 1) + '.';
  const latest = (await agent.files.readdir(directory))
    .filter(
      (entry) =>
        entry.type === 'file' &&
        entry.name.startsWith(prefix) &&
        /^[0-9]{10}$/.test(entry.name.slice(prefix.length))
    )
    .map((entry) => entry.name)
    .sort()
    .at(-1);
  return latest === undefined ? undefined : remoteJoin(directory, latest);
};

const readReconversionReport = async (
  agent: RemoteAgent,
  path: string
): Promise<ReconversionReport> => {
  const snapshot = await latestReconversionReportPath(agent, path);
  if (snapshot === undefined)
    throw new Error('No completed reconversion report.');
  return parseReconversionReport(await agent.files.readFile(snapshot));
};

const waitForReconversionReport = async (
  agent: RemoteAgent,
  path: string,
  predicate: (report: ReconversionReport) => boolean,
  timeoutMs: number
): Promise<ReconversionReport> => {
  const deadline = Date.now() + timeoutMs;
  let lastReport: ReconversionReport | undefined;
  let lastReadError: unknown;

  while (Date.now() < deadline) {
    try {
      lastReport = await readReconversionReport(agent, path);
      lastReadError = undefined;
      if (predicate(lastReport)) {
        return lastReport;
      }
    } catch (error) {
      lastReadError = error;
    }
    await delay(100);
  }
  throw new Error(
    `Timed out waiting for Win32 reconversion report ${path}; last=${JSON.stringify(lastReport)}`,
    { cause: lastReadError }
  );
};

const startReconversion = async (
  agent: RemoteAgent,
  reportPath: string
): Promise<ReconversionReport> => {
  await agent.keyboard.press('F2');
  return await waitForReconversionReport(
    agent,
    reportPath,
    (report) =>
      report.lastAction === 'ok' && report.reconversionState === 'active',
    10_000
  );
};

const resetReconversionDocument = async (
  agent: RemoteAgent,
  reportPath: string,
  previousRevision: number
): Promise<ReconversionReport> => {
  await agent.keyboard.press('F5');
  return await waitForReconversionReport(
    agent,
    reportPath,
    (report) =>
      report.lastAction === 'reset' &&
      report.documentRevision > previousRevision &&
      report.reconversionState === 'none',
    10_000
  );
};

const imeCases = [
  {
    name: 'software amd64',
    architecture: 'amd64',
    executable: 'boxrig_term.exe',
    renderer: 'win32',
    probe: 'boxrig_term_ime_probe.exe',
  },
  {
    name: 'software i686',
    architecture: 'i686',
    executable: 'boxrig_term.exe',
    renderer: 'win32',
    probe: 'boxrig_term_ime_probe.exe',
  },
  {
    name: 'OpenGL amd64',
    architecture: 'amd64',
    executable: 'boxrig_term.exe',
    renderer: 'opengl',
    probe: 'boxrig_term_ogl_ime_probe.exe',
  },
  {
    name: 'OpenGL i686',
    architecture: 'i686',
    executable: 'boxrig_term.exe',
    renderer: 'opengl',
    probe: 'boxrig_term_ogl_ime_probe.exe',
  },
  {
    name: 'D3D11 amd64',
    architecture: 'amd64',
    executable: 'boxrig_term.exe',
    renderer: 'd3d11',
    probe: 'boxrig_term_d3d11_ime_probe.exe',
  },
  {
    name: 'D3D11 i686',
    architecture: 'i686',
    executable: 'boxrig_term.exe',
    renderer: 'd3d11',
    probe: 'boxrig_term_d3d11_ime_probe.exe',
  },
];

describe('Win32 real MS-IME through agent-rover', () => {
  it.each(imeCases)('commits MS-IME text ($name)', async (sample) => {
    const architecture = sample.architecture;
    const executable = sample.executable;
    const configuration = createConfiguration();
    const agent = await connectRemoteAgent({
      timeoutMs: 30_000,
    });
    let remoteDirectory: string | undefined;
    let managedProcess: RemoteManagedProcess | undefined;
    let watchdog: RemoteApplicationProcess | undefined;
    let readyPath: string | undefined;
    let stage = 'initialize remote application';

    try {
      const capabilities = await agent.capabilities();

      expect(capabilities.platform).toBe('windows');
      expect(capabilities.features).toContain('agent.native-windows');
      remoteDirectory = await agent.files.mkdtemp(`boxrig-p2-${Date.now()}-`);
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
          'build/win32/amd64/tests/p2/win32_ime_child.exe',
          'build/win32/amd64/tests/p2/win32_ime_control.exe',
        ],
        localPath: configuration.sourceDirectory,
        mode: 'mirror',
        remotePath: remoteDirectory,
      });

      const applicationPath = remoteJoin(
        remoteDirectory,
        `build/boxrig_term/win32/${architecture}/${executable}`
      );
      const controlPath = remoteJoin(
        remoteDirectory,
        'build/win32/amd64/tests/p2/win32_ime_control.exe'
      );
      const resultPath = remoteJoin(remoteDirectory, 'result.bin');
      readyPath = remoteJoin(remoteDirectory, 'ready.flag');

      managedProcess = await agent.processes.launchManaged({
        arguments: [
          `--renderer=${sample.renderer}`,
          '--',
          'build\\win32\\amd64\\tests\\p2\\win32_ime_child.exe',
          'result.bin',
          'ready.flag',
        ],
        // Observe ConPTY result files and preserve native diagnostics for D3D11.
        captureStdout: true,
        captureStderr: true,
        path: applicationPath,
        workingDirectory: remoteDirectory,
      });
      if (sample.renderer === 'd3d11') {
        // Managed launch enumerates windows first; this guard must start before
        // any query can wait on the sample's UI thread.
        watchdog = await agent.applications.launch({
          path: controlPath,
          arguments: ['watchdog', String(managedProcess.id)],
          createNoWindow: true,
        });
      }
      await waitForRemoteFile(agent, readyPath, 10_000);
      await waitForRemoteFile(agent, resultPath, 10_000);
      const readiness = decodeUtf8(
        await readStableBytes(agent, readyPath, 1, 3_000)
      );

      expect(readiness).toBe('ready\n');
      const runtime = await managedProcess.stdoutText();
      await mkdir(configuration.diagnosticsDirectory, { recursive: true });
      await writeFile(
        resolve(
          configuration.diagnosticsDirectory,
          architecture + '-' + sample.renderer + '-runtime.log'
        ),
        runtime
      );
      expect(runtime).toContain(`boxrig_term: renderer=${sample.renderer}`);
      expect(runtime.match(/boxrig_term: renderer=/gu)).toHaveLength(1);
      let window: AppWindow;
      try {
        window = await managedProcess.waitForWindow(
          {
            className: 'boxrig_win32_sample',
            strict: true,
            visible: true,
          },
          {
            message: 'Timed out waiting for the real boxrig Win32 window.',
            timeoutMs: 20_000,
          }
        );
      } catch (error) {
        const snapshot = await managedProcess.snapshot();
        const stdout = await managedProcess.stdoutText();
        const stderr = await managedProcess.stderrText();

        throw new Error(
          `boxrig exited before opening its window: ${JSON.stringify(snapshot)}; stdout=${JSON.stringify(stdout)}; stderr=${JSON.stringify(stderr)}`,
          { cause: error }
        );
      }
      if (sample.renderer === 'd3d11') {
        try {
          await runControl(
            agent,
            controlPath,
            remoteDirectory,
            'responsive',
            managedProcess.id
          );
        } catch (error) {
          await managedProcess.kill();
          throw error;
        }
      }
      stage = 'focus initial window';
      window = await focusWindow(agent, window);
      stage = 'resize initial window';
      window = await window.setBounds({
        height: 520,
        width: 760,
        x: 80,
        y: 80,
      });
      await window.waitForStableBounds({
        stableIterations: 3,
        timeoutMs: 10_000,
      });
      const initial = await readStableBytes(agent, resultPath, 0, 3_000);
      expect(initial.length).toBe(0);
      // IME cancellation compares complete screenshots. Observe the idle scene
      // across cursor periods so unrelated animation cannot satisfy or break it.
      stage = 'verify a stable IME comparison baseline';
      const videoDirectory = resolve(
        configuration.sourceDirectory,
        'build/test-results/step23-ime-stability',
        architecture,
        sample.renderer,
        String(Date.now())
      );
      await mkdir(videoDirectory, { recursive: true });
      const recordStablePresentation = async (name: string): Promise<void> => {
        const client = (await window.refresh()).clientBounds;
        const videoRect = { x: client.x, y: client.y, width: 200, height: 60 };
        const baseline = await agent.screenshot({ rect: videoRect });
        expect(baseline.clipped).toBe(false);
        const reference = PNG.sync.read(baseline.image);
        await writeFile(
          resolve(videoDirectory, name + '-reference.png'),
          baseline.image
        );
        const videoPath = resolve(videoDirectory, name + '.mp4');
        const video = await agent.recordVideo(2200, videoPath, {
          fps: 15,
          rect: videoRect,
        });
        expect(video.clipped).toBe(false);
        const raw = execFileSync(
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
          { maxBuffer: videoRect.width * videoRect.height * 3 * 100 }
        );
        const stride = videoRect.width * videoRect.height * 3;
        expect(raw.length % stride).toBe(0);
        const frames = raw.length / stride;
        expect(frames).toBe(video.frameCount);
        expect(frames).toBeGreaterThanOrEqual(30);
        let maximumError = 0;
        for (let frame = 0; frame < frames; frame++)
          maximumError = Math.max(
            maximumError,
            cursorVideoStaticError(
              raw.subarray(frame * stride, (frame + 1) * stride),
              reference.data,
              videoRect.width,
              videoRect.height,
              { x: 0, y: 0, width: 0, height: 0 }
            )
          );
        await writeFile(
          resolve(videoDirectory, name + '-results.json'),
          JSON.stringify(
            {
              arch: architecture,
              renderer: sample.renderer,
              frames,
              maximumError,
              threshold: 48,
              rect: videoRect,
              status: maximumError <= 48 ? 'passed' : 'failed',
            },
            null,
            2
          ) + '\n'
        );
        expect(
          maximumError,
          name + ': IME presentation must remain unchanged'
        ).toBeLessThanOrEqual(48);
      };
      await recordStablePresentation('idle');
      stage = 'activate Japanese IME';
      const imeOpenedByTest = await activateJapaneseIme(
        agent,
        window,
        controlPath,
        remoteDirectory,
        managedProcess.id
      );

      await pressPhysicalAscii(agent, 'kanji');
      await delay(400);
      expect((await agent.files.readFile(resultPath)).length).toBe(0);
      const readingCapture = await window.screenshot();
      await recordStablePresentation('preedit');
      await agent.keyboard.press('Space');
      await delay(700);
      const convertedCapture = await window.screenshot();
      expect(convertedCapture.image.equals(readingCapture.image)).toBe(false);
      await agent.keyboard.press('Enter');
      const firstCommit = await readStableBytes(agent, resultPath, 1, 10_000);
      const firstJapaneseCommit = expectJapaneseCommit(firstCommit, 0);

      expect(firstJapaneseCommit).toMatch(/[\u3400-\u9fff]/u);

      const secondStart = firstCommit.length;
      await pressPhysicalAscii(agent, 'watashihanihonjin');
      await delay(300);
      expect((await agent.files.readFile(resultPath)).length).toBe(secondStart);
      await focusWindow(agent, window);
      await runControl(
        agent,
        controlPath,
        remoteDirectory,
        'convert',
        managedProcess.id
      );
      await delay(700);
      stage = 'change converted clause';
      const clauseBefore = await window.screenshot();
      await agent.keyboard.press('ArrowLeft', { modifiers: ['Shift'] });
      await delay(500);
      const clauseAfter = await window.screenshot();
      expect(clauseAfter.image.equals(clauseBefore.image)).toBe(false);
      stage = 'open conversion candidates';
      const candidatesClosed = await agent.screenshot();
      // MS-IME opens the conversion candidate window with Space; arrow keys
      // navigate it only after it opens. Clause resizing can close that window.
      // https://support.microsoft.com/en-us/windows/hardware/input-devices/microsoft-japanese-ime
      await agent.keyboard.press('Space');
      await expect
        .poll(
          async () =>
            (await agent.screenshot()).image.equals(candidatesClosed.image),
          { timeout: 10_000 }
        )
        .toBe(false);
      stage = 'select a conversion candidate';
      const candidateBefore = await agent.screenshot();
      await agent.keyboard.press('ArrowDown');
      await expect
        .poll(
          async () =>
            (await agent.screenshot()).image.equals(candidateBefore.image),
          { timeout: 10_000 }
        )
        .toBe(false);
      await agent.keyboard.press('Enter');
      const secondCommit = await readStableBytes(
        agent,
        resultPath,
        secondStart + 1,
        10_000
      );
      expectJapaneseCommit(secondCommit, secondStart);

      stage = 'cancel converted IME composition with Escape';
      const escapeCancelStart = secondCommit.length;
      const escapeCancelBaseline = await window.screenshot();
      await pressPhysicalAscii(agent, 'torikeshi');
      await delay(400);
      const escapeReading = await window.screenshot();
      expect(escapeReading.image.equals(escapeCancelBaseline.image)).toBe(
        false
      );
      await agent.keyboard.press('Space');
      await delay(700);
      const escapeConverted = await window.screenshot();
      expect(escapeConverted.image.equals(escapeReading.image)).toBe(false);
      await agent.keyboard.press('Escape');
      await delay(500);
      const escapeReverted = await window.screenshot();
      expect(escapeReverted.image.equals(escapeCancelBaseline.image)).toBe(
        false
      );
      expect(escapeReverted.image.equals(escapeConverted.image)).toBe(false);
      await agent.keyboard.press('Escape');
      await delay(500);
      const escapeCanceled = await window.screenshot();
      expect(escapeCanceled.image.equals(escapeCancelBaseline.image)).toBe(
        true
      );
      expect((await agent.files.readFile(resultPath)).length).toBe(
        escapeCancelStart
      );

      await recordStablePresentation('cancelled');
      const cancelStart = secondCommit.length;
      await pressPhysicalAscii(agent, 'torikeshi');
      await delay(300);
      stage = 'minimize with an active composition';
      window = await window.minimize();
      await delay(500);
      stage = 'restore after focus-loss composition cancellation';
      window = await window.restore();
      window = await focusWindow(agent, window);
      await delay(500);
      expect((await agent.files.readFile(resultPath)).length).toBe(cancelStart);

      await pressPhysicalAscii(agent, 'saizu');
      await delay(300);
      expect((await agent.files.readFile(resultPath)).length).toBe(cancelStart);
      window = await window.setBounds({
        height: 440,
        width: 680,
        x: 120,
        y: 100,
      });
      await window.waitForStableBounds({
        stableIterations: 3,
        timeoutMs: 10_000,
      });
      await agent.keyboard.press('Enter');
      const resizedCommit = await readStableBytes(
        agent,
        resultPath,
        cancelStart + 1,
        10_000
      );
      expectJapaneseCommit(resizedCommit, cancelStart);

      const unsupportedReconversionBefore = Buffer.from(resizedCommit);
      await focusWindow(agent, window);
      await runControl(
        agent,
        controlPath,
        remoteDirectory,
        'convert',
        managedProcess.id
      );
      await delay(700);
      const unsupportedReconversionAfter = await readStableBytes(
        agent,
        resultPath,
        unsupportedReconversionBefore.length,
        3_000
      );
      expect(
        unsupportedReconversionAfter.equals(unsupportedReconversionBefore)
      ).toBe(true);

      const finalStart = unsupportedReconversionAfter.length;
      await pressPhysicalAscii(agent, 'owari');
      await delay(300);
      expect((await agent.files.readFile(resultPath)).length).toBe(finalStart);
      await agent.keyboard.press('Enter');
      const finalCommit = await readStableBytes(
        agent,
        resultPath,
        finalStart + 1,
        10_000
      );
      expectJapaneseCommit(finalCommit, finalStart);

      if (imeOpenedByTest) {
        await runControl(
          agent,
          controlPath,
          remoteDirectory,
          'toggle',
          managedProcess.id
        );
        await delay(500);
      }
      await agent.keyboard.press('d', { modifiers: ['Control'] });
      const exited = await managedProcess.waitForExit({
        message: 'The real boxrig Win32 child did not exit after Ctrl+D.',
        timeoutMs: 15_000,
      });
      expect(exited.root.exitCode).toBe(0);
      console.log(
        'PASS: real Win32 MS-IME input, conversion, clauses, candidates, focus, resize, safe unsupported reconversion, and child exit'
      );
    } catch (error) {
      let processState = 'managed process was not launched';
      let childState = 'ready file was not created';

      if (managedProcess !== undefined) {
        try {
          processState = JSON.stringify(await managedProcess.snapshot());
        } catch (snapshotError) {
          processState = `snapshot failed: ${String(snapshotError)}`;
        }
      }
      if (readyPath !== undefined) {
        try {
          childState = decodeUtf8(await agent.files.readFile(readyPath)).trim();
        } catch (readyError) {
          childState = `ready read failed: ${String(readyError)}`;
        }
      }
      try {
        await saveDiagnostics(
          `${configuration.diagnosticsDirectory}-${Date.now()}`,
          {
            agent,
            attachments:
              managedProcess === undefined
                ? []
                : [
                    {
                      kind: 'managedProcess',
                      name: 'boxrig-term',
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
        console.error(
          'Failed to save agent-rover diagnostics.',
          diagnosticsError
        );
      }
      throw new Error(
        `Win32 IME P2 failed at ${stage}; process=${processState}; child=${JSON.stringify(childState)}`,
        { cause: error }
      );
    } finally {
      try {
        if (managedProcess !== undefined) {
          try {
            if (sample.renderer === 'd3d11') {
              await mkdir(configuration.diagnosticsDirectory, {
                recursive: true,
              });
              await writeFile(
                resolve(
                  configuration.diagnosticsDirectory,
                  architecture + '-' + sample.renderer + '-native.log'
                ),
                await managedProcess.stderrText()
              );
            }
          } finally {
            await managedProcess.releaseAsync();
          }

          if (watchdog !== undefined) {
            await agent.processes.waitForExit(watchdog.id, {
              timeoutMs: 10000,
            });
          }
        }
        if (remoteDirectory !== undefined) {
          const deadline = Date.now() + 10_000;
          for (;;) {
            try {
              await agent.files.remove(remoteDirectory, { recursive: true });
              break;
            } catch (error) {
              // An exited image can remain locked after managed process release.
              // Require deletion itself to succeed; do not assume a fixed delay
              // released the file, or hide permanent access/cleanup failures.
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
  });

  it.each(imeCases)('moves MS-IME popup ($name)', async (sample) => {
    const architecture = sample.architecture;
    const executable = sample.probe;
    const configuration = createConfiguration();
    const agent = await connectRemoteAgent({
      timeoutMs: 30_000,
    });
    let remoteDirectory: string | undefined;
    let managedProcess: RemoteManagedProcess | undefined;
    let watchdog: RemoteApplicationProcess | undefined;
    let interaction: RemoteInteractionSession | undefined;
    let readyPath: string | undefined;
    let stage = 'initialize IME movement probe';

    try {
      const capabilities = await agent.capabilities();

      expect(capabilities.platform).toBe('windows');
      expect(capabilities.features).toContain('agent.native-windows');
      remoteDirectory = await agent.files.mkdtemp(
        `boxrig-ime-movement-${Date.now()}-`
      );
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
          'build/win32/amd64/tests/p2/win32_ime_child.exe',
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
      const controlPath = remoteJoin(
        remoteDirectory,
        'build/win32/amd64/tests/p2/win32_ime_control.exe'
      );
      const resultPath = remoteJoin(remoteDirectory, 'movement-result.bin');
      const placementPath = remoteJoin(
        remoteDirectory,
        'movement-placement.json'
      );
      readyPath = remoteJoin(remoteDirectory, 'movement-ready.flag');

      managedProcess = await agent.processes.launchManaged({
        arguments: [
          '--',
          'build\\win32\\amd64\\tests\\p2\\win32_ime_child.exe',
          'movement-result.bin',
          'movement-ready.flag',
        ],
        // Observe ConPTY result files and preserve native diagnostics for D3D11.
        captureStderr: executable.includes('_d3d11'),
        environment: {
          BOXRIG_P2_IME_PLACEMENT_REPORT: 'movement-placement.json',
        },
        path: applicationPath,
        workingDirectory: remoteDirectory,
      });
      if (executable.includes('_d3d11')) {
        // Managed launch enumerates windows first; this guard must start before
        // any query can wait on the sample's UI thread.
        watchdog = await agent.applications.launch({
          path: controlPath,
          arguments: ['watchdog', String(managedProcess.id)],
          createNoWindow: true,
        });
      }
      await waitForRemoteFile(agent, readyPath, 10_000);
      await waitForRemoteFile(agent, resultPath, 10_000);
      let window = await managedProcess.waitForWindow(
        {
          className: 'boxrig_win32_sample',
          strict: true,
          visible: true,
        },
        {
          message: 'Timed out waiting for the Win32 IME movement probe.',
          timeoutMs: 20_000,
        }
      );

      const initialRequest = {
        height: 520,
        width: 760,
        x: 80,
        y: 80,
      };
      const finalRequest = {
        ...initialRequest,
        x: initialRequest.x + 280,
        y: initialRequest.y + 190,
      };

      stage = 'prepare matching candidate-free pixel baselines';
      if (executable.includes('_d3d11')) {
        try {
          await runControl(
            agent,
            controlPath,
            remoteDirectory,
            'responsive',
            managedProcess.id
          );
        } catch (error) {
          await managedProcess.kill();
          throw error;
        }
      }
      window = await window.setBounds(initialRequest);
      window = await window.waitForStableBounds({
        stableIterations: 3,
        timeoutMs: 10_000,
      });
      window = await focusWindow(agent, window);
      const imeOpenedByTest = await activateJapaneseIme(
        agent,
        window,
        controlPath,
        remoteDirectory,
        managedProcess.id
      );
      const firstWindow = await window.refresh();

      window = await window.setBounds(finalRequest);
      window = await window.waitForStableBounds({
        stableIterations: 3,
        timeoutMs: 10_000,
      });
      const finalWindow = await window.refresh();
      const nativeImeWindow = await findNativeImePresentationWindow(
        agent,
        finalWindow.bounds
      );

      expect(finalWindow.bounds.width).toBe(firstWindow.bounds.width);
      expect(finalWindow.bounds.height).toBe(firstWindow.bounds.height);
      const movement = {
        x: finalWindow.bounds.x - firstWindow.bounds.x,
        y: finalWindow.bounds.y - firstWindow.bounds.y,
      };

      expect(movement.x).toBeGreaterThan(0);
      expect(movement.y).toBeGreaterThan(0);
      await delay(300);
      const movedBaseline = await captureNativeImeSurface(
        agent,
        nativeImeWindow
      );

      window = await window.setBounds(initialRequest);
      window = await window.waitForStableBounds({
        stableIterations: 3,
        timeoutMs: 10_000,
      });
      window = await focusWindow(agent, window);
      expect(window.bounds).toEqual(firstWindow.bounds);
      await delay(300);
      const initialBaseline = await captureNativeImeSurface(
        agent,
        nativeImeWindow
      );
      expect((await agent.files.readFile(resultPath)).length).toBe(0);

      stage = 'show the real MS-IME candidate popup';
      await pressPhysicalAscii(agent, 'kanji');
      let initialPlacement = await waitForImePlacementReport(
        agent,
        placementPath,
        (report) => report.kind === 'frame' && report.active && report.visible,
        10_000
      );

      expect(initialPlacement.status).toBeGreaterThanOrEqual(0);
      expect(initialPlacement.screenExclude[2]).toBeGreaterThan(0);
      expect(initialPlacement.screenExclude[3]).toBeGreaterThan(0);
      expect(initialPlacement.screenSpot[0]).toBe(
        initialPlacement.screenOrigin[0] + initialPlacement.clientSpot[0]
      );
      expect(initialPlacement.screenSpot[1]).toBe(
        initialPlacement.screenOrigin[1] + initialPlacement.clientSpot[1]
      );
      await delay(500);
      initialPlacement = await waitForImePlacementReport(
        agent,
        placementPath,
        (report) =>
          report.kind === 'frame' &&
          report.active &&
          report.visible &&
          report.sequence >= initialPlacement.sequence,
        10_000
      );
      const initialCandidateCapture = await captureNativeImeSurface(
        agent,
        nativeImeWindow
      );

      stage = 'hold a physical title-bar drag with the candidate visible';
      interaction = await agent.interaction.start({ restoreCursor: true });
      const dragStart = {
        x: firstWindow.bounds.x + Math.floor(firstWindow.bounds.width / 2),
        y: firstWindow.bounds.y + 15,
      };
      const dragEnd = {
        x: dragStart.x + movement.x,
        y: dragStart.y + movement.y,
      };

      await interaction.mouse.move(dragStart);
      await interaction.mouse.down({ button: 'left' });
      await interaction.mouse.move(dragEnd);
      const heldWindow = await waitForExactWindowBounds(
        managedProcess,
        window,
        finalWindow.bounds
      );
      const heldPlacement = await waitForImePlacementReport(
        agent,
        placementPath,
        (report) =>
          report.kind === 'move' &&
          report.sequence > initialPlacement.sequence &&
          Math.abs(
            report.screenSpot[0] - initialPlacement.screenSpot[0] - movement.x
          ) <= 2 &&
          Math.abs(
            report.screenSpot[1] - initialPlacement.screenSpot[1] - movement.y
          ) <= 2,
        10_000
      );

      expect(heldPlacement.active).toBe(true);
      expect(heldPlacement.visible).toBe(true);
      expect(heldPlacement.status).toBeGreaterThanOrEqual(0);
      expect(heldPlacement.clientSpot).toEqual(initialPlacement.clientSpot);
      expect(heldPlacement.revision).toBe(initialPlacement.revision);
      await delay(300);
      const heldCandidateCapture = await captureNativeImeSurface(
        agent,
        nativeImeWindow
      );

      stage = 'release the physical title-bar drag with the candidate visible';
      await interaction.mouse.up({ button: 'left' });
      const releasedWindow = await waitForExactWindowBounds(
        managedProcess,
        heldWindow,
        finalWindow.bounds
      );
      const releasedPlacement = await waitForImePlacementReport(
        agent,
        placementPath,
        (report) =>
          report.kind === 'move' &&
          report.sequence > heldPlacement.sequence &&
          report.screenSpot[0] === heldPlacement.screenSpot[0] &&
          report.screenSpot[1] === heldPlacement.screenSpot[1],
        10_000
      );

      expect(releasedPlacement.active).toBe(true);
      expect(releasedPlacement.visible).toBe(true);
      expect(releasedPlacement.status).toBeGreaterThanOrEqual(0);
      expect(releasedPlacement.clientSpot).toEqual(initialPlacement.clientSpot);
      expect(releasedPlacement.revision).toBe(initialPlacement.revision);
      await delay(300);
      const releasedCandidateCapture = await captureNativeImeSurface(
        agent,
        nativeImeWindow
      );
      await interaction.releaseAsync();
      interaction = undefined;

      stage = 'measure runtime candidate pixels and placement geometry';
      const initialPopup = detectCandidatePopup(
        initialCandidateCapture,
        initialBaseline,
        initialPlacement.screenSpot,
        initialPlacement.screenSpot
      );
      const heldPopup = detectCandidatePopup(
        heldCandidateCapture,
        movedBaseline,
        initialPlacement.screenSpot,
        heldPlacement.screenSpot
      );
      const releasedPopup = detectCandidatePopup(
        releasedCandidateCapture,
        movedBaseline,
        initialPlacement.screenSpot,
        releasedPlacement.screenSpot
      );
      const nativeBounds = (await nativeImeWindow.refresh()).bounds;

      for (const popup of [initialPopup, heldPopup, releasedPopup]) {
        expect(rectangleContainsPoint(nativeBounds, popup.bounds)).toBe(true);
        expect(
          rectangleContainsPoint(nativeBounds, {
            x: popup.bounds.x + popup.bounds.width - 1,
            y: popup.bounds.y + popup.bounds.height - 1,
          })
        ).toBe(true);
      }
      expect(
        Math.abs(initialPopup.bounds.x - initialPlacement.screenSpot[0])
      ).toBeLessThanOrEqual(320);
      expect(
        Math.abs(initialPopup.bounds.y - initialPlacement.screenSpot[1])
      ).toBeLessThanOrEqual(320);
      expectNear(heldPopup.bounds.x - initialPopup.bounds.x, movement.x, 4);
      expectNear(heldPopup.bounds.y - initialPopup.bounds.y, movement.y, 4);
      expectNear(releasedPopup.bounds.x - initialPopup.bounds.x, movement.x, 4);
      expectNear(releasedPopup.bounds.y - initialPopup.bounds.y, movement.y, 4);
      expectNear(
        heldPopup.bounds.x - heldPlacement.screenSpot[0],
        initialPopup.bounds.x - initialPlacement.screenSpot[0],
        4
      );
      expectNear(
        heldPopup.bounds.y - heldPlacement.screenSpot[1],
        initialPopup.bounds.y - initialPlacement.screenSpot[1],
        4
      );
      expectNear(
        releasedPopup.bounds.x - releasedPlacement.screenSpot[0],
        initialPopup.bounds.x - initialPlacement.screenSpot[0],
        4
      );
      expectNear(
        releasedPopup.bounds.y - releasedPlacement.screenSpot[1],
        initialPopup.bounds.y - initialPlacement.screenSpot[1],
        4
      );
      expectNear(heldPopup.bounds.width, initialPopup.bounds.width, 4);
      expectNear(heldPopup.bounds.height, initialPopup.bounds.height, 4);
      expectNear(releasedPopup.bounds.width, initialPopup.bounds.width, 4);
      expectNear(releasedPopup.bounds.height, initialPopup.bounds.height, 4);

      stage = 'select and commit a candidate after movement';
      await agent.keyboard.press('ArrowDown');
      await delay(300);
      await agent.keyboard.press('Enter');
      const committed = await readStableBytes(agent, resultPath, 1, 10_000);

      expectJapaneseCommit(committed, 0);
      if (imeOpenedByTest) {
        await runControl(
          agent,
          controlPath,
          remoteDirectory,
          'toggle',
          managedProcess.id
        );
        await delay(500);
      }
      await agent.keyboard.press('d', { modifiers: ['Control'] });
      const exited = await managedProcess.waitForExit({
        message: 'The IME movement probe did not exit after Ctrl+D.',
        timeoutMs: 15_000,
      });

      expect(exited.root.exitCode).toBe(0);
      console.log(
        JSON.stringify({
          candidate: {
            held: heldPopup,
            initial: initialPopup,
            released: releasedPopup,
          },
          caret: {
            held: heldPlacement.screenSpot,
            initial: initialPlacement.screenSpot,
            released: releasedPlacement.screenSpot,
          },
          movement,
          nativeImeWindow: {
            bounds: nativeBounds,
            className: nativeImeWindow.className,
            process: nativeImeWindow.process.name,
          },
        })
      );
    } catch (error) {
      try {
        await saveDiagnostics(
          `${configuration.diagnosticsDirectory}-movement-${Date.now()}`,
          {
            agent,
            attachments:
              managedProcess === undefined
                ? []
                : [
                    {
                      kind: 'managedProcess',
                      name: 'boxrig-ime-movement',
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
        console.error(
          'Failed to save IME movement diagnostics.',
          diagnosticsError
        );
      }
      throw new Error(`Win32 IME movement P2 failed at ${stage}.`, {
        cause: error,
      });
    } finally {
      try {
        if (interaction !== undefined) {
          await interaction.releaseAsync();
        }
        if (managedProcess !== undefined) {
          try {
            if (executable.includes('_d3d11')) {
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
            await agent.processes.waitForExit(watchdog.id, {
              timeoutMs: 10000,
            });
          }
        }
        if (remoteDirectory !== undefined) {
          await agent.files.remove(remoteDirectory, { recursive: true });
        }
      } finally {
        agent.release();
      }
    }
  });

  it('reconverts one application-owned range transactionally', async () => {
    const configuration = createConfiguration();
    const agent = await connectRemoteAgent({
      timeoutMs: 30_000,
    });
    let remoteDirectory: string | undefined;
    let managedProcess: RemoteManagedProcess | undefined;
    let reportPath: string | undefined;
    let stage = 'initialize reconversion host';

    try {
      const capabilities = await agent.capabilities();

      expect(capabilities.platform).toBe('windows');
      expect(capabilities.features).toContain('agent.native-windows');
      remoteDirectory = await agent.files.mkdtemp(
        `boxrig-reconversion-p2-${Date.now()}-`
      );
      await agent.files.syncDirectory({
        checksum: 'sha256',
        deleteExtraneous: true,
        exclude: ['build/wine*', 'build/tests/wine*', 'build/test-results'],
        include: [
          'build/win32/amd64/tests/p2/win32_reconversion_host.exe',
          'build/win32/amd64/tests/p2/win32_ime_control.exe',
        ],
        localPath: configuration.sourceDirectory,
        mode: 'mirror',
        remotePath: remoteDirectory,
      });

      const applicationPath = remoteJoin(
        remoteDirectory,
        'build/win32/amd64/tests/p2/win32_reconversion_host.exe'
      );
      const controlPath = remoteJoin(
        remoteDirectory,
        'build/win32/amd64/tests/p2/win32_ime_control.exe'
      );
      reportPath = remoteJoin(remoteDirectory, 'reconversion-report.json');
      managedProcess = await agent.processes.launchManaged({
        arguments: ['reconversion-report.json'],
        captureStderr: true,
        captureStdout: true,
        path: applicationPath,
        workingDirectory: remoteDirectory,
      });
      const initial = await waitForReconversionReport(
        agent,
        reportPath,
        (report) => report.ready,
        10_000
      );
      let window: AppWindow;
      try {
        window = await managedProcess.waitForWindow(
          {
            className: 'boxrig_reconversion_host',
            strict: true,
            visible: true,
          },
          {
            message: 'Timed out waiting for the Win32 reconversion host.',
            timeoutMs: 20_000,
          }
        );
      } catch (error) {
        const snapshot = await managedProcess.snapshot();
        const stdout = await managedProcess.stdoutText();
        const stderr = await managedProcess.stderrText();

        throw new Error(
          `Reconversion host exited before opening its window: ${JSON.stringify(snapshot)}; stdout=${JSON.stringify(stdout)}; stderr=${JSON.stringify(stderr)}`,
          { cause: error }
        );
      }
      window = await focusWindow(agent, window);
      window = await window.setBounds({
        height: 520,
        width: 760,
        x: 80,
        y: 80,
      });
      await window.waitForStableBounds({
        stableIterations: 3,
        timeoutMs: 10_000,
      });
      const imeOpenedByTest = await activateJapaneseIme(
        agent,
        window,
        controlPath,
        remoteDirectory,
        managedProcess.id
      );
      const ready = await waitForReconversionReport(
        agent,
        reportPath,
        (report) => report.ready && report.reconversionCapable,
        10_000
      );

      expect(ready.schema).toBe(1);
      expect(ready.documentIdentity).toBe(7001);
      expect(ready.documentRevision).toBe(initial.documentRevision);
      expect(ready.contextOffset).toBe(100);
      expect(ready.context).toBe('before 日本語 after');
      expect(ready.selectionOffset).toBe(107);
      expect(ready.selectionLength).toBe(9);
      expect(ready.selectedText).toBe('日本語');
      expect(ready.replacementCount).toBe(0);
      expect(ready.terminalOutput).toBe('');

      stage = 'accept a reconverted selection';
      await startReconversion(agent, reportPath);
      await agent.keyboard.press('Space');
      await delay(600);
      await agent.keyboard.press('Enter');
      const accepted = await waitForReconversionReport(
        agent,
        reportPath,
        (report) =>
          report.replacementCount === 1 &&
          report.lastOutcome === 'accepted' &&
          report.reconversionState === 'none',
        10_000
      );

      expect(accepted.documentIdentity).toBe(ready.documentIdentity);
      expect(accepted.documentRevision).toBe(ready.documentRevision + 1);
      expect(accepted.lastRequestId).toBeGreaterThan(0);
      expect(accepted.lastBaseRevision).toBe(ready.documentRevision);
      expect(accepted.lastRangeOffset).toBe(ready.selectionOffset);
      expect(accepted.lastRangeLength).toBe(ready.selectionLength);
      expect(accepted.lastReplacement).toMatch(/[\u3040-\u30ff\u3400-\u9fff]/u);
      expect(accepted.selectedText).toBe(accepted.lastReplacement);
      expect(accepted.context).toBe(`before ${accepted.lastReplacement} after`);
      expect(accepted.terminalOutput).toBe('');

      stage = 'cancel reconversion with Escape';
      const cancelBaseline = await resetReconversionDocument(
        agent,
        reportPath,
        accepted.documentRevision
      );
      await startReconversion(agent, reportPath);
      await agent.keyboard.press('Escape');
      await delay(500);
      if (
        (await readReconversionReport(agent, reportPath)).reconversionState ===
        'active'
      ) {
        await agent.keyboard.press('Escape');
      }
      const canceled = await waitForReconversionReport(
        agent,
        reportPath,
        (report) => report.reconversionState === 'none',
        10_000
      );

      expect(canceled.documentRevision).toBe(cancelBaseline.documentRevision);
      expect(canceled.context).toBe(cancelBaseline.context);
      expect(canceled.replacementCount).toBe(accepted.replacementCount);
      expect(canceled.terminalOutput).toBe('');

      stage = 'cancel reconversion on focus loss';
      const focusBaseline = await resetReconversionDocument(
        agent,
        reportPath,
        canceled.documentRevision
      );
      await startReconversion(agent, reportPath);
      window = await window.minimize();
      const focusCanceled = await waitForReconversionReport(
        agent,
        reportPath,
        (report) => report.reconversionState === 'none',
        10_000
      );

      expect(focusCanceled.documentRevision).toBe(
        focusBaseline.documentRevision
      );
      expect(focusCanceled.context).toBe(focusBaseline.context);
      expect(focusCanceled.replacementCount).toBe(accepted.replacementCount);
      expect(focusCanceled.terminalOutput).toBe('');
      window = await window.restore();
      await window.waitForStableBounds({
        stableIterations: 3,
        timeoutMs: 10_000,
      });
      window = await focusWindow(agent, window);

      stage = 'commit reconversion after resize';
      const resizeBaseline = await resetReconversionDocument(
        agent,
        reportPath,
        focusCanceled.documentRevision
      );
      await startReconversion(agent, reportPath);
      await agent.keyboard.press('Space');
      await delay(500);
      window = await window.setBounds({
        height: 460,
        width: 680,
        x: 120,
        y: 100,
      });
      await window.waitForStableBounds({
        stableIterations: 3,
        timeoutMs: 10_000,
      });
      await agent.keyboard.press('Enter');
      const resized = await waitForReconversionReport(
        agent,
        reportPath,
        (report) =>
          report.replacementCount === accepted.replacementCount + 1 &&
          report.lastOutcome === 'accepted' &&
          report.reconversionState === 'none',
        10_000
      );

      expect(resized.documentRevision).toBe(
        resizeBaseline.documentRevision + 1
      );
      expect(resized.context).toBe(`before ${resized.lastReplacement} after`);
      expect(resized.lastBaseRevision).toBe(resizeBaseline.documentRevision);
      expect(resized.lastRangeOffset).toBe(resizeBaseline.selectionOffset);
      expect(resized.lastRangeLength).toBe(resizeBaseline.selectionLength);
      expect(resized.terminalOutput).toBe('');

      stage = 'invalidate an active reconversion revision';
      const staleBaseline = await resetReconversionDocument(
        agent,
        reportPath,
        resized.documentRevision
      );
      await startReconversion(agent, reportPath);
      await runControl(
        agent,
        controlPath,
        remoteDirectory,
        'change-revision',
        managedProcess.id
      );
      const stale = await waitForReconversionReport(
        agent,
        reportPath,
        (report) =>
          report.lastAction === 'revisionChanged' &&
          report.reconversionState === 'none' &&
          report.reconversionCapable,
        10_000
      );

      expect(stale.documentRevision).toBe(staleBaseline.documentRevision + 1);
      expect(stale.context).toBe(staleBaseline.context);
      expect(stale.replacementCount).toBe(resized.replacementCount);
      expect(stale.terminalOutput).toBe('');

      stage = 'reject one current replacement';
      const rejectBaseline = await resetReconversionDocument(
        agent,
        reportPath,
        stale.documentRevision
      );
      await agent.keyboard.press('F3');
      await waitForReconversionReport(
        agent,
        reportPath,
        (report) => report.lastAction === 'rejectNext',
        10_000
      );
      await startReconversion(agent, reportPath);
      await agent.keyboard.press('Space');
      await delay(500);
      await agent.keyboard.press('Enter');
      const rejected = await waitForReconversionReport(
        agent,
        reportPath,
        (report) =>
          report.replacementCount === resized.replacementCount + 1 &&
          report.lastOutcome === 'rejected' &&
          report.reconversionState === 'none',
        10_000
      );

      expect(rejected.documentRevision).toBe(rejectBaseline.documentRevision);
      expect(rejected.context).toBe(rejectBaseline.context);
      expect(rejected.lastBaseRevision).toBe(rejectBaseline.documentRevision);
      expect(rejected.terminalOutput).toBe('');

      stage = 'restore the normal IME commit sink';
      await pressPhysicalAscii(agent, 'owari');
      await delay(400);
      expect(
        (await readReconversionReport(agent, reportPath)).terminalOutput
      ).toBe('');
      await agent.keyboard.press('Enter');
      const normalCommit = await waitForReconversionReport(
        agent,
        reportPath,
        (report) => report.terminalOutput.length > 0,
        10_000
      );
      expect(normalCommit.terminalOutput).toMatch(
        /[\u3040-\u30ff\u3400-\u9fff]/u
      );
      expect(normalCommit.terminalOutput).not.toMatch(/[A-Za-z]{3}/u);
      expect(normalCommit.terminalOutput).not.toContain('\u001b');
      await delay(500);
      expect(
        (await readReconversionReport(agent, reportPath)).terminalOutput
      ).toBe(normalCommit.terminalOutput);

      if (imeOpenedByTest) {
        await runControl(
          agent,
          controlPath,
          remoteDirectory,
          'toggle',
          managedProcess.id
        );
        await delay(500);
      }
      await window.close();
      const exited = await managedProcess.waitForExit({
        message: 'The Win32 reconversion host did not exit after close.',
        timeoutMs: 15_000,
      });
      expect(exited.root.exitCode).toBe(0);
      console.log(
        'PASS: real Win32 MS-IME transactional reconversion, cancellation, stale handling, rejection, resize, focus, and sink restoration'
      );
    } catch (error) {
      let processState = 'managed process was not launched';

      if (managedProcess !== undefined) {
        try {
          processState = JSON.stringify(await managedProcess.snapshot());
        } catch (snapshotError) {
          processState = `snapshot failed: ${String(snapshotError)}`;
        }
      }
      try {
        await saveDiagnostics(
          `${configuration.diagnosticsDirectory}-reconversion-${Date.now()}`,
          {
            agent,
            attachments: [
              ...(managedProcess === undefined
                ? []
                : [
                    {
                      kind: 'managedProcess' as const,
                      name: 'reconversion-host',
                      process: managedProcess,
                    },
                  ]),
              ...(reportPath === undefined
                ? []
                : [
                    {
                      contentType: 'application/json',
                      kind: 'remoteFile' as const,
                      name: 'reconversion-report',
                      optional: true,
                      path:
                        (await latestReconversionReportPath(
                          agent,
                          reportPath
                        )) ?? reportPath,
                    },
                  ]),
            ],
            captureOptions: {
              eventLogs: { maxEntries: 50 },
              includeDescendants: true,
              maxDescendantDepth: 4,
            },
          }
        );
      } catch (diagnosticsError) {
        console.error(
          'Failed to save reconversion diagnostics.',
          diagnosticsError
        );
      }
      throw new Error(
        `Win32 reconversion P2 failed at ${stage}; process=${processState}`,
        { cause: error }
      );
    } finally {
      try {
        if (managedProcess !== undefined) {
          await managedProcess.releaseAsync();
        }
        if (remoteDirectory !== undefined) {
          await agent.files.remove(remoteDirectory, { recursive: true });
        }
      } finally {
        agent.release();
      }
    }
  });
});
