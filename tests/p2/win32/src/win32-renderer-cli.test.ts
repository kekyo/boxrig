import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

import { connectRemoteAgent } from './agent-connection.js';
import { expect, it } from 'vitest';

it.each(['amd64', 'i686'])(
  'selects the requested renderer and preserves child arguments (%s)',
  async (architecture) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    if (!source) {
      throw new Error('The Makefile must provide BOXRIG_P2_WIN32_SOURCE_DIR.');
    }
    const agent = await connectRemoteAgent({
      timeoutMs: 30_000,
    });
    const diagnostics = resolve(source, 'build/test-results/step20-14');
    let directory: string | undefined;
    try {
      await mkdir(diagnostics, { recursive: true });
      directory = await agent.files.mkdtemp('boxrig-renderer-cli-');
      await agent.files.syncDirectory({
        localPath: resolve(source, 'build/boxrig_term/win32', architecture),
        remotePath: directory + '/build/boxrig_term/win32/' + architecture,
        checksum: 'sha256',
      });
      await agent.files.syncDirectory({
        localPath: resolve(source, 'build/win32', architecture, 'tests'),
        remotePath: directory + '/build/win32/' + architecture + '/tests',
        include: ['test_win32_renderer_cli.exe'],
        checksum: 'sha256',
      });
      const probe = await agent.processes.launchManaged({
        path: `${directory}\\build\\win32\\${architecture}\\tests\\test_win32_renderer_cli.exe`,
        arguments: [
          `build\\boxrig_term\\win32\\${architecture}\\boxrig_term.exe`,
          '--physical',
        ],
        workingDirectory: directory,
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      try {
        const exited = await probe.waitForExit({ timeoutMs: 90_000 });
        const output = (await probe.stdoutText()) + (await probe.stderrText());
        await writeFile(
          resolve(diagnostics, `renderer-cli-${architecture}.log`),
          output
        );
        expect(exited.root.exitCode, output).toBe(0);
        expect(output).toContain('PASS Win32 renderer CLI');
      } finally {
        await probe.releaseAsync();
      }
    } finally {
      try {
        if (directory !== undefined) {
          await agent.files.remove(directory, { recursive: true });
        }
      } finally {
        agent.release();
      }
    }
  }
);
