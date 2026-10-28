import { strict as assert } from 'node:assert';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { it } from 'vitest';
import { connectRemoteAgent } from './agent-connection.js';

it.each([
  ['amd64', 'blink'],
  ['i686', 'blink'],
  ['amd64', 'animation'],
  ['i686', 'animation'],
])('runs the native cursor fixture (%s %s)', async (architecture, fixture) => {
  const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
  assert(source, 'The Makefile must provide BOXRIG_P2_WIN32_SOURCE_DIR.');
  const agent = await connectRemoteAgent({ timeoutMs: 30_000 });
  let directory: string | undefined;
  try {
    directory = await agent.files.mkdtemp('boxrig-cursor-fixture-');
    const executable = `test_win32_${fixture}.exe`;
    await agent.files.writeFile(
      `${directory}\\${executable}`,
      await readFile(
        resolve(source, `build/win32/${architecture}/tests/${executable}`)
      )
    );
    const child = await agent.processes.launchManaged({
      path: `${directory}\\${executable}`,
      workingDirectory: directory,
      captureStdout: true,
      captureStderr: true,
      createNoWindow: true,
    });
    try {
      const result = await child.waitForExit({ timeoutMs: 180_000 });
      const output = (await child.stdoutText()) + (await child.stderrText());
      const diagnostics = resolve(
        source,
        'build/test-results/win32-cursor-fixture'
      );
      await mkdir(diagnostics, { recursive: true });
      await writeFile(
        resolve(diagnostics, `windows-${architecture}-${fixture}.log`),
        `exitCode=${result.root.exitCode}\n${output}`
      );
      assert.equal(result.root.exitCode, 0, output);
      assert.match(
        output,
        fixture === 'blink'
          ? /PASS Win32 cursor blink fixture/
          : /PASS Win32 automatic cursor phases, input, IME, stale timers, failures and shared lifetime/
      );
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
