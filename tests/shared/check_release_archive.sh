#!/bin/bash
# Inspect and run the SDK exactly as it appears in a Linux release archive.
set -euo pipefail
export LC_ALL=C
if [ "$#" -ne 3 ]; then
    echo 'Usage: check_release_archive.sh <archive.tar.gz> <version> <arch>' >&2
    exit 2
fi
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
source "$repository/packaging/common.sh"
archive=$(realpath -- "$1")
version=$2
arch=$3
name=${archive##*/}
name=${name%.tar.gz}
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
mkdir "$root/staged"
tar -xzf "$archive" -C "$root/staged"
test -d "$root/staged/$name"
mv "$root/staged/$name" "$root/relocated"
sdk="$root/relocated"
export PKG_CONFIG_PATH="$sdk/lib/pkgconfig"
mkdir -p "$sdk/bin"

libraries=(boxrig boxrig-ui boxrig-session boxrig-posix boxrig-gtk3 boxrig-gtk4
    boxrig-x11 boxrig-wayland boxrig-render-ogl boxrig-x11-ogl boxrig-wayland-ogl
    boxrig-notification boxrig-background-wayland boxrig-background-gtk3
    boxrig-background-gtk4)
for library in "${libraries[@]}"; do
    file="$sdk/lib/lib$library.so.0"
    test -s "$file" || { echo "FAIL missing shared library: $library" >&2; exit 1; }
    test -s "$sdk/lib/lib$library.a"
    test -L "$sdk/lib/lib$library.so"
    test "$(readlink "$sdk/lib/lib$library.so")" = "lib$library.so.0"
    test -f "$sdk/lib/pkgconfig/$library.pc"
    test "$(pkg-config --modversion "$library")" = "$version"
    test "$(cd -- "$(pkg-config --variable=libdir "$library")" && pwd -P)" = \
        "$(cd -- "$sdk/lib" && pwd -P)"
    test "$(cd -- "$(pkg-config --variable=includedir "$library")" && pwd -P)" = \
        "$(cd -- "$sdk/include" && pwd -P)"
    package_check_elf "$file" "$arch"
    package_check_elf "$sdk/lib/lib$library.a" "$arch"
    readelf -d "$file" > "$root/$library.dynamic"
    grep -Eq "SONAME.*\\[lib$library\\.so\\.0\\]" "$root/$library.dynamic"
    grep -Fq '[$ORIGIN]' "$root/$library.dynamic"
    if grep -Eq 'TEXTREL|/tmp/|/home/' "$root/$library.dynamic"; then
        echo "FAIL non-relocatable shared library: $library" >&2
        exit 1
    fi
    dependencies=$(env -u LD_LIBRARY_PATH LD_BIND_NOW=1 ldd "$file")
    if grep -q 'not found' <<< "$dependencies"; then
        echo "FAIL unresolved shared dependency: $library" >&2
        exit 1
    fi
done

run_consumer() {
    local mode=$1 language=$2 module=$3 source=$4 define=$5
    shift 5
    local compiler=cc standard=c99 flags=() link_flags=() output
    if [ "$language" = c++ ]; then compiler=c++; standard=c++20; fi
    output="$sdk/bin/$mode-$module-$language"
    local compile_flags=(-std="$standard" -Wall -Wextra -Werror)
    case "$module" in *gtk*) ;; *) compile_flags+=(-Wpedantic) ;; esac
    if [ "$mode" = static ]; then
        read -r -a flags <<< "$(pkg-config --cflags --static --libs "$@")"
        compile_flags+=(-DBOXRIG_TEST_STATIC)
    else
        read -r -a flags <<< "$(pkg-config --cflags --libs "$@")"
        link_flags+=('-Wl,-rpath,$ORIGIN/../lib')
    fi
    if [ "$module" = base ] && [ "$mode" = shared ]; then link_flags+=(-ldl); fi
    if [ -n "$define" ]; then compile_flags+=(-D"$define"); fi
    "$compiler" -x "$language" "${compile_flags[@]}" "$repository/$source" \
        "${flags[@]}" "${link_flags[@]}" -o "$output"
    if [ "$mode" = shared ]; then
        readelf -d "$output" > "$root/$module-$language.dynamic"
        if [ "$module" != base ]; then
            grep -Fq "[lib$module.so.0]" "$root/$module-$language.dynamic"
        fi
    else
        if readelf -d "$output" | grep -Eq 'NEEDED.*\[libboxrig[^]]*\.so'; then
            echo "FAIL static consumer loaded a boxrig shared library: $module" >&2
            exit 1
        fi
    fi
    env -u LD_LIBRARY_PATH LD_BIND_NOW=1 "$output"
}

run_all_consumers() {
    local mode=$1 language=$2
    run_consumer "$mode" "$language" base tests/shared/consumer.c '' boxrig-ui boxrig-posix
    run_consumer "$mode" "$language" boxrig-gtk3 tests/gtk3/test_gtk3_package.c '' boxrig-gtk3
    run_consumer "$mode" "$language" boxrig-gtk4 tests/gtk4/test_gtk4_package.c '' boxrig-gtk4
    run_consumer "$mode" "$language" boxrig-x11 tests/shared/consumer_native.c BOXRIG_TEST_X11 boxrig-x11
    run_consumer "$mode" "$language" boxrig-wayland tests/shared/consumer_native.c BOXRIG_TEST_WAYLAND boxrig-wayland
    run_consumer "$mode" "$language" boxrig-render-ogl tests/shared/consumer_release_context.c BOXRIG_TEST_RENDER_OGL boxrig-render-ogl
    run_consumer "$mode" "$language" boxrig-x11-ogl tests/x11/test_x11_opengl_package.c '' boxrig-x11-ogl
    run_consumer "$mode" "$language" boxrig-wayland-ogl tests/wayland/test_wayland_opengl_package.c '' boxrig-wayland-ogl
    run_consumer "$mode" "$language" boxrig-notification tests/notification/test_package.c '' boxrig-notification gio-2.0
    run_consumer "$mode" "$language" boxrig-background-wayland tests/wayland/test_background_effect_package.c '' boxrig-background-wayland
    run_consumer "$mode" "$language" boxrig-background-gtk3 tests/shared/consumer_release_context.c BOXRIG_TEST_BACKGROUND_GTK3 boxrig-background-gtk3
    run_consumer "$mode" "$language" boxrig-background-gtk4 tests/shared/consumer_release_context.c BOXRIG_TEST_BACKGROUND_GTK4 boxrig-background-gtk4
}

for language in c c++; do run_all_consumers shared "$language"; done
find "$sdk/lib" -maxdepth 1 -name '*.so*' -delete
for language in c c++; do run_all_consumers static "$language"; done
printf 'PASS relocated release archive: 15 shared and static libraries (%s, %s)\n' "$version" "$arch"
