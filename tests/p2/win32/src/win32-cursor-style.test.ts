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
  'records cursor style, steady/blink policy and reset (%s %s)',
  async (arch, renderer) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    assert(source, 'repository root required');
    const output = resolve(
      source,
      'build/test-results/step23-cursor-style',
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
      transitions: number;
      minimumPixels: number;
      maximumPixels: number;
      maximumOutsideError: number;
      maximumStaticCellMeanError: number;
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
      directory = await agent.files.mkdtemp('boxrig-blink-video-');
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
          BOXRIG_TEST_SAMPLE_TEXT_BLINK_VIDEO: '1',
          BOXRIG_CURSOR_STYLE_DEMO: '1',
          BOXRIG_TRANSPARENCY_MODE: 'opaque',
        },
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      let window = await child!.waitForWindow(
        {
          className: 'boxrig_win32_sample',
          titleRegex: /^boxrig-blink-ready-/,
          visible: true,
        },
        { timeoutMs: 30000 }
      );
      const match = /boxrig-blink-ready-(\d+)-(\d+)/.exec(window.title);
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
      const cycle = async (): Promise<void> => {
        await agent.keyboard.press('F11', { modifiers: ['Control', 'Shift'] });
      };
      const record = async (
        name: string,
        blinking: boolean,
        kind: string,
        shape: string
      ) => {
        const path = resolve(output, name + '.mp4');
        const metadata = await agent.recordVideo(blinking ? 4000 : 2000, path, {
          fps: 30,
          rect,
        });
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
          { maxBuffer: rect.width * rect.height * 3 * 300 }
        );
        const stride = rect.width * rect.height * 3;
        assert.equal(raw.length % stride, 0);
        const frames = raw.length / stride;
        assert.equal(frames, metadata.frameCount);
        assert(frames >= 45);
        const reference = raw.subarray(15 * stride, 16 * stride);
        const phases: boolean[] = [];
        const pixelCounts: number[] = [];
        let maximumOutsideError = 0,
          maximumStaticCellMeanError = 0;
        const cellsWide = Math.ceil(rect.width / cw);
        const cellErrors = new Float64Array(
          cellsWide * Math.ceil(rect.height / ch)
        );
        const cellSamples = new Uint32Array(cellErrors.length);
        const stroke = kind === 'gold' ? 2 : kind === 'terminal' ? 1 : 3;
        const expectedPixels =
          shape === 'bar'
            ? stroke * ch
            : shape === 'underline'
              ? stroke * cw
              : shape === 'fill'
                ? cw * ch
                : cw * ch -
                  Math.max(0, cw - 2 * stroke) * Math.max(0, ch - 2 * stroke);
        // H.264 4:2:0 chroma mixes a thin colored edge with adjacent dark cells.
        // Relative channel order identifies the hue without requiring source saturation.
        const matches = (pixels: Buffer, at: number): boolean => {
          const red = pixels[at],
            green = pixels[at + 1],
            blue = pixels[at + 2];
          if (Math.max(red, green, blue) < 96) return false;
          if (kind === 'gold') return red > blue + 30 && green > blue + 25;
          if (kind === 'cyan') return green > red + 30 && blue > red + 30;
          if (kind === 'pink') return red > green + 30 && blue > green + 10;
          return Math.max(red, green, blue) - Math.min(red, green, blue) < 40;
        };
        for (let f = 15; f < frames; f++) {
          const pixels = raw.subarray(f * stride, (f + 1) * stride);
          let colored = 0,
            left = cw,
            top = ch,
            right = -1,
            bottom = -1;
          for (let y = 0; y < ch; y++)
            for (let x = 0; x < cw; x++) {
              const at = ((ch + y) * rect.width + 7 * cw + x) * 3;
              if (matches(pixels, at)) {
                colored++;
                left = Math.min(left, x);
                right = Math.max(right, x);
                top = Math.min(top, y);
                bottom = Math.max(bottom, y);
              } else
                assert(
                  Math.max(pixels[at], pixels[at + 1], pixels[at + 2]) <= 110,
                  name + ': wrong cursor color or stale decoration'
                );
            }
          const visible = colored > expectedPixels * 0.45;
          if (visible) {
            if (shape === 'fill')
              assert(
                colored >= cw * ch * 0.9,
                name + ': cursor interior must be filled'
              );
            assert(
              Math.abs(right - left + 1 - (shape === 'bar' ? stroke : cw)) <= 1,
              name + ': cursor width'
            );
            assert(
              Math.abs(
                bottom - top + 1 - (shape === 'underline' ? stroke : ch)
              ) <= 1,
              name + ': cursor height'
            );
          } else assert(colored <= 2, name + ': partial hidden cursor');
          phases.push(visible);
          pixelCounts.push(colored);
          cellErrors.fill(0);
          cellSamples.fill(0);
          for (let y = 0; y < rect.height; y++)
            for (let x = 0; x < rect.width; x++) {
              if (
                y >= ch - 2 &&
                y < 2 * ch + 2 &&
                [1, 4, 7].some(
                  (column) => x >= column * cw - 2 && x < (column + 1) * cw + 2
                )
              )
                continue;
              const at = (y * rect.width + x) * 3;
              const error = Math.abs(
                0.299 * (pixels[at] - reference[at]) +
                  0.587 * (pixels[at + 1] - reference[at + 1]) +
                  0.114 * (pixels[at + 2] - reference[at + 2])
              );
              maximumOutsideError = Math.max(maximumOutsideError, error);
              const cell = Math.floor(y / ch) * cellsWide + Math.floor(x / cw);
              cellErrors[cell] += error;
              cellSamples[cell]++;
            }
          for (let cell = 0; cell < cellErrors.length; cell++)
            if (cellSamples[cell])
              maximumStaticCellMeanError = Math.max(
                maximumStaticCellMeanError,
                cellErrors[cell] / cellSamples[cell]
              );
        }
        let transitions = 0;
        for (let f = 1; f < phases.length; f++)
          if (phases[f] !== phases[f - 1]) transitions++;
        if (blinking) {
          assert(
            phases.includes(true) && phases.includes(false),
            name + ': missing blink phase'
          );
          assert(transitions >= 4, name + ': blink policy was not connected');
        } else
          assert(
            phases.every(Boolean) && transitions === 0,
            name + ': steady policy was not connected'
          );
        assert(
          maximumOutsideError <= 96 && maximumStaticCellMeanError <= 6,
          name + ': static content changed'
        );
        const result = {
          name,
          frames,
          transitions,
          minimumPixels: Math.min(...pixelCounts),
          maximumPixels: Math.max(...pixelCounts),
          maximumOutsideError,
          maximumStaticCellMeanError,
        };
        results.push(result);
        console.log(JSON.stringify(result));
      };
      await record('outline-steady', false, 'gold', 'outline');
      await cycle();
      await record('underline-blink', true, 'cyan', 'underline');
      await cycle();
      await record('bar-steady', false, 'pink', 'bar');
      await cycle();
      await record('terminal-following', true, 'terminal', 'fill');
      const terminalShape = async (
        key: string,
        name: string,
        blinking: boolean,
        shape: string
      ) => {
        await agent.keyboard.press(key);
        const deadline = Date.now() + 15000;
        while ((await window.refresh()).title !== 'boxrig-blink-' + key) {
          assert(
            Date.now() < deadline,
            'DECSCUSR child acknowledgement timeout'
          );
        }
        await record(name, blinking, 'terminal', shape);
      };
      await terminalShape('3', 'decscusr-underline', true, 'underline');
      await terminalShape('6', 'decscusr-bar', false, 'bar');
      await terminalShape('2', 'decscusr-block', false, 'fill');
      await cycle();
      await record('outline-restored', false, 'gold', 'outline');
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
        JSON.stringify({ arch, renderer, results }, null, 2) + '\n'
      );
    }
  }
);
