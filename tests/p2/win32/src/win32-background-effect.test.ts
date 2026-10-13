import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

import { connectRemoteAgent } from './agent-connection.js';
import { expect, it } from 'vitest';

it.each(['amd64', 'i686'])(
  'requests and releases Desktop Acrylic (%s)',
  async (arch) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    if (!source) throw new Error('BOXRIG_P2_WIN32_SOURCE_DIR is required.');
    const agent = await connectRemoteAgent({
      timeoutMs: 30000,
    });
    const diagnostics = resolve(source, 'build/test-results/step27-background');
    let directory: string | undefined;
    try {
      await mkdir(diagnostics, { recursive: true });
      directory = await agent.files.mkdtemp('boxrig-background-api-');
      const path = `${directory}/background.exe`;
      await agent.files.writeFile(
        path,
        await readFile(
          resolve(
            source,
            `build/win32/${arch}/tests/test_background_effect.exe`
          )
        )
      );
      const child = await agent.processes.launchManaged({
        path,
        arguments: ['--physical'],
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      try {
        const exited = await child.waitForExit({ timeoutMs: 30000 });
        const output = (await child.stdoutText()) + (await child.stderrText());
        await writeFile(
          resolve(diagnostics, `acrylic-api-${arch}.log`),
          output
        );
        expect(exited.root.exitCode, output).toBe(0);
        expect(output).toContain('supported=1 accepted=1');
      } finally {
        await child.releaseAsync();
      }
    } finally {
      try {
        if (directory !== undefined)
          await agent.files.remove(directory, { recursive: true });
      } finally {
        agent.release();
      }
    }
  }
);
