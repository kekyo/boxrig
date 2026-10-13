import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { type AppWindow, type RemoteManagedProcess } from 'agent-rover';
import { connectRemoteAgent } from './agent-connection.js';
import { it } from 'vitest';

const cases = (['amd64', 'i686'] as const).flatMap((arch) =>
  (['win32', 'opengl', 'd3d11'] as const).flatMap((renderer) =>
    (['enabled', 'disabled'] as const).map(
      (recognizer) => [arch, renderer, recognizer] as const
    )
  )
);

it.each(cases)(
  'changes native pointer for current validated links (%s %s regex-%s)',
  async (arch, renderer, recognizer) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    assert(source);
    const output = resolve(
      source,
      'build/test-results/step23-link-pointer',
      arch,
      renderer,
      recognizer,
      String(Date.now())
    );
    await mkdir(output, { recursive: true });

    const agent = await connectRemoteAgent({
      timeoutMs: 30000,
    });
    let directory: string | undefined,
      child: RemoteManagedProcess | undefined,
      peer: RemoteManagedProcess | undefined,
      window: AppWindow | undefined;
    const results: Array<Record<string, unknown>> = [];
    let primaryError: unknown;
    let sampleExitCode: number | null | undefined;
    try {
      directory = await agent.files.mkdtemp('boxrig-link-');
      await agent.files.syncDirectory({
        localPath: resolve(source, `build/boxrig_term/win32/${arch}`),
        remotePath: directory,
        checksum: 'sha256',
      });
      await agent.files.writeFile(
        directory + '/helper.exe',
        await readFile(
          resolve(source, `build/win32/${arch}/tests/pty_helper.exe`)
        )
      );
      await agent.files.writeFile(
        directory + '/cursor-probe.exe',
        await readFile(
          resolve(
            source,
            'build/win32/' + arch + '/tests/link_cursor_probe.exe'
          )
        )
      );
      await agent.files.writeFile(
        directory + '/focus-peer.exe',
        await readFile(
          resolve(source, `build/win32/${arch}/tests/blink_focus_peer.exe`)
        )
      );
      child = await agent.processes.launchManaged({
        path: directory + '/boxrig_term.exe',
        arguments: [
          `--renderer=${renderer}`,
          '--',
          '.\\helper.exe',
          '--sample-scenario',
        ],
        workingDirectory: directory,
        environment: {
          BOXRIG_TEST_SAMPLE_LINK_POLICY: '1',
          BOXRIG_TRANSPARENCY_MODE: 'opaque',
          BOXRIG_AUTO_URLS: recognizer === 'enabled' ? '1' : '0',
        },
        captureStderr: true,
        captureStdout: true,
        createNoWindow: true,
      });
      window = await child.waitForWindow(
        {
          className: 'boxrig_win32_sample',
          titleRegex: /^boxrig-link-0-R-/,
          visible: true,
        },
        { timeoutMs: 30000 }
      );
      window = await window.setBounds({ ...window.bounds, x: 40, y: 40 });
      await agent.mouse.click({
        x: window.frameBounds.x + 80,
        y: window.frameBounds.y + 12,
      });
      window = await window.refresh();
      assert(window.active);
      const readState = async () => {
        const deadline = Date.now() + 15000;
        for (;;) {
          window = await window!.refresh();
          const m = /^boxrig-link-(\d+)-([a-zR])-(\d+)-(\d+)$/.exec(
            window.title
          );
          if (m)
            return {
              sequence: Number(m[1]),
              tag: m[2],
              height: Number(m[3]),
              width: Number(m[4]),
            };
          // GetWindowTextLength and GetWindowText can straddle a longer title.
          assert(
            Date.now() < deadline,
            'complete PTY acknowledgement: ' + window.title
          );
        }
      };
      let state = await readState();
      const send = async (key: string) => {
        const sequence = (await readState()).sequence;
        await agent.keyboard.press(key);
        const deadline = Date.now() + 15000;
        do {
          state = await readState();
          assert(Date.now() < deadline, 'PTY acknowledgement ' + key);
        } while (state.sequence <= sequence || state.tag !== key);
      };
      const point = (row: number, column: number, half: number) => ({
        x: window!.clientBounds.x + Math.floor(state.width * (column + half)),
        y: window!.clientBounds.y + Math.floor(state.height * (row + 0.5)),
      });
      const cursor = async () => {
        const probe = await agent.processes.launchManaged({
          path: directory + '/cursor-probe.exe',
          captureStdout: true,
          captureStderr: true,
          createNoWindow: true,
        });
        try {
          const exit = await probe.waitForExit({ timeoutMs: 15000 });
          assert.equal(exit.root.exitCode, 0);
          return JSON.parse(await probe.stdoutText()) as {
            visible: boolean;
            hand: boolean;
            text: boolean;
            mousePresent: number;
          };
        } finally {
          await probe.releaseAsync();
        }
      };
      const expectCursor = async (name: string, hand: boolean) => {
        const deadline = Date.now() + 15000;
        let observed;
        for (;;) {
          observed = await cursor();
          assert.equal(observed.mousePresent, 1, 'physical mouse is connected');
          if (
            observed.visible &&
            observed.hand === hand &&
            observed.text !== hand
          )
            break;
          assert(Date.now() < deadline, name + ': ' + JSON.stringify(observed));
        }
        results.push({ name, hand, observed });
      };
      const move = async (row: number, column: number) => {
        await agent.mouse.move(point(row, column, 0.25));
        await send('z');
      };
      await move(0, 2);
      await expectCursor('explicit-osc8', true);
      await move(1, 5);
      await expectCursor('inferred-url', recognizer === 'enabled');
      await move(2, 5);
      await expectCursor('rejected-osc8-blocks-visible-url', false);
      const initialBounds = window.bounds;
      window = await window.setBounds({
        ...initialBounds,
        width: initialBounds.width - state.width * 40,
      });
      await move(2, 5);
      await expectCursor('reflow-url', recognizer === 'enabled');
      window = await window.setBounds(initialBounds);
      await move(2, 5);
      await expectCursor('reflow-rejected', false);
      await move(3, 5);
      await expectCursor('plain-text', false);
      await move(0, 2);
      await expectCursor('return-to-link', true);
      await send('x');
      await expectCursor('stationary-replaced-text', false);
      await send('o');
      await expectCursor('stationary-new-link', true);
      peer = await agent.processes.launchManaged({
        path: directory + '/focus-peer.exe',
        createNoWindow: true,
      });
      let peerWindow = await peer.waitForWindow(
        { className: 'boxrig_blink_focus_peer', visible: true },
        { timeoutMs: 15000 }
      );
      peerWindow = await peerWindow.setBounds({
        ...peerWindow.bounds,
        x: 1100,
        y: 700,
        width: 250,
        height: 150,
      });
      await agent.mouse.click({
        x: peerWindow.frameBounds.x + 50,
        y: peerWindow.frameBounds.y + 12,
      });
      await agent.mouse.move(point(0, 2, 0.25));
      await expectCursor('unfocused', false);
      await agent.mouse.click({
        x: window.frameBounds.x + 80,
        y: window.frameBounds.y + 12,
      });
      await move(0, 2);
      await expectCursor('refocused', true);
      await agent.mouse.move({
        x: peerWindow.frameBounds.x + 50,
        y: peerWindow.frameBounds.y + 12,
      });
      const leaveDeadline = Date.now() + 15000;
      while ((await cursor()).hand)
        assert(Date.now() < leaveDeadline, 'hand pointer left the terminal');
      await move(0, 2);
      await expectCursor('reentered', true);
      window = await window.minimize();
      window = await window.restore();
      await agent.mouse.click({
        x: window.frameBounds.x + 80,
        y: window.frameBounds.y + 12,
      });
      await move(0, 2);
      await expectCursor('restored', true);
      await send('s');
      await expectCursor('stationary-scroll-url', recognizer === 'enabled');
      await send('s');
      await expectCursor('stationary-scroll-rejected', false);
      await agent.keyboard.press('q');
      const exit = await child.waitForExit({ timeoutMs: 15000 });
      sampleExitCode = exit.root.exitCode;
      assert.equal(sampleExitCode, 0);
    } catch (error) {
      primaryError = error;
      throw error;
    } finally {
      const cleanupErrors: unknown[] = [];
      try {
        for (const cleanup of [
          async () => {
            if (child) await child.releaseAsync();
          },
          async () => {
            if (peer) await peer.releaseAsync();
          },
          async () => {
            if (directory)
              await agent.files.remove(directory, { recursive: true });
          },
        ]) {
          try {
            await cleanup();
          } catch (error) {
            cleanupErrors.push(error);
          }
        }
      } finally {
        agent.release();
      }
      await writeFile(
        resolve(output, 'results.json'),
        JSON.stringify(
          {
            arch,
            renderer,
            recognizer,
            results,
            sampleExitCode,
            failure:
              primaryError === undefined ? undefined : String(primaryError),
            cleanupErrors: cleanupErrors.map(String),
          },
          null,
          2
        ) + '\n'
      );
      if (primaryError === undefined && cleanupErrors.length !== 0)
        throw new AggregateError(cleanupErrors, 'native pointer cleanup');
    }
  }
);
