#!/usr/bin/env bash
set -euo pipefail

# Compare preserved builds serially. Round zero warms up each condition.
if (( $# < 2 )); then
    printf '%s\n' 'usage: run_recognizer_comparison.sh OUTPUT_DIR LABEL=BINARY_DIR [LABEL=BINARY_DIR ...]' >&2
    exit 2
fi
output_directory=$1
shift
rounds=${BOXRIG_MEASURE_ROUNDS:-5}
[[ $rounds =~ ^[1-9][0-9]*$ ]] || exit 2
mkdir -p -- "$output_directory"
[[ ! -e $output_directory/results.tsv ]] || exit 2
labels=()
directories=()
for specification in "$@"; do
    label=${specification%%=*}
    directory=${specification#*=}
    [[ $label != "$specification" && $label =~ ^[A-Za-z0-9_-]+$ &&
       -x $directory/steady_recognizer &&
       -x $directory/steady_recognizer_unwrapped ]] || exit 2
    labels+=("$label")
    directories+=("$(realpath -- "$directory")")
done
{
    printf 'workspace_revision=%s\n' "$(git rev-parse HEAD)"
    printf 'rounds=%s\nround_zero=warmup\n' "$rounds"
    printf 'grid=24x80; histories=0,4096,10024; URL every 1024 lines; both screens\n'
    printf 'initial=resume and consume notifications until complete\n'
    printf 'steady=feed one edit after initial completion, resume and consume notifications\n'
    printf 'unwrapped=allocation wrappers disabled; text work counters remain enabled\n'
    cc --version | head -n 1
    uname -a
    for index in "${!labels[@]}"; do
        printf 'label=%s\n' "${labels[index]}"
        sha256sum -- "${directories[index]}/steady_recognizer" \
            "${directories[index]}/steady_recognizer_unwrapped"
    done
} > "$output_directory/configuration.txt"
git diff > "$output_directory/workspace.diff"
printf 'round\tlabel\tvariant\tmeasurement\n' > "$output_directory/results.tsv"
for (( round = 0; round <= rounds; round++ )); do
    for mode in overwrite append; do
        for history in 0 4096 10024; do
            for (( order = 0; order < ${#labels[@]}; order++ )); do
                index=$order
                if (( round % 2 )); then
                    index=$((${#labels[@]} - 1 - order))
                fi
                for variant in instrumented unwrapped; do
                    executable=${directories[index]}/steady_recognizer
                    [[ $variant != unwrapped ]] || executable+=_unwrapped
                    result=$("$executable" "$mode" "$history" 2>> "$output_directory/stderr.log")
                    [[ $(printf '%s\n' "$result" | wc -l) == 2 &&
                       $result == 'MEASURE recognizer '* ]] || exit 1
                    while IFS= read -r line; do
                        printf '%s\t%s\t%s\t%s\n' "$round" "${labels[index]}" \
                            "$variant" "$line" >> "$output_directory/results.tsv"
                    done <<< "$result"
                done
            done
        done
    done
    printf 'recognizer measurement round %s complete\n' "$round"
done
