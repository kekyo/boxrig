#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd -- "$script_dir/../.." && pwd)

if (( $# < 2 || $# > 7 )); then
    printf '%s\n' 'usage: run_phase_probe.sh BOXRIG_GTK3_PHASE_BINARY OUTPUT_DIR [ROUNDS [HISTORY_LINES [WORKLOAD_LINES [MODE [REFERENCE_BINARY]]]]]' >&2
    exit 2
fi

boxrig_binary=$(realpath -- "$1")
output_directory=$2
rounds=${3:-5}
history_lines=${4:-10024}
workload_lines=${5:-2000}
selected_mode=${6:-all}
reference_binary=${7:-}
if [[ -n "$reference_binary" ]]; then
    reference_binary=$(realpath -- "$reference_binary")
fi
phase_timeout_seconds=${BOXRIG_PHASE_TIMEOUT_SECONDS:-35}
if [[ ! -x "$boxrig_binary" || ! "$rounds" =~ ^[1-9][0-9]*$ ||
      ! "$history_lines" =~ ^(0|[1-9][0-9]*)$ ||
      ! "$workload_lines" =~ ^[1-9][0-9]*$ ||
      ! "$selected_mode" =~ ^(all|ascii|log|cr|utf8|url)$ ||
      ( -n "$reference_binary" && ! -x "$reference_binary" ) ||
      ! "$phase_timeout_seconds" =~ ^[1-9][0-9]*$ ]]; then
    exit 2
fi
for program in cc xvfb-run dbus-run-session timeout; do
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
    "$script_dir/pty_workload.c" "$script_dir/interval_trace.c" -o "$workload_binary"

if [[ ${BOXRIG_PERFORMANCE_SESSION:-} != 1 ]]; then
    private_directory=$(mktemp -d "$output_directory/private.XXXXXXXX")
    export XDG_CONFIG_HOME="$private_directory/config"
    export XDG_DATA_HOME="$private_directory/data"
    export BOXRIG_PERFORMANCE_SESSION=1
    exec xvfb-run -a --server-args='-screen 0 1920x1080x24' \
        dbus-run-session -- "$0" "$boxrig_binary" "$output_directory" \
        "$rounds" "$history_lines" "$workload_lines" "$selected_mode" \
        "$reference_binary"
fi

export GDK_BACKEND=x11
export NO_AT_BRIDGE=1
export BOXRIG_AUTO_URLS=1
mkdir -p -- "$XDG_CONFIG_HOME" "$XDG_DATA_HOME"
results="$output_directory/results.tsv"
printf '%b\n' \
    'round	order	mode	history_lines	workload_lines	start_ns	sent_ns	dsr_ns	marker_ns	recognition_ns	cairo_ns	frame_ns	settled_ns	revision	history_completed_ns' \
    > "$results"
if [[ -n "$reference_binary" ]]; then
    reference_results="$output_directory/reference-results.tsv"
    if [[ ${BOXRIG_REFERENCE_PHASE:-0} == 1 ]]; then
        head -n 1 "$results" > "$reference_results"
    else
    printf '%b\n' \
        'round	order	mode	history_lines	workload_lines	write_ns	write_to_dsr_ns	total_to_dsr_ns' \
        > "$reference_results"
    fi
fi
{
    printf 'boxrig_binary=%s\n' "$boxrig_binary"
    printf 'boxrig_binary_sha256=%s\n' "$(sha256sum -- "$boxrig_binary")"
    printf 'workspace_revision=%s\n' "$(git -C "$project_root" rev-parse HEAD)"
    printf 'reference_binary=%s\n' "$reference_binary"
    printf 'wait_history=%s\nreference_phase=%s\ntrace_intervals=%s\n' \
        "${BOXRIG_PHASE_WAIT_HISTORY:-0}" "${BOXRIG_REFERENCE_PHASE:-0}" \
        "${BOXRIG_TRACE_INTERVALS:-0}"
    printf 'perf_stat=%s\n' "${BOXRIG_PERF_STAT:-0}"
    printf 'perf_scope=main thread only; enable acknowledged before input, disable requested after DSR\n'
    if [[ -n "$reference_binary" ]]; then
        printf 'reference_binary_sha256=%s\n' "$(sha256sum -- "$reference_binary")"
    fi
    printf 'rounds=%s\nhistory_lines=%s\nworkload_lines=%s\n' \
        "$rounds" "$history_lines" "$workload_lines"
    printf 'selected_mode=%s\nboxrig_auto_urls=1\n' "$selected_mode"
    printf 'round_zero=warm-up; omit from measured aggregates\n'
    printf 'marker=OSC 2 title after workload bytes and DSR reply\n'
    printf 'recognition=policy resume reports no remaining core work or events for marker text revision\n'
    printf 'cairo=offscreen renderer returns successfully for marker revision\n'
    printf 'settled=recognition complete, Cairo drawn, component frame complete, no retained view dirty\n'
    printf 'limitation=no GTK compositor or physical display presentation timestamp\n'
    printf 'reference_order=alternating phase and uninstrumented boxrig per mode and round\n'
} > "$output_directory/configuration.txt"

run_phase() {
        local mode=$1
        local round=$2
        local order=$3
        local binary=${4:-$boxrig_binary}
        local prefix=${5:-}
        local destination=${6:-$results}
        local report="$output_directory/$prefix$mode-$round-pty.tsv"
        local phase="$output_directory/$prefix$mode-$round-phase.tsv"
        local phase_fifo="$output_directory/$prefix$mode-$round.fifo"
        local log="$output_directory/$prefix$mode-$round.log"
        local trace_path=
        local pty_trace_path=
        local perf_control=
        local perf_ack=
        local launcher=("$binary")
        if [[ ${BOXRIG_PERF_STAT:-0} == 1 ]]; then
            perf_control="$output_directory/$prefix$mode-$round-perf-control.fifo"
            perf_ack="$output_directory/$prefix$mode-$round-perf-ack.fifo"
            mkfifo -- "$perf_control" "$perf_ack"
            launcher=(perf stat --no-inherit --no-scale --delay=-1 \
                --control="fifo:$perf_control,$perf_ack" \
                -e cycles,instructions,task-clock,cpu-migrations,context-switches -x, \
                -o "$output_directory/$prefix$mode-$round-perf.csv" -- "$binary")
        fi
        if [[ ${BOXRIG_TRACE_INTERVALS:-0} == 1 && -z $prefix ]]; then
            trace_path="$output_directory/$mode-$round-trace.tsv"
            pty_trace_path="$output_directory/$mode-$round-writes.tsv"
        fi
        mkfifo -- "$phase_fifo"
        if ! BOXRIG_PHASE_PROBE_PATH="$phase" \
            BOXRIG_PHASE_PROBE_FIFO="$phase_fifo" \
            BOXRIG_PHASE_TRACE_PATH="$trace_path" BOXRIG_PTY_TRACE_PATH="$pty_trace_path" \
            BOXRIG_PERF_CONTROL="$perf_control" BOXRIG_PERF_ACK="$perf_ack" \
            timeout --signal=TERM --kill-after=3s "${phase_timeout_seconds}s" \
            "${launcher[@]}" --renderer=cairo -- \
            "$workload_binary" "$mode" 24 80 "$history_lines" \
            "$workload_lines" probe "$report" "$phase" "$phase_fifo" \
            > "$log" 2>&1; then
            printf 'failed: mode=%s round=%s log=%s\n' "$mode" "$round" "$log" >&2
            exit 1
        fi
        rm -- "$phase_fifo"
        if [[ -n $perf_control ]]; then rm -- "$perf_control" "$perf_ack"; fi
        if [[ ! -s "$report" || ! -s "$phase" ]]; then
            printf 'missing report: mode=%s round=%s\n' "$mode" "$round" >&2
            exit 1
        fi
        IFS=$'\t' read -r _ _ _ _ _ _ _ _ _ _ _ _ _ _ start_ns sent_ns dsr_ns history_completed_ns \
            < <(tail -n 1 "$report")
        IFS=$'\t' read -r revision marker_ns recognition_ns cairo_ns frame_ns settled_ns \
            < <(tail -n 1 "$phase")
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$round" "$order" "$mode" "$history_lines" "$workload_lines" \
            "$start_ns" "$sent_ns" "$dsr_ns" "$marker_ns" \
            "$recognition_ns" "$cairo_ns" "$frame_ns" "$settled_ns" \
            "$revision" "$history_completed_ns" >> "$destination"
}

run_reference() {
        local mode=$1
        local round=$2
        local order=$3
        if [[ ${BOXRIG_REFERENCE_PHASE:-0} == 1 ]]; then
            run_phase "$mode" "$round" "$order" "$reference_binary" "reference-" "$reference_results"
            return
        fi
        local report="$output_directory/$mode-$round-reference.tsv"
        local log="$output_directory/$mode-$round-reference.log"

        if ! timeout --signal=TERM --kill-after=3s "${phase_timeout_seconds}s" \
            "$reference_binary" --renderer=cairo -- \
            "$workload_binary" "$mode" 24 80 "$history_lines" \
            "$workload_lines" probe "$report" > "$log" 2>&1; then
            printf 'reference failed: mode=%s round=%s log=%s\n' \
                "$mode" "$round" "$log" >&2
            exit 1
        fi
        if [[ ! -s "$report" ]]; then
            printf 'reference report missing: mode=%s round=%s\n' \
                "$mode" "$round" >&2
            exit 1
        fi
        IFS=$'\t' read -r _ _ _ _ _ _ _ _ _ write_ns write_to_dsr_ns \
            total_to_dsr_ns _ _ < <(tail -n 1 "$report")
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$round" "$order" "$mode" "$history_lines" "$workload_lines" \
            "$write_ns" "$write_to_dsr_ns" "$total_to_dsr_ns" \
            >> "$reference_results"
}

modes=(ascii log cr utf8)
if [[ $selected_mode != all ]]; then
    modes=("$selected_mode")
fi
for mode in "${modes[@]}"; do
    for (( round = 0; round <= rounds; round++ )); do
        if [[ -z "$reference_binary" ]]; then
            run_phase "$mode" "$round" 1
        elif [[ ( $round == 0 && ( $mode == log || $mode == utf8 ) ) ||
                ( $round != 0 && $((round % 2)) == 0 ) ]]; then
            run_reference "$mode" "$round" 1
            run_phase "$mode" "$round" 2
        else
            run_phase "$mode" "$round" 1
            run_reference "$mode" "$round" 2
        fi
    done
done

printf 'results: %s\n' "$results"
