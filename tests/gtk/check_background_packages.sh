#!/bin/sh
set -eu
root=$1
compiler=$2
make install-background-gtk3 install-background-gtk4 DESTDIR="$root" PREFIX=/usr
export PKG_CONFIG_PATH="$root/usr/lib/pkgconfig"
for version in 3 4; do
    flags=
    if [ "$version" = 3 ]; then flags=-DBOXRIG_TEST_GTK3=1; fi
    # Link boxrig's static adapter dependencies, using shared toolkit libraries.
    private=$(pkg-config --print-requires-private boxrig-background-gtk"$version" |
        while read -r package constraint; do printf '%s ' "$package"; done)
    # Deliberate pkg-config word splitting for compiler argument lists.
    "$compiler" -std=c99 -Wall -Wextra -Werror $flags \
        $(pkg-config --cflags boxrig-background-gtk"$version") \
        tests/gtk/test_background_effect_package.c \
        $(pkg-config --libs boxrig-background-gtk"$version" $private) \
        -o "$root/gtk$version-consumer"
    xvfb-run -a env GDK_BACKEND=x11 GSK_RENDERER=cairo GTK_A11Y=none NO_AT_BRIDGE=1 "$root/gtk$version-consumer"
done
"$compiler" -std=c99 -Wall -Wextra -Werror \
    $(pkg-config --cflags boxrig-background-wayland) \
    tests/wayland/test_background_effect_package.c \
    $(pkg-config --libs --static boxrig-background-wayland) -o "$root/wayland-consumer"
"$root/wayland-consumer"
