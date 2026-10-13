#!/bin/sh
# Keep UI-only consumers loadable without optional APIs or the PTY runtime.
set -eu

objdump=$1
architecture=$2
report_dir=$3
shift 3
mkdir -p "$report_dir"
case "$architecture" in
    i686) format=pei-i386 ;;
    amd64) format=pei-x86-64 ;;
    *) echo "Unknown architecture: $architecture" >&2; exit 1 ;;
esac
for artifact do
    report="$report_dir/$(basename "$artifact").imports.txt"
    "$objdump" -p "$artifact" > "$report"
    awk -v format="$format" '
        NR == FNR {
            if ($0 !~ /^#/ && NF == 2) {
                allowed[$1 " " $2] = 1
                libraries[$1] = 1
            }
            next
        }
        /file format/ { correct_format = ($NF == format) }
        /^MajorOSystemVersion/ { os_major = $2 }
        /^MinorOSystemVersion/ { os_minor = $2 }
        /^MajorSubsystemVersion/ { subsystem_major = $2 }
        /^MinorSubsystemVersion/ { subsystem_minor = $2 }
        /DLL Name:/ {
            dll = tolower($3)
            if (!libraries[dll]) {
                print "Unaudited UI dependency: " dll > "/dev/stderr"
                invalid = 1
            }
            count++
        }
        /^[[:space:]]+[0-9a-f]+[[:space:]]+[0-9]+[[:space:]]+[A-Za-z_]/ {
            if (!allowed[dll " " $3]) {
                print "Unaudited UI import: " dll " " $3 > "/dev/stderr"
                invalid = 1
            }
        }
        END {
            if (!correct_format || os_major > 6 ||
                (os_major == 6 && os_minor > 0) || subsystem_major > 6 ||
                (subsystem_major == 6 && subsystem_minor > 0) ||
                count == 0 || invalid) exit 1
        }
    ' "$(dirname "$0")/ui_imports_$architecture.txt" "$report" || {
        echo "FAIL basic UI PE contract: $artifact (see $report)" >&2
        exit 1
    }
    sha256sum "$artifact" > "$report.sha256"
    echo "PASS basic UI PE contract: $artifact"
done
