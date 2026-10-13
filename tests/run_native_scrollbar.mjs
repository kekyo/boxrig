/* Exercise the actual driver, native shortcuts and PTY size feedback. */
import assert from "node:assert/strict";
import { spawn, execFileSync } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { resolve } from "node:path";
import { setTimeout } from "node:timers/promises";

const [sample, helper, renderer] = process.argv.slice(2);
assert(sample && helper);
const command = (name, args) =>
  execFileSync(name, args, { encoding: "utf8", timeout: 30000 }).trim();
const wayland = process.env.BOXRIG_TEST_SCROLLBAR_WAYLAND === "1";
const scale = Number(process.env.BOXRIG_TEST_SCROLLBAR_SCALE ?? "1");
assert(scale === 1 || scale === 2);
let child,
  weston,
  runtime,
  id,
  diagnostics = "";
const wait = async (read, predicate, label) => {
  const deadline = Date.now() + 30000;
  for (;;) {
    const result = read();
    if (predicate(result)) return result;
    assert(child?.exitCode == null, `sample exited: ${diagnostics}`);
    assert(
      Date.now() < deadline,
      `${label}: ${JSON.stringify(result)}\n${diagnostics}`,
    );
    await setTimeout(25);
  }
};
try {
  if (wayland) {
    runtime = mkdtempSync(resolve(tmpdir(), "boxrig-scrollbar-"));
    process.env.XDG_RUNTIME_DIR = runtime;
    process.env.WAYLAND_DISPLAY = "boxrig-scrollbar";
    weston = spawn(
      "weston",
      [
        "--debug",
        "--backend=x11",
        "--renderer=pixman",
        "--socket=boxrig-scrollbar",
        "--width=1150",
        "--height=850",
        "--scale=" + scale,
        "--idle-time=0",
        "--no-config",
        "--log=" + runtime + "/weston.log",
      ],
      { env: process.env, stdio: "ignore" },
    );
    await wait(
      () => existsSync(runtime + "/boxrig-scrollbar"),
      Boolean,
      "Weston socket",
    );
  }
  child = spawn(
    resolve(sample),
    [
      ...(renderer ? ["--renderer=" + renderer] : []),
      resolve(helper),
      "--sample-scenario",
    ],
    {
      env: {
        ...process.env,
        BOXRIG_SCROLLBAR: "always",
        BOXRIG_TEST_SAMPLE_SCROLLBAR: "1",
        NO_AT_BRIDGE: "1",
      },
      stdio: ["ignore", "ignore", "pipe"],
    },
  );
  child.stderr.on("data", (data) => {
    diagnostics += data;
  });
  const scene = () => command("weston-debug", ["scene-graph"]);
  const title = () =>
    wayland
      ? /top-level window '(boxrig-scrollbar-[^']*)'/.exec(scene())?.[1]
      : id
        ? command("xdotool", ["getwindowname", id])
        : undefined;
  if (wayland) {
    await wait(
      title,
      (v) => v?.startsWith("boxrig-scrollbar-"),
      "initial title",
    );
    id = /window id (\d+)/.exec(
      readFileSync(runtime + "/weston.log", "utf8"),
    )?.[1];
    assert(id);
    const section = scene()
      .split(/(?=View \d+ \(role )/)
      .find(
        (part) =>
          part.includes("PID " + child.pid + ",") &&
          part.includes("role xdg_toplevel"),
      );
    const position = /position: \((\d+), (\d+)\)/.exec(section ?? "");
    assert(position, section);
    command("xdotool", [
      "windowfocus",
      "--sync",
      id,
      "mousemove",
      "--window",
      id,
      String((Number(position[1]) + 150) * scale),
      String((Number(position[2]) + 100) * scale),
      "click",
      "1",
    ]);
  } else {
    id = command("xdotool", [
      "search",
      "--sync",
      "--onlyvisible",
      "--name",
      "^boxrig-scrollbar-",
    ]).split("\n")[0];
    command("xdotool", ["windowfocus", "--sync", id]);
  }
  const report = () => {
    const match = /^boxrig-scrollbar-(\d+)-(.)-(\d+)-(\d+)-(\d+)-(\d+)$/.exec(
      title() ?? "",
    );
    return (
      match && {
        sequence: Number(match[1]),
        command: match[2],
        ch: Number(match[3]),
        cw: Number(match[4]),
        rows: Number(match[5]),
        columns: Number(match[6]),
      }
    );
  };
  const key = (key) => command("xdotool", ["key", "--clearmodifiers", key]);
  let last = await wait(report, Boolean, "initial grid");
  assert.equal(last.rows, 24);
  assert.equal(last.columns, 80, "initial grid includes scrollbar reservation");
  const query = async (keyName = "g") => {
    const previous = last.sequence;
    key(keyName);
    last = await wait(
      report,
      (v) => v && v.sequence > previous,
      "PTY acknowledgement",
    );
    return last;
  };
  const grid = async (predicate, label) => {
    const deadline = Date.now() + 30000;
    do {
      await query();
      if (predicate(last)) {
        console.log(label, last);
        return last;
      }
      assert(
        Date.now() < deadline,
        `${label}: ${JSON.stringify(last)}\n${diagnostics}`,
      );
    } while (true);
  };
  key("ctrl+shift+F9"); // Empty AUTO hides.
  const expanded = (await grid((v) => v.columns > 80, "auto empty")).columns;
  await query("h");
  await grid((v) => v.columns === 80, "auto history");
  key("ctrl+shift+F9"); // NEVER retains history and releases width.
  await grid((v) => v.columns === expanded, "never with history");
  key("ctrl+shift+F9");
  await grid((v) => v.columns === 80, "always restored");
  key("ctrl+shift+F9"); // AUTO with history.
  await query("a");
  await grid((v) => v.columns === expanded, "auto alternate");
  await query("p");
  await grid((v) => v.columns === 80, "auto primary history");
  await query("c");
  await grid((v) => v.columns === expanded, "auto history cleared");
  key("ctrl+shift+F9"); // NEVER.
  const originalHeight = last.ch;
  key("ctrl+plus");
  await grid(
    (v) => v.ch > originalHeight && v.columns === expanded && v.rows === 24,
    "hidden zoom content-size request",
  );
  key("ctrl+0");
  await grid(
    (v) => v.ch === originalHeight && v.columns === expanded && v.rows === 24,
    "hidden zoom reset",
  );
  key("ctrl+shift+F9");
  const restoredColumns = (
    await grid((v) => v.columns < expanded, "always after zoom")
  ).columns;
  key("ctrl+plus");
  await grid(
    (v) =>
      v.ch > originalHeight && v.columns === restoredColumns && v.rows === 24,
    "visible zoom content-size request",
  );
  key("ctrl+0");
  await grid(
    (v) =>
      v.ch === originalHeight && v.columns === restoredColumns && v.rows === 24,
    "visible zoom reset",
  );
  if (!wayland && /\/x11\//.test(resolve(sample))) {
    const baseWidth = () => {
      const hints = command("xprop", ["-id", id, "WM_NORMAL_HINTS"]);
      const match = /base size: (\d+) by/.exec(hints);
      assert(match, hints);
      return Number(match[1]);
    };
    const reserved = baseWidth();
    assert(reserved > 0);
    while (last.cw <= reserved) {
      const before = { ...last };
      key("ctrl+plus");
      await grid((v) => v.ch > before.ch || v.cw > before.cw, "large cells");
    }
    await query("c");
    const columns = last.columns;
    key("ctrl+shift+F9");
    await grid((v) => v.columns === columns, "hide without changing grid");
    assert.equal(
      baseWidth(),
      0,
      "hidden bar also updates WM resize hints without a grid resize",
    );
    key("ctrl+shift+F9"); // NEVER.
    key("ctrl+shift+F9"); // ALWAYS.
    await query();
    assert.equal(baseWidth(), reserved, "restored bar updates WM resize hints");
  }
  key("q");
  await wait(
    () => child.exitCode,
    (v) => v !== null,
    "clean exit",
  );
  assert.equal(child.exitCode, 0, diagnostics);
  console.log(
    "PASS native scrollbar policy, PTY geometry, history, alternate and content resize",
  );
} finally {
  if (child?.exitCode === null) child.kill("SIGTERM");
  if (weston) weston.kill("SIGTERM");
  if (runtime) rmSync(runtime, { recursive: true, force: true });
  if (diagnostics) console.log(diagnostics);
}
