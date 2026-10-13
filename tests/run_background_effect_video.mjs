/* Lossless recordings of the real compositor, sample and owned backdrop. */
import assert from "node:assert/strict";
import { execFileSync, spawn, spawnSync } from "node:child_process";
import { mkdirSync, mkdtempSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { resolve } from "node:path";
import { setTimeout } from "node:timers/promises";

const [sample, scene, renderer, output] = process.argv.slice(2);
assert(
  sample && scene && renderer && output,
  "sample, scene, renderer and output required",
);
mkdirSync(output, { recursive: true });
const runtime = mkdtempSync(resolve(tmpdir(), "boxrig-blur-"));
const wayland = process.env.BOXRIG_TEST_NESTED_WAYLAND === "1";
const captureDisplay =
  process.env.BOXRIG_TEST_CAPTURE_DISPLAY ?? process.env.DISPLAY;
const environment = {
  ...process.env,
  XDG_CONFIG_HOME: wayland ? process.env.XDG_CONFIG_HOME : runtime,
  XDG_RUNTIME_DIR: wayland ? process.env.XDG_RUNTIME_DIR : runtime,
  KWIN_COMPOSE: "O2",
  LIBGL_ALWAYS_SOFTWARE: process.env.LIBGL_ALWAYS_SOFTWARE ?? "1",
  GDK_BACKEND: wayland ? "wayland" : "x11",
  GTK_A11Y: "none",
  NO_AT_BRIDGE: "1",
};
writeFileSync(
  resolve(runtime, "kwinrc"),
  "[Compositing]\nEnabled=true\nWindowsBlockCompositing=false\n[Plugins]\nblurEnabled=true\nslidingpopupsEnabled=false\nfadeEnabled=false\nfadingpopupsEnabled=false\nsheetEnabled=false\n[Effect-blur]\nBlurStrength=15\nNoiseStrength=0\n",
);
writeFileSync(
  resolve(runtime, "kdeglobals"),
  "[KDE]\nAnimationDurationFactor=0\n",
);
const command = (name, args, options = {}) =>
  execFileSync(name, args, {
    env: environment,
    encoding: "utf8",
    timeout: 30000,
    stdio: ["ignore", "pipe", "pipe"],
    ...options,
  }).trim();
const children = [];
const launch = (name, args, extra = {}) => {
  const child = spawn(name, args, {
    env: { ...environment, ...extra },
    stdio: ["ignore", "pipe", "pipe"],
  });
  let log = "";
  child.stdout.on("data", (data) => {
    log += data;
  });
  child.stderr.on("data", (data) => {
    log += data;
  });
  children.push({ child, log: () => log });
  return child;
};
const wait = async (description, condition) => {
  const deadline = Date.now() + 20000;
  while (!condition()) {
    assert(
      Date.now() < deadline,
      description + "\n" + children.map((c) => c.log()).join("\n"),
    );
    await setTimeout(20);
  }
};
const captureEnvironment = {
  ...environment,
  DISPLAY: captureDisplay,
  XAUTHORITY:
    process.env.BOXRIG_TEST_CAPTURE_XAUTHORITY ?? process.env.XAUTHORITY,
};
const outerCommand = (name, args) =>
  command(name, args, { env: captureEnvironment });
let scriptNumber = 0;
const kwinScript = (source) => {
  const path = resolve(runtime, "control-" + scriptNumber++ + ".js");
  writeFileSync(path, source);
  const reply = command("gdbus", [
    "call",
    "--session",
    "--dest",
    "org.kde.KWin",
    "--object-path",
    "/Scripting",
    "--method",
    "org.kde.kwin.Scripting.loadScript",
    path,
  ]);
  const number = reply.match(/\d+/)?.[0];
  assert(number !== undefined, reply);
  command("gdbus", [
    "call",
    "--session",
    "--dest",
    "org.kde.KWin",
    "--object-path",
    "/Scripting/Script" + number,
    "--method",
    "org.kde.kwin.Script.run",
  ]);
};
try {
  if (!wayland) {
    launch("kwin_x11", ["--replace"]);
    await wait("KWin blur capability", () =>
      command("xprop", ["-root", "_KDE_NET_WM_BLUR_BEHIND_REGION"]).includes(
        " = ",
      ),
    );
  } else {
    const protocols = command("wayland-info", []);
    assert(
      protocols.includes("ext_background_effect_manager_v1"),
      "real compositor must advertise background effects",
    );
    writeFileSync(resolve(output, "protocols.txt"), protocols);
    kwinScript(`const place = (window) => {
      if (window.caption === "plan27-background-ready") {
        window.frameGeometry = {x:80,y:80,width:900,height:700};
        workspace.activeWindow = window;
      }
    };
    workspace.windowAdded.connect((window) => { window.captionChanged.connect(() => place(window)); place(window); });
    workspace.windowList().forEach(place);`);
  }
  const support = command("dbus-send", [
    "--session",
    "--print-reply",
    "--reply-timeout=10000",
    "--dest=org.kde.KWin",
    "/KWin",
    "org.kde.KWin.supportInformation",
  ]);
  writeFileSync(resolve(output, "compositor.txt"), support);
  launch(resolve(scene), []);
  const terminal = launch(
    resolve(sample),
    ["--renderer=" + renderer, resolve(scene), "--terminal"],
    {
      BOXRIG_BACKGROUND_OPACITY: "0",
      BOXRIG_BACKGROUND_EFFECT: "prefer",
      GSK_RENDERER: renderer === "cairo" ? "cairo" : "gl",
    },
  );
  let id;
  if (!wayland) {
    await wait("sample window", () => {
      try {
        id = command("xdotool", [
          "search",
          "--onlyvisible",
          "--name",
          "^plan27-background-ready$",
        ]).split("\n")[0];
        return !!id;
      } catch {
        return false;
      }
    });
    command("xdotool", ["windowmove", "--sync", id, "80", "80"]);
    command("xdotool", ["windowsize", "--sync", id, "900", "700"]);
    command("xdotool", ["windowactivate", "--sync", id]);
    command("xdotool", ["mousemove", "1200", "850"]);
  } else {
    // The nested X11 window's advertised PID can differ from its private
    // D-Bus PID. Select the single window created since the launch snapshot.
    const existing = process.env.BOXRIG_TEST_EXISTING_COMPOSITORS;
    assert(existing !== undefined, "launcher window snapshot required");
    const previousWindows = new Set(existing.split(/\s+/));
    await wait("nested compositor window", () => {
      try {
        const windows = outerCommand("xdotool", [
          "search",
          "--onlyvisible",
          "--name",
          "^KDE Wayland Compositor",
        ])
          .split("\n")
          .filter((window) => !previousWindows.has(window));
        if (windows.length !== 1) return false;
        id = windows[0];
        return !!id;
      } catch {
        return false;
      }
    });
    outerCommand("xdotool", ["windowfocus", "--sync", id]);
    outerCommand("xdotool", ["mousemove", "--window", id, "1100", "700"]);
  }
  const width = 500,
    height = 400,
    bytes = width * height * 3;
  const results = [];
  let reference, textMask, greenMask, originalOrigin, cellWidth, cellHeight;
  const stages = [
    "off",
    "on",
    "resized-on",
    ...(wayland
      ? ["capability-lost", "capability-restored"]
      : ["compositor-restarted"]),
    "off-after",
  ];
  for (const stage of stages) {
    const enabled =
      stage === "on" ||
      stage === "resized-on" ||
      stage === "capability-restored" ||
      stage === "compositor-restarted";
    if (stage === "compositor-restarted") {
      const manager = () =>
        command("xprop", ["-root", "_NET_SUPPORTING_WM_CHECK"]);
      const previous = manager();
      launch("kwin_x11", ["--replace"]);
      await wait(
        "replacement window manager and blur capability",
        () =>
          manager() !== previous &&
          manager().includes("window id #") &&
          command("xprop", [
            "-root",
            "_KDE_NET_WM_BLUR_BEHIND_REGION",
          ]).includes(" = "),
      );
      command("xdotool", ["windowactivate", "--sync", id]);
    }
    const capabilityChange = stage.startsWith("capability-");
    if (capabilityChange) {
      // Only the private nested compositor is controlled through its public
      // org.kde.kwin.Effects D-Bus interface. Keep the client's request intact.
      const log = children.find((c) => c.child === terminal).log;
      const previous = log().length;
      command("gdbus", [
        "call",
        "--session",
        "--dest",
        "org.kde.KWin",
        "--object-path",
        "/Effects",
        "--method",
        "org.kde.kwin.Effects." + (enabled ? "loadEffect" : "unloadEffect"),
        "blur",
      ]);
      await wait("provider observes compositor capability change", () =>
        log()
          .slice(previous)
          .includes(
            "supported=" + Number(enabled) + " accepted=" + Number(enabled),
          ),
      );
    }
    if (stage === "resized-on") {
      if (wayland)
        kwinScript(`workspace.windowList().filter(w => w.caption.startsWith("plan27-effect-")).forEach(w => {
        w.frameGeometry = {x:140,y:120,width:1000,height:650}; workspace.activeWindow = w;
      });`);
      else {
        command("xdotool", ["windowmove", "--sync", id, "140", "120"]);
        command("xdotool", ["windowsize", "--sync", id, "1000", "750"]);
      }
    }
    let geometry;
    if (wayland) {
      if (!capabilityChange) {
        const log = children.find((c) => c.child === terminal).log;
        const previous = log().split("plan27-effect-").length;
        await wait("sample applied Wayland effect", () => {
          if (log().split("plan27-effect-").length > previous) return true;
          outerCommand("xdotool", ["key", enabled ? "F9" : "F8"]);
          return false;
        });
      }
      geometry = {
        X: stages.indexOf(stage) >= 2 ? 140 : 80,
        Y: stages.indexOf(stage) >= 2 ? 120 : 80,
      };
    } else {
      if (stage !== "compositor-restarted")
        command("xdotool", ["key", enabled ? "F9" : "F8"]);
      await wait(
        "sample applied effect",
        () =>
          command("xdotool", ["getwindowname", id]) ===
          (enabled ? "plan27-effect-on" : "plan27-effect-off"),
      );
      geometry = Object.fromEntries(
        command("xdotool", ["getwindowgeometry", "--shell", id])
          .split("\n")
          .map((line) => line.split("=")),
      );
    }
    const captureWayland = (count) => {
      assert(
        process.env.BOXRIG_TEST_KWIN_CAPTURE,
        "KWin capture helper required",
      );
      const captured = spawnSync(
        process.env.BOXRIG_TEST_KWIN_CAPTURE,
        [geometry.X, geometry.Y, width, height, count].map(String),
        { env: environment, timeout: 30000, maxBuffer: 100 * 1024 * 1024 },
      );
      assert.equal(captured.status, 0, captured.stderr?.toString());
      if (count > 1)
        writeFileSync(resolve(output, stage + "-capture.log"), captured.stderr);
      return captured.stdout;
    };
    await wait("Kitty image reached the presented frame", () => {
      const pixels = wayland
        ? captureWayland(1)
        : execFileSync(
            "ffmpeg",
            [
              "-v",
              "error",
              "-nostdin",
              "-f",
              "x11grab",
              "-video_size",
              width + "x" + height,
              "-i",
              captureDisplay + "+" + geometry.X + "," + geometry.Y,
              "-frames:v",
              "1",
              "-f",
              "rawvideo",
              "-pix_fmt",
              "rgb24",
              "pipe:1",
            ],
            { env: captureEnvironment, maxBuffer: bytes * 2 },
          );
      let green = 0;
      for (let p = 0; p < pixels.length; p += 3)
        if (pixels[p] < 8 && pixels[p + 1] > 248 && pixels[p + 2] < 8) green++;
      return green > 600;
    });
    const movie = resolve(output, stage + ".mkv");
    let frames;
    if (wayland) {
      // Use compositor coordinates and completed captures independently of
      // the parent X11 window's presentation and screen capture.
      frames = captureWayland(90);
      command(
        "ffmpeg",
        [
          "-v",
          "error",
          "-y",
          "-f",
          "rawvideo",
          "-pixel_format",
          "rgb24",
          "-video_size",
          width + "x" + height,
          "-framerate",
          "30",
          "-i",
          "pipe:0",
          "-c:v",
          "ffv1",
          "-pix_fmt",
          "bgr0",
          movie,
        ],
        { input: frames, stdio: ["pipe", "pipe", "pipe"] },
      );
    } else {
      outerCommand("ffmpeg", [
        "-v",
        "error",
        "-y",
        "-nostdin",
        "-f",
        "x11grab",
        "-video_size",
        width + "x" + height,
        "-framerate",
        "30",
        "-i",
        captureDisplay + "+" + geometry.X + "," + geometry.Y,
        "-frames:v",
        "90",
        "-c:v",
        "ffv1",
        "-pix_fmt",
        "bgr0",
        movie,
      ]);
      frames = execFileSync(
        "ffmpeg",
        [
          "-v",
          "error",
          "-i",
          movie,
          "-fps_mode",
          "passthrough",
          "-f",
          "rawvideo",
          "-pix_fmt",
          "rgb24",
          "pipe:1",
        ],
        { maxBuffer: 100 * 1024 * 1024 },
      );
    }
    assert.equal(frames.length, bytes * 90);
    command("ffmpeg", [
      "-v",
      "error",
      "-y",
      "-i",
      movie,
      "-frames:v",
      "1",
      resolve(output, stage + ".png"),
    ]);
    const first = frames.subarray(0, bytes);
    const greens = [];
    for (let p = 0; p < width * height; p++)
      if (first[p * 3] < 8 && first[p * 3 + 1] > 248 && first[p * 3 + 2] < 8)
        greens.push(p);
    assert(greens.length > 600, "Kitty image must be visible");
    const left = Math.min(...greens.map((p) => p % width));
    const right = Math.max(...greens.map((p) => p % width));
    const top = Math.min(...greens.map((p) => Math.floor(p / width)));
    const bottom = Math.max(...greens.map((p) => Math.floor(p / width)));
    cellWidth = (right - left + 1) / 12;
    cellHeight = (bottom - top + 1) / 4;
    const origin = { x: left, y: top - 2 * cellHeight };
    assert(origin.y >= 0 && origin.y + 8 * cellHeight < height);
    if (!reference) {
      reference = Buffer.from(first);
      originalOrigin = origin;
      greenMask = greens;
      textMask = [];
      for (let y = origin.y; y < origin.y + 2 * cellHeight; y++)
        for (
          let x = origin.x;
          x < Math.min(width, origin.x + 20 * cellWidth);
          x++
        ) {
          const p = y * width + x;
          if (Math.min(...first.subarray(p * 3, p * 3 + 3)) > 180)
            textMask.push(p);
        }
      assert(textMask.length > 150, "foreground text must be visible");
    }
    const shift =
      (origin.y - originalOrigin.y) * width + origin.x - originalOrigin.x;
    let gradient = 0,
      minimumBackground = 255,
      maximumBackground = 0;
    let maximumTextDifference = 0,
      minimumGreen = 255,
      maximumLeak = 0,
      cursorMin = Infinity,
      cursorMax = 0;
    for (let frame = 0; frame < 90; frame++) {
      const pixels = frames.subarray(frame * bytes, (frame + 1) * bytes);
      let redDifference = 0,
        background = 0,
        count = 0;
      for (let y = 300; y < 360; y++)
        for (let x = 400; x < 480; x++) {
          const p = (y * width + x) * 3;
          redDifference += Math.abs(pixels[p] - pixels[p - 3]);
          background += pixels[p + 1];
          count++;
        }
      gradient += redDifference / count / 90;
      minimumBackground = Math.min(minimumBackground, background / count);
      maximumBackground = Math.max(maximumBackground, background / count);
      let difference = 0,
        green = 0,
        leak = 0,
        cursor = 0;
      for (const p of textMask)
        for (let c = 0; c < 3; c++)
          difference += Math.abs(
            pixels[(p + shift) * 3 + c] - reference[p * 3 + c],
          );
      maximumTextDifference = Math.max(
        maximumTextDifference,
        difference / textMask.length / 3,
      );
      for (const p of greenMask) {
        green += pixels[(p + shift) * 3 + 1];
        leak += Math.max(pixels[(p + shift) * 3], pixels[(p + shift) * 3 + 2]);
      }
      minimumGreen = Math.min(minimumGreen, green / greenMask.length);
      maximumLeak = Math.max(maximumLeak, leak / greenMask.length);
      for (
        let y = origin.y + 7 * cellHeight;
        y < origin.y + 8 * cellHeight;
        y++
      )
        for (let x = origin.x; x < origin.x + cellWidth; x++) {
          const p = (y * width + x) * 3;
          if (Math.min(pixels[p], pixels[p + 1], pixels[p + 2]) > 180) cursor++;
        }
      cursorMin = Math.min(cursorMin, cursor);
      cursorMax = Math.max(cursorMax, cursor);
    }
    const result = {
      stage,
      frames: 90,
      origin,
      cellWidth,
      cellHeight,
      gradient,
      backgroundRange: maximumBackground - minimumBackground,
      maximumTextDifference,
      minimumGreen,
      maximumLeak,
      cursorMin,
      cursorMax,
    };
    results.push(result);
    writeFileSync(
      resolve(output, "results.json"),
      JSON.stringify(results, null, 2) + "\n",
    );
    console.log(JSON.stringify(result));
    if (!enabled) assert(gradient > 10, "sharp backdrop when disabled");
    if (enabled)
      assert(
        gradient < results[0].gradient * 0.4,
        "compositor must blur the backdrop",
      );
    assert(
      result.backgroundRange > 4,
      "stationary terminal must expose moving backdrop",
    );
    assert(
      maximumTextDifference < 8 && minimumGreen > 248 && maximumLeak < 8,
      "foreground must remain unchanged",
    );
    assert(cursorMin === 0 && cursorMax > 5, "cursor must keep blinking");
  }
  writeFileSync(
    resolve(output, "passed.json"),
    JSON.stringify({ renderer, wayland, frames: results.length * 90 }) + "\n",
  );
} finally {
  for (const { child, log } of children.reverse()) {
    if (child.exitCode === null && child.signalCode === null) {
      child.kill("SIGTERM");
      await new Promise((done) => child.once("exit", done));
    }
    writeFileSync(resolve(output, "process-" + child.pid + ".log"), log());
  }
  rmSync(runtime, { recursive: true, force: true });
}
