#!/usr/bin/env bash
set -euo pipefail
if (( $# != 2 )); then
    printf '%s\n' 'usage: run_latency_comparison.sh BUILD_DIR OUTPUT_DIR' >&2
    exit 2
fi
build_directory=$(realpath -- "$1")
output_directory=$2
rounds=${BOXRIG_MEASURE_ROUNDS:-30}
trace_binary="$build_directory/tests/performance/boxrig_term_trace"
phase_binary="$build_directory/tests/performance/boxrig_term_phase"
product_binary="$build_directory/boxrig_term/gtk3/boxrig_term"
mkdir -p -- "$output_directory"
for configuration in utf8-0-cold utf8-10024-cold utf8-10024-ready ascii-10024-cold cr-10024-ready; do
    IFS=- read -r mode history state <<< "$configuration"
    ready=0
    [[ $state != ready ]] || ready=1
    printf 'trace comparison: %s\n' "$configuration"
    BOXRIG_TRACE_INTERVALS=1 BOXRIG_PHASE_WAIT_HISTORY=$ready BOXRIG_REFERENCE_PHASE=1 \
        bash tests/performance/run_phase_probe.sh "$trace_binary" \
            "$output_directory/trace-$configuration" "$rounds" "$history" 2000 "$mode" "$phase_binary" \
            > "$output_directory/trace-$configuration.log" 2>&1
    bash tests/performance/check_phase_trace.sh "$output_directory/trace-$configuration"
done
for history in 0 10024; do
    printf 'ordinary product comparison: utf8-%s\n' "$history"
    BOXRIG_TRACE_INTERVALS=0 BOXRIG_PHASE_WAIT_HISTORY=0 BOXRIG_REFERENCE_PHASE=0 \
        bash tests/performance/run_phase_probe.sh "$phase_binary" \
            "$output_directory/product-utf8-$history" "$rounds" "$history" 2000 utf8 "$product_binary" \
            > "$output_directory/product-utf8-$history.log" 2>&1
done
