#!/bin/sh

set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 LIBDECOR_ENABLED SAMPLE_BINARY NO_LIBDECOR_BINARY" >&2
    exit 2
fi

libdecor_enabled=$1
sample_binary=$2
no_libdecor_binary=$3
runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/boxrig-wayland-build.XXXXXX")
error_log=$runtime_dir/error.log

cleanup() {
    if [ -d "$runtime_dir" ]; then
        find "$runtime_dir" -depth -delete 2>/dev/null || :
    fi
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if ! readelf -d "$sample_binary" | rg -q \
    'Shared library: \[libwayland-client\.so'; then
    echo "native Wayland sample does not link wayland-client" >&2
    exit 1
fi
if ! readelf -d "$no_libdecor_binary" | rg -q \
    'Shared library: \[libwayland-client\.so'; then
    echo "libdecor-disabled sample does not link wayland-client" >&2
    exit 1
fi
if [ "$libdecor_enabled" -eq 1 ]; then
    if ! readelf -d "$sample_binary" | rg -q \
        'Shared library: \[libdecor-0\.so'; then
        echo "auto native Wayland sample does not link detected libdecor" >&2
        exit 1
    fi
elif readelf -d "$sample_binary" | rg -q \
    'Shared library: \[libdecor-0\.so'; then
    echo "auto native Wayland sample unexpectedly links libdecor" >&2
    exit 1
fi
if readelf -d "$no_libdecor_binary" | rg -q \
    'Shared library: \[libdecor-0\.so'; then
    echo "libdecor-disabled sample unexpectedly links libdecor" >&2
    exit 1
fi
if nm -u build/libboxrig-wayland.a | rg -q 'libdecor_'; then
    echo "native Wayland component archive contains libdecor symbols" >&2
    exit 1
fi

status=0
env BOXRIG_WAYLAND_DECORATION=invalid \
    "$sample_binary" -- true >"$error_log" 2>&1 || status=$?
if [ "$status" -ne 2 ] || ! rg -q \
    'BOXRIG_WAYLAND_DECORATION must be auto, server, client, or none' \
    "$error_log"; then
    echo "native Wayland sample did not reject an invalid decoration value" >&2
    sed -n '1,80p' "$error_log" >&2
    exit 1
fi

for binary in "$sample_binary" "$no_libdecor_binary"; do
    status=0
    env -u WAYLAND_SOCKET XDG_SESSION_TYPE=x11 \
        XDG_RUNTIME_DIR="$runtime_dir" WAYLAND_DISPLAY=missing-compositor \
        BOXRIG_WAYLAND_DECORATION=auto \
        "$binary" -- sh -c 'touch "$1"' sh "$runtime_dir/child-started" \
        >"$error_log" 2>&1 || status=$?
    if [ "$status" -eq 0 ] || ! rg -qi \
        'boxrig_term:.*connect.*Wayland compositor' "$error_log" || \
        [ -e "$runtime_dir/child-started" ]; then
        echo "native Wayland sample did not diagnose a missing compositor" >&2
        sed -n '1,80p' "$error_log" >&2
        exit 1
    fi
done

echo "PASS native Wayland sample build and startup contracts"
