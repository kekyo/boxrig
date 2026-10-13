import { afterEach, expect, it, vi } from 'vitest';

const load = {
  alpha: async () => await import('./win32-alpha-feasibility.test.js'),
  background: async () => await import('./win32-background-video.test.js'),
  cli: async () => await import('./win32-cli-opacity.test.js'),
  copy: async () => await import('./win32-copy-policy.test.js'),
  scrollbar: async () => await import('./win32-scrollbar.test.js'),
  transparency: async () => await import('./win32-transparency.test.js'),
  wgl: async () => await import('./win32-wgl-renderer-opacity.test.js'),
  wheel: async () => await import('./win32-wheel-policy.test.js'),
  window: async () => await import('./win32-window-opacity.test.js'),
};

afterEach(() => {
  for (const module of [
    'vitest',
    './agent-connection.js',
    'node:child_process',
    'node:fs/promises',
    'agent-rover',
  ])
    vi.doUnmock(module);
  vi.unstubAllEnvs();
  vi.resetModules();
});

it.each([
  ['copy', 'copies retained', 'clipboard'],
  ['alpha', 'measures native alpha on physical', 'directory'],
  ['cli', 'composites $renderer CLI', 'directory'],
  ['wgl', 'keeps WGL glyphs', 'directory'],
  ['window', 'composites GDI CLI', 'directory'],
  ['window', 'applies one host alpha', 'directory'],
  ['background', 'blurs the moving backdrop', 'local-directory'],
  ['scrollbar', 'updates standard scrollbar', 'local-directory'],
  ['alpha', 'measures native alpha on physical', 'remove'],
  ['copy', 'copies retained', 'remove'],
  ['wheel', 'routes wheel history', 'remove'],
  ['transparency', 'composites pixels', 'remove'],
  ['copy', 'copies retained', 'results'],
  ['wheel', 'routes wheel history', 'results'],
  ['copy', 'copies retained', 'body'],
  ['copy', 'copies retained', 'connect'],
] as const)(
  'releases %s (%s) when %s fails',
  async (file, title, stage) => {
    vi.resetModules();
    vi.stubEnv('BOXRIG_P2_WIN32_SOURCE_DIR', '/boxrig-test');
    vi.stubEnv('AGENT_ROVER_WIN11_HOST', undefined);
    vi.stubEnv('AGENT_ROVER_WIN11_TOKEN', undefined);
    vi.stubEnv('BOXRIG_TEST_DISABLE_SYSTEM_EFFECTS', '0');
    const failure = new Error('injected ' + stage + ' failure');
    const injected = vi.fn().mockRejectedValue(failure);
    const bodyFailure = new Error('remote test body failed');
    const release = vi.fn();
    let releasedBeforeReporting: number | undefined;
    const agent = {
      release,
      [Symbol.dispose]: release,
      capabilities: vi.fn().mockResolvedValue({
        platform: 'windows',
        features: ['agent.native-windows'],
      }),
      clipboard: {
        readText:
          stage === 'clipboard'
            ? injected
            : vi.fn().mockResolvedValue('original'),
        writeText: vi.fn().mockResolvedValue(undefined),
      },
      files: {
        mkdtemp:
          stage === 'directory'
            ? injected
            : vi.fn().mockResolvedValue('C:\\case'),
        remove:
          stage === 'remove' ? injected : vi.fn().mockResolvedValue(undefined),
        syncDirectory:
          stage === 'body' ? injected : vi.fn().mockRejectedValue(bodyFailure),
        writeFile: vi.fn().mockRejectedValue(bodyFailure),
      },
      processes: { launchManaged: vi.fn().mockRejectedValue(bodyFailure) },
    };
    const connect =
      stage === 'connect' ? injected : vi.fn().mockResolvedValue(agent);
    vi.doMock('./agent-connection.js', () => ({ connectRemoteAgent: connect }));
    vi.doMock('agent-rover', async () => ({
      ...(await vi.importActual<typeof import('agent-rover')>('agent-rover')),
      saveDiagnostics: vi.fn().mockResolvedValue(undefined),
    }));
    vi.doMock('node:child_process', async () => ({
      ...(await vi.importActual<typeof import('node:child_process')>(
        'node:child_process'
      )),
      execFileSync: vi.fn(),
    }));
    vi.doMock('node:fs/promises', async () => ({
      ...(await vi.importActual<typeof import('node:fs/promises')>(
        'node:fs/promises'
      )),
      mkdir: async () => {
        if (stage === 'local-directory' && connect.mock.calls.length !== 0)
          await injected();
      },
      readFile: vi.fn().mockResolvedValue(Buffer.from('fixture')),
      writeFile: async (path: string) => {
        if (stage === 'results' && path.endsWith('results.json')) {
          // Purely local reporting must not keep the shared Windows connection.
          releasedBeforeReporting = release.mock.calls.length;
          await injected();
        }
      },
    }));

    // Run the real registered case with a failing remote dependency, without
    // registering another physical test or opening a connection to Windows.
    const cases: Array<{ name: string; run: () => Promise<void> }> = [];
    const register = (name: string, run: () => Promise<void>) => {
      cases.push({ name, run });
    };
    vi.doMock('vitest', async () => ({
      ...(await vi.importActual<typeof import('vitest')>('vitest')),
      describe: (_name: string, suite: () => void) => suite(),
      it: Object.assign(register, {
        each:
          (rows: readonly unknown[]) =>
          (name: string, body: (...args: unknown[]) => Promise<void>) => {
            for (const row of rows)
              register(
                name,
                async () => await body(...(Array.isArray(row) ? row : [row]))
              );
          },
      }),
    }));
    await load[file]();
    const physical = cases.find((test) => test.name.startsWith(title));
    expect(physical).toBeDefined();
    await expect(physical!.run()).rejects.toThrow();
    expect(injected).toHaveBeenCalledOnce();
    expect(connect).toHaveBeenCalledOnce();
    const [options] = connect.mock.calls[0];
    expect(options).not.toHaveProperty('host');
    expect(options).not.toHaveProperty('port');
    expect(options).not.toHaveProperty('authToken');
    expect(release).toHaveBeenCalledTimes(stage === 'connect' ? 0 : 1);
    if (stage === 'results') expect(releasedBeforeReporting).toBe(1);
  },
  10000
);
