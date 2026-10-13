import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

import {
  saveDiagnostics,
  type AppWindow,
  type AppWindowScreenshot,
  type RemoteDesktop,
  type RemoteAgent,
  type RemoteManagedProcess,
} from 'agent-rover';
import { connectRemoteAgent } from './agent-connection.js';
import { delay } from 'async-primitives';
import { PNG } from 'pngjs';
import { describe, expect, it } from 'vitest';

interface TestConfiguration {
  readonly sourceDirectory: string;
  readonly diagnosticsDirectory: string;
}

interface VideoControllerMetadata {
  readonly Name: string;
  readonly DriverVersion: string;
  readonly PNPDeviceID: string;
}

interface HostMetadata {
  readonly osCaption: string;
  readonly osVersion: string;
  readonly osBuild: string;
  readonly sessionName: string | null;
  readonly sessionProtocol: number;
  readonly computerManufacturer: string;
  readonly computerModel: string;
  readonly videoControllers: readonly VideoControllerMetadata[];
}

interface WglProbeReport {
  readonly schemaVersion: number;
  readonly architecture: string;
  readonly environment: string;
  readonly classification: string;
  readonly failureStage: string;
  readonly failureCode: number;
  readonly wine: boolean;
  readonly remoteSession: boolean;
  readonly physicalHostAttested: boolean;
  readonly virtualHost: boolean;
  readonly legacyVendor: string;
  readonly legacyRenderer: string;
  readonly legacyVersion: string;
  readonly coreVendor: string;
  readonly coreRenderer: string;
  readonly coreVersion: string;
  readonly coreMajor: number;
  readonly coreMinor: number;
  readonly coreProfileMask: number;
  readonly wglExtensions: string;
  readonly extensionString: boolean;
  readonly pixelFormatExtension: boolean;
  readonly createContextExtension: boolean;
  readonly createContextProfileExtension: boolean;
  readonly swapControlExtension: boolean;
  readonly framebufferSrgbExtension: boolean;
  readonly robustnessExtension: boolean;
  readonly pixelFormat: number;
  readonly acceleration: number;
  readonly colorBits: number;
  readonly alphaBits: number;
  readonly depthBits: number;
  readonly stencilBits: number;
  readonly sampleBuffers: number;
  readonly samples: number;
  readonly drawToWindow: boolean;
  readonly supportOpenGL: boolean;
  readonly doubleBuffer: boolean;
  readonly srgbCapable: boolean;
  readonly pfdGenericFormat: boolean;
  readonly pfdGenericAccelerated: boolean;
  readonly requiredEntryPoints: boolean;
  readonly offscreenPixel: boolean;
  readonly offscreenPremultipliedAlpha: boolean;
  readonly nativeTransparency: boolean;
  readonly stateRestored: boolean;
  readonly runtimeRobustness: boolean;
  readonly successfulSwaps: number;
  readonly resizeSwap: boolean;
  readonly contextRecreated: boolean;
  readonly technicalCapability: boolean;
}

interface ProbeArchitecture {
  readonly name: 'amd64' | 'i686';
  readonly relativePath: string;
}

const architectures: readonly ProbeArchitecture[] = [
  {
    name: 'amd64',
    relativePath: 'build/win32/amd64/tests/p2/win32_opengl_probe.exe',
  },
  {
    name: 'i686',
    relativePath: 'build/win32/i686/tests/p2/win32_opengl_probe.exe',
  },
];

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
    '../../../build/test-results/p2-win32-wgl-probe'
  ),
});

const remoteJoin = (base: string, relative: string): string =>
  `${base}\\${relative.replaceAll('/', '\\')}`;

const terminateProcesses = async (
  agent: RemoteAgent,
  matches: (path: string) => boolean
): Promise<void> => {
  const processes = await agent.processes.list();
  const related = processes.filter((process) => matches(process.path));

  for (const process of related) {
    await agent.processes.kill(process.id);
  }
  for (const process of related) {
    await agent.processes.waitForExit(process.id, {
      message: `Timed out terminating ${process.path}.`,
      timeoutMs: 10_000,
    });
  }
};

const terminateStaleProbeProcesses = async (
  agent: RemoteAgent
): Promise<void> => {
  await terminateProcesses(agent, (path) => {
    const normalized = path.replaceAll('/', '\\').toLowerCase();

    return (
      normalized.includes('\\boxrig-wgl-') &&
      normalized.endsWith('\\win32_opengl_probe.exe')
    );
  });
};

const terminateDirectoryProcesses = async (
  agent: RemoteAgent,
  remoteDirectory: string
): Promise<void> => {
  const marker = `${remoteDirectory.replaceAll('/', '\\').toLowerCase()}\\`;

  await terminateProcesses(agent, (path) =>
    path.replaceAll('/', '\\').toLowerCase().startsWith(marker)
  );
};

const releaseManagedProcess = async (
  process: RemoteManagedProcess
): Promise<void> => {
  const deadline = Date.now() + 10_000;
  let lastError: unknown;
  const snapshot = await process.snapshot();

  if (snapshot.running) {
    await process.kill();
    await process.waitForExit({
      message: `Timed out terminating managed process ${process.id}.`,
      timeoutMs: 10_000,
    });
  }

  while (Date.now() < deadline) {
    try {
      await process.releaseAsync();
      return;
    } catch (error) {
      lastError = error;
      await delay(100);
    }
  }
  throw new Error(`Could not release managed process ${process.id}.`, {
    cause: lastError,
  });
};

const readManagedText = async (
  operation: () => Promise<string>,
  description: string
): Promise<string> => {
  const deadline = Date.now() + 10_000;
  for (;;) {
    try {
      return await operation();
    } catch (error) {
      // The capture writer may retain its handle after the process exits.
      // Await a successful read; do not retry unrelated or permanent errors.
      if (
        !/CreateFileW for read failed\..*win32Error=32\b/u.test(
          String(error)
        ) ||
        Date.now() >= deadline
      ) {
        throw new Error(`Could not read ${description}.`, { cause: error });
      }
      await delay(100);
    }
  }
};

const runPowerShellJson = async <T>(
  agent: RemoteAgent,
  script: string
): Promise<T> => {
  const process = await agent.processes.launchManaged({
    arguments: ['-NoLogo', '-NoProfile', '-NonInteractive', '-Command', script],
    captureStderr: true,
    captureStdout: true,
    createNoWindow: true,
    path: String.raw`C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe`,
  });

  try {
    const exited = await process.waitForExit({
      message: 'Timed out while collecting physical Windows metadata.',
      timeoutMs: 30_000,
    });
    const stdout = await readManagedText(
      async () => await process.stdoutText(),
      'PowerShell stdout'
    );
    const stderr = await readManagedText(
      async () => await process.stderrText(),
      'PowerShell stderr'
    );

    if (exited.root.exitCode !== 0) {
      throw new Error(
        `PowerShell metadata query exited with ${exited.root.exitCode}; stdout=${JSON.stringify(stdout)}; stderr=${JSON.stringify(stderr)}`
      );
    }
    return JSON.parse(stdout.trim()) as T;
  } finally {
    await releaseManagedProcess(process);
  }
};

// SESSIONNAME may be absent in a process launched by the agent. Query the
// current native session instead of inferring its transport from that variable.
const nativeSessionProtocolScript = String.raw`
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
/// <summary>Reads the current WTS session's transport for the physical-host check.</summary>
public static class BoxrigSessionProtocol {
    [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool WTSQuerySessionInformationW(
        IntPtr server, uint session, int information, out IntPtr buffer, out uint bytes);
    [DllImport("wtsapi32.dll")]
    private static extern void WTSFreeMemory(IntPtr buffer);
    /// <summary>Returns WTSClientProtocolType, where zero denotes the console.</summary>
    public static int Read() {
        IntPtr buffer;
        uint bytes;
        if (!WTSQuerySessionInformationW(IntPtr.Zero, uint.MaxValue, 16, out buffer, out bytes))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        try {
            if (bytes < 2) throw new InvalidOperationException("Missing WTS session protocol");
            return (ushort)Marshal.ReadInt16(buffer);
        } finally { WTSFreeMemory(buffer); }
    }
}
'@;
`;

const queryHostMetadata = async (agent: RemoteAgent): Promise<HostMetadata> =>
  await runPowerShellJson<HostMetadata>(
    agent,
    "$ErrorActionPreference='Stop';" +
      '[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false);' +
      nativeSessionProtocolScript +
      '$os=Get-CimInstance Win32_OperatingSystem;' +
      '$computer=Get-CimInstance Win32_ComputerSystem;' +
      '$gpus=@(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,PNPDeviceID);' +
      '[pscustomobject]@{' +
      'osCaption=$os.Caption;' +
      'osVersion=$os.Version;' +
      'osBuild=$os.BuildNumber;' +
      'sessionName=$env:SESSIONNAME;' +
      'sessionProtocol=[BoxrigSessionProtocol]::Read();' +
      'computerManufacturer=$computer.Manufacturer;' +
      'computerModel=$computer.Model;' +
      'videoControllers=$gpus' +
      '} | ConvertTo-Json -Compress -Depth 5'
  );

const assertPhysicalLocalHost = (metadata: HostMetadata): void => {
  expect(metadata.osCaption).toContain('Windows 11');
  expect(Number(metadata.osBuild)).toBeGreaterThanOrEqual(22_000);
  expect(metadata.sessionProtocol).toBe(0);
  expect(metadata.videoControllers.length).toBeGreaterThan(0);
  for (const controller of metadata.videoControllers) {
    expect(controller.Name.length).toBeGreaterThan(0);
    expect(controller.DriverVersion.length).toBeGreaterThan(0);
  }

  const identity = [
    metadata.computerManufacturer,
    metadata.computerModel,
    ...metadata.videoControllers.map((controller) => controller.Name),
  ]
    .join(' ')
    .toLowerCase();
  for (const virtualMarker of [
    'basic display',
    'hyper-v',
    'indirect display',
    'parallels',
    'qemu',
    'remote display',
    'virtual machine',
    'virtualbox',
    'virtio',
    'vmware',
  ]) {
    expect(identity).not.toContain(virtualMarker);
  }
};

const waitForReport = async (
  agent: RemoteAgent,
  process: RemoteManagedProcess,
  reportPath: string
): Promise<WglProbeReport> => {
  const deadline = performance.now() + 30_000;

  while (performance.now() < deadline) {
    if (await agent.files.exists(reportPath)) {
      const content = (await agent.files.readFile(reportPath)).toString('utf8');

      return JSON.parse(content) as WglProbeReport;
    }
    const snapshot = await process.snapshot();

    if (!snapshot.running) {
      throw new Error(
        `WGL probe exited before writing its report; exit=${snapshot.root.exitCode}; stdout=${JSON.stringify(await process.stdoutText())}; stderr=${JSON.stringify(await process.stderrText())}`
      );
    }
    await delay(75);
  }
  throw new Error('Timed out waiting for the WGL probe report.');
};

const findOpenGlSurface = async (window: AppWindow): Promise<AppWindow> => {
  const surfaces = await window.findDescendants({
    className: 'boxrig_wgl_probe_surface',
    strict: true,
    visible: true,
  });

  expect(surfaces).toHaveLength(1);
  return surfaces[0];
};

const capturePresentedSurface = async (
  window: AppWindow
): Promise<AppWindowScreenshot> => {
  const surface = await findOpenGlSurface(window);
  const deadline = performance.now() + 10_000;
  while (performance.now() < deadline) {
    const capture = await surface.screenshot();
    const png = PNG.sync.read(capture.image);
    expect(capture.clipped).toBe(false);
    expect(png.width).toBeGreaterThan(100);
    expect(png.height).toBeGreaterThan(100);
    let matches = true;
    // Include newly exposed edges so an old center pixel cannot hide a missing repaint.
    for (const x of [4, Math.floor(png.width / 2), png.width - 5]) {
      for (const y of [4, Math.floor(png.height / 2), png.height - 5]) {
        const offset = (y * png.width + x) * 4;
        matches &&=
          Math.abs(png.data[offset] - 32) <= 8 &&
          Math.abs(png.data[offset + 1] - 96) <= 8 &&
          Math.abs(png.data[offset + 2] - 192) <= 8 &&
          png.data[offset + 3] === 255;
      }
    }
    if (matches) return capture;
    await delay(75);
  }
  throw new Error(
    'The OpenGL child did not present its complete resized surface.'
  );
};

const assertAcceptedReport = (
  report: WglProbeReport,
  architecture: ProbeArchitecture
): void => {
  expect(report.schemaVersion).toBe(1);
  expect(report.architecture).toBe(architecture.name);
  expect(report.environment).toBe('native');
  expect(report.classification).toBe('accepted');
  expect(report.failureStage).toBe('');
  expect(report.failureCode).toBe(0);
  expect(report.wine).toBe(false);
  expect(report.remoteSession).toBe(false);
  expect(report.physicalHostAttested).toBe(true);
  expect(report.virtualHost).toBe(false);
  expect(report.legacyVendor.length).toBeGreaterThan(0);
  expect(report.legacyRenderer.length).toBeGreaterThan(0);
  expect(report.legacyVersion.length).toBeGreaterThan(0);
  expect(report.coreVendor.length).toBeGreaterThan(0);
  expect(report.coreRenderer.length).toBeGreaterThan(0);
  expect(report.coreVersion.length).toBeGreaterThan(0);
  expect(
    report.coreMajor > 3 || (report.coreMajor === 3 && report.coreMinor >= 3)
  ).toBe(true);
  expect(report.coreProfileMask & 1).toBe(1);
  expect(report.wglExtensions).toContain('WGL_ARB_extensions_string');
  expect(report.extensionString).toBe(true);
  expect(report.pixelFormatExtension).toBe(true);
  expect(report.createContextExtension).toBe(true);
  expect(report.createContextProfileExtension).toBe(true);
  expect(report.pixelFormat).toBeGreaterThan(0);
  expect(report.acceleration).toBe(0x2027);
  expect(report.colorBits).toBeGreaterThanOrEqual(24);
  expect(report.alphaBits).toBeGreaterThanOrEqual(8);
  expect(report.depthBits).toBe(0);
  expect(report.stencilBits).toBe(0);
  expect(report.drawToWindow).toBe(true);
  expect(report.supportOpenGL).toBe(true);
  expect(report.doubleBuffer).toBe(true);
  expect(report.pfdGenericFormat).toBe(false);
  expect(report.pfdGenericAccelerated).toBe(false);
  expect(report.requiredEntryPoints).toBe(true);
  expect(report.offscreenPixel).toBe(true);
  expect(report.offscreenPremultipliedAlpha).toBe(true);
  expect(report.nativeTransparency).toBe(false);
  expect(report.stateRestored).toBe(true);
  expect(report.successfulSwaps).toBeGreaterThanOrEqual(14);
  expect(report.resizeSwap).toBe(true);
  expect(report.contextRecreated).toBe(true);
  expect(report.technicalCapability).toBe(true);
};

const requireMixedDpi = process.env.BOXRIG_P2_REQUIRE_MIXED_DPI === '1';

const runArchitecture = async (
  agent: RemoteAgent,
  remoteDirectory: string,
  diagnosticsDirectory: string,
  architecture: ProbeArchitecture,
  desktop: RemoteDesktop
): Promise<void> => {
  const reportName = `wgl-${architecture.name}.json`;
  const reportPath = remoteJoin(remoteDirectory, reportName);
  let process: RemoteManagedProcess | undefined;

  try {
    process = await agent.processes.launchManaged({
      arguments: [
        '--expect-environment',
        'native',
        '--physical-host',
        '--wait',
        '--report',
        reportName,
      ],
      captureStderr: true,
      captureStdout: true,
      path: remoteJoin(remoteDirectory, architecture.relativePath),
      workingDirectory: remoteDirectory,
    });
    let window = await process.waitForWindow(
      {
        className: 'boxrig_wgl_probe_host',
        title: `boxrig-wgl-probe-ready-${architecture.name}`,
        visible: true,
      },
      {
        message: `Timed out waiting for the ${architecture.name} WGL probe.`,
        timeoutMs: 30_000,
      }
    );
    const initialReport = await waitForReport(agent, process, reportPath);
    const primary = desktop.monitors.find((monitor) => monitor.primary);
    if (primary === undefined)
      throw new Error('The desktop has no primary monitor.');
    window = await window.setBounds({
      x: primary.workArea.x + 80,
      y: primary.workArea.y + 80,
      width: 640,
      height: 420,
    });
    window = await window.waitForStableBounds({
      stableIterations: 3,
      timeoutMs: 10_000,
    });
    // Ordinary pointer activation also works when Windows denies a background
    // process's SetForegroundWindow request. Keep input outside the GL surface.
    await agent.mouse.click({
      x: window.frameBounds.x + Math.floor(window.frameBounds.width / 2),
      y: Math.floor((window.frameBounds.y + window.clientBounds.y) / 2),
    });
    await expect
      .poll(async () => (await window.refresh()).active, { timeout: 10_000 })
      .toBe(true);
    await capturePresentedSurface(window);
    const initialSurface = (await findOpenGlSurface(window)).clientBounds;
    for (const size of [
      { width: 760, height: 500 },
      { width: 600, height: 400 },
    ]) {
      window = await window.setBounds({ ...window.bounds, ...size });
      window = await window.waitForStableBounds({
        stableIterations: 3,
        timeoutMs: 10_000,
      });
      expect(window.bounds.width).toBe(size.width);
      expect(window.bounds.height).toBe(size.height);
      const surface = await findOpenGlSurface(window);
      expect(surface.clientBounds.width).not.toBe(initialSurface.width);
      expect(surface.clientBounds.height).not.toBe(initialSurface.height);
      const capture = await capturePresentedSurface(window);
      await writeFile(
        resolve(
          diagnosticsDirectory,
          'wgl-' + architecture.name + '-resize-' + size.width + '.png'
        ),
        capture.image
      );
    }
    const observations: {
      monitorId: string;
      dpi: number;
      width: number;
      height: number;
    }[] = [];
    const otherMonitors = desktop.monitors.filter(
      (monitor) => monitor.id !== primary.id
    );
    for (const monitor of [
      primary,
      ...otherMonitors,
      ...(otherMonitors.length > 0 ? [primary] : []),
    ]) {
      window = await window.setBounds({
        x: monitor.workArea.x + 40,
        y: monitor.workArea.y + 40,
        width: 640,
        height: 420,
      });
      window = await window.waitForStableBounds({
        stableIterations: 3,
        timeoutMs: 10_000,
      });
      expect(window.monitorId).toBe(monitor.id);
      expect(window.dpiAwareness).toBe('per-monitor-v2');
      if (window.dpi === null)
        throw new Error('WGL window DPI is unavailable.');
      expect(window.dpi).toBeGreaterThanOrEqual(96);
      if (monitor.dpi !== null) expect(window.dpi).toBe(monitor.dpi);
      const capture = await capturePresentedSurface(window);
      observations.push({
        monitorId: monitor.id,
        dpi: window.dpi,
        width: capture.bounds.width,
        height: capture.bounds.height,
      });
      await writeFile(
        resolve(
          diagnosticsDirectory,
          'wgl-' +
            architecture.name +
            '-monitor-' +
            observations.length +
            '.png'
        ),
        capture.image
      );
    }
    if (requireMixedDpi) {
      expect(
        new Set(observations.map((entry) => entry.dpi)).size
      ).toBeGreaterThan(1);
      expect(observations.at(-1)?.dpi).toBe(observations[0].dpi);
    }
    expect((await agent.desktop()).revision).toBe(desktop.revision);
    await writeFile(
      resolve(
        diagnosticsDirectory,
        'wgl-' + architecture.name + '-desktop.json'
      ),
      JSON.stringify({ desktop, observations }, null, 2) + '\n'
    );
    await window.close();
    const exited = await process.waitForExit({
      message: `Timed out closing the ${architecture.name} WGL probe.`,
      timeoutMs: 10_000,
    });

    expect(exited.root.exitCode, await process.stderrText()).toBe(0);
    const report = await waitForReport(agent, process, reportPath);
    assertAcceptedReport(report, architecture);
    expect(report.successfulSwaps).toBeGreaterThan(
      initialReport.successfulSwaps
    );
    await writeFile(
      resolve(diagnosticsDirectory, reportName),
      JSON.stringify(report, null, 2) + '\n'
    );
  } catch (error) {
    await saveDiagnostics(
      resolve(
        diagnosticsDirectory,
        `wgl-${architecture.name}-failure-${Date.now()}`
      ),
      {
        agent,
        attachments:
          process === undefined
            ? []
            : [
                {
                  kind: 'managedProcess',
                  name: architecture.name,
                  process,
                },
                {
                  contentType: 'application/json',
                  kind: 'remoteFile',
                  name: reportName,
                  optional: true,
                  path: reportPath,
                },
              ],
      }
    );
    throw error;
  } finally {
    if (process !== undefined) {
      await releaseManagedProcess(process);
    }
  }
};

describe('Win32 WGL capability probe through agent-rover', () => {
  it('runs relocated SDK examples and software-only consumers for both ABIs', async () => {
    const configuration = createConfiguration();
    const agent = await connectRemoteAgent({
      timeoutMs: 30_000,
    });
    let remoteDirectory: string | undefined;
    try {
      assertPhysicalLocalHost(await queryHostMetadata(agent));
      await mkdir(configuration.diagnosticsDirectory, { recursive: true });
      remoteDirectory = await agent.files.mkdtemp('boxrig-sdk-');
      for (const architecture of architectures) {
        const sdkPath = `build/tests/win32-sdk-${architecture.name}`;
        await agent.files.syncDirectory({
          localPath: resolve(configuration.sourceDirectory, sdkPath),
          remotePath: remoteJoin(remoteDirectory, sdkPath),
          include: [
            'core/*.exe',
            'software/*.exe',
            'ogl/*.exe',
            'ogl/*.dll',
            'render-d3d11/*.exe',
            'win32-d3d11/*.exe',
          ],
          checksum: 'sha256',
        });
        const consumers = [
          ['core/consumer-ordinary.exe', []],
          ['core/consumer-static.exe', []],
          ['software/consumer-ordinary.exe', []],
          ['software/consumer-static.exe', []],
          [
            'software/win32-consumer.exe',
            [String.raw`C:\Windows\Fonts\consola.ttf`],
          ],
          ['software/component-software.exe', ['--check', '--color-filter']],
          ['ogl/renderer-static.exe', ['--check']],
          ['ogl/renderer-shared.exe', ['--check']],
          [
            'ogl/component-static.exe',
            ['--check', '--require-opengl', '--color-filter'],
          ],
          [
            'ogl/component-shared.exe',
            ['--check', '--require-opengl', '--color-filter'],
          ],
          ['render-d3d11/consumer-ordinary.exe', []],
          ['render-d3d11/consumer-static.exe', []],
          ['win32-d3d11/consumer-ordinary.exe', []],
          ['win32-d3d11/consumer-static.exe', []],
          ['render-d3d11/example-ordinary.exe', ['--check']],
          ['render-d3d11/example-static.exe', ['--check']],
          ['render-d3d11/example-static.exe', ['--check', '--warp']],
          ['win32-d3d11/example-ordinary.exe', ['--check', '--color-filter']],
          ['win32-d3d11/example-static.exe', ['--check', '--color-filter']],
          [
            'win32-d3d11/example-static.exe',
            ['--check', '--transparent', '--color-filter'],
          ],
          [
            'win32-d3d11/example-static.exe',
            ['--check', '--warp', '--color-filter'],
          ],
          [
            'win32-d3d11/example-static.exe',
            ['--check', '--transparent', '--warp'],
          ],
          [
            'win32-d3d11/example-static.exe',
            ['--check', '--transparent', '--fallback'],
          ],
        ] as const;
        for (const [
          caseIndex,
          [executable, arguments_],
        ] of consumers.entries()) {
          const process = await agent.processes.launchManaged({
            path: remoteJoin(
              remoteDirectory,
              `build/tests/win32-sdk-${architecture.name}/${executable}`
            ),
            arguments: [...arguments_],
            captureStdout: true,
            captureStderr: true,
          });
          try {
            const exited = await process.waitForExit({ timeoutMs: 30_000 });
            const output =
              (await readManagedText(
                async () => await process.stdoutText(),
                `${executable} stdout`
              )) +
              (await readManagedText(
                async () => await process.stderrText(),
                `${executable} stderr`
              ));
            await writeFile(
              resolve(
                configuration.diagnosticsDirectory,
                `sdk-${architecture.name}-${caseIndex}-${executable.replace('/', '-')}.log`
              ),
              output
            );
            expect(exited.root.exitCode, output).toBe(0);
            expect(output).toContain('PASS');
          } finally {
            await releaseManagedProcess(process);
          }
        }
      }
    } finally {
      try {
        if (remoteDirectory !== undefined) {
          await terminateDirectoryProcesses(agent, remoteDirectory);
          const deadline = Date.now() + 10_000;
          for (;;) {
            try {
              await agent.files.remove(remoteDirectory, { recursive: true });
              break;
            } catch (error) {
              // Await deletion of released images, including transient file locks.
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

  it('accepts OpenGL 3.3 core on physical Windows for both ABIs', async () => {
    const configuration = createConfiguration();
    const agent = await connectRemoteAgent({
      timeoutMs: 30_000,
    });
    let remoteDirectory: string | undefined;

    try {
      const capabilities = await agent.capabilities();

      expect(capabilities.platform).toBe('windows');
      expect(capabilities.features).toContain('agent.native-windows');
      await terminateStaleProbeProcesses(agent);
      const metadata = await queryHostMetadata(agent);

      assertPhysicalLocalHost(metadata);
      const desktop = await agent.desktop();

      expect(desktop.monitors.length).toBeGreaterThan(0);
      await mkdir(configuration.diagnosticsDirectory, { recursive: true });
      await writeFile(
        resolve(configuration.diagnosticsDirectory, 'host.json'),
        `${JSON.stringify(metadata, null, 2)}\n`
      );
      remoteDirectory = await agent.files.mkdtemp(`boxrig-wgl-${Date.now()}-`);
      await agent.files.syncDirectory({
        checksum: 'sha256',
        deleteExtraneous: true,
        exclude: ['build/wine*', 'build/tests/wine*', 'build/test-results'],
        include: architectures.flatMap((architecture) => [
          architecture.relativePath,
          `build/win32/${architecture.name}/tests/test_win32_opengl_component.exe`,
          `build/win32/${architecture.name}/tests/test_win32_opengl_component_fixture.exe`,
          `build/win32/${architecture.name}/tests/test_win32_d3d11_component_fixture.exe`,
          `build/win32/${architecture.name}/tests/test_win32_component_fixture.exe`,
        ]),
        localPath: configuration.sourceDirectory,
        mode: 'mirror',
        onLockedFile: 'killRelatedProcessesAndRetry',
        relatedProcessPaths: [remoteDirectory],
        remotePath: remoteDirectory,
      });
      for (const architecture of architectures) {
        await runArchitecture(
          agent,
          remoteDirectory,
          configuration.diagnosticsDirectory,
          architecture,
          desktop
        );
        for (const fixture of [
          'test_win32_opengl_component.exe',
          'test_win32_opengl_component_fixture.exe',
          'test_win32_d3d11_component_fixture.exe',
          'test_win32_component_fixture.exe',
        ]) {
          const componentProcess = await agent.processes.launchManaged({
            createNoWindow: true,
            path: remoteJoin(
              remoteDirectory,
              `build/win32/${architecture.name}/tests/${fixture}`
            ),
            arguments: fixture.includes('fixture')
              ? [String.raw`C:\Windows\Fonts\consola.ttf`]
              : [],
            captureStdout: true,
            captureStderr: true,
            workingDirectory: remoteDirectory,
          });
          try {
            const exited = await componentProcess.waitForExit({
              timeoutMs: 60_000,
            });
            const stdout = await componentProcess.stdoutText();
            const stderr = await componentProcess.stderrText();
            await writeFile(
              resolve(
                configuration.diagnosticsDirectory,
                `${architecture.name}-${fixture}.log`
              ),
              stdout + stderr
            );
            expect(exited.root.exitCode, stdout + stderr).toBe(0);
            expect(stdout).toContain('PASS');
          } finally {
            await releaseManagedProcess(componentProcess);
          }
        }
      }
    } finally {
      if (remoteDirectory !== undefined) {
        try {
          await terminateDirectoryProcesses(agent, remoteDirectory);
          await agent.files.remove(remoteDirectory, { recursive: true });
        } catch (cleanupError) {
          console.error(
            'Failed to clean the WGL probe P2 directory.',
            cleanupError
          );
        }
      }
      agent.release();
    }
  });
});
