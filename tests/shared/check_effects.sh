#!/bin/bash
set -euo pipefail
make_command=${1:-make}
repository=$(pwd)
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
"$make_command" -j8 install-shared-effects BUILD_DIR="$root/build" \
    DESTDIR="$root/stage" PREFIX=/opt/boxrig LIBDIR=/opt/boxrig/lib64 \
    PKGCONFIGDIR=/opt/boxrig/lib64/pkgconfig
mv "$root/stage/opt/boxrig" "$root/relocated"
sdk="$root/relocated"
export PKG_CONFIG_PATH="$sdk/lib64/pkgconfig"
mkdir -p "$sdk/bin"
for library in notification background-wayland background-gtk3 background-gtk4; do
    test -f "$sdk/lib64/libboxrig-$library.so.0"
    test -L "$sdk/lib64/libboxrig-$library.so"
    readelf -d "$sdk/lib64/libboxrig-$library.so.0" > "$root/$library-dynamic"
    rg -q "SONAME.*\[libboxrig-$library.so.0\]" "$root/$library-dynamic"
    if rg 'TEXTREL|/tmp/|/home/' "$root/$library-dynamic"; then exit 1; fi
done
# GTK consumers use the same non-pedantic flags as the existing GTK SDK tests.
for language in c c++; do
    compiler=cc
    standard=c99
    if [ "$language" = c++ ]; then compiler=c++; standard=c++20; fi
    "$compiler" -x "$language" -std="$standard" -Wall -Wextra -Wpedantic -Werror \
        "$repository/tests/notification/test_package.c" \
        $(pkg-config --cflags --libs boxrig-notification gio-2.0) \
        '-Wl,-rpath,$ORIGIN/../lib64' -o "$sdk/bin/notification-$language"
    env -u LD_LIBRARY_PATH LD_BIND_NOW=1 "$sdk/bin/notification-$language"
    readelf -d "$sdk/bin/notification-$language" > "$root/notification-$language-dynamic"
    rg -q 'NEEDED.*\[libboxrig-notification.so.0\]' "$root/notification-$language-dynamic"
    "$compiler" -x "$language" -std="$standard" -Wall -Wextra -Wpedantic -Werror \
        "$repository/tests/wayland/test_background_effect_package.c" \
        $(pkg-config --cflags --libs boxrig-background-wayland) \
        '-Wl,-rpath,$ORIGIN/../lib64' -o "$sdk/bin/wayland-background-$language"
    env -u LD_LIBRARY_PATH LD_BIND_NOW=1 "$sdk/bin/wayland-background-$language"
    readelf -d "$sdk/bin/wayland-background-$language" > "$root/wayland-background-$language-dynamic"
    rg -q 'NEEDED.*\[libboxrig-background-wayland.so.0\]' "$root/wayland-background-$language-dynamic"
    for gtk in 3 4; do
        source="$repository/tests/gtk/test_background_effect_package.c"
        define=()
        if [ "$gtk" = 3 ]; then define=(-DBOXRIG_TEST_GTK3=1); fi
        if [ "$language" = c++ ]; then
            source="$repository/tests/shared/consumer_background_gtk.cpp"
        fi
        "$compiler" -x "$language" -std="$standard" -Wall -Wextra -Werror \
            "${define[@]}" "$source" \
            $(pkg-config --cflags --libs "boxrig-background-gtk$gtk") \
            '-Wl,-rpath,$ORIGIN/../lib64' -o "$sdk/bin/gtk$gtk-background-$language"
        env -u LD_LIBRARY_PATH LD_BIND_NOW=1 GDK_BACKEND=x11 GSK_RENDERER=cairo \
            GTK_A11Y=none NO_AT_BRIDGE=1 xvfb-run -a "$sdk/bin/gtk$gtk-background-$language"
        readelf -d "$sdk/bin/gtk$gtk-background-$language" > "$root/gtk$gtk-background-$language-dynamic"
        rg -q "NEEDED.*\[libboxrig-background-gtk$gtk.so.0\]" "$root/gtk$gtk-background-$language-dynamic"
    done
done
protocols="$root/protocols"
mkdir -p "$protocols"
background_xml="$repository/deps/wayland-protocols/staging/ext-background-effect/ext-background-effect-v1.xml"
xdg_xml="$repository/deps/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
text_input_xml="$repository/deps/wayland-protocols/unstable/text-input/text-input-unstable-v3.xml"
wayland-scanner server-header "$background_xml" "$protocols/background-effect-v1-server-protocol.h"
wayland-scanner private-code "$background_xml" "$protocols/background-effect-v1-protocol.c"
wayland-scanner server-header "$xdg_xml" "$protocols/xdg-shell-server-protocol.h"
wayland-scanner private-code "$xdg_xml" "$protocols/xdg-shell-protocol.c"
wayland-scanner server-header "$text_input_xml" "$protocols/text-input-v3-server-protocol.h"
wayland-scanner private-code "$text_input_xml" "$protocols/text-input-v3-protocol.c"
cc -std=c99 -Wall -Wextra -Wpedantic -Werror -I"$protocols" \
    "$repository/tests/wayland/test_background_effect.c" \
    "$protocols/background-effect-v1-protocol.c" \
    $(pkg-config --cflags --libs boxrig-background-wayland wayland-server wayland-client) \
    '-Wl,-rpath,$ORIGIN/../lib64' -o "$sdk/bin/wayland-background-supported"
env -u LD_LIBRARY_PATH LD_BIND_NOW=1 "$sdk/bin/wayland-background-supported"
for gtk in 3 4; do
    define=(-DBOXRIG_TEST_BACKGROUND_EFFECT=1)
    if [ "$gtk" = 3 ]; then define+=(-DBOXRIG_TEST_GTK3=1); fi
    toolkit=gtk4
    if [ "$gtk" = 3 ]; then toolkit=gtk+-3.0; fi
    cc -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -I"$protocols" \
        "${define[@]}" "$repository/tests/gtk/test_background_effect.c" \
        "$repository/tests/gtk/ime_server.c" \
        "$protocols/background-effect-v1-protocol.c" \
        "$protocols/xdg-shell-protocol.c" \
        "$protocols/text-input-v3-protocol.c" \
        $(pkg-config --cflags --libs "boxrig-background-gtk$gtk" "$toolkit" \
            wayland-client wayland-server xkbcommon) \
        '-Wl,-rpath,$ORIGIN/../lib64' -o "$sdk/bin/gtk$gtk-background-supported"
    env -u LD_LIBRARY_PATH LD_BIND_NOW=1 GDK_BACKEND=wayland \
        GSK_RENDERER=cairo GTK_A11Y=none NO_AT_BRIDGE=1 \
        "$sdk/bin/gtk$gtk-background-supported"
done
cc -std=c99 -Wall -Wextra -Wpedantic -Werror \
    "$repository/tests/x11/fake_notification_service.c" \
    $(pkg-config --cflags --libs dbus-1) -o "$sdk/bin/fake-notification-service"
cc -std=c99 -Wall -Wextra -Wpedantic -Werror \
    "$repository/tests/shared/consumer_notification_delivery.c" \
    $(pkg-config --cflags --libs boxrig-notification) \
    '-Wl,-rpath,$ORIGIN/../lib64' -o "$sdk/bin/notification-delivery"
env -u LD_LIBRARY_PATH LD_BIND_NOW=1 timeout 30 dbus-run-session -- bash -c '
    exec 3< <("$1")
    IFS= read -r -n 1 ready <&3
    test "$ready" = R
    "$2" 3
' _ "$sdk/bin/fake-notification-service" "$sdk/bin/notification-delivery"
find "$sdk/lib64" -name '*.so*' -delete
for language in c c++; do
    compiler=cc
    standard=c99
    if [ "$language" = c++ ]; then compiler=c++; standard=c++20; fi
    "$compiler" -x "$language" -std="$standard" -Wall -Wextra -Wpedantic -Werror \
        "$repository/tests/notification/test_package.c" \
        $(pkg-config --cflags --static --libs boxrig-notification gio-2.0) \
        -o "$sdk/bin/static-notification-$language"
    env -u LD_LIBRARY_PATH "$sdk/bin/static-notification-$language"
    "$compiler" -x "$language" -std="$standard" -Wall -Wextra -Wpedantic -Werror \
        "$repository/tests/wayland/test_background_effect_package.c" \
        $(pkg-config --cflags --static --libs boxrig-background-wayland) \
        -o "$sdk/bin/static-wayland-background-$language"
    env -u LD_LIBRARY_PATH "$sdk/bin/static-wayland-background-$language"
    for gtk in 3 4; do
        source="$repository/tests/gtk/test_background_effect_package.c"
        define=()
        if [ "$gtk" = 3 ]; then define=(-DBOXRIG_TEST_GTK3=1); fi
        if [ "$language" = c++ ]; then source="$repository/tests/shared/consumer_background_gtk.cpp"; fi
        "$compiler" -x "$language" -std="$standard" -Wall -Wextra -Werror \
            "${define[@]}" "$source" \
            $(pkg-config --cflags --static --libs "boxrig-background-gtk$gtk") \
            -o "$sdk/bin/static-gtk$gtk-background-$language"
        env -u LD_LIBRARY_PATH GDK_BACKEND=x11 GSK_RENDERER=cairo \
            GTK_A11Y=none NO_AT_BRIDGE=1 xvfb-run -a "$sdk/bin/static-gtk$gtk-background-$language"
    done
done
printf '%s\n' 'PASS relocated shared effects SDK, supported protocols, delivery and static consumers'
