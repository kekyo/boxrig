#!/bin/bash
# Capture the installed notcurses demo in an isolated 80-column GTK terminal.
set -euo pipefail

if [[ $# != 5 ]]; then
    echo "usage: $0 GTK3-BINARY cairo|opengl DEMO-SPEC DELAY OUTPUT-DIRECTORY" >&2
    echo 'Use an empty DEMO-SPEC to run all installed demos.' >&2
    exit 2
fi
if [[ $2 != cairo && $2 != opengl ]]; then
    echo 'renderer must be cairo or opengl' >&2
    exit 2
fi
if [[ ! $3 =~ ^[a-z]*$ || ! $4 =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo 'invalid demo spec or delay' >&2
    exit 2
fi
if [[ ${BOXRIG_NOTCURSES_XVFB:-0} != 1 ]]; then
    exec xvfb-run -a -s '-screen 0 1280x800x24' \
        env BOXRIG_NOTCURSES_XVFB=1 bash "$0" "$@"
fi

binary=$(realpath "$1")
renderer=$2
export BOXRIG_DEMO_SPEC=$3 BOXRIG_DEMO_DELAY=$4
out=$(realpath -m "$5")
mkdir -p "$out"
export BOXRIG_DEMO_OUTPUT="$out"
notcurses-demo -V > "$out/version.txt" 2>&1
ffmpeg -nostdin -y -hide_banner -loglevel error -f x11grab \
    -video_size 1280x800 -framerate 30 -i "$DISPLAY" -c:v ffv1 \
    "$out/video.mkv" > "$out/capture.log" 2>&1 &
recorder=$!
trap 'kill -INT "$recorder" 2>/dev/null || true' EXIT
set +e
GDK_BACKEND=x11 NO_AT_BRIDGE=1 BOXRIG_AUTO_URLS=0 \
    /usr/bin/time -o "$out/resources.txt" -f '%U %S %e %M' \
    timeout -k 3s 180s "$binary" "--renderer=$renderer" -- bash -c '
        stty size > "$BOXRIG_DEMO_OUTPUT/size.txt"
        script -q -e -c '\''notcurses-demo -c -d "$BOXRIG_DEMO_DELAY" ${BOXRIG_DEMO_SPEC:+"$BOXRIG_DEMO_SPEC"}'\'' \
            -O "$BOXRIG_DEMO_OUTPUT/output.bin" -T "$BOXRIG_DEMO_OUTPUT/timing.txt"
        result=$?
        printf "%s\n" "$result" > "$BOXRIG_DEMO_OUTPUT/status.txt"
        sleep 1
        exit "$result"
    ' > "$out/terminal.log" 2>&1
terminal_status=$?
kill -INT "$recorder"
wait "$recorder"
capture_status=$?
set -e
trap - EXIT
[[ $terminal_status == 0 ]]
[[ $capture_status == 0 || $capture_status == 255 ]]
[[ $(cat "$out/status.txt") == 0 ]]
[[ $(cat "$out/size.txt") == '24 80' ]]
printf 'PASS notcurses %s (%s), capture: %s\n' "${BOXRIG_DEMO_SPEC:-all}" "$renderer" "$out"
