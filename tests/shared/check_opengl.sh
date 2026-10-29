#!/bin/bash
set -euo pipefail
make_command=${1:-make}
repository=$(pwd)
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
"$make_command" -j8 install-shared-opengl BUILD_DIR="$root/build" \
    DESTDIR="$root/stage" PREFIX=/opt/boxrig LIBDIR=/opt/boxrig/lib64 \
    PKGCONFIGDIR=/opt/boxrig/lib64/pkgconfig
mv "$root/stage/opt/boxrig" "$root/relocated"
sdk="$root/relocated"
export PKG_CONFIG_PATH="$sdk/lib64/pkgconfig"
mkdir -p "$sdk/bin"
for library in render-ogl x11-ogl wayland-ogl; do
    test -f "$sdk/lib64/libboxrig-$library.so.0"
    test -L "$sdk/lib64/libboxrig-$library.so"
    readelf -d "$sdk/lib64/libboxrig-$library.so.0" > "$root/$library-dynamic"
    rg -q "SONAME.*\[libboxrig-$library.so.0\]" "$root/$library-dynamic"
    if rg 'TEXTREL|/tmp/|/home/' "$root/$library-dynamic"; then exit 1; fi
    if nm -D --defined-only "$sdk/lib64/libboxrig-$library.so.0" | \
        rg ' [A-Z] boxrig_(ui_|terminal_)'; then
        echo "FAIL portable core or UI definitions copied into $library" >&2
        exit 1
    fi
done
rg -q 'NEEDED.*\[libboxrig-ui.so.0\]' "$root/render-ogl-dynamic"
rg -q 'NEEDED.*\[libboxrig-x11.so.0\]' "$root/x11-ogl-dynamic"
rg -q 'NEEDED.*\[libboxrig-render-ogl.so.0\]' "$root/x11-ogl-dynamic"
rg -q 'NEEDED.*\[libboxrig-wayland.so.0\]' "$root/wayland-ogl-dynamic"
rg -q 'NEEDED.*\[libboxrig-render-ogl.so.0\]' "$root/wayland-ogl-dynamic"
for library in x11-ogl wayland-ogl; do
    source="$repository/tests/x11/test_x11_opengl_package.c"
    if [ "$library" = wayland-ogl ]; then
        source="$repository/tests/wayland/test_wayland_opengl_package.c"
    fi
    for language in c c++; do
        compiler=cc
        standard=c99
        if [ "$language" = c++ ]; then compiler=c++; standard=c++20; fi
        "$compiler" -x "$language" -std="$standard" -Wall -Wextra -Wpedantic -Werror \
            "$source" $(pkg-config --cflags --libs "boxrig-$library") \
            '-Wl,-rpath,$ORIGIN/../lib64' -o "$sdk/bin/$library-$language"
        env -u LD_LIBRARY_PATH LD_BIND_NOW=1 "$sdk/bin/$library-$language"
        readelf -d "$sdk/bin/$library-$language" > "$root/$library-$language-consumer-dynamic"
        rg -q "NEEDED.*\[libboxrig-$library.so.0\]" "$root/$library-$language-consumer-dynamic"
    done
done
cc -std=c99 -Wall -Wextra -Wpedantic -Werror \
    "$repository/examples/opengl_renderer/main.c" \
    $(pkg-config --cflags --libs boxrig-render-ogl egl gl) \
    '-Wl,-rpath,$ORIGIN/../lib64' -o "$sdk/bin/renderer"
env -u LD_LIBRARY_PATH LD_BIND_NOW=1 LIBGL_ALWAYS_SOFTWARE=1 \
    xvfb-run -a "$sdk/bin/renderer"
readelf -d "$sdk/bin/renderer" > "$root/renderer-consumer-dynamic"
rg -q 'NEEDED.*\[libboxrig-render-ogl.so.0\]' "$root/renderer-consumer-dynamic"
font_file=$(fc-match -f '%{file}\n' 'DejaVu Sans Mono' | head -n 1)
test -f "$font_file"
cc -std=c99 -Wall -Wextra -Wpedantic -Werror \
    -DBOXRIG_TEST_X11_OPENGL=1 \
    "$repository/tests/x11/test_x11_component_fixture.c" \
    "$repository/tests/frontend_component_fixture.c" \
    $(pkg-config --cflags --libs boxrig-x11-ogl cairo fontconfig) \
    '-Wl,-rpath,$ORIGIN/../lib64' -o "$sdk/bin/x11-opengl-component"
env -u LD_LIBRARY_PATH LD_BIND_NOW=1 LIBGL_ALWAYS_SOFTWARE=1 \
    xvfb-run -a "$sdk/bin/x11-opengl-component" "$font_file"
readelf -d "$sdk/bin/x11-opengl-component" > "$root/x11-component-dynamic"
rg -q 'NEEDED.*\[libboxrig-x11-ogl.so.0\]' "$root/x11-component-dynamic"
protocols="$root/protocols"
mkdir -p "$protocols"
xdg_shell="$repository/deps/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
wayland-scanner client-header "$xdg_shell" "$protocols/xdg-shell-client-protocol.h"
wayland-scanner private-code "$xdg_shell" "$protocols/xdg-shell-protocol.c"
cc -std=c99 -Wall -Wextra -Wpedantic -Werror -I"$protocols" \
    "$repository/tests/shared/consumer_wayland_opengl.c" \
    "$protocols/xdg-shell-protocol.c" \
    $(pkg-config --cflags --libs boxrig-wayland-ogl wayland-client egl gl) \
    '-Wl,-rpath,$ORIGIN/../lib64' -o "$sdk/bin/wayland-opengl-component"
env -u LD_LIBRARY_PATH LD_BIND_NOW=1 LIBGL_ALWAYS_SOFTWARE=1 \
    bash "$repository/tests/wayland/run_weston_test.sh" \
    "${WESTON:-weston}" "$sdk/bin/wayland-opengl-component" gl
readelf -d "$sdk/bin/wayland-opengl-component" > "$root/wayland-component-dynamic"
rg -q 'NEEDED.*\[libboxrig-wayland-ogl.so.0\]' "$root/wayland-component-dynamic"
find "$sdk/lib64" -name '*.so*' -delete
for library in x11-ogl wayland-ogl; do
    source="$repository/tests/x11/test_x11_opengl_package.c"
    if [ "$library" = wayland-ogl ]; then
        source="$repository/tests/wayland/test_wayland_opengl_package.c"
    fi
    for language in c c++; do
        compiler=cc
        standard=c99
        if [ "$language" = c++ ]; then compiler=c++; standard=c++20; fi
        "$compiler" -x "$language" -std="$standard" -Wall -Wextra -Wpedantic -Werror \
            "$source" $(pkg-config --cflags --static --libs "boxrig-$library") \
            -o "$sdk/bin/static-$library-$language"
        env -u LD_LIBRARY_PATH "$sdk/bin/static-$library-$language"
    done
done
cc -std=c99 -Wall -Wextra -Wpedantic -Werror \
    "$repository/examples/opengl_renderer/main.c" \
    $(pkg-config --cflags --static --libs boxrig-render-ogl egl gl) \
    -o "$sdk/bin/static-renderer"
env -u LD_LIBRARY_PATH LIBGL_ALWAYS_SOFTWARE=1 \
    xvfb-run -a "$sdk/bin/static-renderer"
printf '%s\n' 'PASS relocated shared OpenGL SDK and static consumers'
