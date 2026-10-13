import assert from 'node:assert/strict';
import { readFile, mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { type AppWindow, type RemoteManagedProcess } from 'agent-rover';
import { connectRemoteAgent } from './agent-connection.js';
import { delay } from 'async-primitives';
import { PNG } from 'pngjs';
import { it } from 'vitest';

it.each([
  ['amd64', 'win32', 'opaque'],
  ['amd64', 'opengl', 'opaque'],
  ['amd64', 'd3d11', 'opaque'],
  ['i686', 'win32', 'opaque'],
  ['i686', 'opengl', 'opaque'],
  ['i686', 'd3d11', 'opaque'],
  ['amd64', 'win32', 'background'],
  ['i686', 'win32', 'background'],
])(
  'updates standard scrollbar pixels and PTY geometry (%s %s %s)',
  async (arch, renderer, mode) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    assert(source);
    using agent = await connectRemoteAgent({
      timeoutMs: 30000,
    });
    const output = resolve(
      source,
      'build/test-results/step23-scrollbar',
      arch,
      renderer,
      mode
    );
    await mkdir(output, { recursive: true });
    let directory: string | undefined;
    let child: RemoteManagedProcess | undefined;
    try {
      directory = await agent.files.mkdtemp('boxrig-scrollbar-');
      const cwd = await agent.processes.launchManaged({
        path: 'cmd.exe',
        arguments: ['/d', '/c', 'cd'],
        workingDirectory: directory,
        captureStdout: true,
        createNoWindow: true,
      });
      try {
        assert.equal(
          (await cwd.waitForExit({ timeoutMs: 30000 })).root.exitCode,
          0
        );
        directory = (await cwd.stdoutText()).trim();
      } finally {
        await cwd.releaseAsync({ timeoutMs: 30000 });
      }
      await agent.files.writeFile(
        directory + '/sample.exe',
        await readFile(
          resolve(source, `build/boxrig_term/win32/${arch}/boxrig_term.exe`)
        )
      );
      await agent.files.writeFile(
        directory + '/boxrig_term_launcher.exe',
        await readFile(
          resolve(
            source,
            `build/boxrig_term/win32/${arch}/boxrig_term_launcher.exe`
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
        path: directory + '/sample.exe',
        arguments: [
          `--renderer=${renderer}`,
          '--',
          '.\\helper.exe',
          '--sample-scenario',
        ],
        workingDirectory: directory,
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
        environment: {
          BOXRIG_SCROLLBAR: 'always',
          BOXRIG_TEST_SAMPLE_SCROLLBAR: '1',
          BOXRIG_CONPTY: 'system',
          BOXRIG_TRANSPARENCY_MODE: mode,
        },
      });
      let window = await child.waitForWindow(
        {
          className: 'boxrig_win32_sample',
          visible: true,
          titleRegex: /^boxrig-scrollbar-/,
        },
        { timeoutMs: 30000 }
      );
      window = await window.setBounds({ ...window.bounds, x: 40, y: 40 });
      const activate = async (): Promise<void> => {
        window = await window.activate();
        if (!window.active)
          await agent.mouse.click({
            x: window.frameBounds.x + 80,
            y: window.frameBounds.y + 12,
          });
        window = await window.refresh();
        assert(window.active);
        await agent.mouse.move({
          x: window.frameBounds.x + 80,
          y: window.frameBounds.y + 12,
        });
      };
      await activate();
      const read = (current: AppWindow) => {
        const match =
          /^boxrig-scrollbar-(\d+)-(.)-(\d+)-(\d+)-(\d+)-(\d+)$/.exec(
            current.title
          );
        assert(match, current.title);
        return {
          sequence: Number(match[1]),
          ch: Number(match[3]),
          cw: Number(match[4]),
          rows: Number(match[5]),
          columns: Number(match[6]),
        };
      };
      let last = read(window);
      assert.equal(last.rows, 24);
      assert.equal(last.columns, 80);
      const query = async (key: string): Promise<void> => {
        const sequence = last.sequence;
        await agent.keyboard.press(key);
        const deadline = Date.now() + 15000;
        do {
          window = await window.refresh();
          last = read(window);
          if (last.sequence > sequence) return;
          assert(Date.now() < deadline, `PTY acknowledgement: ${window.title}`);
          await delay(20);
        } while (true);
      };
      const grid = async (columns: number, label: string): Promise<void> => {
        const deadline = Date.now() + 15000;
        do {
          await query('G');
          if (last.columns === columns && last.rows === 24) break;
          assert(Date.now() < deadline, `${label}: ${JSON.stringify(last)}`);
        } while (true);
        console.log(arch, renderer, mode, label, last);
      };
      const cycle = async (): Promise<void> => {
        await agent.keyboard.press('F9', { modifiers: ['Control', 'Shift'] });
      };
      const pixels = async (visible: boolean, name: string): Promise<void> => {
        const deadline = Date.now() + 15000;
        do {
          window = await window.refresh();
          const shot = await window.screenshot();
          assert(!shot.clipped);
          const png = PNG.sync.read(shot.image);
          const x =
            window.clientBounds.x -
            shot.bounds.x +
            (visible
              ? window.clientBounds.width - 2
              : last.columns * last.cw - Math.ceil(last.cw / 2));
          const y =
            window.clientBounds.y -
            shot.bounds.y +
            Math.floor(window.clientBounds.height / 2);
          const at = (y * png.width + x) * 4;
          const rgb = Array.from(png.data.subarray(at, at + 3));
          const bar = [0x24, 0x48, 0x90, 0xa0, 0xb0].some((gray) =>
            rgb.every((value) => value === gray)
          );
          // A hidden bar must expose terminal pixels. Desktop pixels during
          // the native restore animation are not a completed terminal frame.
          const content = [
            [15, 25, 35],
            [16, 16, 16],
          ].some((background) =>
            rgb.every((value, index) => value === background[index])
          );
          if (visible ? bar : content) {
            await writeFile(resolve(output, name + '.png'), shot.image);
            return;
          }
          assert(
            Date.now() < deadline,
            `${name}: scrollbar pixel ${rgb.join(',')}`
          );
          await delay(20);
        } while (true);
      };
      await pixels(true, 'initial');
      const initialWidth = window.clientBounds.width;
      const initialHeight = last.ch;
      const initialCellWidth = last.cw;
      const expanded = Math.floor(initialWidth / last.cw);
      assert(expanded > 80);
      await cycle();
      await grid(expanded, 'auto empty');
      await pixels(false, 'auto-empty');
      await query('H');
      await grid(80, 'auto history');
      // Keep the drag captured while policy changes reclaim the bar's width.
      window = await window.refresh();
      const barX = window.clientBounds.x + window.clientBounds.width - 2;
      await agent.mouse.down({
        button: 'left',
        point: {
          x: barX,
          y: window.clientBounds.y + window.clientBounds.height - 8,
        },
      });
      try {
        await agent.mouse.move({ x: barX, y: window.clientBounds.y + 8 });
        const deadline = Date.now() + 15000;
        for (;;) {
          const shot = await window.screenshot();
          const png = PNG.sync.read(shot.image);
          const at =
            ((window.clientBounds.y + 8 - shot.bounds.y) * png.width +
              barX -
              shot.bounds.x) *
            4;
          if (png.data.subarray(at, at + 3).every((value) => value === 0xb0)) {
            await writeFile(
              resolve(output, 'dragged-to-oldest.png'),
              shot.image
            );
            break;
          }
          assert(Date.now() < deadline, 'thumb reaches the top while captured');
          await delay(20);
        }
        await cycle();
        await grid(expanded, 'hide during drag');
      } finally {
        await agent.mouse.up({
          button: 'left',
          point: {
            x: window.clientBounds.x + 20,
            y: window.clientBounds.y + 20,
          },
        });
      }
      await grid(expanded, 'never history');
      await cycle();
      await grid(80, 'always restored');
      await cycle();
      // Windows 11 22631's OS ConPTY serializes alternate-screen changes as
      // ordinary repaint commands; the captured stream contains no 1049h/l.
      // Direct component tests cover alternate-screen policy independently.
      // Keep this driver test on the default OS transport and its real history.
      await query('C');
      await grid(expanded, 'auto clear history');
      window = await window.minimize();
      assert(window.minimized);
      window = await window.restore();
      await activate();
      await grid(expanded, 'hidden after restore');
      await pixels(false, 'restored-hidden');
      await cycle(); // NEVER.
      await agent.keyboard.press('+', { modifiers: ['Control', 'Shift'] });
      const zoomDeadline = Date.now() + 15000;
      do {
        await query('G');
        assert(
          Date.now() < zoomDeadline,
          `zoom while hidden: ${JSON.stringify({ last, bounds: window.clientBounds })}`
        );
      } while (last.ch <= initialHeight && last.cw <= initialCellWidth);
      await grid(expanded, 'hidden zoom');
      await agent.keyboard.press('0', { modifiers: ['Control'] });
      const resetDeadline = Date.now() + 15000;
      do {
        await query('G');
        assert(Date.now() < resetDeadline, 'zoom reset');
      } while (last.ch !== initialHeight || last.cw !== initialCellWidth);
      await cycle(); // ALWAYS.
      await query('G');
      assert(last.columns < expanded);
      await pixels(true, 'restored-visible');
      window = await window.minimize();
      window = await window.restore();
      await activate();
      await pixels(true, 'visible-after-minimize');
      await agent.keyboard.press('Q');
      assert.equal(
        (await child.waitForExit({ timeoutMs: 30000 })).root.exitCode,
        0
      );
      await writeFile(resolve(output, 'stderr.log'), await child.stderrText());
      await child.releaseAsync({ timeoutMs: 30000 });
      child = undefined;
    } finally {
      if (child) {
        await writeFile(
          resolve(output, 'stderr.log'),
          await child.stderrText()
        );
        await child.releaseAsync({ timeoutMs: 30000 });
      }
      if (directory) {
        await agent.files.remove(directory, { recursive: true });
        assert(!(await agent.files.exists(directory)));
      }
      assert.equal((await agent.capabilities()).platform, 'windows');
    }
  }
);
