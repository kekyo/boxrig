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
let weston, runtime, child;
let diagnostics = "";
const results = [];
try {
  if (wayland) {
    runtime = mkdtempSync(resolve(tmpdir(), "boxrig-blink-wayland-"));
    process.env.XDG_RUNTIME_DIR = runtime;
    process.env.WAYLAND_DISPLAY = "boxrig-blink";
    // The shell's startup fade can outlive the first 15 recorded frames.
    // Disable that independent effect while asserting the application's blink.
    // Documented by weston.ini(5): shell.startup-animation = none.
    const configuration = resolve(runtime, "weston.ini");
    writeFileSync(configuration, "[shell]\nstartup-animation=none\n");
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
        "--config=" + configuration,
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
  let origin;
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
    const columns = [1, 4, 7],
      intensities = columns.map(() => []);
    const visibilitySamples = columns.map(() => []);
    const reference = raw.subarray(15 * frameBytes, 16 * frameBytes);
    if (!origin) {
      let left = width,
        top = height,
        right = -1,
        bottom = -1;
      for (let f = 16; f < frames; f++)
        for (let pixel = 0; pixel < width * height; pixel++) {
          const at = pixel * 3;
          if (
            [0, 1, 2].some(
              (channel) =>
                Math.abs(
                  raw[f * frameBytes + at + channel] - reference[at + channel],
                ) > (wayland ? 1 : 0),
            )
          ) {
            left = Math.min(left, pixel % width);
            right = Math.max(right, pixel % width);
            top = Math.min(top, Math.floor(pixel / width));
            bottom = Math.max(bottom, Math.floor(pixel / width));
          }
        }
      assert.equal(
        right - left + 1,
        cellWidth * 7,
        "three independently animated cells",
      );
      assert.equal(bottom - top + 1, cellHeight, "one animated row");
      origin = { x: left - cellWidth, y: top - cellHeight };
    }
    let maximumOutsideError = 0;
    for (let f = 15; f < frames; f++) {
      const pixels = raw.subarray(f * frameBytes, (f + 1) * frameBytes);
      for (let area = 0; area < columns.length; area++) {
        let ink = 0,
          lowest = 255,
          highest = 0;
        for (let y = origin.y + cellHeight; y < origin.y + cellHeight * 2; y++)
          for (
            let x = origin.x + columns[area] * cellWidth;
            x < origin.x + (columns[area] + 1) * cellWidth;
            x++
          ) {
            const at = (y * width + x) * 3;
            ink += pixels[at] + pixels[at + 1] + pixels[at + 2];
            for (let channel = 0; channel < 3; channel++) {
              lowest = Math.min(lowest, pixels[at + channel]);
              highest = Math.max(highest, pixels[at + channel]);
            }
          }
        intensities[area].push(ink);
        // Text uses glyph contrast; the blank filled cursor must cover every
        // pixel. Its minimum intensity rejects the previous outline shape.
        visibilitySamples[area].push(area === 2 ? lowest : highest - lowest);
      }
      for (let y = 0; y < height; y++)
        for (let x = 0; x < width; x++) {
          if (
            y >= origin.y + cellHeight &&
            y < origin.y + cellHeight * 2 &&
            columns.some(
              (column) =>
                x >= origin.x + column * cellWidth &&
                x < origin.x + (column + 1) * cellWidth,
            )
          )
            continue;
          const at = (y * width + x) * 3;
          for (let channel = 0; channel < 3; channel++) {
            maximumOutsideError = Math.max(
              maximumOutsideError,
              Math.abs(pixels[at + channel] - reference[at + channel]),
            );
            if (
              y >= origin.y + cellHeight * 3 &&
              y < origin.y + cellHeight * 4 &&
              x >= origin.x &&
              x < origin.x + cellWidth * 9
            )
              assert(
                pixels[at + channel] <= 1,
                "concealed text became visible",
              );
          }
        }
    }
    assert(
      maximumOutsideError <= (wayland ? 1 : 0),
      name + ": static neighbors changed " + maximumOutsideError,
    );
    const regions = intensities.map((values, index) => {
      const minimum = Math.min(...values),
        maximum = Math.max(...values);
      assert(maximum > 1000, name + ": missing glyph/cursor " + index);
      const phases = visibilitySamples[index].map((value) => value > 64);
      let transitions = 0;
      for (let at = 1; at < phases.length; at++)
        if (phases[at] !== phases[at - 1]) transitions++;
      if (blinking) {
        assert(
          visibilitySamples[index].some((value) => value <= 1),
          name + ": glyph did not disappear " + index,
        );
        assert(phases.includes(true), name + ": glyph never appeared " + index);
        assert(
          transitions >= (index === 1 ? 16 : 4),
          name + ": insufficient transitions " + index,
        );
      } else
        assert(
          phases.every(Boolean) && transitions === 0,
          name + ": reset failed " + index,
        );
      return {
        kind: ["slow", "rapid", "cursor"][index],
        transitions,
        minimum,
        maximum,
      };
    });
    if (blinking)
      assert(
        regions[1].transitions > regions[0].transitions * 1.8,
        "rapid and slow must have different frequencies",
      );
    const result = { name, frames, regions, maximumOutsideError, origin };
    results.push(result);
    console.log(JSON.stringify(result));
  };
  const stage = async (key) => {
    command("xdotool", ["key", "--clearmodifiers", key]);
    const until = Date.now() + 15000;
    while (readTitle() !== "boxrig-blink-" + key) {
      assert(Date.now() < until, "child acknowledgement timeout");
      await setTimeout(20);
    }
  };
  record("slow-rapid-cursor", true);
  await stage("c");
  record("sgr-25", false);
  await stage("z");
  record("sgr-0", false);
  await stage("b");
  record("restored-blink", true);
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
