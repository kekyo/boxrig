#!/bin/sh
# Use the same linked driver and serial records for every measurement.
set -eu
make_command=$1
root=$2
default_archive=$3
reduced="-DBOXRIG_FEATURE_KITTY_GRAPHICS=0 -DBOXRIG_FEATURE_CHECKPOINT=0 -DBOXRIG_FEATURE_HISTORY_DEFLATE=0 -DBOXRIG_FEATURE_TEXT_MONITOR=0 -DBOXRIG_FEATURE_TEXT_RECOGNIZER=0 -DBOXRIG_FEATURE_ALT_SCREEN=0"
measure() {
    directory=$1
    archive=$2
    headers=$3
    flags=$4
    mkdir -p "$directory"
    "${CC:-cc}" -std=c99 -Wall -Wextra -Werror -O2 -I"$headers" $flags tests/measure_embedded_core.c \
        -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free "$archive" -lm -o "$directory/measure"
    printf '%s ' "${directory##*/}"
    size -A "$directory/measure" | awk '$1 == ".text" || $1 == ".rodata" { printf "%s=%s ",$1,$2 }'
    "$directory/measure" 80 24
}
if [ -n "${BOXRIG_BASELINE_DIR:-}" ]; then
    measure "$root/baseline" "$BOXRIG_BASELINE_DIR/libboxrig.a" "$BOXRIG_BASELINE_DIR/include" ""
fi
measure "$root/full" "$default_archive" include ""
for variant in reduced unicode ascii ascii-utf8 unicode-console ascii-console; do
    case $variant in
        reduced) flags="$reduced" ;;
        unicode) flags="$reduced -DBOXRIG_FEATURE_EXTENDED_VT=0" ;;
        ascii) flags="-DBOXRIG_FEATURE_COMPACT_CELLS=1" ;;
        unicode-console) flags="$reduced -DBOXRIG_FEATURE_EXTENDED_VT=0 -DBOXRIG_FEATURE_LOG_CURSOR=1" ;;
        ascii-console) flags="-DBOXRIG_FEATURE_COMPACT_CELLS=1 -DBOXRIG_FEATURE_LOG_CURSOR=1" ;;
        ascii-utf8) flags="-DBOXRIG_FEATURE_COMPACT_CELLS=1 -DBOXRIG_FEATURE_ENCODING_HOOKS=0" ;;
    esac
    directory="$root/$variant"
    archive="$directory/libboxrig.a"
    "$make_command" BUILD_DIR="$directory" CPPFLAGS="$flags" "$archive"
    measure "$directory" "$archive" include "$flags"
    if [ "$variant" != reduced ]; then
        "${CC:-cc}" -std=c99 -Wall -Wextra -Werror -O2 -Iinclude $flags tests/test_embedded_log.c \
            "$archive" -lm -o "$directory/driver"
        "$directory/driver"
    fi
    case $variant in ascii*) "$directory/measure" 40 15 ;; esac
done
