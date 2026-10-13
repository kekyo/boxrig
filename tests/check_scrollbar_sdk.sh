#!/bin/sh
# Compile examples using installed headers and archives, outside the source tree.
set -eu
repository=$(pwd)
host_cc=$1
amd64_cc=$2
i686_cc=$3
root="$repository/build/tests/scrollbar-sdk"
rm -rf "$root"
mkdir -p "$root"
for frontend in gtk3 gtk4 wayland x11 win32-amd64 win32-i686; do
    prefix="$root/$frontend/usr"
    compiler=$host_cc
    example=$frontend
    target="install-$frontend"
    package="boxrig-$frontend"
    pkg_query=--static
    extra_packages=
    case "$frontend" in
        gtk3|gtk4) pkg_query= ;;
        x11) target=install-x11-opengl; package=boxrig-x11-ogl ;;
        win32-amd64) compiler=$amd64_cc; example=win32; package=boxrig-win32 ;;
        win32-i686) compiler=$i686_cc; example=win32; package=boxrig-win32 ;;
    esac
    make "$target" PREFIX="$prefix"
    cp tests/test_scrollbar_sdk.c "$root/$frontend/consumer.c"
    (
        cd "$root/$frontend"
        export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
        unset PKG_CONFIG_PATH
        # Host packages also resolve their public system dependencies.
        case "$frontend" in
            win32-*) ;;
            *) export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"; unset PKG_CONFIG_LIBDIR ;;
        esac
        # Link boxrig's static archive with the installed private dependencies,
        # while leaving the system GTK libraries dynamically linked.
        case "$frontend" in
            gtk3|gtk4)
                extra_packages=$(pkg-config --print-requires-private "$package" | sed 's/[[:space:]].*//')
                ;;
        esac
        "$compiler" -std=c99 -Wall -Wextra -Werror consumer.c \
            -I"$prefix/share/boxrig/examples/$example" \
            $(pkg-config --cflags --libs $pkg_query "$package" $extra_packages) -o consumer.exe
        case "$frontend" in
            win32-*) ;;
            *) ./consumer.exe ;;
        esac
    )
done
