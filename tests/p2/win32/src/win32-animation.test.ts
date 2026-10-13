import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

import { connectRemoteAgent } from './agent-connection.js';
import { expect, it } from 'vitest';

it.each([
  ['amd64', 'animation'],
  ['i686', 'animation'],
  ['amd64', 'blink'],
  ['i686', 'blink'],
  ['amd64', 'modules'],
  ['i686', 'modules'],
])(
  'handles native animation ownership and renderer phases (%s %s)',
  async (architecture, driver) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    if (!source) {
      throw new Error('The Makefile must provide BOXRIG_P2_WIN32_SOURCE_DIR.');
    }
    const agent = await connectRemoteAgent({
      timeoutMs: 30_000,
    });
    const diagnostics = resolve(source, 'build/test-results/step23-blink');
    let directory: string | undefined;
    try {
      await mkdir(diagnostics, { recursive: true });
      directory = await agent.files.mkdtemp('boxrig-animation-');
      const path = `${directory}\\renderer-${architecture}.exe`;
      await agent.files.writeFile(
        path,
        await readFile(
          resolve(
            source,
            `build/win32/${architecture}/tests/${driver === 'modules' ? 'test_animation_modules' : `test_win32_${driver}`}.exe`
          )
        )
      );
      if (driver === 'modules') {
        await agent.files.writeFile(
          `${directory}\\animation_modules.dll`,
          await readFile(
            resolve(
              source,
              `build/win32/${architecture}/tests/animation_modules.dll`
            )
          )
        );
      }
      const probe = await agent.processes.launchManaged({
        path,
        arguments: [],
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      try {
        const exited = await probe.waitForExit({ timeoutMs: 60_000 });
        const output = (await probe.stdoutText()) + (await probe.stderrText());
        await writeFile(
          resolve(diagnostics, `${architecture}-${driver}.log`),
          output
        );
        expect(exited.root.exitCode, output).toBe(0);
        expect(output).toContain('PASS');
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
