#!/bin/sh
set -eu
driver=$1
# Limit this GTK label/button smoke test to Fontconfig's process-lifetime
# allocations. The controller/ownership test runs without any suppressions.
suppression_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LSAN_OPTIONS="${LSAN_OPTIONS:+$LSAN_OPTIONS:}suppressions=$suppression_dir/gtk-fontconfig.lsan:print_suppressions=0"
export LSAN_OPTIONS
output=$(NO_AT_BRIDGE=1 GDK_BACKEND=x11 xvfb-run -a "$driver" --once /bin/sh -c 'exit 127')
case "$output" in
    'Finished. Exit=127 signal=0 output=0 error=0; generation=1 resources=recovered') ;;
    *) printf '%s\n' "$output"; exit 1 ;;
esac
output=$(NO_AT_BRIDGE=1 GDK_BACKEND=x11 xvfb-run -a "$driver" --once /boxrig-spawn-missing-06cd3f24)
case "$output" in
    'Failed to start. Exit=127 signal=0 output=0 error=2; generation=1 resources=recovered') ;;
    *) printf '%s\n' "$output"; exit 1 ;;
esac
printf '%s\n' 'PASS GTK startup example reports exec failure separately from normal exit 127'
