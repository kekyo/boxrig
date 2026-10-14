#!/bin/bash
set -euo pipefail
make_command=${1:-make}
repository=$(pwd)
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
for opengl in 1 0 1; do
    stage="$root/stage-$opengl"
    "$make_command" -j8 install-shared BUILD_DIR="$root/build" \
        GTK_OPENGL="$opengl" DESTDIR="$stage" PREFIX=/usr
    sdk="$root/relocated-$opengl"
    if [ -d "$sdk" ]; then rm -r "$sdk"; fi
    mv "$stage/usr" "$sdk"
    export PKG_CONFIG_PATH="$sdk/lib/pkgconfig"
    mkdir -p "$sdk/bin"
    cc -std=c99 -O2 -Wall -Wextra -Werror \
        "$repository/tests/gtk/test_png_isolation.c" \
        $(pkg-config --cflags --libs boxrig gdk-pixbuf-2.0 gio-2.0) \
        '-Wl,-rpath,$ORIGIN/../lib' -o "$sdk/bin/png-isolation"
    env -u LD_LIBRARY_PATH LD_BIND_NOW=1 "$sdk/bin/png-isolation"
    for gtk in 3 4; do
        cc -std=c99 -O2 -Wall -Wextra -Werror -DBOXRIG_TEST_SHARED \
            "$repository/tests/gtk/test_installed_rendering.c" \
            $(pkg-config --cflags --libs "boxrig-gtk$gtk") -ldl \
            '-Wl,-rpath,$ORIGIN/../lib' -o "$sdk/bin/render-gtk$gtk"
        c++ -x c++ -std=c++20 -Wall -Wextra -Werror \
            "$repository/tests/gtk$gtk/test_gtk${gtk}_package.c" \
            $(pkg-config --cflags --libs "boxrig-gtk$gtk") \
            '-Wl,-rpath,$ORIGIN/../lib' -o "$sdk/bin/header-gtk$gtk"
        env -u LD_LIBRARY_PATH LD_BIND_NOW=1 "$sdk/bin/header-gtk$gtk"
        for policy in 0 1 2; do
            for fault in 0 1; do
                env -u LD_LIBRARY_PATH LD_BIND_NOW=1 NO_AT_BRIDGE=1 GTK_A11Y=none \
                    GIO_USE_VFS=local GSETTINGS_BACKEND=memory GDK_BACKEND=x11 LIBGL_ALWAYS_SOFTWARE=1 \
                    xvfb-run -a dbus-run-session -- "$sdk/bin/render-gtk$gtk" "$opengl" "$policy" "$fault"
            done
        done
    done
    find "$sdk/lib" -name '*.so*' -delete
    for gtk in 3 4; do
        cc -std=c99 -O2 -Wall -Wextra -Werror \
            "$repository/tests/gtk/test_installed_rendering.c" \
            $(pkg-config --cflags --static --libs "boxrig-gtk$gtk") \
            -Wl,--wrap=gtk_gl_area_new -o "$sdk/bin/static-gtk$gtk"
        env -u LD_LIBRARY_PATH NO_AT_BRIDGE=1 GTK_A11Y=none GIO_USE_VFS=local \
            GSETTINGS_BACKEND=memory GDK_BACKEND=x11 LIBGL_ALWAYS_SOFTWARE=1 \
            xvfb-run -a dbus-run-session -- "$sdk/bin/static-gtk$gtk" "$opengl" "$opengl" 0
    done
done
printf '%s\n' 'PASS relocated GTK shared SDKs, Cairo/OpenGL policy and build configuration changes'
