#!/bin/bash
# Build and stage one local SDK family using an already prepared source tree.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
source packaging/common.sh
[[ "$#" -ge 4 && "$#" -le 5 ]] ||
    package_fail 'Usage: build_sdk.sh <linux|win32> <version> <output-directory> <jobs> [linux-target]'
family=$1
version=$2
output=$3
jobs=$4
linux_target=${5:-$(uname -m)}
make_command=${MAKE:-make}
package_validate_version "$version"
package_positive_integer JOBS "$jobs"
package_require "$make_command" install pkg-config
# Do not inherit a caller's make goals or installation paths.
unset MAKEFLAGS MFLAGS MAKEOVERRIDES
case "$family" in
    linux)
        [[ "$linux_target" =~ ^[a-z0-9][a-z0-9._-]*$ ]] ||
            package_fail "Invalid Linux package target: $linux_target"
        package_require gcc g++ tar gzip wayland-scanner
        names=("boxrig-$version-linux-$linux_target")
        "$make_command" -j"$jobs" native
        ;;
    win32)
        package_require zip x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ \
            i686-w64-mingw32-gcc i686-w64-mingw32-g++
        names=("boxrig-$version-win32-amd64" "boxrig-$version-win32-i686")
        "$make_command" -j"$jobs" win32 win32-opengl \
            win32-d3d11-amd64 win32-d3d11-i686
        ;;
    *) package_fail "Unsupported SDK family: $family" ;;
esac

mkdir -p "$output"
output=$(cd -- "$output" && pwd)
stage=$(mktemp -d "$output/.sdk.XXXXXX")
trap 'rm -rf "$stage"' EXIT
archives=()
for name in "${names[@]}"; do
    sdk="$stage/$name"
    if [ "$family" = linux ]; then
        targets=(install-gtk3 install-gtk4 install-wayland install-render-ogl \
            install-x11-opengl install-wayland-opengl install-notification \
            install-background-gtk3 install-background-gtk4)
    else
        arch=${name##*-}
        targets=("install-core-win32-$arch" "install-win32-opengl-$arch" \
            "install-win32-d3d11-$arch")
    fi
    # Install targets share files, so staging runs serially after the build.
    "$make_command" -j1 "${targets[@]}" DESTDIR="$stage" PREFIX="/$name" \
        LIBDIR="/$name/lib" INCLUDEDIR="/$name/include" PKGCONFIGDIR="/$name/lib/pkgconfig"
    install -m 644 README_pack.md "$sdk/README.md"
    install -m 644 LICENSE "$sdk/LICENSE"
    rm "$sdk/share/licenses/boxrig/README_ja.md"
    rm -rf "$sdk/share/boxrig/examples"
    install -d "$sdk/bin"
    if [ "$family" = linux ]; then
        # Session and POSIX libraries have no standalone install targets.
        install -m 644 build/libboxrig-session.a build/libboxrig-posix.a "$sdk/lib"
        install -m 644 include/boxrig/session.h include/boxrig/posix.h "$sdk/include/boxrig"
        for frontend in x11 gtk3 gtk4 wayland; do
            install -m 755 "build/boxrig_term/$frontend/boxrig_term" "$sdk/bin/boxrig_term_$frontend"
        done
        install -m 755 build/boxrig_emitter/posix/boxrig_emitter "$sdk/bin"
        tar -czf "$stage/$name.tar.gz" -C "$stage" "$name"
        archives+=("$stage/$name.tar.gz")
    else
        install -m 755 "build/boxrig_term/win32/$arch/boxrig_term.exe" \
            "build/boxrig_term/win32/$arch/boxrig_term_launcher.exe" \
            "build/boxrig_emitter/win32/$arch/boxrig_emitter.exe" "$sdk/bin"
        (cd "$stage" && zip -qr "$name.zip" "$name")
        archives+=("$stage/$name.zip")
    fi
done
mv "${archives[@]}" "$output/"
