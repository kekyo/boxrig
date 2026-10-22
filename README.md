# boxrig

A C99 library for embedding terminal functionality in your applications.

![boxrig](./images/boxrig-128.png)

[![Project Status: WIP – Initial development is in progress, but there has not yet been a stable, usable release suitable for the public.](https://www.repostatus.org/badges/latest/wip.svg)](https://www.repostatus.org/#wip)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

---

[(For Japanese language/日本語はこちら)](./README_ja.md)

> Please note that this English version of the document was machine-translated and then partially edited, so it may contain inaccuracies.
> We welcome pull requests to correct any errors in the text.

## What is boxrig?

boxrig is a general-purpose library for building terminal applications that display shell and command output and accept keyboard and mouse input.

It handles the features needed to implement a terminal, including text decoration, scrolling, images, IME, and selections.
In addition to shell integration, you can use boxrig to build terminals connected to remote servers and to implement text terminal emulators.

Think of the terminal functionality in `xterm`, made reusable, extended with modern escape sequences and terminal UI elements, and exposed through APIs that work across different environments.

The public API is C99 and can be called from C++ or through an FFI.

## Features

- Multilingual display with UTF-8, combining characters, emoji, bidirectional text, and fallback fonts
- 16-color, 256-color, and RGB output; text decorations, cursors, scrolling, and screen switching
- In-terminal images using Kitty graphics
- Keyboard and mouse handling, and pasting
- Text selection and copying, searching, occurrence monitoring, and decorations for recognition results
- Text input and preedit display with IBus, fcitx 5/4, and Win32 IME
- Configurable fonts, display colors, backgrounds, transparency, and scrollbars
- Support for X11, Wayland, GTK 3/4, and Windows (Win32)
- OpenGL (GL3, EGL) and Direct2D/Direct3D acceleration

The following screenshots show the `boxrig_term` terminal implementation example.
See [Compatibility](./docs/en/compatibility.md) for the support scope of each feature.

A shell displayed with boxrig:

![Shell example](./images/shell.png)

In-terminal images using Kitty graphics:

![Kitty graphics images](./images/kitty-image-static.png)

Transparent background:

TODO: Transparent background screenshot

## Implementation examples

boxrig provides two kinds of libraries: boxrig core and boxrig UI.

- boxrig core parses and interprets bytes, managing characters, attributes, screen contents, and history.
  It does not depend on a window system or I/O devices, so you can reuse it in custom rendering environments.
  Core also provides text retrieval and search, and converts input into output sequences.
  You connect the actual rendering, device input, and communication.
- boxrig UI uses boxrig core to provide reusable rendering and input handling for particular I/O devices.
  Its APIs control X11 widgets and Win32 controls with minimal code and are designed for embedding in your application.

The following excerpts show how to use boxrig.
Both the UI and core examples are excerpts from an application.

### Add a terminal display to an application

On X11, create a terminal, a UI model, and a component with a child window, in that order.

This boxrig UI excerpt assumes that you have already prepared the X11 connection `display` and parent `window`:

```c
    // Get the default boxrig UI configuration
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_x11_terminal_component_config ui_config =
        boxrig_x11_terminal_component_config_default();

    boxrig_terminal *terminal = NULL;
    boxrig_ui_model *model = NULL;
    boxrig_x11_terminal_component *component = NULL;
    ui_config.view.blink.text_enabled = false;
    ui_config.view.blink.cursor_enabled = false;

    // Initialize boxrig UI
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(terminal, &model) != BOXRIG_UI_OK ||
        boxrig_x11_terminal_component_create_child(
            display, window, model, &ui_config, &component) != BOXRIG_UI_OK) {
        goto cleanup;
    }
```

After allocating the component inside its parent window and connecting event handling, you can display text as follows.
Feed received data into the terminal, synchronize its changes, and render.

This excerpt displays “Hello boxrig UI!” in red:

```c
    // Text containing escape sequences for red characters
    const uint8_t text[] = "\033[31mHello boxrig UI!\033[0m";

    size_t consumed = 0;
    boxrig_ui_component_frame_result frame;

    // Feed the text to boxrig and render it with boxrig UI
    if (boxrig_terminal_feed(terminal, text, sizeof(text) - 1, &consumed)
            != BOXRIG_STATUS_OK || consumed != sizeof(text) - 1 ||
        boxrig_ui_model_synchronize(model) != BOXRIG_UI_OK ||
        boxrig_x11_terminal_component_frame(component, &frame) != BOXRIG_UI_OK) {
        goto cleanup;
    }
```

During cleanup, release the objects that hold references first, then close the parent window and X11 connection.

```c
cleanup:
    // Release resources
    boxrig_x11_terminal_component_destroy(component);
    boxrig_ui_model_destroy(model);
    boxrig_terminal_destroy(terminal);
```

For the complete example, see the [UI step-by-step guide](./docs/en/step-by-step/step-by-step-ui.md#implementing-the-first-version).

### Interpret terminal output without a display

boxrig core alone can retrieve characters and attributes after interpreting control sequences.
This code feeds text with a red foreground setting and reads `H` and color index `1` from the first cell.

```c
    // Get the default boxrig configuration
    boxrig_terminal_config config = boxrig_terminal_config_default();

    // Text containing escape sequences for red characters
    const uint8_t text[] = "\033[31mHello\033[0m";

    boxrig_terminal *terminal = NULL;
    size_t consumed = 0;
    boxrig_cell_view cell;
    int result = 1;

    // Initialize boxrig core
    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK)
        return 1;

    // Feed the text and retrieve the display information
    if (boxrig_terminal_feed(terminal, text, sizeof(text) - 1, &consumed)
            != BOXRIG_STATUS_OK || consumed != sizeof(text) - 1 ||
        boxrig_terminal_get_cell(terminal, 0, 0, &cell) != BOXRIG_STATUS_OK)
        goto cleanup;

    // Display the retrieved information and check the color
    if (cell.foreground.kind != BOXRIG_COLOR_INDEXED)
        goto cleanup;
    printf("%.*s color=%u\n", (int)cell.text_length, cell.text,
        (unsigned int)cell.foreground.index);
    result = cell.text_length == 1 && cell.text[0] == 'H' &&
        cell.foreground.index == 1 ? 0 : 1;
cleanup:
    // Release resources
    boxrig_terminal_destroy(terminal);
    return result;
```

This example reads the information and prints it directly.
When using boxrig core, you implement terminal rendering yourself.
Once you know that the text should be red, as above, use your environment's drawing facilities to render it in red.

For the complete example, see [Building a terminal from core](./docs/en/step-by-step/step-by-step-core.md).

## Library structure

| Purpose | What to use |
| --- | --- |
| Embed a terminal in an existing display | boxrig UI and an X11, GTK 3/4, native Wayland, or Win32 component |
| Implement rendering yourself | boxrig core, with common UI or rendering APIs as needed |
| Communicate with a shell or another process | A session and a POSIX or Win32 PTY |
| Use OpenGL or Direct3D 11 | The corresponding optional rendering library |

Your application supplies the window, event loop, and communication peer.
See [Basic concepts](./docs/en/architecture.md) for the roles of these components.
OS requirements depend on the layers you use; the Windows PTY requires Windows 10 version 1809 or later.
Check [Compatibility](./docs/en/compatibility.md).

The files distributed with a Windows application depend on static or DLL linking and the selected ConPTY implementation.
See [Distribution](./docs/en/reference/distribution.md).

---

## Getting started

Use the [Getting started guide](./docs/en/getting-started.md) to choose an environment and configuration.
To prepare boxrig from source, continue with [Building and installing](./docs/en/building.md).

To learn by building an application, choose the guide for your purpose.

- [Embed a terminal with boxrig UI](./docs/en/step-by-step/step-by-step-ui.md)
- [Build a terminal from boxrig core](./docs/en/step-by-step/step-by-step-core.md)
- [Run the bundled terminal example](./docs/en/boxrig_term.md)
- [Try emitting control sequences](./docs/en/boxrig_emitter.md)

## Detailed guides

The [documentation index](./docs/en/index.md) covers integration, individual features, and supported specifications.
Start with the introductory guide, then move to the feature pages you need.

---

## License

boxrig is distributed under the [MIT License](./LICENSE).
Libraries you link and files you distribute have their own licenses.
See the [third-party license guide](./packaging/licenses/README.md).
