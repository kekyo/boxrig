#!/bin/sh

set -eu

if [ "$#" -ne 5 ]; then
    echo "usage: $0 WESTON SAMPLE_BINARY CHILD_BINARY DECORATION EXPECTED_VIEWS" >&2
    exit 2
fi

weston=$1
sample_binary=$2
child_binary=$3
decoration=$4
expected_views=$5
runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/boxrig-wayland-sample.XXXXXX")
wayland_socket=boxrig-wayland-sample-test
weston_log=$runtime_dir/weston.log
sample_log=$runtime_dir/sample.stderr.log
sample_stdout=$runtime_dir/sample.stdout.log
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
        echo "native Wayland sample stderr:" >&2
        sed -n '1,200p' "$sample_log" >&2
    fi
    if [ -f "$sample_stdout" ]; then
        echo "native Wayland sample stdout:" >&2
        sed -n '1,200p' "$sample_stdout" >&2
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

if ! readelf -d "$sample_binary" | rg -q \
    'Shared library: \[libwayland-client\.so'; then
    echo "native Wayland sample does not directly link wayland-client" >&2
    exit 1
fi
if ! readelf -d "$sample_binary" | rg -q \
    'Shared library: \[libEGL\.so'; then
    echo "native Wayland sample does not directly link EGL" >&2
    exit 1
fi
if readelf -d "$sample_binary" | rg -q \
    'Shared library: \[(libX11|libgtk|libgdk|libvulkan)'; then
    echo "native Wayland sample has a forbidden frontend dependency" >&2
    exit 1
fi

chmod 700 "$runtime_dir"
mkfifo "$ready_pipe" "$control_pipe"
exec 3<>"$ready_pipe"
exec 4<>"$control_pipe"
XDG_RUNTIME_DIR=$runtime_dir "$weston" \
    --debug \
    --backend=headless \
    --renderer=pixman \
    --socket="$wayland_socket" \
    --width=800 \
    --height=600 \
    --scale=2 \
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
    BOXRIG_WAYLAND_DECORATION="$decoration" \
    BOXRIG_BACKGROUND_OPACITY=75 \
    BOXRIG_GTK3_WAYLAND_READY_FD=3 \
    BOXRIG_GTK3_WAYLAND_CONTROL_FD=4 \
    "$sample_binary" -- "$child_binary" >"$sample_stdout" 2>"$sample_log" &
sample_pid=$!

phase=$(read_phase) || {
    echo "native Wayland child did not complete its first PTY round trip" >&2
    print_logs
    exit 1
}
if [ "$phase" != pty-ready ]; then
    echo "native Wayland child reported an unexpected phase: $phase" >&2
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
        echo "native Wayland sample exited before mapping its surface" >&2
        print_logs
        exit 1
    fi
    attempt=$((attempt + 1))
    sleep 0.01
done
if [ "$mapped" != true ]; then
    echo "native Wayland sample did not commit its titled surface" >&2
    print_logs
    exit 1
fi

view_count=$(printf '%s\n' "$scene" | rg -c "PID $sample_pid" || :)
if [ "$expected_views" = decorated ] && [ "$view_count" -le 2 ]; then
    echo "native Wayland sample did not map client decoration surfaces" >&2
    printf '%s\n' "$scene" >&2
    print_logs
    exit 1
fi
if [ "$expected_views" = undecorated ] && [ "$view_count" -ne 2 ]; then
    echo "libdecor-disabled Wayland sample mapped unexpected surfaces" >&2
    printf '%s\n' "$scene" >&2
    print_logs
    exit 1
fi

printf 'g' >&4
phase=$(read_phase) || {
    echo "native Wayland child did not receive cell geometry" >&2
    print_logs
    exit 1
}
if [ "$phase" != geometry-ready ]; then
    echo "native Wayland child reported an unexpected phase: $phase" >&2
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
    echo "native Wayland sample did not exit after its controlled child" >&2
    print_logs
    exit 1
fi
sample_status=0
wait "$sample_pid" || sample_status=$?
sample_pid=
if [ "$sample_status" -ne 0 ]; then
    echo "native Wayland sample did not preserve the child exit status" >&2
    print_logs
    exit "$sample_status"
fi
if ! rg -q '^boxrig_term: renderer=opengl$' "$sample_stdout"; then
    echo "native Wayland sample did not select its default OpenGL renderer" >&2
    print_logs
    exit 1
fi
for field in graphics-api graphics-api-version graphics-vendor graphics-device; do
    if ! rg -q "^boxrig_term: $field=.+$" "$sample_stdout"; then
        echo "native Wayland OpenGL sample did not report $field to stdout" >&2
        print_logs
        exit 1
    fi
done

echo "PASS native Wayland sample PTY presentation and $expected_views surface contract"
