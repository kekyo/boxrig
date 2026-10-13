#!/bin/sh

set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 WESTON SAMPLE_BINARY CHILD_BINARY" >&2
    exit 2
fi

weston=$1
sample_binary=$2
child_binary=$3
runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/boxrig-gtk3-sample-weston.XXXXXX")
wayland_socket=boxrig-gtk3-sample-wayland-test
weston_log=$runtime_dir/weston.log
sample_log=$runtime_dir/sample.log
ready_pipe=$runtime_dir/ready.pipe
control_pipe=$runtime_dir/control.pipe
weston_pid=
sample_pid=

cleanup() {
    if [ -n "$sample_pid" ]; then
        kill "-$sample_pid" 2>/dev/null || :
        wait "$sample_pid" 2>/dev/null || :
        sample_pid=
    fi
    if [ -n "$weston_pid" ]; then
        kill "$weston_pid" 2>/dev/null || :
        wait "$weston_pid" 2>/dev/null || :
        weston_pid=
    fi
    if [ -d "$runtime_dir" ]; then
        find "$runtime_dir" -depth -delete 2>/dev/null || :
    fi
}

print_logs() {
    if [ -f "$sample_log" ]; then
        echo "GTK 3 sample log:" >&2
        sed -n '1,200p' "$sample_log" >&2
    fi
    if [ -f "$weston_log" ]; then
        echo "Weston log:" >&2
        sed -n '1,200p' "$weston_log" >&2
    fi
}

read_phase() {
    timeout 15 sh -c \
        'IFS= read -r phase <&3 && printf "%s" "$phase"'
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

chmod 700 "$runtime_dir"
mkfifo "$ready_pipe" "$control_pipe"
exec 3<>"$ready_pipe"
exec 4<>"$control_pipe"
XDG_RUNTIME_DIR=$runtime_dir "$weston" \
    --debug \
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
        print_logs
        exit 1
    fi
    attempt=$((attempt + 1))
    sleep 0.01
done
if [ "$ready" != true ]; then
    echo "Weston did not create the Wayland socket" >&2
    print_logs
    exit 1
fi

setsid env \
    XDG_RUNTIME_DIR="$runtime_dir" \
    WAYLAND_DISPLAY="$wayland_socket" \
    GDK_BACKEND=wayland \
    NO_AT_BRIDGE=1 \
    BOXRIG_GTK3_WAYLAND_READY_FD=3 \
    BOXRIG_GTK3_WAYLAND_CONTROL_FD=4 \
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1 \
    "$sample_binary" -- "$child_binary" >"$sample_log" 2>&1 &
sample_pid=$!

phase=$(read_phase) || {
    echo "GTK 3 sample child did not complete its first PTY round trip" >&2
    print_logs
    exit 1
}
if [ "$phase" != pty-ready ]; then
    echo "GTK 3 sample child reported an unexpected phase: $phase" >&2
    print_logs
    exit 1
fi

attempt=0
mapped=false
while [ "$attempt" -lt 500 ]; do
    scene=$(
        XDG_RUNTIME_DIR="$runtime_dir" \
        WAYLAND_DISPLAY="$wayland_socket" \
        weston-debug scene-graph 2>/dev/null || :
    )
    if printf '%s\n' "$scene" | rg -q \
        "top-level window 'gtk3-wayland-ready' of boxrig_term"; then
        mapped=true
        break
    fi
    if ! kill -0 "$sample_pid" 2>/dev/null; then
        echo "GTK 3 sample exited before mapping its Wayland surface" >&2
        print_logs
        exit 1
    fi
    attempt=$((attempt + 1))
    sleep 0.01
done
if [ "$mapped" != true ]; then
    echo "GTK 3 sample did not commit its titled Wayland surface" >&2
    print_logs
    exit 1
fi

printf 'g' >&4
phase=$(read_phase) || {
    echo "GTK 3 sample child did not receive cell geometry" >&2
    print_logs
    exit 1
}
if [ "$phase" != geometry-ready ]; then
    echo "GTK 3 sample child reported an unexpected phase: $phase" >&2
    print_logs
    exit 1
fi
printf 'q' >&4

attempt=0
exited=false
while [ "$attempt" -lt 1500 ]; do
    state=$(ps -o stat= -p "$sample_pid" 2>/dev/null || :)
    case "$state" in
        '' | Z*)
            exited=true
            break
            ;;
    esac
    attempt=$((attempt + 1))
    sleep 0.01
done
if [ "$exited" != true ]; then
    echo "GTK 3 sample did not exit after its controlled child" >&2
    print_logs
    exit 1
fi
sample_status=0
wait "$sample_pid" || sample_status=$?
sample_pid=
if [ "$sample_status" -ne 0 ]; then
    echo "GTK 3 sample did not preserve the child exit status" >&2
    print_logs
    exit "$sample_status"
fi

echo "PASS GTK3 sample PTY and presentation through GDK Wayland"
