#!/usr/bin/env bash
set -euo pipefail

# Each directory contains preserved binaries built from one revision. Keep
# builds, other benchmarks and the full test suite outside this serial run.
if (( $# < 2 )); then
    printf '%s\n' 'usage: bash run_core_comparison.sh OUTPUT_DIR LABEL=BINARY_DIR [LABEL=BINARY_DIR ...]' >&2
    exit 2
fi
output_directory=$1
shift
rounds=${BOXRIG_MEASURE_ROUNDS:-5}
if [[ ! $rounds =~ ^[1-9][0-9]*$ ]]; then
    printf '%s\n' 'BOXRIG_MEASURE_ROUNDS must be positive' >&2
    exit 2
fi
mkdir -p -- "$output_directory"
if [[ -e $output_directory/results.tsv ]]; then
    printf 'results already exist: %s\n' "$output_directory/results.tsv" >&2
    exit 2
fi

labels=()
directories=()
for specification in "$@"; do
    label=${specification%%=*}
    directory=${specification#*=}
    if [[ $label == "$specification" || ! $label =~ ^[A-Za-z0-9_-]+$ ||
          ! -x $directory/measure_text_workloads ||
          ! -x $directory/measure_text_workloads_unwrapped ]]; then
        printf 'invalid binary specification: %s\n' "$specification" >&2
        exit 2
    fi
    labels+=("$label")
    directories+=("$(realpath -- "$directory")")
done

cases=()
for history in 0 10000; do
    for mode in 0 1 2; do
        cases+=("input-overwrite 24 80 $history 2 8192 $mode")
    done
    for mode in 1 2; do
        cases+=("input-overwrite 80 240 $history 2 2048 $mode")
    done
    for name in input-ascii input-utf8 input-log; do
        for chunk in 64 4096; do
            cases+=("$name 24 80 $history $chunk $((1048576 / chunk)) 1")
        done
    done
done

{
    printf 'workspace_revision=%s\n' "$(git rev-parse HEAD)"
    printf 'rounds=%s\nround_zero=warmup, excluded from aggregates\n' "$rounds"
    printf 'timing=feed through finish_input; setup and destruction excluded\n'
    printf 'unwrapped=allocation/copy counters unavailable; text-work counters remain enabled\n'
    printf 'live_bytes=wrapped allocation payloads, including setup; excludes allocator headers and shared-library allocations\n'
    printf 'allocation_retained_bytes=bytes allocated or reallocated in the interval and still alive at its end\n'
    printf 'frees=non-NULL free calls plus realloc-to-zero in the interval\n'
    printf 'memmove_bytes=dynamic linked memmove calls only, including row metadata; excludes compiler-inlined copies\n'
    printf 'cell_size_bytes=internal cell structure size; not total per-cell heap use\n'
    cc --version | head -n 1
    uname -a
    for index in "${!labels[@]}"; do
        printf 'label=%s directory=%s\n' "${labels[index]}" "${directories[index]}"
        sha256sum -- "${directories[index]}/measure_text_workloads" \
            "${directories[index]}/measure_text_workloads_unwrapped"
    done
    printf 'case=%s\n' "${cases[@]}"
} > "$output_directory/configuration.txt"
git diff > "$output_directory/workspace.diff"
printf 'round\tlabel\tvariant\tmeasurement\n' > "$output_directory/results.tsv"

for (( round = 0; round <= rounds; round++ )); do
    for workload in "${cases[@]}"; do
        read -r -a arguments <<< "$workload"
        for (( order = 0; order < ${#labels[@]}; order++ )); do
            index=$order
            if (( round % 2 )); then
                index=$((${#labels[@]} - 1 - order))
            fi
            for variant in instrumented unwrapped; do
                executable=${directories[index]}/measure_text_workloads
                if [[ $variant == unwrapped ]]; then
                    executable+=_unwrapped
                fi
                result=$("$executable" "${arguments[@]}" 2>> "$output_directory/stderr.log")
                if [[ $result != 'MEASURE text '* ]]; then
                    printf 'invalid measurement: %s\n' "$result" >&2
                    exit 1
                fi
                printf '%s\t%s\t%s\t%s\n' "$round" "${labels[index]}" \
                    "$variant" "$result" >> "$output_directory/results.tsv"
            done
        done
    done
    printf 'core measurement round %s complete\n' "$round"
done
