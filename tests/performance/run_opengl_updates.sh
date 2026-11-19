#!/usr/bin/env bash
# Replay a captured 118 x 45 btop input through the instrumented Linux sample.
set -euo pipefail
[[ $# == 4 && $2 =~ ^(gtk3|x11)$ ]]
binary=$(realpath "$1")
frontend=$2
export BOXRIG_UPDATE_INPUT BOXRIG_UPDATE_DIR BOXRIG_UPDATE_WORK_PATH BOXRIG_UPDATE_TITLE
BOXRIG_UPDATE_INPUT=$(realpath "$3")
BOXRIG_UPDATE_DIR=$(realpath -m "$4")
mkdir "$BOXRIG_UPDATE_DIR"
BOXRIG_UPDATE_WORK_PATH="$BOXRIG_UPDATE_DIR/work.tsv"
BOXRIG_UPDATE_TITLE="boxrig-update-work-$$"
if [[ $frontend == gtk3 ]]; then width=856; height=690; else width=840; height=630; fi
GDK_BACKEND=x11 NO_AT_BRIDGE=1 BOXRIG_AUTO_URLS=1 "$binary" --renderer=opengl -- bash -ec '
  printf "\033]0;%s\007" "$BOXRIG_UPDATE_TITLE"
  while [[ ! -e "$BOXRIG_UPDATE_DIR/start" ]]; do sleep 0.02; done
  deadline=$((SECONDS+30))
  until [[ $(stty size) == "45 118" ]]; do [[ $SECONDS -lt $deadline ]]; sleep 0.02; done
  stty raw -echo
  cat </dev/tty >/dev/null &
  sink=$!
  trap "kill $sink 2>/dev/null || true" EXIT
  node tests/performance/replay_opengl_updates.mjs "$BOXRIG_UPDATE_INPUT" "$BOXRIG_UPDATE_DIR"
' > "$BOXRIG_UPDATE_DIR/terminal.log" 2>&1 &
terminal=$!
trap 'kill "$terminal" 2>/dev/null || true' EXIT
windows=$(timeout 30s xdotool search --sync --onlyvisible --name "^$BOXRIG_UPDATE_TITLE$")
window=
for candidate in $windows; do
  if xprop -id "$candidate" WM_PROTOCOLS | rg -q WM_DELETE_WINDOW &&
     ! xprop -id "$candidate" WM_CLASS | rg -q mutter-x11-frames; then window=$candidate; break; fi
done
[[ -n $window ]]
xdotool windowsize --sync "$window" "$width" "$height"
xdotool windowraise "$window"
touch "$BOXRIG_UPDATE_DIR/start"
wait "$terminal"
test -s "$BOXRIG_UPDATE_DIR/updates.json"
trap - EXIT
