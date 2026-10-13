#!/bin/sh
# Compile consumers from clean, relocated installations without source-tree headers.
set -eu
architecture=$1
compiler=$2
objdump=$3
repository=$(pwd)
root="$repository/build/tests/win32-sdk-$architecture"
rm -rf "$root"
mkdir -p "$root"

make "install-core-win32-$architecture" PREFIX="$root/core-original/usr"
mv "$root/core-original" "$root/core"
core="$root/core/usr"
cp tests/win32/test_core_package.c "$root/core/consumer.c"
cd "$root/core"
export PKG_CONFIG_LIBDIR="$core/lib/pkgconfig"
unset PKG_CONFIG_PATH
for mode in ordinary static; do
    option=
    if [ "$mode" = static ]; then option=--static; fi
    "$compiler" -std=c99 -Wall -Wextra -Wpedantic -Werror consumer.c \
        $(pkg-config --cflags --libs $option boxrig) -o "consumer-$mode.exe"
    "$objdump" -p "consumer-$mode.exe" > "consumer-$mode.imports.txt"
    if rg -i 'DLL Name:.*(user32|gdi32|ole32|opengl|dxgi|d3d|d2d|dcomp|ddraw|conpty)' "consumer-$mode.imports.txt"; then
        echo "FAIL core-only consumer acquired graphics imports" >&2
        exit 1
    fi
done
test ! -e "$core/include/boxrig/ui.h"
test ! -e "$core/include/boxrig/win32"
cd "$repository"
unset PKG_CONFIG_LIBDIR
make "uninstall-core-win32-$architecture" PREFIX="$core"
test ! -e "$core/lib/libboxrig.a"
test ! -e "$core/include/boxrig/terminal.h"
test ! -e "$core/lib/pkgconfig/boxrig.pc"

make "install-win32-$architecture" PREFIX="$root/software-original/usr"
mv "$root/software-original" "$root/software"
software="$root/software/usr"
test ! -e "$software/include/boxrig/render"
test ! -e "$software/include/boxrig/win32/wgl.h"
test ! -e "$software/include/boxrig/win32/opengl.h"
test ! -e "$software/include/boxrig/win32/opengl_component.h"
test ! -e "$software/lib/libboxrig-render-ogl.a"
test ! -e "$software/lib/libboxrig-win32-ogl.a"
test ! -e "$software/lib/pkgconfig/boxrig-win32-ogl.pc"
test ! -e "$software/include/boxrig/win32/d3d11_component.h"
test ! -e "$software/lib/libboxrig-render-d3d11.a"
test ! -e "$software/lib/libboxrig-win32-d3d11.a"
cp tests/test_custom_ui.c "$root/software/consumer.c"
cp tests/win32/test_win32_component.c "$root/software/win32-consumer.c"
cp "$software/share/boxrig/examples/win32/component.c" "$root/software/component.c"
cp "$software/share/boxrig/examples/win32/host_opacity.c" "$root/software/host_opacity.c"
cp "$software/share/boxrig/examples/win32/pty.c" "$root/software/pty.c"
cd "$root/software"
export PKG_CONFIG_LIBDIR="$software/lib/pkgconfig"
unset PKG_CONFIG_PATH
# The static package is usable with both ordinary and --static pkg-config queries.
for mode in ordinary static; do
    option=
    if [ "$mode" = static ]; then option=--static; fi
    "$compiler" -std=c99 -Wall -Wextra -Wpedantic -Werror consumer.c \
        $(pkg-config --cflags --libs $option boxrig-win32) -o "consumer-$mode.exe"
    # The public PTY header does not require Windows 10 SDK declarations.
    "$compiler" -std=c99 -Wall -Wextra -Wpedantic -Werror -D_WIN32_WINNT=0x0501 \
        -DWINVER=0x0501 pty.c $(pkg-config --cflags --libs $option boxrig-win32) \
        -o "pty-$mode.exe"
done
"$compiler" -std=c99 -Wall -Wextra -Wpedantic -Werror -DUNICODE -D_UNICODE \
    win32-consumer.c $(pkg-config --cflags --libs boxrig-win32) \
    -Wl,--wrap=boxrig_win32_input_adapter_resynchronize_candidate -o win32-consumer.exe
"$compiler" -std=c99 -Wall -Wextra -Wpedantic -Werror -DBOXRIG_SOFTWARE_ONLY \
    component.c $(pkg-config --cflags --libs boxrig-win32) -o component-software.exe
"$compiler" -std=c99 -Wall -Wextra -Wpedantic -Werror \
    host_opacity.c $(pkg-config --cflags --libs boxrig-win32) -o host-opacity.exe
for executable in ./*.exe; do
    "$objdump" -p "$executable" > "$executable.imports.txt"
    if rg -i 'DLL Name:.*(opengl|boxrig-.*ogl|conpty|pango|cairo|ddraw|dxgi|d3d11|d3d12|d2d1|dcomp)' "$executable.imports.txt"; then
        echo "FAIL software-only consumer acquired optional imports" >&2
        exit 1
    fi
    if rg ' (CreatePseudoConsole|ResizePseudoConsole|ClosePseudoConsole)$' "$executable.imports.txt"; then
        echo "FAIL software SDK consumer statically imports ConPTY entry points" >&2
        exit 1
    fi
done
cd "$repository"
unset PKG_CONFIG_LIBDIR
make "install-win32-opengl-$architecture" PREFIX="$root/ogl-original/usr"
mv "$root/ogl-original" "$root/ogl"
ogl="$root/ogl/usr"
cp "$ogl/share/boxrig/examples/win32/renderer.c" "$root/ogl/renderer.c"
cp "$ogl/share/boxrig/examples/win32/component.c" "$root/ogl/component.c"
cp "$ogl/bin/"*.dll "$root/ogl/"
cd "$root/ogl"
export PKG_CONFIG_LIBDIR="$ogl/lib/pkgconfig"
for source in renderer component; do
    "$compiler" -std=c99 -Wall -Wextra -Wpedantic -Werror "$source.c" \
        $(pkg-config --cflags boxrig-win32-ogl) -Wl,-Bstatic \
        $(pkg-config --libs --static boxrig-win32-ogl) -Wl,-Bdynamic \
        -o "$source-static.exe"
    "$compiler" -std=c99 -Wall -Wextra -Wpedantic -Werror "$source.c" \
        $(pkg-config --cflags --libs boxrig-win32-ogl) -o "$source-shared.exe"
    # Inspect resolved public dependencies, not spelling in source files.
    "$compiler" -M "$source.c" $(pkg-config --cflags boxrig-win32-ogl) > "$source.headers.txt"
    if rg -i '[/\\]ddraw\.h' "$source.headers.txt"; then
        echo "FAIL public SDK requires DirectDraw headers" >&2
        exit 1
    fi
done
for artifact in ./*.exe ./*.dll; do
    "$objdump" -p "$artifact" > "$artifact.imports.txt"
    if rg -i 'DLL Name:.*ddraw|DirectDrawCreate' "$artifact.imports.txt"; then
        echo "FAIL SDK acquired DirectDraw imports" >&2
        exit 1
    fi
done
echo "PASS relocated $architecture software-only and optional OpenGL SDK consumers"

cd "$repository"
unset PKG_CONFIG_LIBDIR
for package in render-d3d11 win32-d3d11; do
    make "install-$package-$architecture" PREFIX="$root/$package-original/usr"
    mv "$root/$package-original" "$root/$package"
    sdk="$root/$package/usr"
    cp tests/win32/test_d3d11_package.c "$root/$package/consumer.c"
    cd "$root/$package"
    export PKG_CONFIG_LIBDIR="$sdk/lib/pkgconfig"
    define=
    if [ "$package" = win32-d3d11 ]; then
        define=-DBOXRIG_TEST_MANAGED_PACKAGE
    else
        test ! -e "$sdk/include/boxrig/win32/d3d11_component.h"
        test ! -e "$sdk/lib/libboxrig-win32-d3d11.a"
    fi
    for mode in ordinary static; do
        option=
        if [ "$mode" = static ]; then option=--static; fi
        "$compiler" -std=c99 -Wall -Wextra -Wpedantic -Werror $define consumer.c \
            $(pkg-config --cflags --libs $option "boxrig-$package") -o "consumer-$mode.exe"
        "$objdump" -p "consumer-$mode.exe" > "consumer-$mode.imports.txt"
        if rg -i 'DLL Name:.*(ddraw|d3d12|opengl|conpty)' "consumer-$mode.imports.txt"; then
            echo "FAIL D3D11 consumer acquired unrelated imports" >&2
            exit 1
        fi
        if rg -i 'DLL Name:.*(d3d11|d2d1|dcomp)' "consumer-$mode.imports.txt"; then
            echo "FAIL optional D3D11 backend made a GPU DLL mandatory at process load" >&2
            exit 1
        fi
    done
    example=d3d11_renderer
    # This caller-owned-device example explicitly calls D3D11CreateDevice.
    example_libraries=-ld3d11
    if [ "$package" = win32-d3d11 ]; then example=d3d11_component; example_libraries=; fi
    cp "$sdk/share/boxrig/examples/win32/$example.c" "$example.c"
    for mode in ordinary static; do
        option=
        if [ "$mode" = static ]; then option=--static; fi
        "$compiler" -std=c99 -Wall -Wextra -Wpedantic -Werror "$example.c" \
            $(pkg-config --cflags --libs $option "boxrig-$package") $example_libraries -o "example-$mode.exe"
        "$objdump" -p "example-$mode.exe" > "example-$mode.imports.txt"
        if rg -i 'DLL Name:.*(ddraw|d3d12|opengl|conpty)' "example-$mode.imports.txt"; then
            echo "FAIL installed example acquired unrelated imports" >&2
            exit 1
        fi
        if [ "$package" = render-d3d11 ] && rg -i 'DLL Name:.*dcomp' "example-$mode.imports.txt"; then
            echo "FAIL offscreen example acquired presentation imports" >&2
            exit 1
        fi
    done
    cd "$repository"
    unset PKG_CONFIG_LIBDIR
    make "uninstall-$package-$architecture" PREFIX="$sdk"
    test ! -e "$sdk/lib/pkgconfig/boxrig-$package.pc"
    test ! -e "$sdk/lib/libboxrig-$package.a"
done
echo "PASS relocated $architecture optional D3D11 SDK consumers and removal"

make "uninstall-win32-opengl-$architecture" PREFIX="$ogl"
test ! -e "$ogl/lib/libboxrig-render-ogl.a"
test ! -e "$ogl/lib/libboxrig-win32-ogl.a"
test ! -e "$ogl/bin/boxrig-render-ogl.dll"
test ! -e "$ogl/bin/boxrig-win32-ogl.dll"
test ! -e "$ogl/include/boxrig/win32/opengl_component.h"
test -e "$ogl/lib/libboxrig-win32.a"
make "uninstall-win32-$architecture" PREFIX="$software"
test ! -e "$software/lib/libboxrig-win32.a"
test ! -e "$software/include/boxrig/win32/component.h"
test ! -e "$software/lib/pkgconfig/boxrig-win32.pc"
test ! -e "$software/share/boxrig/examples/win32/pty.c"
echo "PASS $architecture core-only SDK, installed examples and independent backend removal"
