#!/bin/bash
set -euo pipefail
make_command=${1:-make}
repository=$(pwd)
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
"$make_command" -j8 install-shared-native BUILD_DIR="$root/build" \
    DESTDIR="$root/stage" PREFIX=/opt/boxrig LIBDIR=/opt/boxrig/lib64 \
    PKGCONFIGDIR=/opt/boxrig/lib64/pkgconfig
mv "$root/stage/opt/boxrig" "$root/relocated"
sdk="$root/relocated"
export PKG_CONFIG_PATH="$sdk/lib64/pkgconfig"
mkdir -p "$sdk/bin"
for frontend in x11 wayland; do
    test -f "$sdk/lib64/libboxrig-$frontend.so.0"
    test -L "$sdk/lib64/libboxrig-$frontend.so"
    readelf -d "$sdk/lib64/libboxrig-$frontend.so.0" > "$root/$frontend-dynamic"
    rg -q "SONAME.*\[libboxrig-$frontend.so.0\]" "$root/$frontend-dynamic"
    if rg 'TEXTREL|/tmp/|/home/' "$root/$frontend-dynamic"; then exit 1; fi
    define=BOXRIG_TEST_X11
    if [ "$frontend" = wayland ]; then define=BOXRIG_TEST_WAYLAND; fi
    for language in c c++; do
        compiler=cc
        standard=c99
        if [ "$language" = c++ ]; then compiler=c++; standard=c++20; fi
        "$compiler" -x "$language" -std="$standard" -Wall -Wextra -Wpedantic -Werror -D"$define" \
            "$repository/tests/shared/consumer_native.c" \
            $(pkg-config --cflags --libs "boxrig-$frontend") \
            '-Wl,-rpath,$ORIGIN/../lib64' -o "$sdk/bin/consumer-$frontend-$language"
        env -u LD_LIBRARY_PATH LD_BIND_NOW=1 "$sdk/bin/consumer-$frontend-$language"
        readelf -d "$sdk/bin/consumer-$frontend-$language" > "$root/$frontend-$language-dynamic"
        rg -q "NEEDED.*\[libboxrig-$frontend.so.0\]" "$root/$frontend-$language-dynamic"
    done
done
cc -std=c99 -Wall -Wextra -Wpedantic -Werror \
    "$repository/tests/x11/test_x11_event_wait.c" \
    $(pkg-config --cflags --libs boxrig-x11 boxrig-posix) \
    '-Wl,-rpath,$ORIGIN/../lib64' -o "$sdk/bin/x11-event-wait"
env -u LD_LIBRARY_PATH LD_BIND_NOW=1 xvfb-run -a "$sdk/bin/x11-event-wait"
readelf -d "$sdk/bin/x11-event-wait" > "$root/x11-event-wait-dynamic"
rg -q 'NEEDED.*\[libboxrig-x11.so.0\]' "$root/x11-event-wait-dynamic"
find "$sdk/lib64" -name '*.so*' -delete
for frontend in x11 wayland; do
    define=BOXRIG_TEST_X11
    if [ "$frontend" = wayland ]; then define=BOXRIG_TEST_WAYLAND; fi
    for language in c c++; do
        compiler=cc
        standard=c99
        if [ "$language" = c++ ]; then compiler=c++; standard=c++20; fi
        "$compiler" -x "$language" -std="$standard" -Wall -Wextra -Wpedantic -Werror -D"$define" \
            "$repository/tests/shared/consumer_native.c" \
            $(pkg-config --cflags --static --libs "boxrig-$frontend") \
            -o "$sdk/bin/consumer-static-$frontend-$language"
        env -u LD_LIBRARY_PATH "$sdk/bin/consumer-static-$frontend-$language"
    done
done
printf '%s\n' 'PASS relocated shared X11 and Wayland SDKs'
