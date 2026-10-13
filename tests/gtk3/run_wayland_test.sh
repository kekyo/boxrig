#!/bin/sh

set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 WESTON TEST_BINARY FONT_FILE" >&2
    exit 2
fi

weston=$1
test_binary=$2
font_file=$3
runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/boxrig-gtk3-weston.XXXXXX")
wayland_socket=boxrig-gtk3-wayland-test
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
    --backend=headless \
    --renderer=pixman \
    --socket="$wayland_socket" \
    --idle-time=0 \
    --no-config \
    --log="$weston_log" &
weston_pid=$!

attempt=0
ready=false
while [ "$attempt" -lt 500 ]; do
    if [ -S "$runtime_dir/$wayland_socket" ]; then
        ready=true
        break
    fi
    if ! kill -0 "$weston_pid" 2>/dev/null; then
        echo "Weston exited before creating the Wayland socket" >&2
        print_weston_log
        exit 1
    fi
    attempt=$((attempt + 1))
    sleep 0.01
done

if [ "$ready" != true ]; then
    echo "Weston did not create the Wayland socket" >&2
    print_weston_log
    exit 1
fi

test_status=0
env \
    XDG_RUNTIME_DIR="$runtime_dir" \
    WAYLAND_DISPLAY="$wayland_socket" \
    GDK_BACKEND=wayland \
    NO_AT_BRIDGE=1 \
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1 \
    "$test_binary" "$font_file" || test_status=$?

if [ "$test_status" -ne 0 ]; then
    print_weston_log
    exit "$test_status"
fi
