#!/bin/bash
set -euo pipefail
build_dir=$1
output=${BOXRIG_TEST_GL_OUTPUT:-"$build_dir/test-results/step24/driver/${GDK_BACKEND:-default}-scale${GDK_SCALE:-1}"}
mkdir -p "$output"
for version in 3 4; do
    # GTK 4.14 may select GLES for its shared context. Exercise its default
    # choice and both APIs, including drivers that cannot share across APIs.
    apis=(default)
    if [ "$version" = 4 ]; then apis+=(gl gles); fi
    for api in "${apis[@]}"; do
        debug=
        if [ "$api" = gl ]; then debug=gl-disable-gles; fi
        if [ "$api" = gles ]; then debug=gl-disable-gl; fi
        for policy in cairo prefer require; do
            for failure in none context capability; do
                env GDK_DEBUG="$debug" BOXRIG_TEST_GL_API="$api" \
                    "$build_dir/tests/gtk${version}_gl_driver" "$policy" "$failure" \
                    "$output/gtk${version}-${api}-${policy}-${failure}.png"
            done
        done
    done
done
