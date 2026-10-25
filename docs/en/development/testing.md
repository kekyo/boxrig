# Testing during development

[日本語](../../ja/development/testing.md)

This guide explains how to check parsing, display, input, and SDK usage after changing boxrig code or public examples.
The tests cover core features, UI display and interaction, and integration into the terminal implementation examples.
For getting started with an application that uses the library, see the [build guide](../building.md).

## Prepare a test environment

Prepare a C99 GCC compiler, Make, Linux GUI development packages, MinGW amd64/i686, Wine, Node.js, and npm.
GUI automation also requires the runtime appropriate to each target, such as Xvfb, Weston, and IBus.
Building the GTK AT-SPI tests requires development packages that make `atspi-2` and `gobject-2.0` available through `pkg-config`. Running the AT-SPI and Orca tests also requires the D-Bus accessibility service, Xvfb, Orca, and `xdotool`.
On Ubuntu 24.04, install `libatspi2.0-dev`, `libglib2.0-dev`, `at-spi2-core`, `dbus-daemon`, `orca`, `xvfb`, `xauth`, `xdotool`, `util-linux`, and `bsdutils`. `dbus-daemon` provides `dbus-run-session`, `util-linux` provides `setsid`, and `bsdutils` provides `script`, used to launch Orca in a terminal.
See [Building and installing](../building.md) for build dependencies, and the [code generation package.json](../../../tools/codegen/package.json) and [Win32 P2 package.json](../../../tests/p2/win32/package.json) for npm dependencies.

```sh
npm ci --prefix tools/codegen
npm ci --prefix tests/p2/win32
```

If packages are missing, record their names and the tests that cannot run, then install them.
An unavailable environment does not count as a passing test.

## Run the full suite

Run from the repository root.

```sh
make -j1 test
```

This takes time because it includes core, UI, rendering, platform components, SDKs, public examples, and code generation tools.
Native targets also use ASan/UBSan, and some Win32 targets test both ABIs through Wine.
Start with sequential execution to avoid shared GUI and Wine state.

Run physical Windows P2 tests separately with `make test-p2-win32` after [preparing the machine](#prepare-a-physical-windows-machine).

In an isolated environment without an accessibility bus, set `NO_AT_BRIDGE=1 GTK_A11Y=none` only for GTK tests that do not require an assistive-technology connection.
`test-gtk3-atspi`, `test-gtk4-atspi`, `test-gtk3-orca`, and `test-gtk4-orca` clear these variables before startup and connect to the D-Bus accessibility service.

Run the following commands for AT-SPI alone. The Orca tests additionally verify that the screen reader generates speech from the terminal text.

```sh
make test-gtk3-atspi test-gtk4-atspi
make test-gtk3-orca test-gtk4-orca
```

## Run tests by feature

During a fix, reproduce the problem with related tests and check the same conditions after the change.
Run the full suite at the end.

| Area | Main targets |
| --- | --- |
| Newlines, keys, encodings | `test-terminal-newline`, `test-terminal-key-mapping`, `test-terminal-codec` |
| Blinking and decorations | `test-text-blink`, `test-cursor-style`, `test-selection-style` |
| Backgrounds and notifications | `test-background-layers`, `test-notification-dispatch`, `test-notification-samples` |
| GTK and IME | `test-ime-clause`, `test-gtk-ime-routing`, `test-gtk-ibus-direct` |
| Fcitx 5 | `test-fcitx5`, `test-fcitx5-panels`, `test-fcitx5-auto` |
| Fcitx 4 | `test-fcitx4`, `test-fcitx4-panels-core`, `test-fcitx4-panels-gtk`, `test-fcitx4-discovery` |
| Wayland input | `test-wayland-input`, `test-wayland-candidates-startup` |
| Build-time code generation | `test-codegen` |
| Win32 first-frame synchronization | `test-win32-first-frame-fence`, `test-win32-sample-first-frame-fence` |

The [Makefile](../../../Makefile) defines target dependencies and execution conditions.
Distinguish targets that only compile an executable from targets that run it.

Real-daemon tests use Mozc with private D-Bus, display servers, and configuration directories.
Separate Podman environments for 4 and 5 test the packaged versions from Debian 13, Ubuntu 24.04, and Ubuntu 26.04.
They do not share the host's IME settings or user dictionary.

```sh
bash tests/ime/run_distribution.sh debian13 fcitx5
bash tests/ime/run_distribution.sh debian13 fcitx4
```

The first argument also accepts `ubuntu2404` and `ubuntu2604`.
Results under `build/test-results/ime-distributions/` include package versions, input-test logs, and images and videos of the sample terminal.
Generation 4 is tested with a real 4.2.9.9 daemon; generation 5 tests also cover its 4 compatibility service and AUTO selection.

## Check SDK and documentation examples

The following tests build examples from installed SDKs and extract and run step-by-step code.
They check that the complete examples corresponding to short README excerpts work with public headers and libraries alone.

```sh
make test-gtk3-package test-gtk4-package test-wayland-package
make test-opengl-packages test-win32-sdk
make test-simple-ui-guide
```

SDK tests build examples from public headers, libraries, and pkg-config files installed in a temporary prefix.
Success that accidentally depends on visible private source-tree headers is insufficient.

For the UI guide, the existing extractor takes code from Markdown, followed by compilation and display/input checks.
The [extractor](../../../tests/test_ui_guide.c) and shared [guide Makefile](../../ja/step-by-step/Makefile) are the integration points.
Even for documentation-only changes, check this connection if extracted code or build inputs change.

## Check GUI, video, and physical machines

Screen tests actually render and check expected characters, colors, images, selections, and coordinates.
When adding or changing a reference image, inspect it visually to confirm that it represents the intended result.

Blinking, IME commits and cancellation, and candidate movement also require temporal checks.
Existing video tests verify frame order and changes.
A single still image is not evidence of an absence of flicker or duplicate commits.

## Prepare a physical Windows machine

Prepare an amd64 Windows 11 PC with the GPU and monitor used for testing.
For display and input tests, sign in to the local console and unlock the desktop.
Run the agent on that interactive desktop.
Do not use a Windows service or an RDP-only session to verify the physical display.

Use the same agent-rover version on the controller and the physical machine.
The current pinned version is 0.10.0 in the [Win32 P2 package.json](../../../tests/p2/win32/package.json), and the default connection section is `win11`.
Configure the endpoint in an untracked `.agent-rover` file.

```ini
[win11]
windows-test.example.invalid=<access-token>
```

The SDK searches the working directory first, then the home directory if the file or requested section is absent.
P2 tests run through npm have `tests/p2/win32/` as their working directory.
See the [official agent-rover configuration guide](https://github.com/kekyo/agent-rover/blob/0.10.0/README_ja.md#接続設定ファイル) for details.
The default port is 39397, and protocol messages are not encrypted.
Use an environment consistent with the [upstream transport requirements](https://github.com/kekyo/agent-rover/blob/0.10.0/README_ja.md#特徴), such as an access-controlled LAN or VPN, and keep tokens out of logs and screen recordings.

Record the OS build, GPU and driver, display scaling, font, and IME, and check that the target window is in the foreground and receives input.
After preparation, run from the repository root.

```sh
make test-p2-win32
```

Wait for a connection if the physical machine is in use.
Once the tests finish, clean up owned processes and temporary files, restore changed screen, focus, pointer, and settings state, and release the connection.
Record any checks prevented by disconnection or desktop locking, along with their reasons.
Record Wine, real-Windows, and physical-GPU results separately.

## Record failures and untested conditions

Record the target commit, command, environment, exit code, logs, PASS/FAIL/SKIP, and reasons.
Keep both an initial failure and a successful rerun; a rerun alone does not establish that instability is resolved.

Investigate compiler warnings, runtime environment warnings, and expected fault-injection diagnostics separately.
Determine whether a failure comes from the planned change, existing implementation, or environment, and revisit the plan when needed.
Do not infer success with unconnected external services or unused OS versions from tests using simulated environments.

[Documentation index](../index.md)
