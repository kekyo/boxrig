import { type RemoteAgent, type RemoteManagedProcess } from 'agent-rover';
import { connectRemoteAgent } from './agent-connection.js';
import { delay } from 'async-primitives';
import { describe, expect, it } from 'vitest';

interface TestConfiguration {
  readonly sourceDirectory: string;
}

interface ArchitecturePaths {
  readonly name: 'amd64' | 'i686';
  readonly transportTest: string;
  readonly sampleTest: string;
  readonly sample: string;
  readonly sampleProbe: string;
  readonly launcher: string;
  readonly ptyHelper: string;
  readonly emitter: string;
  readonly runtimeFiles: readonly string[];
}

const amd64Architecture: ArchitecturePaths = {
  name: 'amd64',
  transportTest: 'build/win32/amd64/tests/test_win32_pty.exe',
  sampleTest: 'build/win32/amd64/tests/test_win32_sample.exe',
  sample: 'build/boxrig_term/win32/amd64/boxrig_term.exe',
  sampleProbe: 'build/win32/amd64/boxrig-sample-probe.exe',
  launcher: 'build/boxrig_term/win32/amd64/boxrig_term_launcher.exe',
  ptyHelper: 'build/win32/amd64/tests/pty_helper.exe',
  emitter: 'build/boxrig_emitter/win32/amd64/boxrig_emitter.exe',
  runtimeFiles: [
    'build/win32/amd64/boxrig_term_launcher.exe',
    'build/win32/amd64/conpty.dll',
    'build/win32/amd64/x64/OpenConsole.exe',
    'build/win32/amd64/arm64/OpenConsole.exe',
    'build/win32/amd64/tests/conpty.dll',
    'build/win32/amd64/tests/x64/OpenConsole.exe',
    'build/win32/amd64/tests/arm64/OpenConsole.exe',
    'build/boxrig_term/win32/amd64/conpty.dll',
    'build/boxrig_term/win32/amd64/x64/OpenConsole.exe',
    'build/boxrig_term/win32/amd64/arm64/OpenConsole.exe',
  ],
};

const architectures: readonly ArchitecturePaths[] = [
  amd64Architecture,
  {
    name: 'i686',
    transportTest: 'build/win32/i686/tests/test_win32_pty.exe',
    sampleTest: 'build/win32/i686/tests/test_win32_sample.exe',
    sample: 'build/boxrig_term/win32/i686/boxrig_term.exe',
    sampleProbe: 'build/win32/i686/boxrig-sample-probe.exe',
    launcher: 'build/boxrig_term/win32/i686/boxrig_term_launcher.exe',
    ptyHelper: 'build/win32/i686/tests/pty_helper.exe',
    emitter: 'build/boxrig_emitter/win32/i686/boxrig_emitter.exe',
    runtimeFiles: [
      'build/win32/i686/boxrig_term_launcher.exe',
      'build/win32/i686/conpty.dll',
      'build/win32/i686/x86/OpenConsole.exe',
      'build/win32/i686/x64/OpenConsole.exe',
      'build/win32/i686/arm64/OpenConsole.exe',
      'build/win32/i686/tests/conpty.dll',
      'build/win32/i686/tests/x86/OpenConsole.exe',
      'build/win32/i686/tests/x64/OpenConsole.exe',
      'build/win32/i686/tests/arm64/OpenConsole.exe',
      'build/boxrig_term/win32/i686/conpty.dll',
      'build/boxrig_term/win32/i686/x86/OpenConsole.exe',
      'build/boxrig_term/win32/i686/x64/OpenConsole.exe',
      'build/boxrig_term/win32/i686/arm64/OpenConsole.exe',
    ],
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
});

const remoteJoin = (base: string, relative: string): string =>
  `${base}\\${relative.replaceAll('/', '\\')}`;

const remoteRelative = (relative: string): string =>
  relative.replaceAll('/', '\\');

const terminateDirectoryProcesses = async (
  agent: RemoteAgent,
  remoteDirectory: string
): Promise<void> => {
  const marker = `${remoteDirectory.replaceAll('/', '\\').toLowerCase()}\\`;
  const processes = await agent.processes.list();
  const related = processes.filter((process) =>
    process.path.replaceAll('/', '\\').toLowerCase().startsWith(marker)
  );

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
  read: () => Promise<string>,
  description: string
): Promise<string> => {
  const deadline = Date.now() + 10_000;
  let lastError: unknown;

  while (Date.now() < deadline) {
    try {
      return await read();
    } catch (error) {
      lastError = error;
      await delay(100);
    }
  }
  throw new Error(`Could not read ${description}.`, { cause: lastError });
};

const stopAndDescribeManagedProcess = async (
  agent: RemoteAgent,
  remoteDirectory: string,
  process: RemoteManagedProcess
): Promise<string> => {
  const snapshot = await process.snapshot();

  await terminateDirectoryProcesses(agent, remoteDirectory);
  const stdout = await readManagedText(
    async () => await process.stdoutText(),
    `stdout for managed process ${process.id}`
  );
  const stderr = await readManagedText(
    async () => await process.stderrText(),
    `stderr for managed process ${process.id}`
  );
  return `snapshot=${JSON.stringify(snapshot)}; stdout=${JSON.stringify(stdout)}; stderr=${JSON.stringify(stderr)}`;
};

const runArchitecture = async (
  agent: RemoteAgent,
  remoteDirectory: string,
  architecture: ArchitecturePaths,
  policy: 'system' | 'packaged-required'
): Promise<void> => {
  let process: RemoteManagedProcess | undefined;

  try {
    process = await agent.processes.launchManaged({
      arguments: [
        '--emitter-transport',
        remoteRelative(architecture.launcher),
        remoteRelative(architecture.emitter),
      ],
      captureStderr: true,
      captureStdout: true,
      createNoWindow: true,
      environment: {
        BOXRIG_TEST_ALLOW_CONPTY_NORMALIZATION: 'windows',
        BOXRIG_CONPTY: policy,
        BOXRIG_CONPTY_DIRECTORY: remoteJoin(
          remoteDirectory,
          `build/win32/${architecture.name}`
        ),
      },
      path: remoteJoin(remoteDirectory, architecture.transportTest),
      workingDirectory: remoteDirectory,
    });
    const managedProcess = process;
    const snapshot = await managedProcess.waitForExit({
      message: `Timed out running the ${architecture.name} ConPTY transport test.`,
      timeoutMs: 30_000,
    });
    const stdout = await readManagedText(
      async () => await managedProcess.stdoutText(),
      `stdout for managed process ${managedProcess.id}`
    );
    const stderr = await readManagedText(
      async () => await managedProcess.stderrText(),
      `stderr for managed process ${managedProcess.id}`
    );

    if (snapshot.root.exitCode !== 0) {
      throw new Error(
        `${architecture.name} ConPTY transport exited with ${snapshot.root.exitCode}; stdout=${JSON.stringify(stdout)}; stderr=${JSON.stringify(stderr)}`
      );
    }
    expect(stdout).toContain('PASS Win32 redirected and ConPTY');
    expect(stdout).toContain(
      `test-pty-backend=${policy === 'system' ? 'system' : 'packaged'}`
    );
    console.log(`${architecture.name}/${policy}: ${stdout.trim()}`);
  } finally {
    if (process !== undefined) {
      await releaseManagedProcess(process);
    }
  }
};

const runSampleArchitecture = async (
  agent: RemoteAgent,
  remoteDirectory: string,
  architecture: ArchitecturePaths
): Promise<void> => {
  let process: RemoteManagedProcess | undefined;

  try {
    process = await agent.processes.launchManaged({
      arguments: [
        '--emitter-conpty',
        remoteRelative(architecture.sample),
        remoteRelative(architecture.emitter),
      ],
      captureStderr: true,
      captureStdout: true,
      createNoWindow: true,
      environment: {
        BOXRIG_TEST_ALLOW_CONPTY_NORMALIZATION: 'windows',
        BOXRIG_CONPTY: 'packaged-required',
        BOXRIG_CONPTY_DIRECTORY: remoteJoin(
          remoteDirectory,
          `build/win32/${architecture.name}`
        ),
      },
      path: remoteJoin(remoteDirectory, architecture.sampleTest),
      workingDirectory: remoteDirectory,
    });
    const managedProcess = process;
    const snapshot = await managedProcess.waitForExit({
      message: `Timed out running the ${architecture.name} emitter sample test.`,
      timeoutMs: 120_000,
    });
    const stdout = await readManagedText(
      async () => await managedProcess.stdoutText(),
      `stdout for managed process ${managedProcess.id}`
    );
    const stderr = await readManagedText(
      async () => await managedProcess.stderrText(),
      `stderr for managed process ${managedProcess.id}`
    );

    if (snapshot.root.exitCode !== 0) {
      throw new Error(
        `${architecture.name} emitter sample test exited with ${snapshot.root.exitCode}; stdout=${JSON.stringify(stdout)}; stderr=${JSON.stringify(stderr)}`
      );
    }
    expect(stdout).toContain('PASS Win32 sample emitter ConPTY cases');
    expect(stdout).not.toContain('SKIP Win32 emitter');
  } finally {
    if (process !== undefined) {
      await releaseManagedProcess(process);
    }
  }
};

const runBackgroundOpacity = async (
  agent: RemoteAgent,
  remoteDirectory: string,
  architecture: ArchitecturePaths
): Promise<void> => {
  let process: RemoteManagedProcess | undefined;

  try {
    process = await agent.processes.launchManaged({
      arguments: [
        '--background-opacity',
        remoteRelative(architecture.sample),
        remoteRelative(architecture.ptyHelper),
      ],
      captureStderr: true,
      captureStdout: true,
      createNoWindow: true,
      path: remoteJoin(remoteDirectory, architecture.sampleTest),
      workingDirectory: remoteDirectory,
    });
    const managedProcess = process;
    const snapshot = await managedProcess.waitForExit({
      message: `Timed out running the ${architecture.name} background opacity test.`,
      timeoutMs: 30_000,
    });
    const stdout = await readManagedText(
      async () => await managedProcess.stdoutText(),
      `stdout for managed process ${managedProcess.id}`
    );
    const stderr = await readManagedText(
      async () => await managedProcess.stderrText(),
      `stderr for managed process ${managedProcess.id}`
    );

    if (snapshot.root.exitCode !== 0) {
      throw new Error(
        `${architecture.name} background opacity test exited with ${snapshot.root.exitCode}; stdout=${JSON.stringify(stdout)}; stderr=${JSON.stringify(stderr)}`
      );
    }
    expect(stdout).toContain('PASS Win32 sample background opacity policy');
  } finally {
    if (process !== undefined) {
      await releaseManagedProcess(process);
    }
  }
};

const runMoreCtrlC = async (
  agent: RemoteAgent,
  remoteDirectory: string,
  architecture: ArchitecturePaths
): Promise<void> => {
  let process: RemoteManagedProcess | undefined;

  try {
    process = await agent.processes.launchManaged({
      arguments: ['--more-ctrl-c', remoteRelative(architecture.sample)],
      captureStderr: true,
      captureStdout: true,
      createNoWindow: true,
      environment: {
        BOXRIG_TEST_ALLOW_CONPTY_NORMALIZATION: 'windows',
        BOXRIG_CONPTY: 'packaged-required',
        BOXRIG_CONPTY_DIRECTORY: remoteJoin(
          remoteDirectory,
          `build/win32/${architecture.name}`
        ),
      },
      path: remoteJoin(remoteDirectory, architecture.sampleTest),
      workingDirectory: remoteDirectory,
    });
    const managedProcess = process;
    const snapshot = await managedProcess.waitForExit({
      message: `Timed out running the ${architecture.name} MORE Ctrl-C test.`,
      timeoutMs: 120_000,
    });
    const stdout = await readManagedText(
      async () => await managedProcess.stdoutText(),
      `stdout for managed process ${managedProcess.id}`
    );
    const stderr = await readManagedText(
      async () => await managedProcess.stderrText(),
      `stderr for managed process ${managedProcess.id}`
    );

    if (snapshot.root.exitCode !== 0) {
      throw new Error(
        `${architecture.name} MORE Ctrl-C test exited with ${snapshot.root.exitCode}; stdout=${JSON.stringify(stdout)}; stderr=${JSON.stringify(stderr)}`
      );
    }
    expect(stdout).toContain('PASS Win32 sample MORE Ctrl-C case');
  } finally {
    if (process !== undefined) {
      await releaseManagedProcess(process);
    }
  }
};

const runSharedScenario = async (
  agent: RemoteAgent,
  remoteDirectory: string,
  architecture: ArchitecturePaths,
  probe: boolean,
  renderer: 'win32' | 'opengl' | 'd3d11'
): Promise<void> => {
  let process: RemoteManagedProcess | undefined;
  const profile = `${probe ? 'pressure-overlay-geometry' : 'colors'}-${renderer}`;
  const probeLog = remoteJoin(
    remoteDirectory,
    `${architecture.name}-probe.log`
  );

  try {
    process = await agent.processes.launchManaged({
      arguments: [
        '--shared-scenario',
        remoteRelative(probe ? architecture.sampleProbe : architecture.sample),
        remoteRelative(architecture.ptyHelper),
      ],
      captureStderr: true,
      captureStdout: true,
      createNoWindow: true,
      environment: {
        BOXRIG_TEST_ALLOW_CONPTY_NORMALIZATION: 'windows',
        BOXRIG_CONPTY: 'packaged-required',
        BOXRIG_CONPTY_DIRECTORY: remoteJoin(
          remoteDirectory,
          `build/win32/${architecture.name}`
        ),
        BOXRIG_TEST_SAMPLE_COLORS: probe ? '0' : '1',
        BOXRIG_TEST_SAMPLE_RENDERER: `--renderer=${renderer}`,
        BOXRIG_TEST_SAMPLE_PASTE_PRESSURE: probe ? '1' : '0',
        BOXRIG_TEST_SAMPLE_OVERLAYS: probe ? '1' : '0',
        BOXRIG_TEST_SAMPLE_GEOMETRY: probe ? '1' : '0',
        BOXRIG_TEST_SAMPLE_PROBE_LOG: `${architecture.name}-probe.log`,
      },
      path: remoteJoin(remoteDirectory, architecture.sampleTest),
      workingDirectory: remoteDirectory,
    });
    const managedProcess = process;
    const snapshot = await managedProcess.waitForExit({
      message: `Timed out running the ${architecture.name} shared ${profile} scenario.`,
      timeoutMs: 120_000,
    });
    const stdout = await readManagedText(
      async () => await managedProcess.stdoutText(),
      `stdout for managed process ${managedProcess.id}`
    );
    const stderr = await readManagedText(
      async () => await managedProcess.stderrText(),
      `stderr for managed process ${managedProcess.id}`
    );

    if (snapshot.root.exitCode !== 0) {
      const diagnostics = (await agent.files.exists(probeLog))
        ? (await agent.files.readFile(probeLog)).toString('utf8')
        : '';

      throw new Error(
        `${architecture.name} shared ${profile} scenario exited with ${snapshot.root.exitCode}; stdout=${JSON.stringify(stdout)}; stderr=${JSON.stringify(stderr)}; probe=${JSON.stringify(diagnostics)}`
      );
    }
    expect(stdout).toContain('PASS Win32 sample follows shared PTY');
    if (!probe) {
      expect(stdout).toContain(
        'PASS Win32 sample color filter off identity corrected cycle'
      );
    }
    expect(stdout).not.toContain('SKIP');
    expect(stdout).not.toContain('P2 Win32 shared geometry PTY query');
    console.log(
      `PASS: real Win32 ${architecture.name} shared ${profile} scenario`
    );
  } finally {
    if (process !== undefined) {
      await releaseManagedProcess(process);
    }
  }
};

describe('Win32 real ConPTY emitter transport through agent-rover', () => {
  it('preserves Ctrl-C, emitter transports, and shared sample scenarios', async () => {
    const configuration = createConfiguration();
    const agent = await connectRemoteAgent({
      timeoutMs: 30_000,
    });
    let remoteDirectory: string | undefined;

    try {
      const capabilities = await agent.capabilities();

      expect(capabilities.platform).toBe('windows');
      expect(capabilities.features).toContain('agent.native-windows');
      remoteDirectory = await agent.files.mkdtemp(
        `boxrig-conpty-${Date.now()}-`
      );
      const resolveDirectory = await agent.processes.launchManaged({
        path: 'cmd.exe',
        arguments: ['/d', '/c', 'cd'],
        workingDirectory: remoteDirectory,
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      try {
        expect(
          (await resolveDirectory.waitForExit({ timeoutMs: 30_000 })).root
            .exitCode
        ).toBe(0);
        remoteDirectory = (await resolveDirectory.stdoutText()).trim();
        expect(remoteDirectory).toMatch(/^[a-z]:\\/i);
      } finally {
        await resolveDirectory.releaseAsync({ timeoutMs: 30_000 });
      }
      await agent.files.syncDirectory({
        checksum: 'sha256',
        deleteExtraneous: true,
        exclude: [
          'build/wine*',
          'build/tests/wine*',
          'build/test-results',
          '**/node_modules',
          '.git',
        ],
        include: architectures.flatMap((architecture) => [
          architecture.transportTest,
          architecture.sampleTest,
          architecture.sample,
          architecture.sampleProbe,
          architecture.launcher,
          architecture.ptyHelper,
          architecture.emitter,
          ...architecture.runtimeFiles,
        ]),
        localPath: configuration.sourceDirectory,
        mode: 'mirror',
        onLockedFile: 'killRelatedProcessesAndRetry',
        relatedProcessPaths: [remoteDirectory],
        remotePath: remoteDirectory,
      });

      /* SendInput requires foreground ownership, which Windows can deny to a
       * second GUI test process. Exercise the reported amd64 path once, then
       * verify architecture-neutral ConPTY behavior for both binaries. */
      await runMoreCtrlC(agent, remoteDirectory, amd64Architecture);
      for (const architecture of architectures) {
        await runBackgroundOpacity(agent, remoteDirectory, architecture);
        await runArchitecture(agent, remoteDirectory, architecture, 'system');
        await runArchitecture(
          agent,
          remoteDirectory,
          architecture,
          'packaged-required'
        );
        await runSampleArchitecture(agent, remoteDirectory, architecture);
        for (const renderer of ['win32', 'opengl', 'd3d11'] as const) {
          await runSharedScenario(
            agent,
            remoteDirectory,
            architecture,
            false,
            renderer
          );
        }
        await runSharedScenario(
          agent,
          remoteDirectory,
          architecture,
          true,
          'win32'
        );
      }
      console.log(
        'PASS: real Win32 amd64 MORE Ctrl-C and amd64/i686 ConPTY preserved sample meaning'
      );
    } finally {
      if (remoteDirectory !== undefined) {
        try {
          await terminateDirectoryProcesses(agent, remoteDirectory);
          await agent.files.remove(remoteDirectory, { recursive: true });
        } catch (cleanupError) {
          console.error(
            'Failed to clean the ConPTY P2 directory.',
            cleanupError
          );
        }
      }
      agent.release();
    }
  });
});
