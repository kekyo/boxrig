#!/bin/bash
# Observe synchronized text/image updates on the caller's X11 display.
set -euo pipefail
[[ ( $# == 5 || ( $# == 7 && ${7:-} =~ ^(gtk3|x11)$ ) ) && $2 =~ ^(cairo|opengl)$ &&
   $3 =~ ^[1-9][0-9]*$ && $4 =~ ^[1-9][0-9]*$ ]]
binary=$(realpath "$1")
renderer=$2
rows=$3
columns=$4
out=$(realpath -m "$5")
performance=$(realpath "${BOXRIG_PERFORMANCE_DIR:-build/tests/performance}")
mkdir -p "$(dirname "$out")"
mkdir "$out"
mkfifo "$out/control.fifo"
export BOXRIG_LATENCY_OUT="$out" BOXRIG_LATENCY_TOOLS="$performance"
export BOXRIG_LATENCY_TITLE="boxrig-frame-latency-$$"
export BOXRIG_LATENCY_SIZE="$rows $columns"
export BOXRIG_PHASE_TRACE_PATH="$out/trace.tsv"
urls=0
width=$((columns*7+30))
height=$((rows*14+60))
if [[ $# == 7 ]]; then
  export BOXRIG_CAPTURE_INPUT BOXRIG_CAPTURE_PEER BOXRIG_IMAGE_WORK_PATH="$out/image-phases.tsv"
  BOXRIG_CAPTURE_INPUT=$(realpath "$6")
  BOXRIG_CAPTURE_PEER=$(realpath tests/performance/replay_marked_capture.mjs)
  if [[ $(jq -r '.workload' "$BOXRIG_CAPTURE_INPUT/manifest.json") == btop ]]; then urls=1; fi
  if [[ $7 == x11 ]]; then width=$((columns*7+14)); height=$((rows*14)); fi
fi
export LC_ALL=C.UTF-8
"$performance/annotation_gl_vendor" > "$out/gpu.txt"
fc-match monospace > "$out/font.txt"
git rev-parse HEAD > "$out/revision.txt"
sha256sum "$binary" > "$out/binary.sha256"
GDK_BACKEND=x11 NO_AT_BRIDGE=1 BOXRIG_AUTO_URLS="$urls" "$binary" "--renderer=$renderer" -- bash -ec '
  printf "\033]0;%s\007" "$BOXRIG_LATENCY_TITLE"
  while [[ ! -e "$BOXRIG_LATENCY_OUT/start" ]]; do sleep 0.02; done
  deadline=$((SECONDS+30))
  until [[ $(stty size) == "$BOXRIG_LATENCY_SIZE" ]]; do
    [[ $SECONDS -lt $deadline ]] || exit 2
    sleep 0.02
  done
  "$BOXRIG_LATENCY_TOOLS/terminal_geometry" > "$BOXRIG_LATENCY_OUT/geometry.json" || exit
  touch "$BOXRIG_LATENCY_OUT/ready"
  if [[ -n ${BOXRIG_CAPTURE_INPUT:-} ]]; then
    stty raw -echo
    cat </dev/tty >/dev/null &
    sink=$!
    trap "kill $sink 2>/dev/null || true" EXIT
    node "$BOXRIG_CAPTURE_PEER" "$BOXRIG_CAPTURE_INPUT" "$BOXRIG_LATENCY_OUT"
    exit
  fi
  exec "$BOXRIG_LATENCY_TOOLS/frame_latency" peer "$BOXRIG_LATENCY_OUT"
' > "$out/terminal.log" 2>&1 &
terminal=$!
cleanup() {
  result=$?
  printf '%s\n' "$result" > "$out/status.txt"
  kill "$terminal" 2>/dev/null || true
}
trap cleanup EXIT
windows=$(timeout 30s xdotool search --sync --onlyvisible --name "^$BOXRIG_LATENCY_TITLE$")
window=
for candidate in $windows; do
  if xprop -id "$candidate" WM_CLASS | rg -q mutter-x11-frames; then continue; fi
  if xprop -id "$candidate" WM_PROTOCOLS | rg -q WM_DELETE_WINDOW; then window=$candidate; break; fi
done
[[ -n $window ]]
xdotool windowsize --sync "$window" "$width" "$height"
xdotool windowmove "$window" 40 40
xdotool windowraise "$window"
touch "$out/start"
deadline=$((SECONDS+30))
while [[ ! -e "$out/ready" ]]; do
  kill -0 "$terminal"
  [[ $SECONDS -lt $deadline ]]
  sleep 0.02
done
read -r cw ch < <(jq -r '[.cell_width,.cell_height]|@tsv' "$out/geometry.json")
"$performance/frame_latency" probe "$window" "$out" "$rows" "$columns" "$cw" "$ch" "$terminal" > "$out/probe.log" 2>&1
wait "$terminal"
node tests/performance/check_frame_latency.mjs "$out" "$renderer"
ffmpeg -nostdin -hide_banner -loglevel error -f concat -safe 0 -i "$out/video.ffconcat" \
  -c:v ffv1 -level 3 -pix_fmt bgr0 -fps_mode vfr -enc_time_base demux "$out/video.mkv"
ffprobe -v error -select_streams v:0 -show_entries frame=best_effort_timestamp_time \
  -of csv=p=0 "$out/video.mkv" > "$out/video-times.txt"
node tests/performance/check_frame_latency.mjs "$out" "$renderer" "$out/video-times.txt"
printf '0\n' > "$out/status.txt"
trap - EXIT
echo "PASS frame latency $renderer $columns x $rows: $out"
