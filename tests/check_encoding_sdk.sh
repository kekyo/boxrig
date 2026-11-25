#!/bin/sh
# Install all three settings into the same SDK to catch stale headers or archives.
set -eu
make_command=$1
directory=$2
codegen=$3
stage=$(mkdir -p "$directory/sdk" && cd "$directory/sdk" && pwd)
for setting in default 0 1; do
    flags=
    expected=1
    if [ "$setting" != default ]; then
        flags="-DBOXRIG_FEATURE_ENCODING_HOOKS=$setting"
        expected=$setting
    fi
    "$make_command" BUILD_DIR="$directory" CODEGEN_DIR="$codegen" CPPFLAGS="$flags" \
        PREFIX=/usr DESTDIR="$stage" install-core
    headers="$stage/usr/include"
    archive="$stage/usr/lib/libboxrig.a"
    printf '#include <boxrig/config.h>\n#if BOXRIG_FEATURE_ENCODING_HOOKS != %s\n#error SDK setting\n#endif\n' "$expected" |
        "${CC:-cc}" -x c -std=c99 -I"$headers" -fsyntax-only -
    "${CC:-cc}" -std=c99 -O2 -Wall -Wextra -Werror -I"$headers" tests/test_encoding_hooks.c \
        "$archive" -lm -o "$directory/consumer"
    "$directory/consumer"
    if [ "$setting" = 0 ]; then
        mkdir -p "$directory/offline/include/boxrig"
        cp "$headers/boxrig/config.h" "$directory/offline/include/boxrig/config.h"
    fi
    other=$((1-expected))
    if printf '#include <boxrig/terminal.h>\n' |
        "${CC:-cc}" -x c -std=c99 -I"$headers" -DBOXRIG_FEATURE_ENCODING_HOOKS=$other \
        -fsyntax-only - 2>"$directory/conflict.log"; then exit 1; fi
    grep -q 'conflicts with the installed boxrig library' "$directory/conflict.log"
    if [ "$expected" = 0 ] && nm -g --defined-only "$archive" |
        grep -Eq ' [Tt] boxrig_(terminal_(set_encoding_hooks|finish_encoding_output)|encoding_reserve)$'; then exit 1; fi
done
offline=$(cd "$directory/offline" && pwd)
"$make_command" BUILD_DIR="$offline" BOXRIG_PREGENERATED_CONFIG="$offline/include/boxrig/config.h" core
if nm -g --defined-only "$offline/libboxrig.a" | grep -q ' T boxrig_terminal_set_encoding_hooks$'; then
    echo 'The offline library must use the settings in its packaged header.' >&2
    exit 1
fi
printf '%s\n' 'PASS default, disabled and re-enabled encoding SDKs in the same build directory'
