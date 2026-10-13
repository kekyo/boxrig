import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { type RemoteManagedProcess } from 'agent-rover';
import { connectRemoteAgent } from './agent-connection.js';
import { it } from 'vitest';

it.each(['win32', 'opengl', 'd3d11'])(
  'blurs the moving backdrop and preserves terminal content (%s)',
  async (renderer) => {
    const source = process.env.BOXRIG_P2_WIN32_SOURCE_DIR;
    assert(source, 'BOXRIG_P2_WIN32_SOURCE_DIR is required.');
    using agent = await connectRemoteAgent({
      timeoutMs: 30000,
    });
    // This opt-in exercise restores the user's exact setting in finally.
    // Microsoft.Windows.Setting.Accessibility uses this same registry value.
    const systemEffectsDisabled =
      process.env.BOXRIG_TEST_DISABLE_SYSTEM_EFFECTS === '1';
    let originalTransparency: number | undefined;
    const transparencySetting = async (value: number | undefined) => {
      const command =
        "$key = 'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize'; " +
        (value === undefined
          ? ''
          : `Set-ItemProperty -Path $key -Name EnableTransparency -Value ${value}; `) +
        'Get-ItemPropertyValue -Path $key -Name EnableTransparency';
      const process = await agent.processes.launchManaged({
        path: 'powershell.exe',
        arguments: ['-NoProfile', '-NonInteractive', '-Command', command],
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      try {
        const exited = await process.waitForExit({ timeoutMs: 30000 });
        assert.equal(exited.root.exitCode, 0, await process.stderrText());
        const result = Number((await process.stdoutText()).trim());
        assert(result === 0 || result === 1);
        return result;
      } finally {
        await process.releaseAsync();
      }
    };
    const output = resolve(
      source,
      systemEffectsDisabled
        ? 'build/test-results/step27-background-system-disabled'
        : 'build/test-results/step27-background',
      renderer,
      String(Date.now())
    );
    await mkdir(output, { recursive: true });
    let directory: string | undefined;
    let backdrop: RemoteManagedProcess | undefined;
    let child: RemoteManagedProcess | undefined;
    try {
      if (systemEffectsDisabled) {
        originalTransparency = await transparencySetting(undefined);
        assert.equal(await transparencySetting(0), 0);
      }
      directory = await agent.files.mkdtemp('boxrig-background-video-');
      for (const [file, local] of [
        ['scene.exe', 'build/win32/amd64/tests/background_effect_scene.exe'],
        [
          'boxrig_term_launcher.exe',
          'build/boxrig_term/win32/amd64/boxrig_term_launcher.exe',
        ],
        [
          'terminal.exe',
          'build/boxrig_term/win32/amd64/boxrig_term_background_probe.exe',
        ],
      ])
        await agent.files.writeFile(
          `${directory}/${file}`,
          await readFile(resolve(source, local))
        );
      backdrop = await agent.processes.launchManaged({
        path: `${directory}/scene.exe`,
        createNoWindow: true,
      });
      const backdropWindow = await backdrop.waitForWindow({
        className: 'boxrig_background_effect_scene',
        visible: true,
      });
      child = await agent.processes.launchManaged({
        path: `${directory}/terminal.exe`,
        arguments: [
          `--renderer=${renderer}`,
          '--',
          '.\\scene.exe',
          '--terminal',
        ],
        workingDirectory: directory,
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
        environment: {
          BOXRIG_BACKGROUND_OPACITY: '0',
          BOXRIG_BACKGROUND_EFFECT: 'require',
          BOXRIG_TRANSPARENCY_MODE: 'background',
        },
      });
      let window = await child.waitForWindow({
        className: 'boxrig_win32_sample',
        title: 'plan27-background-ready',
        visible: true,
      });
      window = await window.setBounds({
        x: 80,
        y: 80,
        width: 760,
        height: 540,
      });
      const monitors = await agent.monitors();
      const scaledMonitor = monitors.find(
        (monitor) => monitor.dpi !== null && monitor.dpi !== window.dpi
      );
      assert(scaledMonitor, 'a second monitor with different DPI is required');
      await agent.mouse.click({
        x: window.frameBounds.x + 80,
        y: window.frameBounds.y + 12,
      });
      const width = 320,
        height = 240,
        frameSize = width * height * 3;
      let textMask: number[] = [],
        imageMask: number[] = [];
      let cellWidth = 0,
        cellHeight = 0;
      let reference = Buffer.alloc(0);
      const results: Array<Record<string, string | number>> = [];
      for (const stage of [
        'off',
        'on',
        'resized-on',
        'dpi-off',
        'dpi-on',
        'off-after',
      ]) {
        const enabled =
          stage === 'on' || stage === 'resized-on' || stage === 'dpi-on';
        if (stage === 'resized-on')
          window = await window.setBounds({
            x: 140,
            y: 120,
            width: 860,
            height: 620,
          });
        if (stage === 'dpi-off') {
          await backdropWindow.setBounds({
            x: scaledMonitor.bounds.x + 20,
            y: scaledMonitor.bounds.y + 20,
            width: 1100,
            height: 900,
          });
          window = await window.setBounds({
            x: scaledMonitor.bounds.x + 80,
            y: scaledMonitor.bounds.y + 80,
            width: 900,
            height: 700,
          });
          window = await window.waitForPlacement({
            monitorId: scaledMonitor.id,
          });
          window = await window.waitForStableBounds({ timeoutMs: 15000 });
          // Establish the same scene after the terminal's grid has reflowed.
          await agent.keyboard.press('F7');
        }
        await agent.keyboard.press(enabled ? 'F9' : 'F8');
        window = await child.waitForWindow({
          className: 'boxrig_win32_sample',
          title: enabled ? 'plan27-effect-on' : 'plan27-effect-off',
          visible: true,
        });
        const deadline = Date.now() + 15000;
        for (;;) {
          const shot = await window.screenshot();
          const rgba = execFileSync(
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
              'rgba',
              'pipe:1',
            ],
            { input: shot.image, maxBuffer: 32 * 1024 * 1024 }
          );
          let green = 0;
          for (let i = 0; i < rgba.length; i += 4)
            if (rgba[i] < 8 && rgba[i + 1] > 248 && rgba[i + 2] < 8) green++;
          if (green > 600) {
            await writeFile(resolve(output, `${stage}.png`), shot.image);
            break;
          }
          assert(
            Date.now() < deadline,
            'Kitty graphics must reach the presented frame'
          );
        }
        window = await window.waitForStableBounds({ timeoutMs: 15000 });
        const movie = resolve(output, `${stage}.mp4`);
        const recorded = await agent.recordVideo(3000, movie, {
          fps: 30,
          quality: 95,
          rect: {
            x: window.clientBounds.x,
            y: window.clientBounds.y,
            width,
            height,
          },
        });
        assert(
          !recorded.clipped && recorded.frameCount >= 75,
          'complete video capture'
        );
        const pixels = execFileSync(
          'ffmpeg',
          [
            '-v',
            'error',
            '-i',
            movie,
            '-f',
            'rawvideo',
            '-pix_fmt',
            'rgb24',
            'pipe:1',
          ],
          { maxBuffer: 64 * 1024 * 1024 }
        );
        const frames = pixels.length / frameSize;
        assert(Number.isInteger(frames) && frames >= 75);
        if (stage === 'off' || stage === 'dpi-off') {
          textMask = [];
          imageMask = [];
          reference = pixels.subarray(0, frameSize);
          let left = width,
            right = 0,
            top = height,
            bottom = 0;
          for (let y = 0; y < height; y++)
            for (let x = 0; x < width; x++) {
              const i = (y * width + x) * 3;
              if (
                y > 2 &&
                y < 28 &&
                pixels[i] > 180 &&
                pixels[i + 1] > 180 &&
                pixels[i + 2] > 180
              ) {
                // The fixture puts text on an opaque application background,
                // isolating glyph antialiasing from the moving desktop.
                textMask.push(i);
              }
              if (pixels[i] < 8 && pixels[i + 1] > 248 && pixels[i + 2] < 8) {
                imageMask.push(i);
                left = Math.min(left, x);
                right = Math.max(right, x);
                top = Math.min(top, y);
                bottom = Math.max(bottom, y);
              }
            }
          assert(
            textMask.length > 25 && imageMask.length > 600,
            'foreground text and Kitty pixels'
          );
          cellWidth = Math.round((right - left + 1) / 12);
          cellHeight = Math.round((bottom - top + 1) / 4);
        }
        let gradient = 0,
          backgroundMin = 255,
          backgroundMax = 0,
          cursorMin = Infinity,
          cursorMax = 0;
        let minimumText = 255,
          minimumGreen = 255,
          maximumImageLeak = 0,
          maximumTextDifference = 0;
        for (let frame = 0; frame < frames; frame++) {
          const offset = frame * frameSize;
          let background = 0,
            count = 0,
            edges = 0,
            edgeCount = 0;
          for (let y = 130; y < 210; y += 4)
            for (let x = 140; x < 310; x++) {
              const i = offset + (y * width + x) * 3;
              background += pixels[i + 1];
              count++;
              edges += Math.abs(pixels[i] - pixels[i - 3]);
              edgeCount++;
            }
          gradient += edges / edgeCount;
          backgroundMin = Math.min(backgroundMin, background / count);
          backgroundMax = Math.max(backgroundMax, background / count);
          let text = 0,
            green = 0,
            leak = 0,
            cursor = 0;
          let difference = 0;
          for (const index of textMask) {
            text +=
              (pixels[offset + index] +
                pixels[offset + index + 1] +
                pixels[offset + index + 2]) /
              3;
            for (let channel = 0; channel < 3; channel++)
              difference += Math.abs(
                pixels[offset + index + channel] - reference[index + channel]
              );
          }
          maximumTextDifference = Math.max(
            maximumTextDifference,
            difference / (textMask.length * 3)
          );
          for (const index of imageMask) {
            green += pixels[offset + index + 1];
            leak += (pixels[offset + index] + pixels[offset + index + 2]) / 2;
          }
          minimumText = Math.min(minimumText, text / textMask.length);
          minimumGreen = Math.min(minimumGreen, green / imageMask.length);
          maximumImageLeak = Math.max(
            maximumImageLeak,
            leak / imageMask.length
          );
          for (let y = cellHeight * 7 + 2; y < cellHeight * 8 - 2; y++)
            for (let x = 1; x < cellWidth - 1; x++) {
              const i = offset + (y * width + x) * 3;
              if (pixels[i] > 160 && pixels[i + 1] > 160 && pixels[i + 2] > 160)
                cursor++;
            }
          cursorMin = Math.min(cursorMin, cursor);
          cursorMax = Math.max(cursorMax, cursor);
        }
        gradient /= frames;
        results.push({
          stage,
          frames,
          gradient,
          backgroundRange: backgroundMax - backgroundMin,
          minimumText,
          minimumGreen,
          maximumImageLeak,
          maximumTextDifference,
          cursorMin,
          cursorMax,
          dpi: window.dpi ?? 0,
          monitorDpi:
            monitors.find((monitor) => monitor.id === window.monitorId)?.dpi ??
            0,
          dpiAwareness: window.dpiAwareness ?? 'unknown',
          systemTransparency: systemEffectsDisabled ? 0 : 1,
        });
        await writeFile(
          resolve(output, 'validation.json'),
          JSON.stringify(results, null, 2)
        );
        assert(
          enabled ? gradient < 3 : gradient > 10,
          `${stage}: background blur/clear boundary ${gradient}`
        );
        assert(
          systemEffectsDisabled && enabled
            ? backgroundMax - backgroundMin < 4
            : backgroundMax - backgroundMin > 4,
          `${stage}: OS effect policy determines the visible backdrop`
        );
        assert(
          maximumTextDifference < 12 &&
            minimumGreen > 240 &&
            maximumImageLeak < 12,
          `${stage}: foreground stays opaque and sharp`
        );
        assert(
          cursorMin === 0 && cursorMax > 5,
          `${stage}: cursor keeps blinking`
        );
      }
      const logs = (await child.stdoutText()) + (await child.stderrText());
      await writeFile(resolve(output, 'sample.log'), logs);
      assert(
        logs.includes('background-effect=acrylic supported=1 accepted=1'),
        'native effect diagnostics reach redirected stderr'
      );
    } finally {
      try {
        try {
          if (child !== undefined) await child.releaseAsync();
        } finally {
          try {
            if (backdrop !== undefined) await backdrop.releaseAsync();
          } finally {
            if (directory !== undefined)
              await agent.files.remove(directory, { recursive: true });
          }
        }
      } finally {
        if (originalTransparency !== undefined)
          assert.equal(
            await transparencySetting(originalTransparency),
            originalTransparency
          );
      }
    }
  }
);
