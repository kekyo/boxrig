#!/bin/sh
# boxrig - Under MIT. Run from the repository root, on the chosen display.
set -eu
input=${1:?usage: run_measurements.sh INPUT OUTPUT_DIRECTORY [CPU]}
output=${2:?usage: run_measurements.sh INPUT OUTPUT_DIRECTORY [CPU]}
processor=${3:-2}
mkdir -p "$output"
make -j4 build/render-measurement/replay build/render-measurement/replay-control > "$output/build.log" 2>&1
{
    git rev-parse HEAD
    git status --short
    uname -a
    pkg-config --modversion gtk+-3.0 glib-2.0 pangocairo
    sha256sum build/render-measurement/replay build/render-measurement/replay-control
    if test "$input" != synthetic; then sha256sum "$input"; fi
    printf 'DISPLAY=%s CPU=%s INPUT=%s\n' "${DISPLAY:-}" "$processor" "$input"
    for property in scaling_governor energy_performance_preference scaling_min_freq scaling_max_freq; do
        path="/sys/devices/system/cpu/cpu$processor/cpufreq/$property"
        if test -r "$path"; then printf '%s=' "$property"; cat "$path"; fi
    done
    if command -v nvidia-smi >/dev/null 2>&1; then
        nvidia-smi --query-gpu=name,driver_version,temperature.gpu --format=csv
    fi
} > "$output/environment.txt"
git diff --binary > "$output/worktree.patch"
for trial in 1 2 3; do
    if test "$trial" = 2; then modes='control detail'; else modes='detail control'; fi
    for mode in $modes; do
        binary=build/render-measurement/replay
        if test "$mode" = control; then binary=build/render-measurement/replay-control; fi
        for backend in core cairo opengl; do
            prefix="$output/$trial-$mode-$backend"
            status=0
            taskset -c "$processor" env GDK_BACKEND=x11 NO_AT_BRIDGE=1 \
                "$binary" "$backend" "$input" "$mode" > "$prefix.jsonl" 2> "$prefix.log" || status=$?
            printf '%s\n' "$status" > "$prefix.exit"
            if test "$status" != 0; then cat "$prefix.log" >&2; exit "$status"; fi
        done
    done
done
