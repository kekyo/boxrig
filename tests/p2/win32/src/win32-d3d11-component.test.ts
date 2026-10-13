import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

import { connectRemoteAgent } from './agent-connection.js';
import { expect, it } from 'vitest';

it.each([
  ['amd64', 'warp', 'transactions'],
  ['i686', 'warp', 'transactions'],
  ['amd64', 'hardware', 'transactions'],
  ['i686', 'hardware', 'transactions'],
  ['amd64', 'warp', 'font'],
  ['i686', 'warp', 'font'],
  ['amd64', 'hardware', 'font'],
  ['i686', 'hardware', 'font'],
])(
  'draws into managed DirectComposition surfaces (%s %s %s)',
  async (architecture, driver, scenario) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    if (!source) {
      throw new Error('The Makefile must provide BOXRIG_P2_WIN32_SOURCE_DIR.');
    }
    const agent = await connectRemoteAgent({
      timeoutMs: 30_000,
    });
    const diagnostics = resolve(source, 'build/test-results/step20-13');
    let directory: string | undefined;
    try {
      await mkdir(diagnostics, { recursive: true });
      directory = await agent.files.mkdtemp('boxrig-d3d11-');
      const path = `${directory}\\renderer-${architecture}.exe`;
      await agent.files.writeFile(
        path,
        await readFile(
          resolve(
            source,
            `build/win32/${architecture}/tests/test_d3d11_component.exe`
          )
        )
      );
      const probe = await agent.processes.launchManaged({
        path,
        arguments: ['--physical', `--${driver}`, `--${scenario}`],
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      try {
        const exited = await probe.waitForExit({ timeoutMs: 60_000 });
        const output = (await probe.stdoutText()) + (await probe.stderrText());
        expect(exited.root.exitCode, output).toBe(0);
        expect(output).toContain('PASS d3d11 managed presentation');
      } finally {
        try {
          await writeFile(
            resolve(diagnostics, `${architecture}-${driver}-${scenario}.log`),
            (await probe.stdoutText()) + (await probe.stderrText())
          );
        } finally {
          await probe.releaseAsync();
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
