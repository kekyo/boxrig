#!/bin/bash
set -euo pipefail
build_dir=$1
for api in gl gles; do
    gtk3_gl=
    gtk4_debug=gl-disable-gles
    if [ "$api" = gles ]; then
        gtk3_gl=gles
        gtk4_debug=gl-disable-gl
    fi
    env GDK_GL="$gtk3_gl" BOXRIG_TEST_GL_API="$api" "$build_dir/tests/gtk3_gl_surface"
    env GDK_GL="$gtk3_gl" BOXRIG_TEST_GL_API="$api" "$build_dir/tests/gtk3_gl_surface" rgb
    env GDK_DEBUG="$gtk4_debug" BOXRIG_TEST_GL_API="$api" "$build_dir/tests/gtk4_gl_surface"
done
