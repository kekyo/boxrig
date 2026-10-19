#!/usr/bin/env bash
set -euo pipefail
if (( $# != 2 )); then
    echo "usage: $0 BINARY_DIR OUTPUT_DIR" >&2
    exit 2
fi
directory=$(realpath "$1")
output=$(realpath -m "$2")
mkdir -p "$output"
[[ ! -e "$output/configuration.txt" ]]
{
    date -Iseconds
    uname -a
    perf version
    sha256sum "$directory/profile_input" "$directory/profile_input_unwrapped"
    echo 'grid=24x80 history=10000 chunk=4096 bytes=16777216; monitor bytes=65536'
    echo 'counted calls and perf exclude setup/destruction; serial=true'
} > "$output/configuration.txt"
for workload in ascii utf8 graphemes; do
    for condition in default image monitor; do
        flags=()
        mode=1
        calls=4096
        [[ $condition != image ]] || mode=2
        if [[ $condition == monitor ]]; then
            flags=(BOXRIG_PROFILE_MONITOR=1)
            calls=16
        fi
        name="$workload-$condition"
        env "${flags[@]}" "$directory/profile_input" "input-$workload" \
            24 80 10000 4096 "$calls" "$mode" > "$output/$name-counts.log"
        control="$output/$name-control.fifo"
        ack="$output/$name-ack.fifo"
        mkfifo "$control" "$ack"
        env "${flags[@]}" BOXRIG_PERF_CONTROL="$control" BOXRIG_PERF_ACK="$ack" \
            perf record --delay=-1 --control="fifo:$control,$ack" \
            -e cpu-clock:u -F 997 --call-graph dwarf,8192 \
            -o "$output/$name.data" -- "$directory/profile_input_unwrapped" \
            "input-$workload" 24 80 10000 4096 "$calls" "$mode" \
            > "$output/$name-profile.log" 2> "$output/$name-perf.log"
        rm "$control" "$ack"
        perf report -i "$output/$name.data" --stdio --no-children --sort symbol \
            --percent-limit 0.1 > "$output/$name-self.txt"
        perf report -i "$output/$name.data" --stdio --children --sort symbol \
            --percent-limit 0.5 -g none > "$output/$name-inclusive.txt"
        echo "profile complete: $name"
    done
done
