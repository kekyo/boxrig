#!/usr/bin/env bash
set -euo pipefail
if (( $# != 8 )); then
    echo "usage: $0 BINARY PEER PROBE cairo|opengl HISTORY idle|transcript none|video OUTPUT" >&2
    exit 2
fi
binary=$(realpath "$1")
peer=$(realpath "$2")
probe=$(realpath "$3")
renderer=$4
history=$5
load=$6
mode=$7
output=$(realpath -m "$8")
link_mode=${BOXRIG_ANNOTATION_LINK_MODE:-regex}
[[ $renderer == cairo || $renderer == opengl ]]
[[ $load == idle || $load == transcript ]]
[[ $mode == none || $mode == video ]]
[[ $link_mode == regex || $link_mode == osc8 || $link_mode == both ]]
auto_urls=1
if [[ $link_mode == osc8 ]]; then auto_urls=0; fi
mkdir -p "$output"
if [[ ${BOXRIG_ANNOTATION_GTK_XVFB:-} != 1 && ${BOXRIG_PLAN18_HARDWARE_GL:-0} != 1 ]]; then
    BOXRIG_ANNOTATION_GTK_XVFB=1 exec xvfb-run -a -s '-screen 0 1280x800x24' \
        bash "$0" "$binary" "$peer" "$probe" "$renderer" "$history" "$load" "$mode" "$output"
fi
gl_env=(LIBGL_ALWAYS_SOFTWARE=1)
display_backend=Xvfb
software_gl=1
if [[ ${BOXRIG_PLAN18_HARDWARE_GL:-0} == 1 ]]; then
    gl_env=()
    display_backend=${DISPLAY:-unknown}
    software_gl=${LIBGL_ALWAYS_SOFTWARE:-unset}
fi
title="boxrig-annotation-gtk-$$"
if [[ $(basename "$binary") == annotation_gtk_counted* ]]; then
    export BOXRIG_ANNOTATION_COUNT_PATH="$output/count.tsv"
fi
{
    date -Iseconds
    printf 'harness_revision=%s\n' "$(git rev-parse HEAD)"
    printf 'binary_source_revision=%s\n' "${BOXRIG_ANNOTATION_BINARY_REVISION:-$(git -C "$(dirname "$binary")" rev-parse HEAD)}"
    printf 'binary=%s renderer=%s requested_history=%s load=%s capture=%s url_period=%s link_mode=%s auto_urls=%s\n' \
        "$binary" "$renderer" "$history" "$load" "$mode" \
        "${BOXRIG_ANNOTATION_URL_PERIOD:-100}" "$link_mode" "$auto_urls"
    printf 'display_backend=%s depth=24 LIBGL_ALWAYS_SOFTWARE=%s LD_PRELOAD=%s\n' \
        "$display_backend" "$software_gl" "${LD_PRELOAD:-}"
    sha256sum "$binary" "$peer" "$probe"
    fc-match -f 'font=%{family}|%{style}|%{file}\n' monospace
    ffmpeg -version | head -n 1
    "$(dirname "$probe")/annotation_gl_vendor" || echo 'gl_vendor=unavailable'
} > "$output/configuration.txt"
terminal_pid=
original_focus=
if [[ ${BOXRIG_PLAN18_HARDWARE_GL:-0} == 1 ]]; then
    original_focus=$(xdotool getactivewindow 2>/dev/null || true)
fi
cleanup() {
    if [[ -n $terminal_pid ]]; then kill "$terminal_pid" 2>/dev/null || true; fi
    if [[ -n $original_focus ]]; then xdotool windowactivate "$original_focus" 2>/dev/null || true; fi
}
trap cleanup EXIT
env BOXRIG_AUTO_URLS="$auto_urls" BOXRIG_ANNOTATION_LINK_MODE="$link_mode" \
    GDK_BACKEND=x11 GDK_SCALE=1 GDK_DPI_SCALE=1 \
    NO_AT_BRIDGE=1 "${gl_env[@]}" /usr/bin/time -o "$output/resources.txt" \
    -f 'user_s=%U system_s=%S elapsed_s=%e max_rss_kb=%M' \
    timeout --signal=TERM --kill-after=2s 90s \
    "$binary" "--renderer=$renderer" -- "$peer" "$title" "$output/peer.tsv" "$load" "$history" \
    > "$output/terminal.log" 2>&1 &
terminal_pid=$!
window=$(timeout 20s xdotool search --sync --onlyvisible --name "^$title$" | head -n 1)
[[ $window =~ ^[0-9]+$ ]]
if [[ ${BOXRIG_PLAN18_HARDWARE_GL:-0} == 1 ]]; then xdotool windowactivate --sync "$window"; fi
BOXRIG_ANNOTATION_GTK_LOAD="$load" BOXRIG_ANNOTATION_LINK_MODE="$link_mode" \
    "$probe" "$window" "$output" "$mode" \
    > "$output/probe.log" 2>&1
wait "$terminal_pid"
terminal_pid=
if [[ $mode == video ]]; then
    node tests/performance/check_annotation_gtk.mjs "$output/peer.tsv" "$output/probe.tsv" "$output/frames.tsv"
else
    node tests/performance/check_annotation_gtk.mjs "$output/peer.tsv" "$output/probe.tsv"
fi
if [[ $mode == video ]]; then
    ffmpeg -nostdin -hide_banner -loglevel error -y -f concat -safe 0 \
        -i "$output/video.ffconcat" -c:v ffv1 -level 3 -pix_fmt bgr0 \
        -fps_mode vfr -enc_time_base demux \
        "$output/annotation.mkv"
    frame_count=$(wc -l < "$output/frames.tsv")
    frame_count=$((frame_count - 1))
    encoded_count=$(ffprobe -v error -count_frames -select_streams v:0 \
        -show_entries stream=nb_read_frames -of default=noprint_wrappers=1:nokey=1 \
        "$output/annotation.mkv")
    [[ $encoded_count == "$frame_count" ]]
    ffprobe -v error -select_streams v:0 -show_entries frame=best_effort_timestamp_time \
        -of csv=p=0 "$output/annotation.mkv" > "$output/video-times.txt"
    node tests/performance/check_annotation_gtk.mjs "$output/peer.tsv" "$output/probe.tsv" \
        "$output/frames.tsv" "$output/video-times.txt" > "$output/video-check.log"
fi
echo "annotation gtk complete: renderer=$renderer history=$history load=$load capture=$mode"
