#!/usr/bin/env bash
set -euo pipefail
directory=$1
shopt -s nullglob
traces=("$directory"/*-trace.tsv)
(( ${#traces[@]} != 0 ))
for trace in "${traces[@]}"; do
    awk -F '\t' '
        NR == 1 { if ($0 != "# dropped=0") exit 1 }
        NR > 2 {
            if (NF != 8 || $2 <= 0 || $3 < $2 || $4 < 0 || $4 > $3 - $2 + 1000) exit 1
            counts[$1]++
            if ($1 != "read" && $7 != 0) exit 1
        }
        END {
            split("session read feed core_recognition projection regex policy cairo frame final_marker", names, " ")
            for (i in names) if (!counts[names[i]]) exit 1
        }
    ' "$trace"
    writes=${trace%-trace.tsv}-writes.tsv
    [[ -s $writes ]]
    awk -F '\t' '
        NR == 1 { if ($0 != "# dropped=0") exit 1 }
        NR > 2 { if (NF != 8 || $1 != "write" || $2 <= 0 || $3 < $2 || $5 <= 0 || $7 != 0) exit 1; bytes += $5 }
        END { if (bytes == 0) exit 1 }
    ' "$writes"
done
printf 'PASS phase intervals: %s traces\n' "${#traces[@]}"
