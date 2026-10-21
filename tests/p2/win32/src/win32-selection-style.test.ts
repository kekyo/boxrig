import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { type AppWindow, type RemoteManagedProcess } from 'agent-rover';
import { connectRemoteAgent } from './agent-connection.js';
import { it } from 'vitest';
it.each([
  ['amd64', 'win32'],
  ['amd64', 'opengl'],
  ['amd64', 'd3d11'],
  ['i686', 'win32'],
  ['i686', 'opengl'],
  ['i686', 'd3d11'],
])(
  'records selection fill, outline, alpha and reset (%s %s)',
  async (arch, renderer) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    assert(source, 'repository root required');
    const output = resolve(
      source,
      'build/test-results/step23-selection-style',
      arch,
      renderer,
      String(Date.now())
    );
    await mkdir(output, { recursive: true });

    const agent = await connectRemoteAgent({
      timeoutMs: 30000,
    });
    let directory: string | undefined;
    let child: RemoteManagedProcess | undefined;
    const results: Array<{
      name: string;
      frames: number;
      maximumInteriorError: number;
      maximumOutsideMeanError: number;
    }> = [];
    const historyResults: Array<{
      name: string;
      frames: number;
      maximumPatchError: number;
    }> = [];
    const activate = async (window: AppWindow): Promise<AppWindow> => {
      let observed = await window.refresh();
      if (!observed.active) {
        const cursor = await agent.cursor();
        const left = observed.frameBounds.x + 80,
          right = observed.frameBounds.x + observed.frameBounds.width - 180;
        await agent.mouse.click({
          x:
            Math.abs(cursor.point.x - left) > Math.abs(cursor.point.x - right)
              ? left
              : right,
          y: observed.frameBounds.y + 12,
        });
      }
      const deadline = Date.now() + 15000;
      while (!observed.active) {
        assert(Date.now() < deadline, 'activation timeout');
        observed = await observed.refresh();
      }
      return observed;
    };
    try {
      directory = await agent.files.mkdtemp('boxrig-selection-video-');
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
          BOXRIG_TEST_SAMPLE_SELECTION_STYLE_VIDEO: '1',
          BOXRIG_SELECTION_STYLE_DEMO: '1',
          BOXRIG_COLOR_FILTER_DEMO: '1',
          BOXRIG_TRANSPARENCY_MODE: 'opaque',
        },
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      let window = await child!.waitForWindow(
        {
          className: 'boxrig_win32_sample',
          titleRegex: /^boxrig-selection-ready-/,
          visible: true,
        },
        { timeoutMs: 30000 }
      );
      const match = /boxrig-selection-ready-(\d+)-(\d+)/.exec(window.title);
      assert(match, window.title);
      const ch = Number(match[1]),
        cw = Number(match[2]);
      window = await window.setBounds({ ...window.bounds, x: 40, y: 40 });
      window = await window.waitForPlacement(
        { bounds: window.bounds },
        { timeoutMs: 15000 }
      );
      window = await activate(window);
      if (!window.active) {
        await agent.mouse.click({
          x: window.frameBounds.x + 80,
          y: window.frameBounds.y + 12,
        });
        window = await window.refresh();
      }
      assert(window.active, 'sample active');
      await agent.keyboard.press('F12', { modifiers: ['Control', 'Shift'] });
      await agent.mouse.move({
        x: window.frameBounds.x + window.frameBounds.width - 80,
        y: window.frameBounds.y + 12,
      });
      const rect = {
        x: window.clientBounds.x,
        y: window.clientBounds.y,
        width: 320,
        height: 160,
      };
      await agent.mouse.drag(
        { x: rect.x + cw + 1, y: rect.y + ch + Math.floor(ch / 2) },
        { x: rect.x + 12 * cw - 1, y: rect.y + ch + Math.floor(ch / 2) },
        { button: 'left' }
      );
      await agent.mouse.move({
        x: window.frameBounds.x + 80,
        y: window.frameBounds.y + 12,
      });
      const blend = (
        base: number[],
        color: number[],
        alpha: number
      ): number[] =>
        base.map((value, index) =>
          Math.round((value * (255 - alpha) + color[index] * alpha) / 255)
        );
      const background = [24, 42, 63];
      let reference: Buffer | undefined;
      const record = async (name: string, mode: number): Promise<void> => {
        const path = resolve(output, name + '.mp4');
        const metadata = await agent.recordVideo(2000, path, { fps: 30, rect });
        assert(!metadata.clipped);
        const raw = execFileSync(
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
            '-fps_mode',
            'passthrough',
            'pipe:1',
          ],
          { maxBuffer: rect.width * rect.height * 3 * 100 }
        );
        const stride = rect.width * rect.height * 3;
        assert.equal(raw.length % stride, 0);
        const frames = raw.length / stride;
        assert.equal(frames, metadata.frameCount);
        assert(frames >= 45);
        let maximumInteriorError = 0,
          maximumOutsideMeanError = 0;
        const expected =
          mode === 0
            ? background
            : blend(
                background,
                mode === 1 ? [60, 150, 240] : [255, 190, 40],
                96
              );
        for (let frame = 15; frame < frames; frame++) {
          const pixels = raw.subarray(frame * stride, (frame + 1) * stride);
          if (!reference) reference = Buffer.from(pixels);
          // The first selected cell is blank. Keep away from the one/two-pixel
          // outline because H.264 chroma shares samples across thin edges.
          const sum = [0, 0, 0];
          let count = 0;
          for (let y = ch + 3; y < 2 * ch - 3; y++)
            for (let x = cw + 3; x < 2 * cw - 2; x++) {
              const at = (y * rect.width + x) * 3;
              for (let c = 0; c < 3; c++) sum[c] += pixels[at + c];
              count++;
            }
          assert(count > 0, 'interior color patch exists');
          for (let c = 0; c < 3; c++)
            maximumInteriorError = Math.max(
              maximumInteriorError,
              Math.abs(sum[c] / count - expected[c])
            );
          let outsideError = 0,
            outsideCount = 0;
          for (let y = 0; y < Math.min(rect.height, 6 * ch); y++)
            for (let x = 0; x < Math.min(rect.width, 30 * cw); x++) {
              if (
                y >= ch - 2 &&
                y < 2 * ch + 2 &&
                x >= cw - 2 &&
                x < 12 * cw + 2
              )
                continue;
              const at = (y * rect.width + x) * 3;
              for (let c = 0; c < 3; c++) {
                outsideError += Math.abs(pixels[at + c] - reference[at + c]);
                outsideCount++;
              }
            }
          maximumOutsideMeanError = Math.max(
            maximumOutsideMeanError,
            outsideError / outsideCount
          );
          // An outline is present only in modes zero/two; its hue differs from
          // both the underlying terminal and the blue/gold interior fill.
          if (mode !== 1) {
            let edge = 0;
            for (let y = ch; y < ch + 2; y++)
              for (let x = cw + 1; x < 2 * cw - 1; x++) {
                const at = (y * rect.width + x) * 3;
                if (
                  mode === 0
                    ? pixels[at + 2] > pixels[at] + 40
                    : pixels[at] > pixels[at + 2] + 60
                )
                  edge++;
              }
            assert(edge > 0, name + ': outline hue missing');
          }
        }
        assert(
          maximumInteriorError <= 25,
          name + ': incorrect selection fill ' + maximumInteriorError
        );
        assert(
          maximumOutsideMeanError <= 6,
          name + ': unselected content changed'
        );
        results.push({
          name,
          frames,
          maximumInteriorError,
          maximumOutsideMeanError,
        });
        console.log(JSON.stringify(results.at(-1)));
      };
      await record('outline', 0);
      await agent.keyboard.press('F10', { modifiers: ['Control', 'Shift'] });
      await record('fill', 1);
      await agent.keyboard.press('F10', { modifiers: ['Control', 'Shift'] });
      await record('fill-outline', 2);
      await agent.keyboard.press('F10', { modifiers: ['Control', 'Shift'] });
      await record('outline-restored', 0);
      await agent.keyboard.press('h');
      await child.waitForWindow(
        {
          className: 'boxrig_win32_sample',
          title: 'boxrig-selection-history-ready',
          visible: true,
        },
        { timeoutMs: 15000 }
      );
      await agent.keyboard.press('F10', { modifiers: ['Control', 'Shift'] });
      const recordHistory = async (
        name: string,
        firstRow: number,
        lastRow: number
      ) => {
        const path = resolve(output, name + '.mp4');
        const metadata = await agent.recordVideo(2000, path, { fps: 30, rect });
        assert(!metadata.clipped);
        const raw = execFileSync(
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
            '-fps_mode',
            'passthrough',
            'pipe:1',
          ],
          { maxBuffer: rect.width * rect.height * 3 * 100 }
        );
        const stride = rect.width * rect.height * 3;
        const frames = raw.length / stride;
        assert.equal(frames, metadata.frameCount);
        assert(frames >= 45);
        let maximumPatchError = 0;
        const fill = blend(background, [60, 150, 240], 96);
        for (let f = 15; f < frames; f++) {
          const pixels = raw.subarray(f * stride, (f + 1) * stride);
          for (
            let row = 1;
            row < Math.min(8, Math.floor(rect.height / ch));
            row++
          ) {
            for (let column = 1; column < 6; column++) {
              const expected =
                row >= firstRow && row < lastRow && column >= 2 && column < 5
                  ? fill
                  : background;
              const sum = [0, 0, 0];
              let samples = 0;
              for (let y = row * ch + 3; y < (row + 1) * ch - 3; y++) {
                for (let x = column * cw + 3; x < (column + 1) * cw - 2; x++) {
                  const at = (y * rect.width + x) * 3;
                  for (let c = 0; c < 3; c++) sum[c] += pixels[at + c];
                  samples++;
                }
              }
              assert(samples > 0);
              for (let c = 0; c < 3; c++)
                maximumPatchError = Math.max(
                  maximumPatchError,
                  Math.abs(sum[c] / samples - expected[c])
                );
            }
          }
        }
        assert(
          maximumPatchError <= 25,
          name + ': incorrect history selection ' + maximumPatchError
        );
        historyResults.push({ name, frames, maximumPatchError });
        console.log(JSON.stringify(historyResults.at(-1)));
      };
      await agent.keyboard.press('F9', { modifiers: ['Control', 'Shift'] });
      await recordHistory('history-anchor', 5, 7);
      await agent.keyboard.press('F9', { modifiers: ['Control', 'Shift'] });
      await recordHistory('history-expanded', 0, 8);
      await agent.keyboard.press('F9', { modifiers: ['Control', 'Shift'] });
      await recordHistory('history-cleared', 0, 0);
      await agent.keyboard.press('q');
      const exit = await child.waitForExit({ timeoutMs: 30000 });
      assert.equal(exit.root.exitCode, 0);
    } finally {
      try {
        if (child) {
          await writeFile(
            resolve(output, 'sample.log'),
            (await child.stdoutText()) + (await child.stderrText())
          );
          await child.releaseAsync();
        }
        if (directory) await agent.files.remove(directory, { recursive: true });
      } finally {
        agent.release();
      }
      await writeFile(
        resolve(output, 'results.json'),
        JSON.stringify({ arch, renderer, results, historyResults }, null, 2) +
          '\n'
      );
    }
  }
);
