import { readFile } from 'node:fs/promises';
import { join } from 'node:path';
import { expect, it } from 'vitest';

import { connectRemoteAgent } from './agent-connection.js';

it('separates real ConPTY spawn errors from exit 127/259 and releases owned handles', async () => {
  const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
  if (!source) throw new Error('BOXRIG_P2_WIN32_SOURCE_DIR is required.');
  const agent = await connectRemoteAgent({ timeoutMs: 30_000 });
  let directory: string | undefined;
  try {
    expect((await agent.capabilities()).platform).toBe('windows');
    directory = await agent.files.mkdtemp('boxrig-spawn-');
    const resolve = await agent.processes.launchManaged({
      path: 'cmd.exe',
      arguments: ['/d', '/c', 'cd'],
      workingDirectory: directory,
      captureStdout: true,
      createNoWindow: true,
    });
    try {
      expect(
        (await resolve.waitForExit({ timeoutMs: 30_000 })).root.exitCode
      ).toBe(0);
      directory = (await resolve.stdoutText()).trim();
      expect(directory).toMatch(/^[a-z]:\\/i);
    } finally {
      await resolve.releaseAsync({ timeoutMs: 30_000 });
    }
    for (const architecture of ['amd64', 'i686']) {
      const location = `${directory}\\${architecture}`;
      await agent.files.mkdir(location);
      for (const name of [
        'test_pty_spawn.exe',
        'test_pty_runtime.exe',
        'pty_runtime_fixture.dll',
        'pty_runtime_missing.dll',
      ]) {
        await agent.files.writeFile(
          `${location}\\${name}`,
          await readFile(
            join(source, `build/win32/${architecture}/tests/${name}`)
          )
        );
      }
      await agent.files.writeFile(
        `${location}\\conpty.dll`,
        await readFile(
          join(source, `build/generated/conpty/${architecture}/conpty.dll`)
        )
      );
      await agent.files.mkdir(`${location}\\x64`);
      await agent.files.writeFile(
        `${location}\\x64\\OpenConsole.exe`,
        await readFile(
          join(source, 'build/generated/conpty/hosts/x64/OpenConsole.exe')
        )
      );
      for (const policy of ['mock', 'system', 'packaged-required']) {
        const child = await agent.processes.launchManaged({
          path: `${location}\\${policy === 'mock' ? 'test_pty_runtime.exe' : 'test_pty_spawn.exe'}`,
          arguments:
            policy === 'mock'
              ? []
              : policy === 'system'
                ? [policy]
                : [policy, location],
          workingDirectory: location,
          captureStdout: true,
          captureStderr: true,
          createNoWindow: true,
          killTreeOnRelease: true,
        });
        try {
          const result = await child.waitForExit({ timeoutMs: 120_000 });
          const output =
            (await child.stdoutText()) + (await child.stderrText());
          expect(
            result.root.exitCode,
            `${architecture}/${policy}: ${output}`
          ).toBe(0);
          expect(output).toContain(
            policy === 'mock'
              ? 'PASS packaged policies'
              : 'PASS real ConPTY startup errors, exit 127/259, Cardio waits and borrowed HANDLE lifetime'
          );
          console.log(`${architecture}/${policy}: ${output.trim()}`);
        } finally {
          await child.releaseAsync({ timeoutMs: 30_000 });
        }
      }
    }
    const marker = `${directory.toLowerCase()}\\`;
    expect(
      (await agent.processes.list()).filter((child) =>
        child.path.toLowerCase().startsWith(marker)
      )
    ).toEqual([]);
  } finally {
    try {
      if (directory !== undefined)
        await agent.files.remove(directory, { recursive: true });
    } finally {
      agent.release();
    }
  }
});
