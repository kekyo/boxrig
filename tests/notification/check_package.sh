#!/bin/sh
set -eu
root=$1
compiler=$2
make install-notification DESTDIR="$root" PREFIX=/usr
export PKG_CONFIG_PATH="$root/usr/lib/pkgconfig"
"$compiler" -std=c99 -Wall -Wextra -Werror \
    $(pkg-config --cflags boxrig-notification) \
    tests/notification/test_package.c \
    $(pkg-config --libs boxrig-notification gio-2.0) -o "$root/notification-consumer"
"$root/notification-consumer"
