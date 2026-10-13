import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { type AppWindow, type RemoteManagedProcess } from 'agent-rover';
import { connectRemoteAgent } from './agent-connection.js';
import { it } from 'vitest';

const readCopyState = async (readTitle: () => Promise<string>) => {
  const deadline = Date.now() + 15000;
  for (;;) {
    const title = await readTitle();
    const m = /^boxrig-copy-(\d+)-([a-zMR])-(\d+)-(\d+)-(\d+)-(\d+)$/.exec(
      title
    );
    if (m) {
      return {
        sequence: Number(m[1]),
        tag: m[2],
        height: Number(m[3]),
        width: Number(m[4]),
        reports: Number(m[5]),
        buttonReports: Number(m[6]),
      };
    }
    // agent-rover 0.7.0 reads title length and contents separately. Wait for
    // a complete acknowledgement if the title grows between those reads.
    // https://github.com/kekyo/agent-rover/blob/0.7.0/agents/windows/win32_windows.cpp#L89-L100
    assert(Date.now() < deadline, `Incomplete copy acknowledgement: ${title}`);
  }
};

it('reads a complete copy acknowledgement when its title grows', async () => {
  const titles = ['boxrig-copy-10-z-15-7-0-', 'boxrig-copy-10-z-15-7-0-0'];
  const state = await readCopyState(async () => {
    const title = titles.shift();
    assert(title, 'a complete acknowledgement was available');
    return title;
  });
  assert.deepEqual(state, {
    sequence: 10,
    tag: 'z',
    height: 15,
    width: 7,
    reports: 0,
    buttonReports: 0,
  });
});

it.each([
  ['amd64', 'win32'],
  ['amd64', 'opengl'],
  ['amd64', 'd3d11'],
  ['i686', 'win32'],
  ['i686', 'opengl'],
  ['i686', 'd3d11'],
])(
  'copies retained selections once on right click (%s %s)',
  async (arch, renderer) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    assert(source);
    const output = resolve(
      source,
      'build/test-results/step23-copy',
      arch,
      renderer,
      String(Date.now())
    );
    await mkdir(output, { recursive: true });

    const results: Array<Record<string, string | number>> = [];
    try {
      using agent = await connectRemoteAgent({
        timeoutMs: 30000,
      });
      let directory: string | undefined,
        child: RemoteManagedProcess | undefined,
        window: AppWindow | undefined;
      const originalClipboard = await agent.clipboard.readText();
      try {
        directory = await agent.files.mkdtemp('boxrig-copy-');
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
            BOXRIG_TEST_SAMPLE_COPY_POLICY: '1',
            BOXRIG_TRANSPARENCY_MODE: 'opaque',
          },
          captureStderr: true,
          captureStdout: true,
          createNoWindow: true,
        });
        window = await child.waitForWindow(
          {
            className: 'boxrig_win32_sample',
            titleRegex: /^boxrig-copy-0-R-/,
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
          return await readCopyState(async () => {
            window = await window!.refresh();
            return window.title;
          });
        };
        let state = await readState();
        const send = async (key: string) => {
          const sequence = (await readState()).sequence;
          await agent.keyboard.press(key);
          const deadline = Date.now() + 15000;
          do {
            state = await readState();
            assert(
              Date.now() < deadline,
              `PTY acknowledgement ${key} after ${sequence}: ${JSON.stringify(state)}`
            );
          } while (state.sequence <= sequence || state.tag !== key);
        };
        const point = (row: number, column: number, half: number) => ({
          x: window!.clientBounds.x + Math.floor(state.width * (column + half)),
          y: window!.clientBounds.y + Math.floor(state.height * (row + 0.5)),
        });
        const select = async (
          row1: number,
          col1: number,
          row2: number,
          col2: number,
          mods: ReadonlyArray<'Control' | 'Shift'>
        ) => {
          for (const m of mods) await agent.keyboard.down(m);
          try {
            await agent.mouse.down({
              button: 'left',
              point: point(row1, col1, 0.25),
            });
            await agent.mouse.move(point(row2, col2, 0.75));
            await agent.mouse.up({ button: 'left' });
          } finally {
            for (const m of [...mods].reverse()) await agent.keyboard.up(m);
          }
          await send('z');
        };
        const right = async () => {
          await agent.mouse.click(point(4, 10, 0.25), { button: 'right' });
          await send('z');
        };
        const expectClipboard = async (expected: string, name: string) => {
          const text = (await agent.clipboard.readText()).replaceAll(
            '\r\n',
            '\n'
          );
          assert.equal(text, expected, name);
          results.push({ name, text });
        };
        await agent.clipboard.writeText('sentinel');
        await right();
        await expectClipboard('sentinel', 'no-selection-keeps-clipboard');
        await select(2, 0, 2, 10, []);
        await agent.keyboard.press('c', { modifiers: ['Control', 'Shift'] });
        await send('z');
        await expectClipboard('copy-target', 'keyboard-copy-baseline');
        await select(4, 1, 4, 1, []);
        await right();
        await expectClipboard('copy-target', 'empty-keeps-clipboard');
        await select(0, 1, 0, 3, []);
        const linear = '\u754ce\u0301';
        await right();
        await expectClipboard(linear, 'linear-right-copy');
        await agent.clipboard.writeText('after-press');
        await agent.mouse.down({ button: 'right', point: point(4, 10, 0.25) });
        await send('z');
        await expectClipboard(linear, 'copy-on-press');
        await agent.clipboard.writeText('between-press-and-release');
        await agent.mouse.up({ button: 'right' });
        await send('z');
        await expectClipboard(
          'between-press-and-release',
          'release-does-not-copy-again'
        );
        await agent.keyboard.press('c', { modifiers: ['Control', 'Shift'] });
        await send('z');
        await expectClipboard(linear, 'selection-retained');
        await send('r');
        await right();
        await expectClipboard(linear, 'reporting-copy-exclusion');
        assert.equal(state.reports, 0);
        await select(0, 1, 1, 3, ['Control']);
        const rectangle = '\u754ce\u0301\n\u754co\u0308';
        await right();
        await expectClipboard(rectangle, 'rectangle-right-copy');
        assert.equal(state.reports, 0);
        await send('x');
        await right();
        await expectClipboard(rectangle, 'stale-keeps-clipboard');
        assert.equal(state.reports, 0);
        assert.match(await child.stderrText(), /could not copy selection/);
        await select(4, 1, 4, 1, ['Shift']);
        // Observe each input before releasing the button, so Windows cannot
        // coalesce away all valid drag-motion reports in mode 1002.
        const waitForReports = async (
          reports: number,
          buttonReports: number
        ) => {
          const deadline = Date.now() + 15000;
          do {
            state = await readState();
            assert(
              Date.now() < deadline,
              `mouse reports >= ${reports}, buttons = ${buttonReports}: ${JSON.stringify(state)}`
            );
          } while (
            state.reports < reports ||
            state.buttonReports < buttonReports
          );
          assert.equal(state.buttonReports, buttonReports);
        };
        await agent.mouse.down({ button: 'right', point: point(4, 10, 0.25) });
        await waitForReports(1, 1);
        const reportsBeforeMotion = state.reports;
        await agent.mouse.move(point(4, 11, 0.25));
        await waitForReports(reportsBeforeMotion + 1, 1);
        await agent.mouse.up({ button: 'right' });
        await waitForReports(state.reports + 1, 2);
        await expectClipboard(rectangle, 'empty-reporting-fallback');
        assert.equal(state.buttonReports, 2);
        await send('n');
        await agent.keyboard.press('q');
        const exit = await child.waitForExit({ timeoutMs: 15000 });
        assert.equal(exit.root.exitCode, 0);
      } catch (error) {
        if (window) {
          const current = await window.refresh();
          const screenshot = await current.screenshot();
          await writeFile(resolve(output, 'failure.png'), screenshot.image);
          await writeFile(
            resolve(output, 'failure.json'),
            JSON.stringify(
              { title: current.title, error: String(error) },
              null,
              2
            )
          );
        }
        if (child)
          await writeFile(
            resolve(output, 'stderr.txt'),
            await child.stderrText()
          );
        throw error;
      } finally {
        if (child) await child.releaseAsync();
        if (directory) await agent.files.remove(directory, { recursive: true });
        await agent.clipboard.writeText(originalClipboard);
      }
    } finally {
      await writeFile(
        resolve(output, 'results.json'),
        JSON.stringify({ results }, null, 2) + '\n'
      );
    }
  }
);
