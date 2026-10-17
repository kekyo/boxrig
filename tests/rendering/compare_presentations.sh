#!/bin/sh
# boxrig - Under MIT. Compare real GTK RGB/RGBA destinations on the chosen display.
set -eu
input=${1:?usage: compare_presentations.sh INPUT OUTPUT_DIRECTORY [CPU]}
output=${2:?usage: compare_presentations.sh INPUT OUTPUT_DIRECTORY [CPU]}
processor=${3:-2}
mkdir -p "$output"
make -j4 build/render-measurement/replay build/render-measurement/replay-control > "$output/build.log" 2>&1
{
    git rev-parse HEAD
    git status --short
    uname -a
    pkg-config --modversion gtk+-3.0 glib-2.0 pangocairo
    sha256sum build/render-measurement/replay build/render-measurement/replay-control "$input"
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
    modes='detail control'
    formats='rgba rgb'
    if test "$trial" = 2; then modes='control detail'; formats='rgb rgba'; fi
    for mode in $modes; do
        binary=build/render-measurement/replay
        if test "$mode" = control; then binary=build/render-measurement/replay-control; fi
        for activity in plain blink; do
            for format in $formats; do
                prefix="$output/$trial-$mode-$activity-$format"
                unset BOXRIG_MEASURE_RGBA BOXRIG_MEASURE_BLINK
                if test "$format" = rgba; then export BOXRIG_MEASURE_RGBA=1; fi
                if test "$activity" = blink; then export BOXRIG_MEASURE_BLINK=1; fi
                status=0
                taskset -c "$processor" env GDK_BACKEND=x11 NO_AT_BRIDGE=1 \
                    "$binary" opengl "$input" "$mode" > "$prefix.jsonl" 2> "$prefix.log" || status=$?
                printf '%s\n' "$status" > "$prefix.exit"
                if test "$status" != 0; then cat "$prefix.log" >&2; exit "$status"; fi
            done
        done
    done
done
