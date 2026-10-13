import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { type AppWindow, type RemoteManagedProcess } from 'agent-rover';
import { connectRemoteAgent } from './agent-connection.js';
import { it } from 'vitest';

it.each(
  [
    ['amd64', 'win32'],
    ['amd64', 'opengl'],
    ['amd64', 'd3d11'],
    ['i686', 'win32'],
    ['i686', 'opengl'],
    ['i686', 'd3d11'],
  ].flatMap(([arch, renderer]) =>
    ['system', 'packaged-required'].map((policy) => [arch, renderer, policy])
  )
)(
  'routes wheel history and zoom once (%s %s %s)',
  async (arch, renderer, policy) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    assert(source);
    const output = resolve(
      source,
      'build/test-results/step23-wheel',
      arch,
      renderer,
      policy,
      String(Date.now())
    );
    await mkdir(output, { recursive: true });

    const results: Array<Record<string, number | string>> = [];
    let diagnostics = '';
    try {
      using agent = await connectRemoteAgent({
        timeoutMs: 30000,
      });
      let directory: string | undefined,
        child: RemoteManagedProcess | undefined,
        window: AppWindow | undefined;
      try {
        directory = await agent.files.mkdtemp('boxrig-wheel-');
        const resolver = await agent.processes.launchManaged({
          path: 'cmd.exe',
          arguments: ['/d', '/c', 'cd'],
          workingDirectory: directory,
          captureStdout: true,
          createNoWindow: true,
        });
        try {
          assert.equal(
            (await resolver.waitForExit({ timeoutMs: 30000 })).root.exitCode,
            0
          );
          directory = (await resolver.stdoutText()).trim();
          assert.match(directory, /^[a-z]:\\/i);
        } finally {
          await resolver.releaseAsync();
        }
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
            BOXRIG_TEST_SAMPLE_WHEEL_POLICY: '1',
            BOXRIG_TRANSPARENCY_MODE: 'opaque',
            BOXRIG_CONPTY: policy,
            BOXRIG_CONPTY_DIRECTORY: directory,
          },
          captureStderr: true,
          captureStdout: true,
          createNoWindow: true,
        });
        window = await child.waitForWindow(
          {
            className: 'boxrig_win32_sample',
            titleRegex: /^boxrig-wheel-0-R-/,
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
            const m =
              /^boxrig-wheel-(\d+)-([a-zMR])-(\d+)-(\d+)-(\d+)-(\d+)-(\d+)$/.exec(
                window.title
              );
            if (m)
              return {
                sequence: Number(m[1]),
                tag: m[2],
                height: Number(m[3]),
                width: Number(m[4]),
                rows: Number(m[5]),
                columns: Number(m[6]),
                reports: Number(m[7]),
              };
            assert(
              Date.now() < deadline,
              'complete PTY acknowledgement: ' + window.title
            );
          }
        };
        let state = await readState();
        const initial = { ...state };
        const send = async (key: string) => {
          const sequence = state.sequence;
          await agent.keyboard.press(key);
          const until = Date.now() + 15000;
          do {
            state = await readState();
            assert(Date.now() < until, 'PTY acknowledgement ' + key);
          } while (state.sequence <= sequence);
          return state;
        };
        const rect = async () => {
          window = await window!.refresh();
          return {
            x: window.clientBounds.x,
            y: window.clientBounds.y,
            width: 600,
            height: 240,
          };
        };
        const colorRow = (bytes: Buffer, at: number, tolerance: number) => {
          let closest = -1,
            error = Infinity;
          for (let row = 0; row < 72; row++) {
            const distance = Math.max(
              ...[16 + row * 3, 190 - row * 2, 50 + row * 2].map((c, i) =>
                Math.abs(bytes[at + i]! - c)
              )
            );
            if (distance < error) {
              error = distance;
              closest = row;
            }
          }
          return error <= tolerance ? closest : -1;
        };
        const observeRow = async () => {
          const area = await rect(),
            capture = await agent.screenshot({ rect: area });
          const bytes = execFileSync(
            'ffmpeg',
            [
              '-v',
              'error',
              '-f',
              'image2pipe',
              '-i',
              'pipe:0',
              '-f',
              'rawvideo',
              '-pix_fmt',
              'rgb24',
              'pipe:1',
            ],
            {
              input: capture.image,
              maxBuffer: area.width * area.height * 3 + 10000,
            }
          );
          await writeFile(resolve(output, 'last.png'), capture.image);
          return colorRow(
            bytes,
            (Math.floor(state.height / 2) * area.width +
              Math.floor(state.width * 10.5)) *
              3,
            1
          );
        };
        const expectRow = async (expected: number, name: string) => {
          const until = Date.now() + 15000;
          let actual;
          do {
            actual = await observeRow();
            assert(
              Date.now() < until,
              `${name}: expected ${expected}, got ${actual}`
            );
          } while (actual !== expected);
          results.push({ name, row: actual, ...state });
        };
        const wheel = async (
          delta: number,
          modifiers: string[],
          repeat: number
        ) => {
          window = await window!.refresh();
          const point = {
            x: window.clientBounds.x + Math.floor(state.width * 2.5),
            y: window.clientBounds.y + Math.floor(state.height * 2.5),
          };
          for (const m of modifiers) await agent.keyboard.down(m);
          try {
            for (let i = 0; i < repeat; i++)
              await agent.mouse.wheel({ deltaY: delta, point });
          } finally {
            for (const m of [...modifiers].reverse())
              await agent.keyboard.up(m);
          }
        };
        const record = async (name: string, expected: number) => {
          const area = await rect(),
            path = resolve(output, name + '.mp4');
          await agent.mouse.move({
            x: window!.frameBounds.x + 80,
            y: window!.frameBounds.y + 12,
          });
          const metadata = await agent.recordVideo(2000, path, {
            fps: 30,
            rect: area,
          });
          assert(!metadata.clipped);
          const bytes = execFileSync(
            'ffmpeg',
            [
              '-v',
              'error',
              '-i',
              path,
              '-f',
              'rawvideo',
              '-pix_fmt',
              'rgb24',
              'pipe:1',
            ],
            { maxBuffer: area.width * area.height * 3 * 100 }
          );
          const stride = area.width * area.height * 3;
          assert.equal(bytes.length / stride, metadata.frameCount);
          assert(metadata.frameCount >= 45);
          for (let f = 15; f < metadata.frameCount; f++) {
            const row = colorRow(
              bytes,
              f * stride +
                (Math.floor(state.height / 2) * area.width +
                  Math.floor(state.width * 10.5)) *
                  3,
              5
            );
            assert(Math.abs(row - expected) <= 1, name + ' stable history row');
          }
          results.push({
            name,
            row: expected,
            frames: metadata.frameCount,
            ...state,
          });
        };
        const live = await observeRow();
        assert(live >= 3);
        await record('live', live);
        await wheel(120, [], 1);
        await expectRow(live - 3, 'up-three');
        await wheel(-120, [], 1);
        await expectRow(live, 'down-three');
        for (let i = 0; i < 4; i++) {
          await wheel(30, [], 1);
          await expectRow(i === 3 ? live - 3 : live, 'quarter-step-' + i);
        }
        await wheel(-120, [], 1);
        await expectRow(live, 'quarter-step-reset');
        await wheel(120, [], 40);
        await expectRow(0, 'oldest-clamp');
        await wheel(120, [], 4);
        await expectRow(0, 'oldest-stays');
        await wheel(-120, [], 40);
        await expectRow(live, 'live-clamp');
        await send('r');
        await wheel(120, [], 1);
        const until = Date.now() + 15000;
        do {
          state = await readState();
          assert(Date.now() < until, 'mouse report');
        } while (state.reports !== 1);
        await expectRow(live, 'reporting-no-history');
        await wheel(120, ['Shift'], 1);
        await expectRow(live - 3, 'shift-override');
        await wheel(-120, ['Shift'], 1);
        await expectRow(live, 'shift-restore');
        await wheel(120, ['Control'], 1);
        await send('z');
        assert(state.height > initial.height || state.width > initial.width);
        assert.equal(state.rows, initial.rows);
        assert.equal(state.columns, initial.columns);
        assert.equal(state.reports, 1);
        await expectRow(live, 'zoom');
        await record('zoomed', live);
        await agent.keyboard.press('0', { modifiers: ['Control'] });
        await send('z');
        assert.equal(state.height, initial.height);
        assert.equal(state.width, initial.width);
        await expectRow(live, 'reset');
        await record('reset', live);
        await wheel(-120, ['Control', 'Shift'], 1);
        await send('z');
        assert(state.height < initial.height || state.width < initial.width);
        assert.equal(state.reports, 1);
        await agent.keyboard.press('0', { modifiers: ['Control'] });
        await send('z');
        await send('n');
        await wheel(120, [], 1);
        await expectRow(live - 3, 'primary-before-alternate');
        // System ConPTY on build 22631 regenerates alternate-screen changes as
        // ordinary redraws. Its wheel/zoom coverage above observes that transport;
        // preserving an independent primary viewport requires VT passthrough.
        if (policy === 'packaged-required') {
          await send('a');
          await wheel(120, [], 10);
          await send('z');
          assert.equal(state.reports, 1);
          await send('p');
          await expectRow(live - 3, 'alternate-preserves-primary');
        }
        await agent.keyboard.press('q');
        const exit = await child.waitForExit({ timeoutMs: 15000 });
        assert.equal(exit.root.exitCode, 0);
        // Read the complete report after redirected stdio has been flushed.
        diagnostics = await child.stdoutText();
        assert(
          diagnostics.includes(
            `pty-backend=${policy === 'system' ? 'system' : 'packaged'} fallback=0`
          ),
          diagnostics
        );
      } finally {
        if (child) await child.releaseAsync();
        if (directory) await agent.files.remove(directory, { recursive: true });
      }
    } finally {
      await writeFile(
        resolve(output, 'results.json'),
        JSON.stringify({ policy, diagnostics, results }, null, 2) + '\n'
      );
    }
  }
);
