/* Lossless native recordings assert observed phases and unchanged neighbors. */
import assert from "node:assert/strict";
import { spawn, execFileSync } from "node:child_process";
import {
  mkdirSync,
  writeFileSync,
  mkdtempSync,
  existsSync,
  rmSync,
  readFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { setTimeout } from "node:timers/promises";
import { resolve } from "node:path";

const [sample, helper, output, renderer] = process.argv.slice(2);
assert(
  sample && helper && output,
  "sample, PTY helper and output directory required",
);
mkdirSync(output, { recursive: true });
const command = (name, args) =>
  execFileSync(name, args, { encoding: "utf8", timeout: 30000 }).trim();
const wayland = process.env.BOXRIG_TEST_BLINK_WAYLAND === "1";
const fontWidth = process.env.BOXRIG_TEST_FONT_CURSOR === "1";
const wide = fontWidth || process.env.BOXRIG_TEST_WIDE_CURSOR === "1";
const wideColumns = fontWidth ? 3 : 2;
const boxes = wide || process.env.BOXRIG_TEST_CURSOR_BOX === "1";
let weston, runtime, child;
let diagnostics = "";
const results = [];
try {
  if (fontWidth) {
    const xmlPath = resolve(output, "fonts.conf");
    const directory = resolve("tests/fixtures").replaceAll("&", "&amp;");
    writeFileSync(xmlPath, `<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig><include>/etc/fonts/fonts.conf</include><dir>${directory}</dir>
<match target="pattern"><test name="family"><string>monospace</string></test>
<edit name="family" mode="prepend" binding="strong"><string>Boxrig Width Fixture</string></edit>
</match></fontconfig>`);
    process.env.FONTCONFIG_FILE = xmlPath;
    process.env.BOXRIG_AMBIGUOUS_WIDTH = "font";
  }
  if (wayland) {
    runtime = mkdtempSync(resolve(tmpdir(), "boxrig-blink-wayland-"));
    process.env.XDG_RUNTIME_DIR = runtime;
    process.env.WAYLAND_DISPLAY = "boxrig-blink";
    weston = spawn(
      "weston",
      [
        "--debug",
        "--backend=x11",
        "--renderer=pixman",
        "--socket=boxrig-blink",
        "--width=1000",
        "--height=650",
        "--idle-time=0",
        "--no-config",
        "--log=" + runtime + "/weston.log",
      ],
      { env: process.env, stdio: "ignore" },
    );
    const deadline = Date.now() + 15000;
    while (!existsSync(runtime + "/boxrig-blink")) {
      assert(Date.now() < deadline, "Weston socket timeout");
      await setTimeout(20);
    }
  }
  const arguments_ = [
    ...(renderer ? ["--renderer=" + renderer] : []),
    resolve(helper),
    "--sample-scenario",
  ];
  child = spawn(resolve(sample), arguments_, {
    env: {
      ...process.env,
      BOXRIG_TEST_SAMPLE_TEXT_BLINK_VIDEO: "1",
      BOXRIG_CURSOR_STYLE_DEMO: boxes ? "2" : "1",
      NO_AT_BRIDGE: "1",
    },
    stdio: ["ignore", "ignore", "pipe"],
  });
  child.stderr.on("data", (data) => {
    diagnostics += data;
  });
  const scene = () => command("weston-debug", ["scene-graph"]);
  const readTitle = () =>
    wayland
      ? /top-level window '(boxrig-blink-[^']*)'/.exec(scene())?.[1]
      : command("xdotool", ["getwindowname", id]);
  let id, values, title;
  if (wayland) {
    const until = Date.now() + 30000;
    while (!(title = readTitle())?.startsWith("boxrig-blink-ready-")) {
      assert(Date.now() < until, "Wayland child ready timeout");
      await setTimeout(20);
    }
    id = /window id (\d+)/.exec(
      readFileSync(runtime + "/weston.log", "utf8"),
    )?.[1];
    assert(id, "Weston X11 window");
    const section = scene()
      .split(/(?=View \d+ \(role )/)
      .find(
        (part) =>
          part.includes("PID " + child.pid + ",") &&
          part.startsWith("View ") &&
          part.includes("role xdg_toplevel"),
      );
    const position = /position: \((\d+), (\d+)\) -> \((\d+), (\d+)\)/.exec(
      section ?? "",
    );
    assert(position, section);
    values = {
      X: position[1],
      Y: position[2],
      WIDTH: Number(position[3]) - Number(position[1]),
      HEIGHT: Number(position[4]) - Number(position[2]),
    };
    command("xdotool", ["windowfocus", "--sync", id]);
    command("xdotool", [
      "mousemove",
      "--window",
      id,
      String(Number(values.X) + 200),
      String(Number(values.Y) + 100),
      "click",
      "1",
    ]);
  } else {
    id = command("xdotool", [
      "search",
      "--sync",
      "--onlyvisible",
      "--name",
      "^boxrig-blink-ready-",
    ]).split("\n")[0];
    title = readTitle();
    values = Object.fromEntries(
      command("xdotool", ["getwindowgeometry", "--shell", id])
        .split("\n")
        .map((line) => line.split("=")),
    );
  }
  const match = /^boxrig-blink-ready-(\d+)-(\d+)$/.exec(title);
  assert(match, title);
  const cellHeight = Number(match[1]),
    cellWidth = Number(match[2]);
  command("xdotool", ["windowfocus", "--sync", id]);
  command("xdotool", ["mousemove", "1200", "1000"]);
  const width = Math.min(Number(values.WIDTH), 500),
    height = Math.min(Number(values.HEIGHT), 200);
  const frameBytes = width * height * 3;
  // A compositor can fade the newly mapped surface beyond a fixed frame
  // offset. Observe the complete, opaque gold outline before recording.
  const paintedDeadline = Date.now() + 15000;
  const expectedOutlinePixels = boxes
    ? cellWidth * cellHeight
    : cellWidth * cellHeight -
      Math.max(0, cellWidth - 4) * Math.max(0, cellHeight - 4);
  let origin;
  for (;;) {
    const pixels = execFileSync(
      "ffmpeg",
      [
        "-nostdin",
        "-loglevel",
        "error",
        "-f",
        "x11grab",
        "-draw_mouse",
        "0",
        "-video_size",
        width + "x" + height,
        "-i",
        process.env.DISPLAY + "+" + values.X + "," + values.Y,
        "-frames:v",
        "1",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb24",
        "pipe:1",
      ],
      { maxBuffer: frameBytes * 2, timeout: 30000 },
    );
    let gold = 0,
      left = width,
      top = height,
      right = -1,
      bottom = -1;
    for (let y = 0; y < height; y++)
      for (let x = 0; x < width; x++) {
        const at = (y * width + x) * 3;
        if (
          pixels[at] !== 255 ||
          pixels[at + 1] !== 215 ||
          pixels[at + 2] !== 0
        )
          continue;
        gold++;
        left = Math.min(left, x);
        top = Math.min(top, y);
        right = Math.max(right, x);
        bottom = Math.max(bottom, y);
      }
    if (
      gold === expectedOutlinePixels &&
      right - left + 1 === cellWidth &&
      bottom - top + 1 === cellHeight
    ) {
      if (boxes) origin = { x: left - 7 * cellWidth, y: top - cellHeight };
      break;
    }
    assert(Date.now() < paintedDeadline, "initial opaque cursor presentation");
    await setTimeout(20);
  }
  const recordPixels = (name, blinking) => {
    const file = resolve(output, name + ".mkv");
    execFileSync(
      "ffmpeg",
      [
        "-y",
        "-nostdin",
        "-loglevel",
        "error",
        "-f",
        "x11grab",
        "-framerate",
        "30",
        "-draw_mouse",
        "0",
        "-video_size",
        width + "x" + height,
        "-i",
        process.env.DISPLAY + "+" + values.X + "," + values.Y,
        "-t",
        blinking ? "4" : "2",
        "-c:v",
        "ffv1",
        file,
      ],
      { timeout: 30000 },
    );
    const raw = execFileSync(
      "ffmpeg",
      [
        "-nostdin",
        "-loglevel",
        "error",
        "-i",
        file,
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb24",
        "pipe:1",
      ],
      { maxBuffer: frameBytes * 300, timeout: 30000 },
    );
    assert.equal(raw.length % frameBytes, 0);
    const frames = raw.length / frameBytes;
    assert(frames >= 45);
    return { raw, frames };
  };
  const record = (name, blinking, kind, shape, hollow = false) => {
    const { raw, frames } = recordPixels(name, blinking);
    const reference = raw.subarray(15 * frameBytes, 16 * frameBytes);
    const matches = (pixels, at) => {
      const [red, green, blue] = pixels.subarray(at, at + 3);
      if (kind === "gold")
        return red >= 245 && green >= 205 && green <= 225 && blue <= 10;
      if (kind === "cyan")
        return red <= 10 && green >= 210 && green <= 230 && blue >= 245;
      if (kind === "pink")
        return (
          red >= 245 && green >= 70 && green <= 90 && blue >= 170 && blue <= 190
        );
      return (
        Math.min(red, green, blue) >= 96 &&
        Math.max(red, green, blue) - Math.min(red, green, blue) <= 3
      );
    };
    if (!origin) {
      let left = width,
        top = height,
        right = -1,
        bottom = -1;
      for (let y = 0; y < height; y++)
        for (let x = 0; x < width; x++) {
          if (!matches(reference, (y * width + x) * 3)) continue;
          left = Math.min(left, x);
          right = Math.max(right, x);
          top = Math.min(top, y);
          bottom = Math.max(bottom, y);
        }
      assert.equal(right - left + 1, cellWidth, "initial gold cursor width");
      assert.equal(bottom - top + 1, cellHeight, "initial gold cursor height");
      origin = { x: left - 7 * cellWidth, y: top - cellHeight };
    }
    const phases = [];
    let maximumOutsideError = 0;
    const stroke = boxes
      ? 4
      : kind === "gold"
        ? 2
        : kind === "terminal"
          ? 1
          : 3;
    const boxWidth = shape === "bar" ? stroke : cellWidth;
    const boxHeight = shape === "underline" ? stroke : cellHeight;
    const expected = boxes
      ? boxWidth * boxHeight -
        (hollow ? Math.max(0, boxWidth - 2) * Math.max(0, boxHeight - 2) : 0)
      : shape === "bar"
        ? stroke * cellHeight
        : shape === "underline"
          ? stroke * cellWidth
          : shape === "fill"
            ? cellWidth * cellHeight
            : cellWidth * cellHeight -
              Math.max(0, cellWidth - stroke * 2) *
                Math.max(0, cellHeight - stroke * 2);
    for (let f = 15; f < frames; f++) {
      const pixels = raw.subarray(f * frameBytes, (f + 1) * frameBytes);
      let colored = 0,
        left = cellWidth,
        top = cellHeight,
        right = -1,
        bottom = -1;
      for (let y = 0; y < cellHeight; y++)
        for (let x = 0; x < cellWidth; x++) {
          const at =
            ((origin.y + cellHeight + y) * width +
              origin.x +
              7 * cellWidth +
              x) *
            3;
          if (matches(pixels, at)) {
            colored++;
            left = Math.min(left, x);
            right = Math.max(right, x);
            top = Math.min(top, y);
            bottom = Math.max(bottom, y);
          } else
            assert(
              Math.max(...pixels.subarray(at, at + 3)) <= 20,
              name + ": previous cursor color survived",
            );
        }
      assert(
        colored === 0 || colored === expected,
        name + ": incomplete cursor " + colored + "/" + expected,
      );
      if (colored !== 0) {
        assert.equal(
          right - left + 1,
          shape === "bar" ? stroke : cellWidth,
          name + ": width",
        );
        assert.equal(
          bottom - top + 1,
          shape === "underline" ? stroke : cellHeight,
          name + ": height",
        );
      }
      phases.push(colored !== 0);
      for (let y = 0; y < height; y++)
        for (let x = 0; x < width; x++) {
          if (
            y >= origin.y + cellHeight &&
            y < origin.y + 2 * cellHeight &&
            [1, 4, 7].some(
              (column) =>
                x >= origin.x + column * cellWidth &&
                x < origin.x + (column + 1) * cellWidth,
            )
          )
            continue;
          const at = (y * width + x) * 3;
          for (let channel = 0; channel < 3; channel++)
            maximumOutsideError = Math.max(
              maximumOutsideError,
              Math.abs(pixels[at + channel] - reference[at + channel]),
            );
        }
    }
    let transitions = 0;
    for (let f = 1; f < phases.length; f++)
      if (phases[f] !== phases[f - 1]) transitions++;
    if (blinking) {
      assert(
        phases.includes(false) && phases.includes(true),
        name + ": missing blink phase",
      );
      assert(transitions >= 4, name + ": blink policy not connected");
    } else
      assert(
        phases.every(Boolean) && transitions === 0,
        name + ": steady policy not connected",
      );
    assert(
      maximumOutsideError <= (wayland ? 1 : 0),
      name + ": static neighbor changed",
    );
    const result = {
      name,
      kind,
      shape,
      frames,
      transitions,
      maximumOutsideError,
      origin,
    };
    results.push(result);
    console.log(JSON.stringify(result));
  };
  if (wide) {
    const send = async (key) => {
      command("xdotool", ["key", "--clearmodifiers", key]);
      const deadline = Date.now() + 15000;
      while (readTitle() !== "boxrig-blink-" + key) {
        assert(Date.now() < deadline, "wide cursor child acknowledgement");
        await setTimeout(20);
      }
    };
    await send("z");
    await send(fontWidth ? "f" : "w");
    await send("h");
    let underlay = recordPixels("wide-hidden", false).raw.subarray(
      15 * frameBytes,
      16 * frameBytes,
    );
    const recordWide = (name, column, columns, blinking, hollow = false) => {
      const { raw, frames } = recordPixels(name, blinking);
      const x0 = origin.x + column * cellWidth,
        y0 = origin.y + cellHeight;
      const cursorWidth = columns * cellWidth;
      const phases = [];
      for (let f = 15; f < frames; f++) {
        const pixels = raw.subarray(f * frameBytes, (f + 1) * frameBytes);
        let gold = 0,
          left = width,
          right = -1,
          top = height,
          bottom = -1;
        let outsideError = 0,
          insideError = 0;
        for (let y = 0; y < height; y++)
          for (let x = 0; x < width; x++) {
            const at = (y * width + x) * 3;
            const inside =
              x >= x0 && x < x0 + cursorWidth && y >= y0 && y < y0 + cellHeight;
            const error = Math.max(
              ...[0, 1, 2].map((c) =>
                Math.abs(pixels[at + c] - underlay[at + c]),
              ),
            );
            if (inside) insideError = Math.max(insideError, error);
            // GTK draws the acknowledgement title in client decorations. Only
            // terminal pixels belong to the unchanged-underlay contract.
            else if (x >= origin.x && y >= origin.y)
              outsideError = Math.max(outsideError, error);
            if (
              pixels[at] === 255 &&
              pixels[at + 1] === 215 &&
              pixels[at + 2] === 0
            ) {
              assert(inside, name + ": cursor escaped its grapheme");
              gold++;
              left = Math.min(left, x);
              right = Math.max(right, x);
              top = Math.min(top, y);
              bottom = Math.max(bottom, y);
            }
          }
        assert(
          outsideError <= (wayland ? 1 : 0),
          name + ": old cursor or neighbor changed",
        );
        if (gold) {
          assert.equal(
            right - left + 1,
            cursorWidth,
            name + ": complete character width",
          );
          assert.equal(
            bottom - top + 1,
            cellHeight,
            name + ": character height",
          );
          if (hollow)
            assert.equal(
              gold,
              cursorWidth * cellHeight - (cursorWidth - 2) * (cellHeight - 2),
            );
          else
            assert(
              gold > (cursorWidth * cellHeight) / 4,
              name + ": filled character",
            );
        } else
          assert(
            insideError <= (wayland ? 1 : 0),
            name + ": hidden phase did not restore text",
          );
        phases.push(gold !== 0);
      }
      const transitions = phases
        .slice(1)
        .filter((phase, i) => phase !== phases[i]).length;
      if (blinking)
        assert(
          phases.includes(true) && phases.includes(false) && transitions >= 4,
          name + ": both blink phases",
        );
      else assert(phases.every(Boolean), name + ": steady unfocused outline");
      const result = { name, columns, frames, transitions, hollow };
      results.push(result);
      console.log(JSON.stringify(result));
    };
    await send("v");
    recordWide("wide-leading", 7, wideColumns, true);
    await send("j");
    recordWide("wide-continuation", 7, wideColumns, true);
    if (fontWidth) {
      await send("k");
      recordWide("wide-second-continuation", 7, wideColumns, true);
    }
    command("xdotool", ["windowfocus", "--sync", "1"]);
    recordWide("wide-unfocused", 7, wideColumns, false, true);
    command("xdotool", ["windowfocus", "--sync", id]);
    recordWide("wide-refocused", 7, wideColumns, true);
    await send(fontWidth ? "l" : "n");
    recordWide("wide-to-narrow", 7 + wideColumns, 1, true);
    await send("j");
    recordWide("wide-return", 7, wideColumns, true);
    await send(fontWidth ? "p" : "o");
    await send("h");
    underlay = recordPixels("overwritten-hidden", false).raw.subarray(
      15 * frameBytes,
      16 * frameBytes,
    );
    for (let y = 0; y < cellHeight; y++)
      for (let x = 0; x < (wideColumns - 1) * cellWidth; x++) {
        const at =
          ((origin.y + cellHeight + y) * width + origin.x + 8 * cellWidth + x) *
          3;
        const blank =
          ((origin.y + cellHeight + y) * width +
            origin.x +
            15 * cellWidth +
            x) *
          3;
        // The new X can contribute a faint antialiased edge to the space.
        assert(
          [0, 1, 2].every(
            (c) =>
              Math.abs(underlay[at + c] - underlay[blank + c]) <=
              (x === 0 ? 3 : 0),
          ),
          "overwriting a wide character restores its former continuation cell",
        );
      }
    await send("v");
    recordWide("wide-overwritten", 7, 1, true);
  } else if (boxes) {
    for (const shape of ["fill", "underline", "bar"]) {
      record(shape + "-focused", true, "gold", shape);
      command("xdotool", ["windowfocus", "--sync", "1"]);
      record(shape + "-unfocused", false, "gold", shape, true);
      command("xdotool", ["windowfocus", "--sync", id]);
      record(shape + "-refocused", true, "gold", shape);
      command("xdotool", ["key", "--clearmodifiers", "ctrl+shift+F11"]);
    }
  } else {
    record("outline-steady", false, "gold", "outline");
    command("xdotool", ["key", "--clearmodifiers", "ctrl+shift+F11"]);
    record("underline-blink", true, "cyan", "underline");
    command("xdotool", ["key", "--clearmodifiers", "ctrl+shift+F11"]);
    record("bar-steady", false, "pink", "bar");
    command("xdotool", ["key", "--clearmodifiers", "ctrl+shift+F11"]);
    record("terminal-following", true, "terminal", "fill");
    const terminalShape = async (key, name, blinking, shape) => {
      command("xdotool", ["key", "--clearmodifiers", key]);
      const deadline = Date.now() + 15000;
      while (readTitle() !== "boxrig-blink-" + key) {
        assert(Date.now() < deadline, "DECSCUSR child acknowledgement timeout");
        await setTimeout(20);
      }
      record(name, blinking, "terminal", shape);
    };
    await terminalShape("3", "decscusr-underline", true, "underline");
    await terminalShape("6", "decscusr-bar", false, "bar");
    await terminalShape("2", "decscusr-block", false, "fill");
    command("xdotool", ["key", "--clearmodifiers", "ctrl+shift+F11"]);
    record("outline-restored", false, "gold", "outline");
  }
  command("xdotool", ["key", "--clearmodifiers", "q"]);
} finally {
  if (child) child.kill("SIGTERM");
  if (weston) weston.kill("SIGTERM");
  writeFileSync(
    resolve(output, "results.json"),
    JSON.stringify({ results, diagnostics }, null, 2) + "\n",
  );
  if (runtime) {
    if (existsSync(runtime + "/weston.log"))
      writeFileSync(
        resolve(output, "weston.log"),
        readFileSync(runtime + "/weston.log"),
      );
    rmSync(runtime, { recursive: true, force: true });
  }
}
