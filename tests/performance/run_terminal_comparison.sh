#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd -- "$script_dir/../.." && pwd)

usage() {
    printf '%s\n' \
        'usage: run_terminal_comparison.sh BOXRIG_GTK3_BINARY OUTPUT_DIR [ROUNDS [HISTORY_LINES [WORKLOAD_LINES [MODE]]]]' \
        'Compares 80x24 GTK3 terminals with Monospace 9 and 10000 scrollback lines.' \
        'The timing boundary is the PTY child receiving a DSR reply, not completed display presentation.' >&2
}

if (( $# < 2 || $# > 6 )); then
    usage
    exit 2
fi

boxrig_binary=$(realpath -- "$1")
output_directory=$2
rounds=${3:-3}
history_lines=${4:-0}
workload_lines=${5:-2000}
selected_mode=${6:-all}

if [[ ! -x "$boxrig_binary" || ! "$rounds" =~ ^[1-9][0-9]*$ ||
      ! "$history_lines" =~ ^(0|[1-9][0-9]*)$ ||
      ! "$workload_lines" =~ ^[1-9][0-9]*$ ||
      ! "$selected_mode" =~ ^(all|ascii|log|cr|utf8)$ ]]; then
    usage
    exit 2
fi

for program in cc gnome-terminal gsettings xvfb-run dbus-run-session timeout; do
    if ! command -v "$program" >/dev/null 2>&1; then
        printf 'required command missing: %s\n' "$program" >&2
        exit 2
    fi
done

mkdir -p -- "$output_directory"
output_directory=$(cd -- "$output_directory" && pwd)
if [[ ${BOXRIG_PERFORMANCE_SESSION:-} != 1 &&
      -e "$output_directory/results.tsv" ]]; then
    printf 'results already exist: %s/results.tsv\n' "$output_directory" >&2
    exit 2
fi
workload_binary="$output_directory/pty_workload"
cc -std=c99 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror \
    "$script_dir/pty_workload.c" -o "$workload_binary"

if [[ ${BOXRIG_PERFORMANCE_SESSION:-} != 1 ]]; then
    private_directory=$(mktemp -d "$output_directory/private.XXXXXXXX")
    export XDG_CONFIG_HOME="$private_directory/config"
    export XDG_DATA_HOME="$private_directory/data"
    export BOXRIG_PERFORMANCE_SESSION=1
    exec xvfb-run -a --server-args='-screen 0 1920x1080x24' \
        dbus-run-session -- "$0" "$boxrig_binary" "$output_directory" \
        "$rounds" "$history_lines" "$workload_lines" "$selected_mode"
fi

if [[ -z ${XDG_CONFIG_HOME:-} || -z ${XDG_DATA_HOME:-} ]]; then
    printf '%s\n' 'private XDG directories are missing' >&2
    exit 2
fi
export GDK_BACKEND=x11
export NO_AT_BRIDGE=1
mkdir -p -- "$XDG_CONFIG_HOME" "$XDG_DATA_HOME"

profile_uuid=38758e33-41e8-45f8-9423-bca803f9f2cd
profile_schema="org.gnome.Terminal.Legacy.Profile:/org/gnome/terminal/legacy/profiles:/:$profile_uuid/"
gsettings set org.gnome.Terminal.ProfilesList list "['$profile_uuid']"
gsettings set org.gnome.Terminal.ProfilesList default "'$profile_uuid'"
gsettings set "$profile_schema" visible-name 'boxrig-comparison'
gsettings set "$profile_schema" use-system-font false
gsettings set "$profile_schema" font 'Monospace 9'
gsettings set "$profile_schema" scrollback-unlimited false
gsettings set "$profile_schema" scrollback-lines 10000
gsettings set "$profile_schema" scrollbar-policy 'never'

results="$output_directory/results.tsv"
printf '%b\n' \
    'round	order	frontend	mode	rows	columns	cell_height	cell_width	history_lines	history_bytes	workload_lines	workload_bytes	write_ns	write_to_dsr_ns	total_to_dsr_ns	dsr_row	dsr_column' > "$results"
{
    printf 'boxrig_binary=%s\n' "$boxrig_binary"
    printf 'private_config=%s\n' "$XDG_CONFIG_HOME"
    printf 'workspace_revision=%s\n' "$(git -C "$project_root" rev-parse HEAD)"
    printf 'boxrig_binary_sha256=%s\n' "$(sha256sum -- "$boxrig_binary")"
    printf 'boxrig_auto_urls=%s\n' "${BOXRIG_AUTO_URLS:-default}"
    printf 'gnome_terminal_version=%s\n' "$(gnome-terminal --version)"
    printf 'font=%s\n' "$(gsettings get "$profile_schema" font)"
    printf 'font_match=%s\n' "$(fc-match -f '%{family}|%{style}|%{file}\n' monospace | head -n 1)"
    printf 'columns=80\nrows=24\nscrollback_limit=10000\n'
    printf 'rounds=%s\nhistory_lines=%s\nworkload_lines=%s\n' \
        "$rounds" "$history_lines" "$workload_lines"
    printf 'selected_mode=%s\n' "$selected_mode"
    printf 'round_zero=one warm-up per frontend and mode; omit from measured aggregates\n'
    printf 'measurement=start before workload writes; sent after writes; processed after DSR reply\n'
    printf 'limitation=DSR confirms input processing through the query; it does not confirm painting or screen presentation\n'
    printf 'cell_geometry=boxrig CSI 16 t; GNOME VTE 0.76 does not support this query, so its cell dimensions are recorded as zero (unknown)\n'
} > "$output_directory/configuration.txt"

run_once() {
    local frontend=$1
    local mode=$2
    local round=$3
    local order=$4
    local report="$output_directory/$mode-$round-$frontend.tsv"
    local log="$output_directory/$mode-$round-$frontend.log"
    local status=0

    rm -f -- "$report"
    if [[ $frontend == boxrig ]]; then
        timeout --signal=TERM --kill-after=3s 35s \
            "$boxrig_binary" --renderer=cairo -- \
            "$workload_binary" "$mode" 24 80 "$history_lines" \
            "$workload_lines" probe "$report" > "$log" 2>&1 || status=$?
    else
        timeout --signal=TERM --kill-after=3s 35s \
            gnome-terminal --profile=boxrig-comparison \
            --geometry=80x24 --wait -- \
            "$workload_binary" "$mode" 24 80 "$history_lines" \
            "$workload_lines" skip "$report" > "$log" 2>&1 || status=$?
    fi
    if (( status != 0 )) || [[ ! -s "$report" ]]; then
        printf 'failed: round=%s frontend=%s mode=%s exit=%s log=%s\n' \
            "$round" "$frontend" "$mode" "$status" "$log" >&2
        return 1
    fi
    printf '%s\t%s\t%s\t%s\n' \
        "$round" "$order" "$frontend" "$(tail -n 1 "$report")" >> "$results"
}

modes=(ascii log cr utf8)
if [[ $selected_mode != all ]]; then
    modes=("$selected_mode")
fi
for mode in "${modes[@]}"; do
    if [[ $mode == ascii || $mode == cr ]]; then
        run_once boxrig "$mode" 0 1
        run_once gnome "$mode" 0 2
    else
        run_once gnome "$mode" 0 1
        run_once boxrig "$mode" 0 2
    fi
    for (( round = 1; round <= rounds; round++ )); do
        if (( round % 2 == 1 )); then
            run_once boxrig "$mode" "$round" 1
            run_once gnome "$mode" "$round" 2
        else
            run_once gnome "$mode" "$round" 1
            run_once boxrig "$mode" "$round" 2
        fi
    done
done

printf 'results: %s\n' "$results"
