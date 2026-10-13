# boxrig SDK

## What this SDK provides

boxrig is a C99 library for embedding terminal functionality in applications.
boxrig UI adds text, images, selections, IME, and other terminal features to existing windows.
See the online [README features and screenshots](https://github.com/kekyo/boxrig/blob/main/README.md#features) for examples of the display.

boxrig core interprets terminal output and manages characters, attributes, screen contents, and history.
It is independent of window systems, so you can use it for custom rendering or text retrieval and search without a display.
boxrig UI adds rendering and input handling to core.
Your application connects the parent window, event loop, and shell communication.

The modules included in an SDK depend on the configuration selected during installation.
See [Library structure](https://github.com/kekyo/boxrig/blob/main/README.md#library-structure) to choose the UI modules you need.

## Your first example

This program uses boxrig core to interpret `Hello` and print the `H` read from the first cell.
It needs no window or shell.
Save the complete code as `hello.c`.

<!-- ui-guide-source: hello.c -->
```c
#include <boxrig/terminal.h>
#include <stdio.h>

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *terminal = NULL;
    const uint8_t text[] = "Hello";
    size_t consumed = 0;
    boxrig_cell_view cell;
    int result = 1;

    if (boxrig_terminal_create(&config, &terminal) != BOXRIG_STATUS_OK)
        return 1;
    if (boxrig_terminal_feed(terminal, text, sizeof(text) - 1, &consumed)
            != BOXRIG_STATUS_OK || consumed != sizeof(text) - 1 ||
        boxrig_terminal_get_cell(terminal, 0, 0, &cell) != BOXRIG_STATUS_OK)
        goto cleanup;
    printf("%.*s\n", (int)cell.text_length, cell.text);
    result = cell.text_length == 1 && cell.text[0] == 'H' ? 0 : 1;
cleanup:
    boxrig_terminal_destroy(terminal);
    return result;
}
```

Link with a compiler and ABI matching the SDK.
For example, to use an amd64 Windows core SDK from Linux, replace `/path/to/sdk` below with the SDK's absolute path.

```sh
export PKG_CONFIG_LIBDIR=/path/to/sdk/lib/pkgconfig
x86_64-w64-mingw32-gcc -std=c99 hello.c -o hello.exe \
    $(pkg-config --cflags --static --libs boxrig)
```

Running `hello.exe` on Windows prints `H` followed by a newline and exits successfully.
For complete UI examples, use the web guides below and the repository's [examples/](https://github.com/kekyo/boxrig/tree/main/examples) directory.

## Header and library locations

| Location in the SDK | Contents |
| --- | --- |
| `include/boxrig/` | Public headers and API comments |
| `lib/`, `lib/pkgconfig/` | Libraries and link requirements |
| `share/licenses/boxrig/` | Original licenses and configuration-specific notes |

The `.pc` files derive their prefix from their location inside the SDK.
Preserve the directory structure when moving an SDK, and do not mix SDKs with different ABIs.

## Files required for execution and distribution

Static libraries are the default, but the selected UI's system shared libraries must also be available at runtime.
When using a DLL for Windows OpenGL, distribute that DLL as well.
The standard configuration uses OS-supplied ConPTY and does not require bundling Microsoft executables.
If you explicitly choose bundled ConPTY, include the files matching the ABI and their license documents.

See [Distribution](https://github.com/kekyo/boxrig/blob/main/docs/en/reference/distribution.md) and [Platform support](https://github.com/kekyo/boxrig/blob/main/docs/en/reference/platform-support.md) online for details.
The launcher specific to the bundled terminal is not required by every application using the SDK.

## Detailed guides

The [documentation index](https://github.com/kekyo/boxrig/blob/main/docs/en/index.md), [UI step-by-step](https://github.com/kekyo/boxrig/blob/main/docs/en/step-by-step/step-by-step-ui.md), and [core step-by-step](https://github.com/kekyo/boxrig/blob/main/docs/en/step-by-step/step-by-step-core.md) are web documents in the repository.
They do not assume that the SDK contains a `docs/` directory.
Use documentation for the same source version as your SDK.

## License

boxrig uses the MIT License.
For third-party code and data, consult the original licenses and notes in the SDK's `share/licenses/boxrig/` directory.
The repository also contains [license documentation](https://github.com/kekyo/boxrig/blob/main/packaging/licenses/README.md) and records of where dependencies were obtained.
