import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

import { connectRemoteAgent } from './agent-connection.js';
import { expect, it } from 'vitest';

it.each(
  ['amd64', 'i686'].flatMap((architecture) =>
    ['test_terminal_io_views', 'test_win32_input_adapter'].map((binary) => [
      architecture,
      binary,
    ])
  )
)(
  'checks configured native inputs and Unicode views on physical Windows (%s %s)',
  async (architecture, binary) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    if (!source) {
      throw new Error('The Makefile must provide BOXRIG_P2_WIN32_SOURCE_DIR.');
    }
    const agent = await connectRemoteAgent({
      timeoutMs: 30_000,
    });
    const diagnostics = resolve(
      source,
      'build/test-results/step23-terminal-io'
    );
    let directory: string | undefined;
    try {
      await mkdir(diagnostics, { recursive: true });
      directory = await agent.files.mkdtemp('boxrig-terminal-io-');
      const path = `${directory}\\renderer-${architecture}.exe`;
      await agent.files.writeFile(
        path,
        await readFile(
          resolve(source, `build/win32/${architecture}/tests/${binary}.exe`)
        )
      );
      const probe = await agent.processes.launchManaged({
        path,
        arguments:
          binary === 'test_win32_input_adapter' ? ['--terminal-io-only'] : [],
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      try {
        const exited = await probe.waitForExit({ timeoutMs: 60_000 });
        const output = (await probe.stdoutText()) + (await probe.stderrText());
        await writeFile(
          resolve(diagnostics, `${architecture}-${binary}.log`),
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
