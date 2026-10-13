#!/bin/sh
# Audit linked portable consumers separately from their behavioral tests.
set -eu

objdump=$1
architecture=$2
report_dir=$3
shift 3
mkdir -p "$report_dir"
case "$architecture" in
    i686) format=pei-i386; minor=0 ;;
    amd64) format=pei-x86-64; minor=2 ;;
    *) echo "Unknown architecture: $architecture" >&2; exit 1 ;;
esac
for artifact do
    report="$report_dir/$(basename "$artifact").imports.txt"
    "$objdump" -p "$artifact" > "$report"
    awk -v format="$format" -v minor="$minor" '
        NR == FNR {
            if ($0 !~ /^#/ && NF == 2) allowed[$1 " " $2] = 1
            next
        }
        /file format/ { correct_format = ($NF == format) }
        /^MajorOSystemVersion/ { os_major = $2 }
        /^MinorOSystemVersion/ { os_minor = $2 }
        /^MajorSubsystemVersion/ { subsystem_major = $2 }
        /^MinorSubsystemVersion/ { subsystem_minor = $2 }
        /DLL Name:/ {
            dll = tolower($3)
            if (dll != "kernel32.dll" && dll != "msvcrt.dll") {
                print "Unexpected portable dependency: " $3 > "/dev/stderr"
                invalid = 1
            }
            count++
        }
        /^[[:space:]]+[0-9a-f]+[[:space:]]+[0-9]+[[:space:]]+[A-Za-z_]/ {
            if (!allowed[dll " " $3]) {
                print "Unaudited minimum-OS import: " dll " " $3 > "/dev/stderr"
                invalid = 1
            }
        }
        END {
            if (!correct_format || os_major != 5 || os_minor != minor ||
                subsystem_major != 5 || subsystem_minor != minor ||
                count != 2 || invalid) exit 1
        }
    ' "$(dirname "$0")/core_imports_$architecture.txt" "$report" || {
        echo "FAIL portable PE contract: $artifact (see $report)" >&2
        exit 1
    }
    sha256sum "$artifact" > "$report.sha256"
    echo "PASS portable PE contract: $artifact"
done
