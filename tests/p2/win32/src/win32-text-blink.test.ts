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
  'records slow/rapid text, cursor and rendition reset (%s %s)',
  async (arch, renderer) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    assert(source, 'repository root required');
    const output = resolve(
      source,
      'build/test-results/step23-text-blink',
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
      maximumOutsideError: number;
      maximumStaticCellMeanError: number;
      regions: Array<{
        kind: string;
        transitions: number;
        minimum: number;
        maximum: number;
      }>;
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
        await resolver.releaseAsync({ timeoutMs: 30000 });
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
          BOXRIG_TEST_SAMPLE_TEXT_BLINK_VIDEO: '1',
          BOXRIG_TRANSPARENCY_MODE: 'opaque',
          // System ConPTY on build 22631 normalizes SGR 6 to SGR 5. This
          // protocol-specific test requires the package's VT passthrough.
          BOXRIG_CONPTY: 'packaged-required',
          BOXRIG_CONPTY_DIRECTORY: directory,
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
      const stage = async (key: string) => {
        await agent.keyboard.press(key);
        window = await child!.waitForWindow(
          {
            className: 'boxrig_win32_sample',
            title: 'boxrig-blink-' + key,
            visible: true,
          },
          { timeoutMs: 15000 }
        );
      };
      const record = async (name: string, blinking: boolean) => {
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
        const columns = [1, 4, 7],
          visibilitySamples: number[][] = columns.map(() => []);
        const reference = raw.subarray(15 * stride, 16 * stride);
        let maximumOutsideError = 0;
        let maximumStaticCellMeanError = 0;
        const cellsWide = Math.ceil(rect.width / cw);
        const cellErrors = new Float64Array(
          cellsWide * Math.ceil(rect.height / ch)
        );
        const cellSamples = new Uint32Array(cellErrors.length);
        for (let f = 15; f < frames; f++) {
          const pixels = raw.subarray(f * stride, (f + 1) * stride);
          cellErrors.fill(0);
          cellSamples.fill(0);
          for (let area = 0; area < columns.length; area++) {
            let minimum = 255,
              maximum = 0;
            for (let y = ch; y < ch * 2; y++)
              for (
                let x = columns[area] * cw;
                x < (columns[area] + 1) * cw;
                x++
              ) {
                const at = (y * rect.width + x) * 3;
                const luminance =
                  0.299 * pixels[at] +
                  0.587 * pixels[at + 1] +
                  0.114 * pixels[at + 2];
                minimum = Math.min(minimum, luminance);
                maximum = Math.max(maximum, luminance);
              }
            // Text uses glyph contrast; the blank filled cursor must cover
            // every pixel, so an outline cannot satisfy its visible phase.
            visibilitySamples[area].push(
              area === 2 ? minimum : maximum - minimum
            );
          }
          for (let y = 0; y < rect.height; y++)
            for (let x = 0; x < rect.width; x++) {
              if (
                y >= ch - 2 &&
                y < ch * 2 + 2 &&
                columns.some(
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
              if (y >= ch * 3 && y < ch * 4 && x < cw * 9)
                assert(
                  0.299 * pixels[at] +
                    0.587 * pixels[at + 1] +
                    0.114 * pixels[at + 2] <=
                    24,
                  'concealed text became visible'
                );
            }
          for (let cell = 0; cell < cellErrors.length; cell++) {
            if (cellSamples[cell] !== 0)
              maximumStaticCellMeanError = Math.max(
                maximumStaticCellMeanError,
                cellErrors[cell] / cellSamples[cell]
              );
          }
        }
        // H.264 perturbs isolated glyph edges even at its highest quality.
        // Bound both individual peaks and each cell's average error; missing
        // static glyphs exceed their full foreground contrast. Native pixel
        // fixtures separately require exact equality over sixteen cycles.
        assert(
          maximumOutsideError <= 96 && maximumStaticCellMeanError <= 6,
          name +
            ': static content changed ' +
            JSON.stringify({ maximumOutsideError, maximumStaticCellMeanError })
        );
        const regions = visibilitySamples.map((values, index) => {
          const minimum = Math.min(...values),
            maximum = Math.max(...values);
          const phases = values.map((value) => value > 64);
          let transitions = 0;
          for (let at = 1; at < phases.length; at++)
            if (phases[at] !== phases[at - 1]) transitions++;
          if (blinking) {
            assert(
              minimum <= 24 && maximum > 96,
              name + ': missing visible/hidden phase ' + index
            );
            assert(
              transitions >= (index === 1 ? 16 : 4),
              name + ': insufficient transitions ' + index
            );
          } else
            assert(
              phases.every(Boolean) && transitions === 0,
              name + ': reset failed ' + index
            );
          return {
            kind: ['slow', 'rapid', 'cursor'][index]!,
            transitions,
            minimum,
            maximum,
          };
        });
        if (blinking)
          assert(
            regions[1]!.transitions > regions[0]!.transitions * 1.8,
            'rapid and slow frequencies must differ'
          );
        const result = {
          name,
          frames,
          regions,
          maximumOutsideError,
          maximumStaticCellMeanError,
        };
        results.push(result);
        console.log(JSON.stringify(result));
      };
      await record('slow-rapid-cursor', true);
      await stage('c');
      await record('sgr-25', false);
      await stage('z');
      await record('sgr-0', false);
      await stage('b');
      await record('restored-blink', true);
      await agent.keyboard.press('q');
      const exit = await child.waitForExit({ timeoutMs: 30000 });
      assert.equal(exit.root.exitCode, 0);
      assert.match(await child.stdoutText(), /pty-backend=packaged fallback=0/);
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
