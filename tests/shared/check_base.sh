#!/bin/bash
set -euo pipefail
make_command=${1:-make}
repository=$(pwd)
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
build="$root/build"
stage="$root/stage"
"$make_command" -j8 shared-base install-shared-base BUILD_DIR="$build" \
    DESTDIR="$stage" PREFIX=/opt/boxrig LIBDIR=/opt/boxrig/lib64 \
    PKGCONFIGDIR=/opt/boxrig/lib64/pkgconfig
mv "$stage/opt/boxrig" "$root/relocated"
sdk="$root/relocated"
export PKG_CONFIG_PATH="$sdk/lib64/pkgconfig"
mkdir -p "$sdk/bin"
for language in c c++; do
    compiler=cc
    standard=c99
    if [ "$language" = c++ ]; then compiler=c++; standard=c++20; fi
    "$compiler" -x "$language" -std="$standard" -Wall -Wextra -Wpedantic -Werror \
        "$repository/tests/shared/consumer.c" \
        $(pkg-config --cflags --libs boxrig-ui boxrig-posix) -ldl \
        '-Wl,-rpath,$ORIGIN/../lib64' -o "$sdk/bin/consumer-$language"
    env -u LD_LIBRARY_PATH LD_BIND_NOW=1 "$sdk/bin/consumer-$language"
done
for driver in selection_history selection_layout_provider annotation_eviction; do
    cc -std=c99 -Wall -Wextra -Wpedantic -Werror \
        "$repository/tests/test_$driver.c" $(pkg-config --cflags --libs boxrig-ui) \
        '-Wl,-rpath,$ORIGIN/../lib64' -o "$sdk/bin/$driver"
    env -u LD_LIBRARY_PATH LD_BIND_NOW=1 "$sdk/bin/$driver"
done
for library in boxrig boxrig-ui boxrig-session boxrig-posix; do
    readelf -d "$sdk/lib64/lib$library.so.0" > "$root/dynamic"
    rg -q "SONAME.*\[lib$library.so.0\]" "$root/dynamic"
    if rg 'TEXTREL|/tmp/|/home/' "$root/dynamic"; then exit 1; fi
done
# A second build must preserve products rather than silently rebuilding them.
find "$build" -type f -printf '%P %T@\n' | sort > "$root/before"
"$make_command" -j8 shared-base BUILD_DIR="$build" PREFIX=/opt/boxrig \
    LIBDIR=/opt/boxrig/lib64 PKGCONFIGDIR=/opt/boxrig/lib64/pkgconfig
find "$build" -type f -printf '%P %T@\n' | sort > "$root/after"
cmp "$root/before" "$root/after"
# The same metadata supports a distribution containing only static archives.
find "$sdk/lib64" -name '*.so*' -delete
cc -std=c99 -Wall -Wextra -Wpedantic -Werror -DBOXRIG_TEST_STATIC \
    "$repository/tests/shared/consumer.c" \
    $(pkg-config --cflags --static --libs boxrig-ui boxrig-posix) \
    -o "$sdk/bin/consumer-static"
"$sdk/bin/consumer-static"
printf '%s\n' 'PASS shared base SDK, relocation, private codecs, static linking and incremental build'
