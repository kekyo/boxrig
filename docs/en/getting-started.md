# Getting started

[日本語](../ja/getting-started.md)

Try displaying a shell with boxrig UI and reading characters and colors with boxrig core.
First look at the [features and screenshots in the README](../../README.md#features) and choose the configuration closest to your application.
This guide assumes that you can build a C99 program.

## Choose what to build

Start with boxrig UI if you want to embed a terminal in a window.
Its components provide fonts and input handling for X11, GTK 3/4, native Wayland, and Win32 applications.

Use boxrig core for a custom rendering environment or analysis without a display.
It interprets bytes sent to a terminal and exposes characters, colors, cursor state, and other information.

| Configuration | Result to check on this page | Implementation guide |
| --- | --- | --- |
| boxrig UI | Interact with a shell in an X11 window | [UI step-by-step](./step-by-step/step-by-step-ui.md) |
| boxrig core | Print `H color=1` without opening a window | [Core step-by-step](./step-by-step/step-by-step-core.md) |

Interacting with a shell requires a transport such as a PTY as well as a display.
Creating core and starting a child process are separate operations.
See also [the roles of the components](./architecture.md).

## Prepare the environment and SDK

To work from the source tree, install Git, GCC, GNU Make, and pkg-config, then obtain the dependency sources as described in [Building and installing](./building.md).
Run the following commands from the repository root.

```sh
git submodule update --init
make build/libboxrig.a
```

This builds the core static library.
Terminal parsing does not require linking X11 or GTK.
However, the current shared Makefile queries UI dependencies through pkg-config when it reads its configuration.
If it reports missing dependencies, consult the [build environment table](./building.md#obtain-sources-and-dependencies).

If you already have an installed SDK, you can use its public headers and libraries.
See the [installation table](./building.md#install-the-modules-you-need) for available installation targets and pkg-config names.
To try an SDK without the source tree, use the [example in the SDK README](../../README_pack.md#your-first-example).
The following sections use libraries and examples from the source tree.

## Run a minimal example

### Display a shell with UI

On a system with X11, these commands open a terminal window in which you can interact with a shell.
This executable combines all stages of the UI step-by-step guide.

```sh
make -C docs/ja/step-by-step
./docs/ja/step-by-step/simple_ui_term
```

`simple_ui_term` combines boxrig UI display and input with shell startup and PTY communication.
To begin with a program that displays fixed text, go to [Implementing the first version](./step-by-step/step-by-step-ui.md#implementing-the-first-version).
For other platforms, choose an option under [Starting boxrig_term on each platform](./boxrig_term.md#start-on-each-platform).

### Read characters and colors with core

This example interprets terminal output containing a color setting without opening a display.
The [core example in the README](../../README.md#interpret-terminal-output-without-a-display) is an excerpt from a function body.
Create `hello_core.c` with `#include <boxrig/terminal.h>` and `#include <stdio.h>`, and place the example inside the body of `int main(void) { ... }`.
Build it using `include/` and `build/libboxrig.a` as follows.

```sh
cc -std=c99 -Wall -Wextra -Wpedantic -Werror \
  -Iinclude hello_core.c build/libboxrig.a -o hello_core
./hello_core
```

The output is:

```text
H color=1
```

The example feeds `Hello` with a red foreground setting into the terminal and reads the top-left cell.
The control sequence itself does not appear in the cell: the result is the character `H` and palette index `1`.

## Check the results

The core example returns exit code 0 on success.
It checks the character and color index it reads, so compilation alone is not its success condition.

If the UI example cannot open a window, check that an X11 connection and the required fonts are available.
You can also use XWayland on Wayland, but this differs from native Wayland integration.
For a platform other than X11, select the frontend described in [Running the bundled demo](./boxrig_term.md).

## Choose the next guide

| Next task | Document |
| --- | --- |
| Build a small terminal incrementally | [UI step-by-step](./step-by-step/step-by-step-ui.md) |
| Draw cells yourself | [Core step-by-step](./step-by-step/step-by-step-core.md) |
| Learn which operations your application implements | [Basic concepts](./architecture.md) |
| Change the build configuration or installation prefix | [Building and installing](./building.md) |
| Use a particular feature | [Documentation index](./index.md) |

[Back to the documentation index](./index.md)
