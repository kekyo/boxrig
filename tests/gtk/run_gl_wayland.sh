#!/bin/bash
set -euo pipefail
runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/boxrig-gtk-gl-wayland.XXXXXX")
weston_pid=
cleanup() {
    if [ -n "$weston_pid" ]; then
        kill "$weston_pid" 2>/dev/null || :
        wait "$weston_pid" 2>/dev/null || :
    fi
    rm -r "$runtime_dir"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
XDG_RUNTIME_DIR="$runtime_dir" weston --backend="${BOXRIG_TEST_WESTON_BACKEND:-headless}" --renderer=gl \
    --width=1280 --height=800 --socket=boxrig-gtk-gl --no-config --idle-time=0 \
    --log="$runtime_dir/weston.log" &
weston_pid=$!
for ((attempt = 0; attempt < 1000; attempt++)); do
    if [ -S "$runtime_dir/boxrig-gtk-gl" ]; then break; fi
    if ! kill -0 "$weston_pid" 2>/dev/null; then
        cat "$runtime_dir/weston.log"
        exit 1
    fi
    sleep 0.01
done
if [ ! -S "$runtime_dir/boxrig-gtk-gl" ]; then
    cat "$runtime_dir/weston.log"
    exit 1
fi
env XDG_RUNTIME_DIR="$runtime_dir" WAYLAND_DISPLAY=boxrig-gtk-gl \
    GDK_BACKEND=wayland GSK_RENDERER=cairo NO_AT_BRIDGE=1 \
    ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$@"
