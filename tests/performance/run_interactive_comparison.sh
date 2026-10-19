#!/usr/bin/env bash
set -euo pipefail
if (( $# < 3 || $# > 4 )); then
    echo "usage: $0 BOXRIG_BINARY HELPER_DIR OUTPUT_DIR [ROUNDS]" >&2
    exit 2
fi
binary=$(realpath "$1")
helpers=$(realpath "$2")
output=$(realpath -m "$3")
rounds=${4:-3}
mkdir -p "$output"
if [[ ${BOXRIG_INTERACTIVE_SESSION:-} != 1 ]]; then
    [[ ! -e "$output/configuration.txt" ]]
    private=$(mktemp -d "$output/private.XXXXXXXX")
    export XDG_CONFIG_HOME="$private/config" XDG_DATA_HOME="$private/data"
    export BOXRIG_INTERACTIVE_SESSION=1
    exec dbus-run-session -- bash "$0" "$binary" "$helpers" "$output" "$rounds"
fi
export GDK_BACKEND=x11 GDK_SCALE=1 GDK_DPI_SCALE=1 NO_AT_BRIDGE=1
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME"
uuid=38758e33-41e8-45f8-9423-bca803f9f2cd
schema="org.gnome.Terminal.Legacy.Profile:/org/gnome/terminal/legacy/profiles:/:$uuid/"
gsettings set org.gnome.Terminal.ProfilesList list "['$uuid']"
gsettings set org.gnome.Terminal.ProfilesList default "'$uuid'"
gsettings set "$schema" visible-name 'boxrig-interactive'
gsettings set "$schema" use-system-font false
gsettings set "$schema" font 'Monospace 9'
gsettings set "$schema" scrollback-unlimited false
gsettings set "$schema" scrollback-lines 10000
gsettings set "$schema" scrollbar-policy 'never'
gsettings set "$schema" scroll-on-output false
gsettings set "$schema" cursor-blink-mode 'off'
gsettings set "$schema" use-theme-colors false
gsettings set "$schema" background-color '#000000'
gsettings set "$schema" foreground-color '#ffffff'
original_focus=$(xdotool getactivewindow)
read -r original_x original_y _ <<< "$(xdotool getmouselocation)"
original_x=${original_x#x:}
original_y=${original_y#y:}
terminal_pid=
cleanup() {
    if [[ -n $terminal_pid ]]; then kill "$terminal_pid" 2>/dev/null || true; fi
    xdotool windowactivate "$original_focus" 2>/dev/null || true
    xdotool mousemove "$original_x" "$original_y" 2>/dev/null || true
}
trap cleanup EXIT
{
    date -Iseconds
    git rev-parse HEAD
    uname -a
    xrandr --current
    gnome-terminal --version
    ffmpeg -version | head -n 1
    fc-match -f '%{family}|%{style}|%{file}\n' monospace
    sha256sum "$binary" "$helpers/interactive_workload" "$helpers/interactive_probe"
    echo "rounds=$rounds round_zero=warmup modes=${BOXRIG_INTERACTIVE_MODES:-video none}"
    echo 'grid=80x24 font=Monospace9 history=10000 monitor=HDMI-0 capture_target_hz=60'
    echo 'load=32 URL lines per 10ms; missed periods recorded; scroll region=2..24'
    printf 'key start=XTEST request; endpoints=PTY child receive and X11 window pixels; root_capture=%s\n' "${BOXRIG_CAPTURE_ROOT:-0}"
} > "$output/configuration.txt"
frontends=(boxrig gnome)
for (( round=0; round<=rounds; round++ )); do
    for mode in ${BOXRIG_INTERACTIVE_MODES:-video none}; do
        for order in 0 1; do
            frontend=${frontends[(round + order) % 2]}
            case_dir="$output/$round-$frontend-$mode"
            mkdir -p "$case_dir"
            title="boxrig-interactive-$$-$round-$frontend-$mode"
            if [[ $frontend == boxrig ]]; then
                command=("$binary" --renderer=cairo --)
            else
                command=(gnome-terminal --profile=boxrig-interactive --geometry=80x24 --wait --)
            fi
            timeout --signal=TERM --kill-after=3s 90s "${command[@]}" \
                "$helpers/interactive_workload" "$title" "$case_dir/child.tsv" \
                > "$case_dir/terminal.log" 2>&1 &
            terminal_pid=$!
            window=$(timeout 45s xdotool search --sync --onlyvisible --name "^$title$")
            [[ $window =~ ^[0-9]+$ ]]
            xdotool windowmove --sync "$window" 1400 100
            xdotool windowactivate --sync "$window"
            "$helpers/interactive_probe" "$window" "$case_dir" "$mode" > "$case_dir/probe.log" 2>&1
            wait "$terminal_pid"
            terminal_pid=
            if [[ $mode == video ]]; then
                ffmpeg -nostdin -hide_banner -loglevel warning -y -f concat -safe 0 \
                    -i "$case_dir/video.ffconcat" -c:v ffv1 -level 3 -pix_fmt bgr0 \
                    -fps_mode vfr -enc_time_base demux "$case_dir/video.mkv" \
                    > "$case_dir/encode.log" 2>&1
                ffprobe -v error -select_streams v:0 -show_entries frame=best_effort_timestamp_time \
                    -of csv=p=0 "$case_dir/video.mkv" > "$case_dir/video-timestamps.txt"
            fi
            bash tests/performance/check_interactive.sh "$case_dir" > "$case_dir/check.log" 2>&1
            echo "interactive complete: round=$round frontend=$frontend mode=$mode"
        done
    done
done
