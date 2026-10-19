#!/usr/bin/env bash
set -euo pipefail
if (( $# != 3 )); then
    echo "usage: $0 OUTPUT_DIR LABEL=BINARY_DIR LABEL=BINARY_DIR" >&2
    exit 2
fi
output=$(realpath -m "$1")
mkdir -p "$output"
shift
variants=("$@")
{
    date -Iseconds
    uname -a
    echo 'warmup=0 measured=1..5 iterations=1000 serial=true'
    for variant in "${variants[@]}"; do
        directory=${variant#*=}
        sha256sum "$directory/measure_pangocairo_partial" \
            "$directory/measure_pangocairo_partial_unwrapped"
    done
} > "$output/configuration.txt"
printf 'round\torder\tvariant\tmeasurement\n' > "$output/results.tsv"
for round in {0..5}; do
    for columns in 80 240; do
        for cells in 1 8 "$columns"; do
            for suffix in '' _unwrapped; do
                for order in 0 1; do
                    index=$(( (round + order) % 2 ))
                    variant=${variants[index]}
                    label=${variant%%=*}
                    directory=${variant#*=}
                    log="$output/$round-$columns-$cells-$label${suffix}.log"
                    "$directory/measure_pangocairo_partial$suffix" \
                        "$columns" "$cells" 1000 > "$log" 2>&1
                    printf '%s\t%s\t%s\t%s\n' "$round" "$order" "$label" \
                        "$(cat "$log")" >> "$output/results.tsv"
                done
            done
        done
    done
done
echo "results: $output/results.tsv"
