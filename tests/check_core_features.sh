#!/bin/sh
# Compile a public C consumer for each individual switch and their combination.
set -eu
make_command=$1
root=$2
shift 2
all=
for feature in "$@"; do all="$all -DBOXRIG_FEATURE_$feature=0"; done
for feature in "$@" combined; do
    flags="-DBOXRIG_FEATURE_$feature=0"
    if [ "$feature" = combined ]; then flags=$all; fi
    directory="$root/$feature"
    "$make_command" BUILD_DIR="$directory" CPPFLAGS="$flags" "$directory/libboxrig.a"
    # The flags here describe the same source build. SDK consumers use config.h.
    "${CC:-cc}" -std=c99 -Wall -Wextra -Werror -Iinclude $flags \
        tests/test_core_features.c "$directory/libboxrig.a" -lm -o "$directory/consumer"
    "$directory/consumer"
done
