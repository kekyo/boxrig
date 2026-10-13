import { spawn, type ChildProcessWithoutNullStreams } from 'node:child_process';

import { createDeferred } from 'async-primitives';
import {
  createGtkAppLauncher,
  type GtkAppEnvironment,
  type GtkWindowElement,
} from 'gestament';
import { toPass } from 'gestament/testing';
import { afterEach, describe, expect, it } from 'vitest';

interface TestConfiguration {
  readonly samplePath: string;
  readonly ptyHelperPath: string;
  readonly clipboardPeerPath: string;
}

interface ProcessResult {
  readonly code: number | null;
  readonly stdout: string;
  readonly stderr: string;
}

interface ClipboardOwner {
  readonly release: () => Promise<void>;
}

const requireEnvironment = (name: string): string => {
  const value = process.env[name];

  if (value === undefined || value.length === 0) {
    throw new Error(`${name} must be defined by Makefile.`);
  }
  return value;
};

const configuration: TestConfiguration = {
  samplePath: requireEnvironment('BOXRIG_GTK3_SAMPLE'),
  ptyHelperPath: requireEnvironment('BOXRIG_GTK3_PTY_HELPER'),
  clipboardPeerPath: requireEnvironment('BOXRIG_GTK3_CLIPBOARD_PEER'),
};

const launcher = createGtkAppLauncher({
  accessibilitySession: 'minimal',
  appPath: configuration.samplePath,
  args: [
    ...(process.env.BOXRIG_GTK_RENDERER === undefined
      ? []
      : [`--renderer=${process.env.BOXRIG_GTK_RENDERER}`]),
    '--',
    configuration.ptyHelperPath,
  ],
  display: 'xvfb',
  env: { GTK_MODULES: undefined },
  outputBufferBytes: 64 * 1024,
  timeoutMs: 15_000,
  xvfbTrayHost: false,
});

afterEach(async () => {
  await launcher.release();
});

const observeProcess = (
  child: ChildProcessWithoutNullStreams,
  signal: AbortSignal
): Readonly<{
  completion: Promise<ProcessResult>;
  stdout: () => string;
}> => {
  const completed = createDeferred<ProcessResult>(signal);
  let stdout = '';
  let stderr = '';

  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stdout.on('data', (chunk: string) => {
    stdout += chunk;
  });
  child.stderr.on('data', (chunk: string) => {
    stderr += chunk;
  });
  child.once('error', (error) => {
    completed.reject(error);
  });
  child.once('close', (code) => {
    completed.resolve({ code, stderr, stdout });
  });
  return {
    completion: completed.promise,
    stdout: () => stdout,
  };
};

const peerEnvironment = (environment: GtkAppEnvironment): NodeJS.ProcessEnv =>
  ({ ...process.env, ...environment }) as NodeJS.ProcessEnv;

const readClipboard = async (
  environment: GtkAppEnvironment,
  target: 'primary' | 'clipboard'
): Promise<string> => {
  const signal = AbortSignal.timeout(10_000);
  const child = spawn(configuration.clipboardPeerPath, ['get', target], {
    env: peerEnvironment(environment),
    signal,
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  const observation = observeProcess(child, signal);

  child.stdin.end();
  const result = await observation.completion;
  expect(result.code, result.stderr).toBe(0);
  return result.stdout;
};

const startClipboardOwner = async (
  environment: GtkAppEnvironment,
  target: 'primary' | 'clipboard',
  text: string
): Promise<ClipboardOwner> => {
  const signal = AbortSignal.timeout(10_000);
  const child = spawn(configuration.clipboardPeerPath, ['set', target, text], {
    env: peerEnvironment(environment),
    signal,
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  const observation = observeProcess(child, signal);

  await toPass(
    () => {
      expect(observation.stdout()).toContain('READY\n');
    },
    { message: `clipboard ${target} owner did not become ready` }
  );
  return {
    release: async () => {
      child.stdin.end();
      const result = await observation.completion;
      expect(result.code, result.stderr).toBe(0);
    },
  };
};

const waitForTitle = async (
  window: GtkWindowElement,
  expected: string
): Promise<void> => {
  await toPass(
    async () => {
      expect((await window.info()).name).toBe(expected);
    },
    { message: `GTK 3 sample did not publish title ${expected}` }
  );
};

const pressShortcut = async (
  input: Readonly<{
    setModifier: (
      modifier: 'shift' | 'control',
      pressed: boolean
    ) => Promise<void>;
    pressKey: (key: string) => Promise<void>;
  }>,
  key: string
): Promise<void> => {
  await input.setModifier('control', true);
  await input.setModifier('shift', true);
  try {
    await input.pressKey(key);
  } finally {
    await input.setModifier('shift', false);
    await input.setModifier('control', false);
  }
};

describe('GTK 3 terminal sample', () => {
  it('keeps the PTY grid and cell geometry aligned through repeated font shrink and restore', async () => {
    const app = await launcher.launch();
    const terminal = await app.getById('boxrig-terminal');
    const window = await app.windowAt(0);

    if (window === undefined || window.kind !== 'window') {
      throw new Error('GTK 3 sample did not expose a top-level window.');
    }
    await window.activate();
    await app.input.pressKey('g');
    await waitForTitle(window, 'grid-24-80');
    const initial = (await terminal.capture()).bounds;

    for (let cycle = 0; cycle < 3; cycle++) {
      for (const [key, shift, shrink] of [
        ['minus', false, true],
        ['equal', true, false],
        ['minus', false, true],
        ['0', false, false],
      ] as const) {
        await app.input.setModifier('control', true);
        await app.input.setModifier('shift', shift);
        try {
          await app.input.pressKey(key);
        } finally {
          await app.input.setModifier('shift', false);
          await app.input.setModifier('control', false);
        }
        await toPass(async () => {
          const { bounds } = await terminal.capture();

          if (shrink) {
            expect(bounds.width).toBeLessThanOrEqual(initial.width);
            expect(bounds.height).toBeLessThan(initial.height);
          } else {
            expect(bounds.width).toBe(initial.width);
            expect(bounds.height).toBe(initial.height);
          }
        });
        const { bounds } = await terminal.capture();

        // Alternate cell and grid replies so an old title cannot acknowledge
        // the next shortcut. Both replies come from the real PTY child.
        await app.input.pressKey('p');
        await waitForTitle(
          window,
          `cell-${bounds.height / 24}-${bounds.width / 80}`
        );
        await app.input.pressKey('g');
        await waitForTitle(window, 'grid-24-80');
      }
    }
    await app.input.pressKey('q');
    await toPass(async () => {
      expect((await app.output()).exitCode).toBe(0);
    });
  });

  it('applies native resize round trips to the PTY through gestament', async () => {
    const app = await launcher.launch(['--report-resize']);
    const terminal = await app.getById('boxrig-terminal');
    const window = await app.windowAt(0);

    if (window === undefined || window.kind !== 'window') {
      throw new Error('GTK 3 sample did not expose a top-level window.');
    }
    await window.activate();
    await app.input.pressKey('g');
    await waitForTitle(window, 'grid-24-80');
    const initialWindow = await window.bounds();
    const initialTerminal = await terminal.capture();
    const cellWidth = initialTerminal.bounds.width / 80;
    const cellHeight = initialTerminal.bounds.height / 24;

    expect(cellWidth).toBeGreaterThan(0);
    expect(cellHeight).toBeGreaterThan(0);
    for (const [rows, columns] of [
      [30, 96],
      [20, 72],
      [30, 96],
      [24, 80],
    ] as const) {
      await window.resizeTo(
        initialWindow.width + (columns - 80) * cellWidth,
        initialWindow.height + (rows - 24) * cellHeight
      );
      await toPass(async () => {
        const capture = await terminal.capture();

        expect(capture.bounds.width).toBe(columns * cellWidth);
        expect(capture.bounds.height).toBe(rows * cellHeight);
        expect((await window.info()).name).toBe(`grid-${rows}-${columns}`);
      });
    }
    await app.input.pressKey('q');
    await toPass(async () => {
      expect((await app.output()).exitCode).toBe(0);
    });
  });

  it('handles selection, clipboard, paste, and PTY input through gestament', async () => {
    const app = await launcher.launch();
    const terminal = await app.getById('boxrig-terminal');
    const terminalInfo = await terminal.info();
    const window = await app.windowAt(0);
    const environment = await app.environment();

    expect(terminalInfo.roleName).toBe('terminal');
    expect(terminalInfo.interfaces).not.toContain('Text');
    if (window === undefined || window.kind !== 'window') {
      throw new Error('GTK 3 sample did not expose a top-level window.');
    }
    await window.activate();
    await app.input.pressKey('x');
    await waitForTitle(window, 'host-input-ready');

    const capture = await terminal.capture();
    const cellWidth = capture.bounds.width / 80;
    const cellHeight = capture.bounds.height / 24;
    const startX = Math.floor(capture.bounds.x + cellWidth * 0.2);
    const endX = Math.floor(capture.bounds.x + cellWidth * 10.8);
    const rowY = Math.floor(capture.bounds.y + cellHeight * 0.5);

    expect(cellWidth).toBeGreaterThan(0);
    expect(cellHeight).toBeGreaterThan(0);
    await app.input.moveMouseTo(startX, rowY);
    await app.input.setMouseButton('left', true);
    await app.input.moveMouseTo(endX, rowY);
    await app.input.setMouseButton('left', false);

    await pressShortcut(app.input, 'c');
    await app.input.pressKey('d');
    await waitForTitle(window, 'clipboard-copy-ready');
    expect(await readClipboard(environment, 'clipboard')).toBe('copy-target');
    expect(await readClipboard(environment, 'primary')).toBe('copy-target');

    const secondRowY = Math.floor(capture.bounds.y + cellHeight * 1.5);
    const rectangleEndX = Math.floor(capture.bounds.x + cellWidth * 4.8);

    await app.input.moveMouseTo(startX, rowY);
    await app.input.setModifier('control', true);
    await app.input.setMouseButton('left', true);
    await app.input.setModifier('control', false);
    await app.input.setModifier('shift', true);
    await app.input.moveMouseTo(rectangleEndX, secondRowY);
    await app.input.setModifier('shift', false);
    await app.input.setMouseButton('left', false);
    await app.input.pressKey('e');
    await waitForTitle(window, 'rectangular-copy-ready');
    expect(await readClipboard(environment, 'primary')).toBe('copy-\nblock');

    const clipboardOwner = await startClipboardOwner(
      environment,
      'clipboard',
      'P'
    );
    try {
      await pressShortcut(app.input, 'v');
      await waitForTitle(window, 'clipboard-paste-received');
    } finally {
      await clipboardOwner.release();
    }

    const primaryOwner = await startClipboardOwner(environment, 'primary', 'M');
    try {
      await app.input.moveMouseTo(startX, rowY);
      await app.input.setMouseButton('middle', true);
      await app.input.setMouseButton('middle', false);
      await waitForTitle(window, 'primary-paste-received');
    } finally {
      await primaryOwner.release();
    }

    await app.input.pressKey('q');
    await toPass(
      async () => {
        expect((await app.output()).exitCode).toBe(0);
      },
      { message: 'GTK 3 sample did not preserve the PTY child exit status' }
    );
  });
});
