#!/bin/sh

set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "usage: $0 WESTON TEST_BINARY [RENDERER]" >&2
    exit 2
fi

weston=$1
test_binary=$2
renderer=${3:-pixman}
detect_leaks=1
if [ "$renderer" = gl ]; then
    # EGL vendors can retain process-global allocations after eglTerminate.
    # The sanitizer-backed renderer and component fixtures still run with leak
    # detection enabled; keep address and undefined-behavior checks here.
    detect_leaks=0
fi
runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/boxrig-native-wayland.XXXXXX")
wayland_socket=boxrig-native-wayland-test
weston_log=$runtime_dir/weston.log
weston_pid=

cleanup() {
    if [ -n "$weston_pid" ]; then
        kill "$weston_pid" 2>/dev/null || :
        wait "$weston_pid" 2>/dev/null || :
        weston_pid=
    fi
    if [ -d "$runtime_dir" ]; then
        find "$runtime_dir" -depth -delete 2>/dev/null || :
    fi
}

print_weston_log() {
    if [ -f "$weston_log" ]; then
        echo "Weston log:" >&2
        sed -n '1,200p' "$weston_log" >&2
    fi
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

chmod 700 "$runtime_dir"
XDG_RUNTIME_DIR=$runtime_dir "$weston" \
    --debug \
    --backend=headless \
    --renderer="$renderer" \
    --socket="$wayland_socket" \
    --width=800 \
    --height=600 \
    --scale=2 \
    --idle-time=0 \
    --no-config \
    --log="$weston_log" &
weston_pid=$!

attempt=0
while [ ! -S "$runtime_dir/$wayland_socket" ]; do
    if ! kill -0 "$weston_pid" 2>/dev/null; then
        echo "Weston exited before creating the Wayland socket" >&2
        print_weston_log
        exit 1
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 500 ]; then
        echo "Weston did not create the Wayland socket" >&2
        print_weston_log
        exit 1
    fi
    sleep 0.01
done

test_status=0
timeout 30 env \
    XDG_RUNTIME_DIR="$runtime_dir" \
    WAYLAND_DISPLAY="$wayland_socket" \
    ASAN_OPTIONS="detect_leaks=$detect_leaks:halt_on_error=1" \
    UBSAN_OPTIONS=halt_on_error=1 \
    "$test_binary" || test_status=$?
if [ "$test_status" -ne 0 ]; then
    print_weston_log
    exit "$test_status"
fi
