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
    const files = [
      [`${root}/tests/test_win32_sample.exe`, 'test.exe'],
      [`${root}/tests/${helper}.exe`, 'helper.exe'],
      [`${root}/boxrig_term_launcher.exe`, 'boxrig_term_launcher.exe'],
      [
        policy === 'fallback'
          ? `${root}/boxrig-opacity-unavailable.exe`
          : `build/boxrig_term/win32/${architecture}/boxrig_term.exe`,
        'sample.exe',
      ],
    ];
    for (const [local, remote] of files) {
      await agent.files.writeFile(
        `${directory}\\${remote}`,
        await readFile(resolve(source, local))
      );
    }
    const child = await agent.processes.launchManaged({
      path: `${directory}\\test.exe`,
      arguments: [
        `--${policy === 'background' || policy === 'fallback' ? `${policy}-opacity` : policy}`,
        `${directory}\\sample.exe`,
        ...(policy === 'more-ctrl-c' ? [] : [`${directory}\\helper.exe`]),
      ],
      workingDirectory: directory,
      environment:
        policy === 'background' ? { BOXRIG_TRANSPARENCY_MODE: 'opaque' } : {},
      captureStdout: true,
      captureStderr: true,
      createNoWindow: true,
    });
    try {
      const result = await child.waitForExit({ timeoutMs: 120_000 });
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
