import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { type AppWindow, type RemoteManagedProcess } from 'agent-rover';
import { connectRemoteAgent } from './agent-connection.js';
import { it } from 'vitest';
import { PNG } from 'pngjs';
import { cursorVideoStaticError } from './cursor-video-error.js';
it.each([
  ['amd64', 'win32'],
  ['amd64', 'opengl'],
  ['amd64', 'd3d11'],
  ['i686', 'win32'],
  ['i686', 'opengl'],
  ['i686', 'd3d11'],
])(
  'records native cursor phases and stable neighbors (%s %s)',
  async (arch, renderer) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    assert(source, 'repository root required');
    const output = resolve(
      source,
      'build/test-results/step23-blink',
      arch,
      renderer,
      String(Date.now())
    );
    await mkdir(output, { recursive: true });

    const agent = await connectRemoteAgent({
      timeoutMs: 30000,
    });
    let directory: string | undefined;
    let child: RemoteManagedProcess | undefined,
      peer: RemoteManagedProcess | undefined;
    const results: Array<{
      name: string;
      frames: number;
      transitions: number;
      maximumOutsideError: number;
      column: number;
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
          BOXRIG_TEST_SAMPLE_BLINK_VIDEO: '1',
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
      const capture = async () => {
        const shot = await agent.screenshot({ rect });
        assert(!shot.clipped);
        return { png: shot.image, image: PNG.sync.read(shot.image) };
      };
      const ink = (pixels: Uint8Array, channels: number, column: number) => {
        let sum = 0;
        for (let y = ch; y < ch * 2; y++)
          for (let x = column * cw; x < (column + 1) * cw; x++) {
            const at = (y * rect.width + x) * channels;
            sum += pixels[at] + pixels[at + 1] + pixels[at + 2];
          }
        return sum;
      };
      const blankCorners = (image: PNG) =>
        [
          0,
          rect.width - 1,
          (rect.height - 1) * rect.width,
          rect.width * rect.height - 1,
        ].every(
          (pixel) =>
            image.data[pixel * 4] === 0 &&
            image.data[pixel * 4 + 1] === 0 &&
            image.data[pixel * 4 + 2] === 0
        );
      await stage('s');
      let on;
      const onDeadline = Date.now() + 15000;
      do {
        on = await capture();
        assert(Date.now() < onDeadline, 'steady cursor did not appear');
      } while (!blankCorners(on.image) || ink(on.image.data, 4, 1) < 1000);
      await writeFile(resolve(output, 'steady.png'), on.png);
      await stage('h');
      let hidden;
      const offDeadline = Date.now() + 15000;
      do {
        hidden = await capture();
        assert(Date.now() < offDeadline, 'hidden cursor did not disappear');
      } while (
        !blankCorners(hidden.image) ||
        ink(hidden.image.data, 4, 1) > 200
      );
      await writeFile(resolve(output, 'hidden.png'), hidden.png);
      const onInk = ink(on.image.data, 4, 1);
      let changed = 0;
      for (let y = 0; y < rect.height; y++)
        for (let x = 0; x < rect.width; x++) {
          const at = (y * rect.width + x) * 4;
          if (
            !on.image.data
              .subarray(at, at + 3)
              .equals(hidden.image.data.subarray(at, at + 3))
          ) {
            assert(
              y >= ch && y < ch * 2 && x >= cw && x < cw * 2,
              'noncursor screenshot changed'
            );
            changed++;
          }
        }
      assert.equal(
        changed,
        cw * ch,
        'the default cursor fills one whole blank cell'
      );
      const record = async (
        name: string,
        blinking: boolean,
        column: number,
        visible: boolean
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
          { maxBuffer: 320 * 160 * 3 * 300 }
        );
        const stride = 320 * 160 * 3;
        assert.equal(raw.length % stride, 0);
        const count = raw.length / stride;
        assert.equal(count, metadata.frameCount);
        const phases = [];
        let maximumOutsideError = 0;
        for (let f = 15; f < count; f++) {
          const bytes = raw.subarray(f * stride, (f + 1) * stride);
          const phase = ink(bytes, 3, column) > onInk / 2;
          phases.push(phase);
          maximumOutsideError = Math.max(
            maximumOutsideError,
            cursorVideoStaticError(
              bytes,
              hidden.image.data,
              rect.width,
              rect.height,
              {
                x: column * cw - 2,
                y: ch - 2,
                width: cw + 4,
                height: ch + 4,
              }
            )
          );
        }
        // Compare luminance over 2x2 blocks to allow H.264 color-edge artifacts.
        // Each static glyph's disappearance and a stale cursor must still fail.
        assert(
          maximumOutsideError <= 48,
          `${name}: static content error ${maximumOutsideError}`
        );
        let transitions = 0;
        for (let i = 1; i < phases.length; i++)
          if (phases[i] !== phases[i - 1]) transitions++;
        if (blinking) {
          assert(phases.includes(true) && phases.includes(false));
          assert(transitions >= 4, `${name}: ${transitions} transitions`);
        } else
          assert(
            phases.every((p) => p === visible),
            `${name}: unexpected cursor phase`
          );
        const result = {
          name,
          frames: count,
          transitions,
          maximumOutsideError,
          column,
        };
        results.push(result);
        console.log(JSON.stringify(result));
      };
      await stage('b');
      await record('blink', true, 1, true);
      await stage('s');
      await record('steady', false, 1, true);
      await stage('m');
      await record('moved', true, 4, true);
      await stage('h');
      await record('hidden-cursor', false, 4, false);
      await stage('v');
      await record('visible-cursor', true, 4, true);
      peer = await agent.processes.launchManaged({
        path: directory + '/focus-peer.exe',
        createNoWindow: true,
      });
      let peerWindow = await peer.waitForWindow(
        { className: 'boxrig_blink_focus_peer', visible: true },
        { timeoutMs: 30000 }
      );
      peerWindow = await peerWindow.setBounds({
        ...peerWindow.bounds,
        x: 700,
        y: 500,
        width: 400,
        height: 250,
      });
      peerWindow = await activate(peerWindow);
      if (!peerWindow.active)
        await agent.mouse.click({
          x: peerWindow.frameBounds.x + 80,
          y: peerWindow.frameBounds.y + 12,
        });
      await record('unfocused', false, 4, true);
      await peer.releaseAsync();
      peer = undefined;
      window = await activate(window);
      if (!window.active)
        await agent.mouse.click({
          x: window.frameBounds.x + 80,
          y: window.frameBounds.y + 12,
        });
      await record('refocused', true, 4, true);
      window = await window.minimize();
      window = await window.restore();
      window = await activate(window);
      if (!window.active)
        await agent.mouse.click({
          x: window.frameBounds.x + 80,
          y: window.frameBounds.y + 12,
        });
      await record('restored', true, 4, true);
      await agent.keyboard.press('q');
      const exit = await child.waitForExit({ timeoutMs: 30000 });
      assert.equal(exit.root.exitCode, 0);
    } finally {
      try {
        if (peer) await peer.releaseAsync();
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
