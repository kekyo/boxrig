/* Lossless native recordings assert observed phases and unchanged neighbors. */
import assert from "node:assert/strict";
import { spawn, execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
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
const switching = process.env.BOXRIG_TEST_RENDER_SWITCH_VIDEO === "1";
let weston, runtime, child, recording;
let diagnostics = "";
let renderOutput = "";
let initialPresentation;
const results = [];
try {
  if (wayland) {
    runtime = mkdtempSync(resolve(tmpdir(), "boxrig-blink-wayland-"));
    process.env.XDG_RUNTIME_DIR = runtime;
    process.env.WAYLAND_DISPLAY = "boxrig-blink";
    weston = spawn(
      "weston",
      [
        "--debug",
        "--backend=x11",
        "--renderer=" + (process.env.BOXRIG_TEST_WESTON_RENDERER ?? "pixman"),
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
      BOXRIG_TEST_SAMPLE_BLINK_VIDEO: "1",
      ...(switching ? { BOXRIG_RENDERER_DEMO: "1" } : {}),
      NO_AT_BRIDGE: "1",
    },
    stdio: ["ignore", "pipe", "pipe"],
  });
  child.stdout.on("data", (data) => {
    renderOutput += data;
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
  // The fixture's two gold cells mark a fully painted, opaque surface. A
  // compositor fade can outlast any fixed number of discarded video frames.
  const paintedDeadline = Date.now() + 15000;
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
        `${width}x${height}`,
        "-i",
        `${process.env.DISPLAY}+${values.X},${values.Y}`,
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
    assert.equal(pixels.length, frameBytes);
    let gold = 0,
      left = width,
      top = height,
      right = -1,
      bottom = -1;
    for (let at = 0; at < frameBytes; at += 3) {
      if (pixels[at] !== 255 || pixels[at + 1] !== 215 || pixels[at + 2] !== 0)
        continue;
      const x = (at / 3) % width,
        y = Math.floor(at / 3 / width);
      gold++;
      left = Math.min(left, x);
      top = Math.min(top, y);
      right = Math.max(right, x);
      bottom = Math.max(bottom, y);
    }
    if (
      gold === cellWidth * 2 * cellHeight &&
      right - left + 1 === cellWidth * 2 &&
      bottom - top + 1 === cellHeight
    ) {
      initialPresentation = { gold, left, top, right, bottom };
      break;
    }
    assert(Date.now() < paintedDeadline, "initial opaque fixture presentation");
    await setTimeout(20);
  }
  const record = (name, blinking) => {
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
        `${width}x${height}`,
        "-i",
        `${process.env.DISPLAY}+${values.X},${values.Y}`,
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
    const states = new Map();
    const sequence = [];
    // Allow native input and visibility changes to settle before comparison.
    for (let index = 15; index < frames; index++) {
      const pixels = raw.subarray(index * frameBytes, (index + 1) * frameBytes);
      let hash = createHash("sha256").update(pixels).digest("hex");
      // Weston can round the final fade sample by one 8-bit level. Preserve
      // larger changes, including any disappearing glyph, as distinct states.
      if (wayland && !states.has(hash)) {
        for (const [key, candidate] of states) {
          let near = true;
          for (let at = 0; at < frameBytes; at++) {
            if (Math.abs(candidate.pixels[at] - pixels[at]) > 1) {
              near = false;
              break;
            }
          }
          if (near) {
            hash = key;
            break;
          }
        }
      }
      const state = states.get(hash) ?? { count: 0, pixels };
      state.count++;
      states.set(hash, state);
      sequence.push(hash);
    }
    assert.equal(
      states.size,
      blinking ? 2 : 1,
      `${name}: ${states.size} visual states`,
    );
    let transitions = 0;
    for (let index = 1; index < sequence.length; index++)
      if (sequence[index] !== sequence[index - 1]) transitions++;
    let bounds;
    if (blinking) {
      assert(transitions >= 4, `${name}: ${transitions} phase transitions`);
      const [a, b] = [...states.values()];
      assert(a.count >= 10 && b.count >= 10);
      let x0 = width,
        y0 = height,
        x1 = -1,
        y1 = -1,
        changed = 0;
      for (let index = 0; index < width * height; index++) {
        const offset = index * 3;
        if (
          Math.abs(a.pixels[offset] - b.pixels[offset]) > (wayland ? 1 : 0) ||
          Math.abs(a.pixels[offset + 1] - b.pixels[offset + 1]) >
            (wayland ? 1 : 0) ||
          Math.abs(a.pixels[offset + 2] - b.pixels[offset + 2]) >
            (wayland ? 1 : 0)
        ) {
          const x = index % width,
            y = Math.floor(index / width);
          x0 = Math.min(x0, x);
          x1 = Math.max(x1, x);
          y0 = Math.min(y0, y);
          y1 = Math.max(y1, y);
          changed++;
        }
      }
      assert(
        changed === cellWidth * cellHeight &&
          x1 - x0 + 1 === cellWidth &&
          y1 - y0 + 1 === cellHeight,
        `${name}: expected exactly one filled cursor cell`,
      );
      bounds = {
        x: x0,
        y: y0,
        width: x1 - x0 + 1,
        height: y1 - y0 + 1,
        changed,
      };
    }
    results.push({
      name,
      frames,
      states: states.size,
      transitions,
      bounds,
      roundingTolerance: wayland ? 1 : 0,
    });
    console.log(JSON.stringify(results.at(-1)));
    return bounds;
  };
  const stage = (key) => {
    command("xdotool", ["key", "--clearmodifiers", key]);
    const until = Date.now() + 15000;
    while (readTitle() !== "boxrig-blink-" + key) {
      assert(Date.now() < until, "child acknowledgement timeout");
      execFileSync("sleep", ["0.02"]);
    }
  };
  if (switching) {
    const captureFrame = () =>
      execFileSync(
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
    const sameFrame = (a, b) => {
      if (a.length !== frameBytes || b.length !== frameBytes) return false;
      for (let at = 0; at < frameBytes; at++)
        if (Math.abs(a[at] - b[at]) > (wayland ? 1 : 0)) return false;
      return true;
    };
    const stableFrame = async () => {
      const deadline = Date.now() + 15000;
      let previous = captureFrame();
      let matches = 0;
      while (matches < 2) {
        await setTimeout(20);
        const next = captureFrame();
        matches = sameFrame(previous, next) ? matches + 1 : 0;
        previous = next;
        assert(Date.now() < deadline, "native frame did not settle");
      }
      return previous;
    };
    const choose = async (policy) => {
      const start = renderOutput.length;
      const backend = policy === 0 ? 1 : 2;
      command("xdotool", [
        "key",
        "--clearmodifiers",
        "ctrl+shift+F" + (6 + policy),
      ]);
      const completed = new RegExp(
        "render-policy=" +
          policy +
          " render-backend=" +
          backend +
          " render-pending=0 render-reason=1 render-status=0",
      );
      const deadline = Date.now() + 15000;
      while (!completed.test(renderOutput.slice(start))) {
        assert(
          Date.now() < deadline,
          "render policy " +
            policy +
            " did not complete:\n" +
            renderOutput +
            diagnostics,
        );
        await setTimeout(20);
      }
    };
    stage("s");
    const childrenPath =
      "/proc/" + child.pid + "/task/" + child.pid + "/children";
    const originalChildren = readFileSync(childrenPath, "utf8").trim();
    assert(originalChildren.length > 0, "sample PTY child exists");
    await stableFrame();
    await choose(2);
    const gl = await stableFrame();
    await choose(0);
    const cairo = await stableFrame();
    for (const [name, pixels] of [
      ["cairo", cairo],
      ["opengl", gl],
    ]) {
      execFileSync(
        "ffmpeg",
        [
          "-y",
          "-nostdin",
          "-loglevel",
          "error",
          "-f",
          "rawvideo",
          "-pixel_format",
          "rgb24",
          "-video_size",
          width + "x" + height,
          "-i",
          "pipe:0",
          "-frames:v",
          "1",
          resolve(output, name + ".png"),
        ],
        { input: pixels, timeout: 30000 },
      );
    }
    const file = resolve(output, "render-switch.mkv");
    recording = spawn(
      "ffmpeg",
      [
        "-y",
        "-loglevel",
        "error",
        "-f",
        "x11grab",
        "-framerate",
        "60",
        "-draw_mouse",
        "0",
        "-video_size",
        width + "x" + height,
        "-i",
        process.env.DISPLAY + "+" + values.X + "," + values.Y,
        "-progress",
        "pipe:1",
        "-c:v",
        "ffv1",
        file,
      ],
      { stdio: ["pipe", "pipe", "pipe"] },
    );
    let progress = "";
    recording.stdout.on("data", (data) => {
      progress += data;
    });
    recording.stderr.on("data", (data) => {
      diagnostics += data;
    });
    const readyDeadline = Date.now() + 15000;
    while (!/frame=[1-9]/.test(progress)) {
      assert(Date.now() < readyDeadline, "video capture did not begin");
      await setTimeout(20);
    }
    const selections = [2, 0, 1, 0, 2, 0, 1, 0, 2, 0, 1, 0];
    for (const policy of selections) {
      await choose(policy);
      assert(
        sameFrame(await stableFrame(), policy === 0 ? cairo : gl),
        "completed backend image",
      );
      assert.equal(
        readFileSync(childrenPath, "utf8").trim(),
        originalChildren,
        "PTY child preserved",
      );
    }
    await new Promise((resolveExit, rejectExit) => {
      const guard = globalThis.setTimeout(
        () => rejectExit(new Error("video finalization timeout")),
        15000,
      );
      recording.once("exit", (code) => {
        globalThis.clearTimeout(guard);
        if (code === 0) resolveExit();
        else rejectExit(new Error("ffmpeg exit " + code));
      });
      recording.stdin.end("q");
    });
    recording = undefined;
    stage("h");
    stage("s");
    assert(sameFrame(await stableFrame(), cairo), "PTY input after switching");
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
      { maxBuffer: frameBytes * 4000, timeout: 30000 },
    );
    assert.equal(raw.length % frameBytes, 0);
    const frames = raw.length / frameBytes;
    assert(frames >= selections.length, "recorded native transition frames");
    const counts = { cairo: 0, opengl: 0 };
    for (let index = 0; index < frames; index++) {
      const pixels = raw.subarray(index * frameBytes, (index + 1) * frameBytes);
      if (sameFrame(pixels, cairo)) counts.cairo++;
      else if (sameFrame(pixels, gl)) counts.opengl++;
      else
        assert.fail(
          "incomplete or stale terminal image at video frame " + index,
        );
    }
    results.push({
      name: "render-switch",
      frames,
      selections,
      counts,
      identicalBackends: sameFrame(cairo, gl),
      childPids: originalChildren,
      inputAcknowledged: true,
      roundingTolerance: wayland ? 1 : 0,
    });
    console.log(JSON.stringify(results.at(-1)));
  } else {
    const first = record("blink", true);
    stage("s");
    record("steady", false);
    stage("m");
    const moved = record("moved", true);
    assert.equal(moved.x - first.x, cellWidth * 3);
    assert.equal(moved.y, first.y);
    stage("h");
    record("hidden-cursor", false);
    stage("v");
    record("visible-cursor", true);
    command("xdotool", ["windowfocus", "--sync", "1"]);
    record("unfocused", false);
    command("xdotool", ["windowfocus", "--sync", id]);
    record("refocused", true);
    command("xdotool", ["windowunmap", id]);
    command("xdotool", ["windowmap", "--sync", id]);
    command("xdotool", ["windowfocus", "--sync", id]);
    record("reshown", true);
  }
  command("xdotool", ["key", "--clearmodifiers", "q"]);
} finally {
  if (recording) recording.kill("SIGTERM");
  if (child) child.kill("SIGTERM");
  if (weston) weston.kill("SIGTERM");
  writeFileSync(
    resolve(output, "results.json"),
    JSON.stringify(
      { initialPresentation, results, diagnostics, renderOutput },
      null,
      2,
    ) + "\n",
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
