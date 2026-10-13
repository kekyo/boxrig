#!/bin/bash
set -euo pipefail
build_dir=$(realpath "${1:-build}")
result_dir="$build_dir/test-results/step25/25-4-compositor"
mkdir -p "$result_dir"
if ! command -v sway >/dev/null; then
    echo 'Sway is required for the real text-input-v3/input-method-v2 test. Install the sway package.' >&2
    exit 1
fi
probe_runtime=$(mktemp -d "${TMPDIR:-/tmp}/boxrig-surrounding-wayland.XXXXXX")
sway_pid=
cleanup() {
    if [ -n "$sway_pid" ]; then
        kill "$sway_pid" 2>/dev/null || :
        wait "$sway_pid" 2>/dev/null || :
    fi
    rm -r "$probe_runtime"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_runtime/config" <<'EOF'
xwayland disable
seat seat0 fallback true
default_border none
for_window [app_id="boxrig-step25"] floating enable
EOF
export XDG_RUNTIME_DIR="$probe_runtime"
unset WAYLAND_DISPLAY SWAYSOCK
# Sway checks host kernel modules even for this nested software-only backend.
# No proprietary GPU is used by the Xvfb/pixman test.
WLR_BACKENDS=x11 WLR_RENDERER=pixman LIBGL_ALWAYS_SOFTWARE=1 sway --unsupported-gpu --config "$probe_runtime/config" \
    > "$result_dir/sway.log" 2>&1 &
sway_pid=$!
for ((attempt = 0; attempt < 3000; attempt++)); do
    for socket in "$probe_runtime"/wayland-*; do
        if [ -S "$socket" ]; then export WAYLAND_DISPLAY="${socket##*/}"; break; fi
    done
    if [ -n "${WAYLAND_DISPLAY:-}" ]; then break; fi
    if ! kill -0 "$sway_pid"; then cat "$result_dir/sway.log"; exit 1; fi
    sleep 0.01
done
test -n "${WAYLAND_DISPLAY:-}"
for mode in ${BOXRIG_PROBE_MODES:-none empty editable attach readonly reject busy geometry}; do
    log="$result_dir/$mode.log"
    timeout --kill-after=5 45 env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
        "$build_dir/tests/wayland_surrounding_compositor" "$mode" > "$log" 2>&1 || {
            cat "$log" "$result_dir/sway.log"
            exit 1
        }
    cat "$log"
done
