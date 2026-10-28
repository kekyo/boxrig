import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

import { expect, it } from 'vitest';

import { connectRemoteAgent } from './agent-connection.js';

it.each(['amd64', 'i686'])(
  'keeps one visible ConPTY session through two Win32 parent moves (%s)',
  async (architecture) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    if (!source) {
      throw new Error('The Makefile must provide BOXRIG_P2_WIN32_SOURCE_DIR.');
    }
    const agent = await connectRemoteAgent({ timeoutMs: 30_000 });
    const diagnostics = resolve(
      source,
      'build/test-results/plan15-win32-transfer'
    );
    let directory: string | undefined;
    try {
      await mkdir(diagnostics, { recursive: true });
      directory = await agent.files.mkdtemp('boxrig-transfer-');
      const path = `${directory}\\win32_transfer.exe`;
      await agent.files.writeFile(
        path,
        await readFile(
          resolve(
            source,
            `build/win32/${architecture}/examples/win32_transfer.exe`
          )
        )
      );
      const child = await agent.processes.launchManaged({
        path,
        arguments: ['--verify'],
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      try {
        const exited = await child.waitForExit({ timeoutMs: 90_000 });
        const output = (await child.stdoutText()) + (await child.stderrText());
        expect(exited.root.exitCode, output).toBe(0);
        expect(output).toContain('TRANSFER 1');
        expect(output).toContain('TRANSFER 0');
        expect(output).toContain(
          'PASS win32 transfer ConPTY session survived two moves'
        );
      } finally {
        try {
          await writeFile(
            resolve(diagnostics, `${architecture}.log`),
            (await child.stdoutText()) + (await child.stderrText())
          );
        } finally {
          await child.releaseAsync();
        }
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
