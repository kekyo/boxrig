#!/usr/bin/env bash
set -euo pipefail
output=$(mktemp -d "$3/phase-trace-test.XXXXXXXX")
BOXRIG_TRACE_INTERVALS=1 BOXRIG_PHASE_WAIT_HISTORY=1 BOXRIG_REFERENCE_PHASE=1 \
    bash tests/performance/run_phase_probe.sh "$1" "$output" 1 32 20 utf8 "$2"
bash tests/performance/check_phase_trace.sh "$output"
for trace in "$output"/*-trace.tsv; do
    awk -F '\t' '
        $1 == "history_marker" { history = $2; history_count++ }
        $1 == "final_marker" { final = $2; final_count++ }
        END { if (history_count != 1 || final_count != 1 || history >= final) exit 1 }
    ' "$trace"
done
[[ $(wc -l < "$output/results.tsv") == 3 && $(wc -l < "$output/reference-results.tsv") == 3 ]]
for report in "$output/results.tsv" "$output/reference-results.tsv"; do
    awk -F '\t' 'NR > 1 { if ($15 <= 0 || $15 > $6) exit 1 }' "$report"
done
printf 'PASS recognition completed before workload: %s\n' "$output"
