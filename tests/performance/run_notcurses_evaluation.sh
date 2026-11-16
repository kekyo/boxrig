#!/bin/bash
# Capture a reproducible notcurses run on the caller's X11 display.
set -euo pipefail
if [[ $# != 12 || ! $2 =~ ^(cairo|opengl|auto)$ ||
      ! $3 =~ ^[0-9]+$ || ! $4 =~ ^[0-9]+$ || ! $5 =~ ^[0-9]+$ ||
      ! $6 =~ ^[a-z]*$ || ! $7 =~ ^[0-9]+([.][0-9]+)?$ ||
      ! $9 =~ ^[01]$ || ! ${10} =~ ^[01]$ ||
      ! ${11} =~ ^[1-9][0-9]*$ || ! ${12} =~ ^[1-9][0-9]*$ ]]; then
  echo "usage: $0 BINARY RENDERER WIDTH HEIGHT ZOOM DEMO-SPEC DELAY NEW-OUTPUT RECORD PROFILE ROWS COLUMNS" >&2
  exit 2
fi
binary=$(realpath "$1")
renderer=$2
width=$3
height=$4
zoom=$5
export STEP20_SPEC=$6 STEP20_DELAY=$7
out=$(realpath -m "$8")
record=$9
profile=${10}
mkdir -p "$(dirname "$out")"
# An existing directory is never overwritten, even after an interrupted run.
mkdir "$out"
export STEP20_ROWS=${11} STEP20_COLUMNS=${12}
performance=$(realpath "${BOXRIG_PERFORMANCE_DIR:-build/tests/performance}")
export STEP20_GEOMETRY="$performance/terminal_geometry"
export STEP20_OUT="$out" STEP20_TITLE="boxrig-step20-$$"
export LC_ALL=C.UTF-8
notcurses-demo -V > "$out/version.txt" 2>&1
fc-match monospace > "$out/font.txt"
"$performance/annotation_gl_vendor" > "$out/gpu.txt"
git rev-parse HEAD > "$out/revision.txt"
sha256sum "$binary" > "$out/binary.sha256"
jq -n --arg binary "$binary" --arg renderer "$renderer" --arg spec "$STEP20_SPEC" \
  --arg delay "$STEP20_DELAY" --arg display "$DISPLAY" --argjson zoom "$zoom" \
  --argjson rows "$STEP20_ROWS" --argjson columns "$STEP20_COLUMNS" \
  '{binary:$binary,renderer:$renderer,spec:$spec,delay:$delay,seed:"notcurses -c",
    display:$display,zoom:$zoom,expected_rows:$rows,expected_columns:$columns}' > "$out/request.json"
GDK_BACKEND=x11 NO_AT_BRIDGE=1 BOXRIG_AUTO_URLS=0 "$binary" "--renderer=$renderer" -- bash -c '
  printf "\033]0;%s\007" "$STEP20_TITLE"
  while [[ ! -e "$STEP20_OUT/start" ]]; do sleep 0.02; done
  stty size > "$STEP20_OUT/size.txt"
  if ! "$STEP20_GEOMETRY" > "$STEP20_OUT/geometry.json" ||
     [[ $(cat "$STEP20_OUT/size.txt") != "$STEP20_ROWS $STEP20_COLUMNS" ]]; then
    echo 2 > "$STEP20_OUT/demo-status.txt"
    touch "$STEP20_OUT/done"
    exit 2
  fi
  printf "%s\n" "$TERM" > "$STEP20_OUT/term.txt"
  script -q -e -c '\''/usr/bin/time -o "$STEP20_OUT/demo-resources.txt" -f "%U %S %e %M" notcurses-demo -c -d "$STEP20_DELAY" ${STEP20_SPEC:+"$STEP20_SPEC"}'\'' \
    -O "$STEP20_OUT/output.bin" -T "$STEP20_OUT/timing.txt"
  result=$?
  printf "%s\n" "$result" > "$STEP20_OUT/demo-status.txt"
  touch "$STEP20_OUT/done"
  while [[ ! -e "$STEP20_OUT/release" ]]; do sleep 0.02; done
  exit "$result"
' > "$out/terminal.log" 2>&1 &
terminal=$!
printf '%s\n' "$terminal" > "$out/terminal.pid"
recorder=
profiler=
cleanup() {
  result=$?
  printf '%s\n' "$result" > "$out/evaluation-status.txt"
  [[ -z $recorder ]] || kill -INT "$recorder" 2>/dev/null || true
  [[ -z $profiler ]] || kill -INT "$profiler" 2>/dev/null || true
  if kill -0 "$terminal" 2>/dev/null; then
    kill "$terminal" 2>/dev/null || true
  fi
  if [[ ! -e "$out/terminal-status.txt" ]]; then
    wait "$terminal" && terminal_status=0 || terminal_status=$?
    printf '%s\n' "$terminal_status" > "$out/terminal-status.txt"
  fi
}
trap cleanup EXIT
windows=$(timeout 20s xdotool search --sync --onlyvisible --name "^$STEP20_TITLE$")
window=
for candidate in $windows; do
  if xprop -id "$candidate" WM_CLASS | rg -q mutter-x11-frames; then continue; fi
  if xprop -id "$candidate" WM_PROTOCOLS | rg -q WM_DELETE_WINDOW; then
    window=$candidate
    break
  fi
done
[[ -n $window ]]
xprop -id "$window" > "$out/window-properties.txt"
for ((i=0; i<zoom; i++)); do xdotool key --window "$window" --clearmodifiers ctrl+plus; done
sleep 0.5
if [[ $width != 0 ]]; then timeout 10s xdotool windowsize --sync "$window" "$width" "$height"; fi
sleep 0.5
xdotool getwindowgeometry --shell "$window" > "$out/window.txt"
if [[ $record == 1 ]]; then
  ffmpeg -nostdin -y -hide_banner -loglevel error -f x11grab -window_id "$window" \
    -framerate 30 -draw_mouse 0 -i "$DISPLAY" -c:v ffv1 "$out/video.mkv" > "$out/capture.log" 2>&1 &
  recorder=$!
fi
if [[ $profile == 1 ]]; then
  setsid perf record -q -F 199 -e cpu-clock:u --call-graph dwarf,8192 -p "$terminal" \
    -o "$out/perf.data" > "$out/perf.log" 2>&1 &
  profiler=$!
fi
cat "/proc/$terminal/stat" > "$out/stat-before.txt"
date +%s.%N > "$out/start-time.txt"
touch "$out/start"
deadline=$((SECONDS+600))
while [[ ! -e "$out/done" ]]; do
  kill -0 "$terminal"
  [[ $SECONDS -lt $deadline ]]
  sleep 0.05
done
date +%s.%N > "$out/end-time.txt"
cat "/proc/$terminal/stat" > "$out/stat-after.txt"
cat "/proc/$terminal/status" > "$out/process-status.txt"
if [[ -n $profiler ]]; then
  kill -INT "$profiler"
  set +e
  wait "$profiler"
  profile_status=$?
  set -e
  printf '%s\n' "$profile_status" > "$out/profiler-status.txt"
  [[ $profile_status == 0 || $profile_status == 130 || $profile_status == 143 ]]
  profiler=
  perf report --stdio --no-children --percent-limit 1 -i "$out/perf.data" > "$out/profile.txt" 2> "$out/report.log"
fi
sleep 0.5
if [[ -n $recorder ]]; then
  kill -INT "$recorder"
  set +e
  wait "$recorder"
  capture_status=$?
  set -e
  printf '%s\n' "$capture_status" > "$out/capture-status.txt"
  [[ $capture_status == 0 || $capture_status == 255 ]]
  ffprobe -v error -count_packets -select_streams v:0 \
    -show_entries stream=nb_read_packets -of default=nw=1:nk=1 "$out/video.mkv" > "$out/video-packets.txt"
  [[ $(cat "$out/video-packets.txt") -gt 0 ]]
  recorder=
fi
touch "$out/release"
set +e
wait "$terminal"
result=$?
set -e
printf '%s\n' "$result" > "$out/terminal-status.txt"
[[ $result == 0 && $(cat "$out/demo-status.txt") == 0 ]]
read -r cell_width cell_height < <(jq -r '[.cell_width,.cell_height]|@tsv' "$out/geometry.json")
"$performance/notcurses_replay" "$out/output.bin" "$STEP20_ROWS" "$STEP20_COLUMNS" \
  "$cell_width" "$cell_height" script > "$out/replay.json"
jq -s '.[0] + {geometry:.[1],replay:.[2],demo_exit:0,terminal_exit:0}' \
  "$out/request.json" "$out/geometry.json" "$out/replay.json" > "$out/result.json"
printf '0\n' > "$out/evaluation-status.txt"
trap - EXIT
printf 'PASS %s %s cells=%s\n' "$out" "$renderer" "$(cat "$out/size.txt")"
