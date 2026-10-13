import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { type AppWindow, type RemoteManagedProcess } from 'agent-rover';
import { connectRemoteAgent } from './agent-connection.js';
import { it } from 'vitest';

const cases = (['amd64', 'i686'] as const).flatMap((arch) => [
  ...(['win32', 'opengl', 'd3d11'] as const).map(
    (renderer) => [arch, renderer, 'opaque'] as const
  ),
  [arch, 'win32', 'background'] as const,
]);
const osc = 'https://example.test/osc?x=1&y=2';
const regex =
  'https://example.test/regex/long/path/segment-one/segment-two/end';

it.each(cases)(
  'opens exactly one validated native link (%s %s %s)',
  async (arch, renderer, transparency) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    assert(source);
    const output = resolve(
      source,
      'build/test-results/step23-link-activation',
      arch,
      renderer,
      transparency,
      String(Date.now())
    );
    await mkdir(output, { recursive: true });
    const agent = await connectRemoteAgent({
      timeoutMs: 30000,
    });
    let directory: string | undefined;
    let child: RemoteManagedProcess | undefined;
    let window: AppWindow | undefined;
    let failure: unknown;
    let sampleExitCode: number | null | undefined;
    const results: Array<Record<string, unknown>> = [];
    const expected: string[] = [];
    let diagnostics = '';
    try {
      directory = await agent.files.mkdtemp('boxrig-link-open-');
      await agent.files.syncDirectory({
        localPath: resolve(source, `build/boxrig_term/win32/${arch}`),
        remotePath: directory,
        checksum: 'sha256',
      });
      await agent.files.writeFile(
        directory + '/boxrig_term.exe',
        await readFile(
          resolve(
            source,
            `build/tests/frontend_link_probes/win32/${arch}/boxrig_term.exe`
          )
        )
      );
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
          BOXRIG_TEST_SAMPLE_LINK_POLICY: '1',
          BOXRIG_TRANSPARENCY_MODE: transparency,
          BOXRIG_BACKGROUND_OPACITY:
            transparency === 'background' ? '75' : '100',
          BOXRIG_AUTO_URLS: '1',
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
      const point = (row: number, column: number) => ({
        x: window!.clientBounds.x + Math.floor(state.width * (column + 0.25)),
        y: window!.clientBounds.y + Math.floor(state.height * (row + 0.5)),
      });
      const requests = async () => {
        diagnostics = await child!.stderrText();
        assert(!diagnostics.includes('BOXRIG_TEST_URI_CONTRACT_FAILED'));
        assert(!diagnostics.includes('BOXRIG_TEST_URI_ENCODING_FAILED'));
        return [
          ...diagnostics.matchAll(/^BOXRIG_TEST_URI_OPEN=(.*)\r?$/gm),
        ].map((m) => m[1].replace(/\r$/, ''));
      };
      const expectOpen = async (name: string, uri: string) => {
        expected.push(uri);
        const deadline = Date.now() + 15000;
        let actual;
        do {
          actual = await requests();
          assert(Date.now() < deadline, name + ': ' + JSON.stringify(actual));
        } while (actual.length < expected.length);
        assert.deepEqual(actual, expected, name);
        results.push({ name, uris: actual });
      };
      const positive = async (name: string) => {
        await agent.mouse.click(point(1, 5));
        await expectOpen(name, regex);
      };
      await agent.mouse.move(point(0, 2));
      await send('z');
      assert.deepEqual(await requests(), [], 'hover and output do not launch');
      await agent.mouse.click(point(0, 2));
      await expectOpen('OSC 8 exact URI once', osc);
      await positive('regex full URI once');
      await agent.mouse.drag(point(0, 1), point(0, 8));
      await positive('drag does not launch');
      for (const count of [2, 3]) {
        for (let click = 0; click < count; click++)
          await agent.mouse.click(point(0, 2));
        await positive(
          count === 2
            ? 'double click does not launch'
            : 'triple click does not launch'
        );
      }
      await agent.mouse.click(point(2, 5));
      await positive('rejected OSC 8 does not fall back');
      await agent.mouse.click(point(0, 2), { modifiers: ['Shift'] });
      await positive('Shift selection does not launch');
      await agent.mouse.click(point(0, 2), { modifiers: ['Control'] });
      await positive('Control selection does not launch');
      const previousLeaveCount = (
        (await child.stderrText()).match(/BOXRIG_TEST_LINK_LEAVE/g) ?? []
      ).length;
      await agent.mouse.click(point(0, 2));
      await agent.mouse.move({
        x: window.frameBounds.x + window.frameBounds.width + 20,
        y: window.frameBounds.y + 20,
      });
      const leaveDeadline = Date.now() + 15000;
      while (
        ((await child.stderrText()).match(/BOXRIG_TEST_LINK_LEAVE/g) ?? [])
          .length <= previousLeaveCount
      )
        assert(Date.now() < leaveDeadline, 'native leave acknowledgement');
      await positive('leave cancels pending launch');
      await agent.mouse.click(point(0, 2));
      await send('x');
      await positive('text replacement cancels pending launch');
      await send('o');
      await agent.mouse.click(point(0, 2));
      await expectOpen(
        'missing handler is reported once',
        'https://example.test/new'
      );
      await positive('input continues after launch failure');
      diagnostics = await child.stderrText();
      assert.equal((diagnostics.match(/could not open URL/g) ?? []).length, 1);
      await agent.keyboard.press('q');
      sampleExitCode = (await child.waitForExit({ timeoutMs: 15000 })).root
        .exitCode;
      assert.equal(sampleExitCode, 0);
      assert.deepEqual(
        await requests(),
        expected,
        'no delayed duplicate on shutdown'
      );
    } catch (error) {
      failure = error;
      throw error;
    } finally {
      const cleanupErrors: unknown[] = [];
      for (const cleanup of [
        async () => {
          if (child) await child.releaseAsync();
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
      agent.release();
      await writeFile(
        resolve(output, 'results.json'),
        JSON.stringify(
          {
            arch,
            renderer,
            transparency,
            results,
            sampleExitCode,
            failure: failure === undefined ? undefined : String(failure),
            cleanupErrors: cleanupErrors.map(String),
          },
          null,
          2
        ) + '\n'
      );
      await writeFile(resolve(output, 'sample.stderr.log'), diagnostics);
      if (failure === undefined && cleanupErrors.length !== 0)
        throw new AggregateError(cleanupErrors, 'link activation cleanup');
    }
  }
);
