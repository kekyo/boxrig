#!/usr/bin/env bash
set -euo pipefail
if (( $# != 3 )); then
    echo "usage: $0 OUTPUT_DIR LABEL=BINARY_DIR LABEL=BINARY_DIR" >&2
    exit 2
fi
output=$(realpath -m "$1")
mkdir -p "$output"
[[ ! -e "$output/results.tsv" ]]
shift
variants=("$@")
launcher=()
if [[ -n ${BOXRIG_INPUT_CPU:-} ]]; then launcher=(taskset -c "$BOXRIG_INPUT_CPU"); fi
{
    date -Iseconds
    uname -a
    echo 'warmup=0 measured=1..5 rows=24 columns=80 history=10000 chunk=4096 serial=true'
    echo 'default bytes=4194304 image bytes=1048576 monitor bytes=65536'
    printf 'cpu_affinity=%s\n' "${BOXRIG_INPUT_CPU:-unrestricted}"
    for variant in "${variants[@]}"; do
        sha256sum "${variant#*=}/profile_input" "${variant#*=}/profile_input_unwrapped"
    done
} > "$output/configuration.txt"
printf 'round\torder\tvariant\tcondition\tmeasurement\n' > "$output/results.tsv"
for round in {0..5}; do
    for workload in ascii utf8 graphemes; do
        for condition in default image monitor; do
            flags=()
            mode=1
            calls=1024
            if [[ $condition == image ]]; then mode=2; calls=256; fi
            if [[ $condition == monitor ]]; then flags=(BOXRIG_PROFILE_MONITOR=1); calls=16; fi
            for order in 0 1; do
                index=$(( (round + order) % 2 ))
                variant=${variants[index]}
                directory=${variant#*=}
                label=${variant%%=*}
                log="$output/$round-$workload-$condition-$label.log"
                env "${flags[@]}" "${launcher[@]}" "$directory/profile_input_unwrapped" "input-$workload" \
                    24 80 10000 4096 "$calls" "$mode" > "$log" 2>&1
                while IFS= read -r line; do
                    printf '%s\t%s\t%s\t%s\t%s\n' "$round" "$order" "$label" \
                        "$condition" "$line" >> "$output/results.tsv"
                done < "$log"
            done
        done
    done
    echo "input comparison round complete: $round"
done
