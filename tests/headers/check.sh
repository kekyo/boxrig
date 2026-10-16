#!/bin/sh
# Compile every public header independently for both supported languages.
set -eu
cc=$1
cxx=$2
platform=$3
for header in include/boxrig/*.h include/boxrig/*/*.h; do
    flags=''
    case "$platform:$header" in
        native:*win32*|native:*/render/d3d11.h) continue ;;
        win32:*posix*|win32:*x11*|win32:*wayland*|win32:*gtk*|win32:*notification/*) continue ;;
        native:*gtk4*) flags=$(pkg-config --cflags gtk4) ;;
        native:*gtk*|native:*notification/*) flags=$(pkg-config --cflags gtk+-3.0) ;;
    esac
    for language in c c++; do
        case "$language" in
            c) compiler=$cc; standard=c99 ;;
            c++) compiler=$cxx; standard=c++20 ;;
        esac
        # GTK 4's own headers use large enum constants outside strict ISO C99.
        "$compiler" -x "$language" -std="$standard" -Wall -Wextra -Werror \
            -Iinclude $flags -DBOXRIG_PUBLIC_HEADER="\"${header#include/}\"" \
            -fsyntax-only tests/headers/standalone.c
    done
done
