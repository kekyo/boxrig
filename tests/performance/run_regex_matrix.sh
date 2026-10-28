#!/bin/sh
# Use one fixed build at a time; callers may pin this script with taskset.
set -eu
if [ "$#" -ne 2 ]; then
    echo "usage: run_regex_matrix.sh build-directory output-directory" >&2
    exit 2
fi
build=$1
output=$2
mkdir -p "$output"
for pattern in http capture2 capture4 prefix anchor unicode; do
    for length in 64 512 8192; do
        for outcome in match miss; do
            iterations=1000
            if [ "$length" = 8192 ]; then iterations=100; fi
            name="$pattern-$length-$outcome"
            "$build/tests/performance/regex_cost" "$pattern" "$length" \
                "$outcome" "$iterations" > "$output/$name.log"
            "$build/tests/performance/regex_cost_observed" "$pattern" "$length" \
                "$outcome" 1 > "$output/$name-observed.log"
        done
    done
done
