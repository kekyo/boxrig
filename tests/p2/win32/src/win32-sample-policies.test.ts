import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

import { expect, it } from 'vitest';
import { connectRemoteAgent } from './agent-connection.js';

it.each([
  ['amd64', 'background'],
  ['i686', 'background'],
  ['amd64', 'fallback'],
  ['i686', 'fallback'],
  ['amd64', 'first-frame-fence'],
  ['i686', 'first-frame-fence'],
  ['amd64', 'more-ctrl-c'],
  ['amd64', 'pointer'],
  ['i686', 'pointer'],
  ['amd64', 'colors'],
  ['i686', 'colors'],
  ['amd64', 'pressure'],
  ['i686', 'pressure'],
  ['amd64', 'ime-movement'],
  ['i686', 'ime-movement'],
  ['amd64', 'sample'],
  ['i686', 'sample'],
  ['amd64', 'shutdown'],
  ['i686', 'shutdown'],
])('preserves native sample behavior (%s %s)', async (architecture, policy) => {
  const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
  if (!source) {
    throw new Error('The Makefile must provide BOXRIG_P2_WIN32_SOURCE_DIR.');
  }
  const agent = await connectRemoteAgent({ timeoutMs: 30_000 });
  let directory: string | undefined;
  try {
    directory = await agent.files.mkdtemp('boxrig-sample-policy-');
    const probe = await agent.processes.launchManaged({
      path: 'cmd.exe',
      arguments: ['/d', '/c', 'cd'],
      workingDirectory: directory,
      captureStdout: true,
      createNoWindow: true,
    });
    try {
      expect(
        (await probe.waitForExit({ timeoutMs: 30_000 })).root.exitCode
      ).toBe(0);
      directory = (await probe.stdoutText()).trim();
      expect(directory).toMatch(/^[a-z]:\\/i);
    } finally {
      await probe.releaseAsync({ timeoutMs: 30_000 });
    }
    const root = `build/win32/${architecture}`;
    const helper =
      policy === 'first-frame-fence' ? 'first_frame_child' : 'pty_helper';
    const samples: Record<string, string> = {
      fallback: `${root}/boxrig-opacity-unavailable.exe`,
      pressure: `${root}/boxrig-sample-probe.exe`,
      'ime-movement': `${root}/boxrig-win32-ime-movement-probe.exe`,
      shutdown: `${root}/boxrig-win32-shutdown-probe.exe`,
    };
    const completeScenario = policy === 'sample' || policy === 'shutdown';
    const hosts =
      architecture === 'amd64' ? ['x64', 'arm64'] : ['x86', 'x64', 'arm64'];
    const files = [
      [`${root}/tests/test_win32_sample.exe`, 'test.exe'],
      [`${root}/tests/${helper}.exe`, 'helper.exe'],
      [`${root}/boxrig_term_launcher.exe`, 'boxrig_term_launcher.exe'],
      [
        samples[policy] ??
          `build/boxrig_term/win32/${architecture}/boxrig_term.exe`,
        'sample.exe',
      ],
      ...(completeScenario
        ? [
            [`${root}/boxrig-win32-notification-test.exe`, 'notification.exe'],
            [
              `build/boxrig_emitter/win32/${architecture}/boxrig_emitter.exe`,
              'emitter.exe',
            ],
          ]
        : []),
      ...(completeScenario
        ? [
            [`${root}/conpty.dll`, 'conpty.dll'],
            ...hosts.map((host) => [
              `${root}/${host}/OpenConsole.exe`,
              `${host}\\OpenConsole.exe`,
            ]),
          ]
        : []),
    ];
    if (completeScenario) {
      for (const host of hosts)
        await agent.files.mkdir(`${directory}\\${host}`);
    }
    for (const [local, remote] of files) {
      await agent.files.writeFile(
        `${directory}\\${remote}`,
        await readFile(resolve(source, local))
      );
    }
    const mode =
      policy === 'background' || policy === 'fallback'
        ? `${policy}-opacity`
        : policy === 'colors' || policy === 'pressure'
          ? 'shared-scenario'
          : policy;
    const args = completeScenario
      ? [
          `${directory}\\sample.exe`,
          `${directory}\\notification.exe`,
          `${directory}\\helper.exe`,
          policy === 'shutdown' ? '-' : `${directory}\\emitter.exe`,
        ]
      : [
          `--${mode}`,
          `${directory}\\sample.exe`,
          ...(policy === 'more-ctrl-c' ? [] : [`${directory}\\helper.exe`]),
        ];
    const environment: Record<string, string> = {
      BOXRIG_TEST_ALLOW_CONPTY_NORMALIZATION: 'windows',
    };
    if (policy === 'background')
      environment.BOXRIG_TRANSPARENCY_MODE = 'opaque';
    if (policy === 'colors') environment.BOXRIG_TEST_SAMPLE_COLORS = '1';
    if (policy === 'pressure') {
      environment.BOXRIG_TEST_SAMPLE_PASTE_PRESSURE = '1';
      environment.BOXRIG_TEST_SAMPLE_OVERLAYS = '1';
      environment.BOXRIG_TEST_SAMPLE_GEOMETRY = '1';
    }
    if (completeScenario) {
      // Like the interactive-resize test, compare one unchanged VT scene.
      // System ConPTY repaints the newly exposed background after resizing.
      environment.BOXRIG_CONPTY = 'packaged-required';
      environment.BOXRIG_CONPTY_DIRECTORY = directory;
      environment.BOXRIG_TEST_MODAL_RESIZE_CAPTURE_PREFIX = `${directory}\\resize`;
    }
    const child = await agent.processes.launchManaged({
      path: `${directory}\\test.exe`,
      arguments: args,
      workingDirectory: directory,
      environment,
      captureStdout: true,
      captureStderr: true,
      createNoWindow: true,
    });
    try {
      const result = await child.waitForExit({ timeoutMs: 180_000 });
      const output = (await child.stdoutText()) + (await child.stderrText());
      const diagnostics = resolve(
        source,
        'build/test-results/win32-sample-policies'
      );
      await mkdir(diagnostics, { recursive: true });
      await writeFile(
        resolve(diagnostics, `${architecture}-${policy}.log`),
        output
      );
      if (completeScenario) {
        const bits = architecture === 'amd64' ? 64 : 32;
        for (const band of [
          'held-right',
          'held-bottom',
          'held-content',
          'released-right',
          'released-bottom',
          'released-content',
        ]) {
          const remote = `${directory}\\resize-${bits}-${band}.bmp`;
          if (await agent.files.exists(remote)) {
            await writeFile(
              resolve(diagnostics, `${architecture}-${policy}-${band}.bmp`),
              await agent.files.readFile(remote)
            );
          }
        }
      }
      expect(result.root.exitCode, output).toBe(0);
      expect(output).toContain('PASS Win32');
    } finally {
      await child.releaseAsync({ timeoutMs: 30_000 });
    }
  } finally {
    try {
      if (directory !== undefined)
        await agent.files.remove(directory, { recursive: true });
    } finally {
      agent.release();
    }
  }
});
