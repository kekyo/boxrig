import { readFile } from 'node:fs/promises';
import { join } from 'node:path';

import { connectRemoteAgent } from './agent-connection.js';
import { expect, it } from 'vitest';

it('runs relocated SDK and terminal executables with explicit ConPTY policies', async () => {
  const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
  if (!source) throw new Error('BOXRIG_P2_WIN32_SOURCE_DIR is required.');
  const agent = await connectRemoteAgent({
    timeoutMs: 30_000,
  });
  let directory: string | undefined;
  try {
    expect((await agent.capabilities()).platform).toBe('windows');
    const version = await agent.processes.launchManaged({
      path: 'cmd.exe',
      arguments: ['/d', '/c', 'ver'],
      captureStdout: true,
      captureStderr: true,
      createNoWindow: true,
    });
    try {
      expect(
        (await version.waitForExit({ timeoutMs: 30_000 })).root.exitCode
      ).toBe(0);
      console.log((await version.stdoutText()).trim());
    } finally {
      await version.releaseAsync({ timeoutMs: 30_000 });
    }
    directory = await agent.files.mkdtemp('boxrig-system-pty-');
    const resolveDirectory = await agent.processes.launchManaged({
      path: 'cmd.exe',
      arguments: ['/d', '/c', 'cd'],
      workingDirectory: directory,
      captureStdout: true,
      captureStderr: true,
      createNoWindow: true,
    });
    try {
      expect(
        (await resolveDirectory.waitForExit({ timeoutMs: 30_000 })).root
          .exitCode
      ).toBe(0);
      directory = (await resolveDirectory.stdoutText()).trim();
      expect(directory).toMatch(/^[a-z]:\\/i);
    } finally {
      await resolveDirectory.releaseAsync({ timeoutMs: 30_000 });
    }
    for (const architecture of ['amd64', 'i686']) {
      const location = `${directory}\\${architecture}`;
      await agent.files.mkdir(location);
      const artifacts = [
        [
          'pty.exe',
          `build/tests/win32-sdk-${architecture}/software/pty-static.exe`,
        ],
        [
          'test_pty_runtime.exe',
          `build/win32/${architecture}/tests/test_pty_runtime.exe`,
        ],
        [
          'pty_runtime_fixture.dll',
          `build/win32/${architecture}/tests/pty_runtime_fixture.dll`,
        ],
        [
          'pty_runtime_missing.dll',
          `build/win32/${architecture}/tests/pty_runtime_missing.dll`,
        ],
        [
          'boxrig_term.exe',
          `build/boxrig_term/win32/${architecture}/boxrig_term.exe`,
        ],
        [
          'boxrig_term_launcher.exe',
          `build/boxrig_term/win32/${architecture}/boxrig_term_launcher.exe`,
        ],
      ] as const;
      for (const [name, local] of artifacts) {
        await agent.files.writeFile(
          `${location}\\${name}`,
          await readFile(join(source, local))
        );
      }
      for (const packaged of [false, true]) {
        if (packaged) {
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
        }
        const cases: {
          name: string;
          arguments: string[];
          environment: Record<string, string>;
          exitCode: number;
          expected: string;
        }[] = packaged
          ? [
              {
                name: 'pty.exe',
                arguments: [],
                environment: {
                  BOXRIG_CONPTY: 'packaged-required',
                  BOXRIG_CONPTY_DIRECTORY: location,
                },
                exitCode: 0,
                expected: 'backend=packaged fallback=0 attempts=1',
              },
              {
                name: 'pty.exe',
                arguments: [],
                environment: {
                  BOXRIG_CONPTY: 'packaged-preferred',
                  BOXRIG_CONPTY_DIRECTORY: location,
                },
                exitCode: 0,
                expected: 'backend=packaged fallback=0 attempts=1',
              },
              {
                name: 'pty.exe',
                arguments: [],
                environment: {},
                exitCode: 0,
                expected: 'backend=system fallback=0 attempts=1',
              },
              {
                name: 'boxrig_term.exe',
                arguments: [
                  '--renderer=win32',
                  '--',
                  'cmd.exe',
                  '/d',
                  '/c',
                  'exit',
                  '0',
                ],
                environment: {
                  BOXRIG_CONPTY: 'packaged-required',
                  BOXRIG_CONPTY_DIRECTORY: location,
                },
                exitCode: 0,
                expected:
                  'boxrig_term: pty-backend=packaged fallback=0 attempts=1',
              },
            ]
          : [
              {
                name: 'pty.exe',
                arguments: [],
                environment: {
                  BOXRIG_CONPTY: 'packaged-required',
                  BOXRIG_CONPTY_DIRECTORY: `${location}\\absent`,
                },
                exitCode: 1,
                expected: 'PTY creation failed:',
              },
              {
                name: 'pty.exe',
                arguments: [],
                environment: {
                  BOXRIG_CONPTY: 'packaged-preferred',
                  BOXRIG_CONPTY_DIRECTORY: `${location}\\absent`,
                },
                exitCode: 0,
                expected: 'backend=system fallback=1 attempts=2',
              },
              {
                name: 'pty.exe',
                arguments: [],
                environment: {},
                exitCode: 0,
                expected: 'PASS SDK PTY input, output, resize and child exit',
              },
              {
                name: 'test_pty_runtime.exe',
                arguments: [],
                environment: {},
                exitCode: 0,
                expected: 'PASS system ConPTY selection',
              },
              {
                name: 'boxrig_term.exe',
                environment: {},
                exitCode: 0,
                arguments: [
                  '--renderer=win32',
                  '--',
                  'cmd.exe',
                  '/d',
                  '/c',
                  'exit',
                  '0',
                ],
                expected: 'boxrig_term: pty-backend=system',
              },
              {
                name: 'boxrig_term.exe',
                arguments: [
                  '--renderer=win32',
                  '--',
                  'cmd.exe',
                  '/d',
                  '/c',
                  'exit',
                  '0',
                ],
                environment: {
                  BOXRIG_CONPTY: 'packaged-required',
                  BOXRIG_CONPTY_DIRECTORY: `${location}\\absent`,
                },
                exitCode: 1,
                expected: 'boxrig_term: pty-backend=none fallback=0 attempts=1',
              },
              {
                name: 'boxrig_term.exe',
                arguments: [
                  '--renderer=win32',
                  '--',
                  'cmd.exe',
                  '/d',
                  '/c',
                  'exit',
                  '0',
                ],
                environment: {
                  BOXRIG_CONPTY: 'packaged-preferred',
                  BOXRIG_CONPTY_DIRECTORY: `${location}\\absent`,
                },
                exitCode: 0,
                expected:
                  'boxrig_term: pty-backend=system fallback=1 attempts=2',
              },
            ];
        for (const test of cases) {
          const child = await agent.processes.launchManaged({
            path: `${location}\\${test.name}`,
            arguments: test.arguments,
            environment: test.environment,
            workingDirectory: location,
            captureStdout: true,
            captureStderr: true,
            createNoWindow: true,
            killTreeOnRelease: true,
          });
          try {
            const result = await child.waitForExit({ timeoutMs: 30_000 });
            const stdout = await child.stdoutText();
            const stderr = await child.stderrText();
            expect(
              result.root.exitCode,
              `${architecture}/${test.name}: ${stdout}\n${stderr}`
            ).toBe(test.exitCode);
            expect(stdout + stderr).toContain(test.expected);
            if (
              test.name === 'pty.exe' &&
              test.expected.includes('backend=packaged')
            ) {
              expect(stdout).toContain(
                'PASS SDK PTY input, output, resize and child exit'
              );
              expect(stdout).toContain(
                `dll-machine=${architecture === 'amd64' ? '8664' : '014c'} host-machine=8664 host-file-validated=1 host-process=unverified`
              );
              expect(stdout).toMatch(/dll-version=\d+\.\d+\.\d+\.\d+/);
            }
            console.log(`PASS ${architecture}/${test.name}: ${test.expected}`);
          } catch (error) {
            console.error(
              `${architecture}/${test.name}`,
              await child.snapshot(),
              await child.stdoutText(),
              await child.stderrText()
            );
            throw error;
          } finally {
            await child.releaseAsync({ timeoutMs: 30_000 });
          }
        }
      }
    }
    const marker = `${directory.toLowerCase()}\\`;
    expect(
      (await agent.processes.list()).filter((child) =>
        child.path.toLowerCase().startsWith(marker)
      )
    ).toEqual([]);
    await agent.files.remove(directory, { recursive: true });
    expect(await agent.files.exists(directory)).toBe(false);
    directory = undefined;
    expect((await agent.capabilities()).platform).toBe('windows');
  } finally {
    try {
      if (directory !== undefined)
        await agent.files.remove(directory, { recursive: true });
    } finally {
      agent.release();
    }
  }
});
