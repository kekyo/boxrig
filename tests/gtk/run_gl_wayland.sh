#!/bin/bash
set -euo pipefail
runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/boxrig-gtk-gl-wayland.XXXXXX")
weston_pid=
debug_options=()
if [ "${BOXRIG_TEST_WESTON_DEBUG:-0}" = 1 ]; then debug_options+=(--debug); fi
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
# Drivers supply their own input service. Do not let weston-keyboard claim
# the single input-panel binding before that service can connect.
cat > "$runtime_dir/weston.ini" <<'EOF'
[input-method]
path=/bin/true
EOF
XDG_RUNTIME_DIR="$runtime_dir" weston --backend="${BOXRIG_TEST_WESTON_BACKEND:-headless}" --renderer=gl \
    "${debug_options[@]}" \
    --width=1280 --height=800 --socket=boxrig-gtk-gl --config="$runtime_dir/weston.ini" --idle-time=0 \
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
    BOXRIG_TEST_WESTON_LOG="$runtime_dir/weston.log" \
    GDK_BACKEND=wayland GSK_RENDERER=cairo NO_AT_BRIDGE=1 \
    ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$@"
