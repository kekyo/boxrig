import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

import { connectRemoteAgent } from './agent-connection.js';
import { expect, it } from 'vitest';

it.each(['amd64', 'i686'])(
  'keeps the native pointer visible over the terminal (%s)',
  async (architecture) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    if (!source) {
      throw new Error('The Makefile must provide BOXRIG_P2_WIN32_SOURCE_DIR.');
    }
    const agent = await connectRemoteAgent({
      timeoutMs: 30_000,
    });
    const diagnostics = resolve(source, 'build/test-results/win32-pointer');
    let directory: string | undefined;
    try {
      await mkdir(diagnostics, { recursive: true });
      const desktopCursor = await agent.cursor();
      await writeFile(
        resolve(diagnostics, `desktop-${architecture}.json`),
        JSON.stringify(desktopCursor, null, 2) + '\n'
      );
      expect(
        desktopCursor.visible,
        'The desktop cursor must be visible before launching boxrig; check the interactive desktop and mouse connection.'
      ).toBe(true);
      directory = await agent.files.mkdtemp('boxrig-pointer-');
      await agent.files.syncDirectory({
        localPath: resolve(source, 'build/boxrig_term/win32', architecture),
        remotePath: directory + '/build/boxrig_term/win32/' + architecture,
        checksum: 'sha256',
      });
      await agent.files.syncDirectory({
        localPath: resolve(source, 'build/win32', architecture, 'tests'),
        remotePath: directory + '/build/win32/' + architecture + '/tests',
        include: ['test_win32_sample.exe', 'pty_helper.exe'],
        checksum: 'sha256',
      });
      const probe = await agent.processes.launchManaged({
        path: `${directory}\\build\\win32\\${architecture}\\tests\\test_win32_sample.exe`,
        arguments: [
          '--pointer',
          `build\\boxrig_term\\win32\\${architecture}\\boxrig_term.exe`,
          `build\\win32\\${architecture}\\tests\\pty_helper.exe`,
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
          resolve(diagnostics, `pointer-${architecture}.log`),
          output
        );
        expect(exited.root.exitCode, output).toBe(0);
        expect(output).toContain('PASS Win32 sample pointer remains visible');
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
