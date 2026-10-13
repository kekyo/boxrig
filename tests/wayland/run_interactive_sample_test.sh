#!/bin/sh

set -eu

if [ "$#" -ne 4 ]; then
    echo "usage: $0 WESTON SAMPLE_BINARY CHILD_BINARY X11_PIXEL_PROBE" >&2
    exit 2
fi

weston=$1
sample_binary=$2
child_binary=$3
pixel_probe=$4
runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/boxrig-wayland-interactive.XXXXXX")
wayland_socket=boxrig-wayland-interactive-test
weston_log=$runtime_dir/weston.log
sample_log=$runtime_dir/sample.log
ready_pipe=$runtime_dir/ready.pipe
control_pipe=$runtime_dir/control.pipe
plugin_dir=$runtime_dir/libdecor-plugins
weston_pid=
sample_pid=

cleanup() {
    if [ -n "$sample_pid" ]; then
        kill -TERM "-$sample_pid" 2>/dev/null || :
        attempt=0
        while kill -0 "$sample_pid" 2>/dev/null && [ "$attempt" -lt 100 ]; do
            attempt=$((attempt + 1))
            sleep 0.01
        done
        kill -KILL "-$sample_pid" 2>/dev/null || :
        wait "$sample_pid" 2>/dev/null || :
        sample_pid=
    fi
    if [ -n "$weston_pid" ]; then
        kill -TERM "$weston_pid" 2>/dev/null || :
        attempt=0
        while kill -0 "$weston_pid" 2>/dev/null && [ "$attempt" -lt 100 ]; do
            attempt=$((attempt + 1))
            sleep 0.01
        done
        kill -KILL "$weston_pid" 2>/dev/null || :
        wait "$weston_pid" 2>/dev/null || :
        weston_pid=
    fi
    if [ "${BOXRIG_KEEP_WAYLAND_INTERACTIVE_TEST:-0}" = 1 ]; then
        echo "native Wayland interactive artifacts: $runtime_dir" >&2
    elif [ -d "$runtime_dir" ]; then
        find "$runtime_dir" -depth -delete 2>/dev/null || :
    fi
}

print_logs() {
    if [ -f "$sample_log" ]; then
        echo "native Wayland interactive sample log:" >&2
        sed -n '1,200p' "$sample_log" >&2
    fi
    if [ -f "$weston_log" ]; then
        echo "Weston X11 log:" >&2
        sed -n '1,200p' "$weston_log" >&2
    fi
}

read_phase() {
    timeout 15 sh -c \
        'IFS= read -r phase <&3 && printf "%s" "$phase"'
}

scene_graph() {
    XDG_RUNTIME_DIR=$runtime_dir \
        WAYLAND_DISPLAY=$wayland_socket \
        weston-debug scene-graph 2>/dev/null
}

view_state() {
    scene=$1
    records=$(printf '%s\n' "$scene" | awk -v target="$sample_pid" '
        /^[[:space:]]*View [0-9]+ / {
            active = index($0, "PID " target ",") != 0
            if (active && index($0, "role xdg_toplevel") != 0) {
                role = "root"
            } else if (active && index($0, "role wl_subsurface") != 0) {
                role = "sub"
            } else {
                active = 0
            }
            next
        }
        active && /^[[:space:]]*position:/ {
            x1 = $2
            y1 = $3
            x2 = $5
            y2 = $6
            gsub(/[(),]/, "", x1)
            gsub(/[(),]/, "", y1)
            gsub(/[(),]/, "", x2)
            gsub(/[(),]/, "", y2)
            positioned = 1
            next
        }
        active && /\[fully opaque\]/ {
            opacity = "opaque"
            next
        }
        active && /\[not opaque\]/ {
            opacity = "transparent"
            next
        }
        active && /^[[:space:]]*format:/ && positioned {
            print role, x1, y1, x2, y2, $3, opacity
            active = 0
            positioned = 0
            opacity = "unknown"
        }
    ')
    root=$(printf '%s\n' "$records" | awk '$1 == "root" { print; exit }')
    if [ -z "$root" ]; then
        return 1
    fi
    set -- $root
    root_x=$2
    root_y=$3
    root_width=$(($4 - $2))
    root_height=$(($5 - $3))
    root_format=$6
    root_opacity=$7
    child=$(printf '%s\n' "$records" | awk \
        -v x="$root_x" -v y="$root_y" \
        '$1 == "sub" && $2 == x && $3 == y { print; exit }')
    if [ -z "$child" ]; then
        return 1
    fi
    set -- $child
    child_width=$(($4 - $2))
    child_height=$(($5 - $3))
    printf '%s %s %s %s %s %s %s %s %s %s\n' \
        "$root_width" "$root_height" "$child_width" "$child_height" \
        "$root_format" "$6" "$root_opacity" "$7" "$root_x" "$root_y"
}

wait_for_larger_synchronized_view() {
    previous_width=$1
    attempt=0
    while [ "$attempt" -lt 500 ]; do
        scene=$(scene_graph || :)
        state=$(view_state "$scene" || :)
        if [ -n "$state" ]; then
            set -- $state
            if [ "$1" -gt "$previous_width" ] &&
                [ "$1" -eq "$3" ] && [ "$2" -eq "$4" ]; then
                printf '%s\n' "$state"
                return 0
            fi
        fi
        if ! kill -0 "$sample_pid" 2>/dev/null; then
            return 1
        fi
        attempt=$((attempt + 1))
        sleep 0.01
    done
    return 1
}

wait_for_synchronized_view_geometry() {
    expected_width=$1
    expected_height=$2
    expected_x=$3
    expected_y=$4
    attempt=0
    while [ "$attempt" -lt 500 ]; do
        scene=$(scene_graph || :)
        state=$(view_state "$scene" || :)
        if [ -n "$state" ]; then
            set -- $state
            if [ "$1" -eq "$expected_width" ] &&
                [ "$2" -eq "$expected_height" ] &&
                [ "$1" -eq "$3" ] && [ "$2" -eq "$4" ] &&
                [ "$9" -eq "$expected_x" ] &&
                [ "${10}" -eq "$expected_y" ]; then
                printf '%s\n' "$state"
                return 0
            fi
        fi
        if ! kill -0 "$sample_pid" 2>/dev/null; then
            return 1
        fi
        attempt=$((attempt + 1))
        sleep 0.01
    done
    return 1
}

wait_for_transparent_view() {
    attempt=0
    while [ "$attempt" -lt 500 ]; do
        scene=$(scene_graph || :)
        state=$(view_state "$scene" || :)
        if [ -n "$state" ]; then
            set -- $state
            if [ "$1" -eq "$3" ] && [ "$2" -eq "$4" ] &&
                [ "$5" = ARGB8888 ] && [ "$6" = ARGB8888 ] &&
                [ "$8" = transparent ]; then
                printf '%s\n' "$state"
                return 0
            fi
        fi
        if ! kill -0 "$sample_pid" 2>/dev/null; then
            return 1
        fi
        attempt=$((attempt + 1))
        sleep 0.01
    done
    return 1
}

wait_for_opacity_pixel() {
    window=$1
    x=$2
    y=$3
    before_red=$4
    before_green=$5
    before_blue=$6
    attempt=0
    while [ "$attempt" -lt 500 ]; do
        pixel=$("$pixel_probe" "$window" "$x" "$y" || :)
        if [ -n "$pixel" ]; then
            set -- $pixel
            red_difference=$(($1 - before_red))
            green_difference=$(($2 - before_green))
            blue_difference=$(($3 - before_blue))
            [ "$red_difference" -lt 0 ] && red_difference=$((-red_difference))
            [ "$green_difference" -lt 0 ] && \
                green_difference=$((-green_difference))
            [ "$blue_difference" -lt 0 ] && \
                blue_difference=$((-blue_difference))
            if [ $((red_difference + green_difference + blue_difference)) -ge 48 ]; then
                return 0
            fi
        fi
        if ! kill -0 "$sample_pid" 2>/dev/null; then
            return 1
        fi
        attempt=$((attempt + 1))
        sleep 0.01
    done
    return 1
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

for command in xdotool weston-debug timeout awk pkg-config; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "native Wayland interactive test requires $command" >&2
        exit 1
    fi
done
if [ ! -x "$pixel_probe" ]; then
    echo "native Wayland interactive test requires $pixel_probe" >&2
    exit 1
fi

libdecor_libdir=$(pkg-config --variable=libdir 'libdecor-0 >= 0.2.0')
cairo_plugin=$libdecor_libdir/libdecor/plugins-1/libdecor-cairo.so
if [ ! -f "$cairo_plugin" ]; then
    echo "native Wayland interactive test requires $cairo_plugin" >&2
    exit 1
fi

chmod 700 "$runtime_dir"
mkdir "$plugin_dir"
ln -s "$cairo_plugin" "$plugin_dir/libdecor-cairo.so"
mkfifo "$ready_pipe" "$control_pipe"
exec 3<>"$ready_pipe"
exec 4<>"$control_pipe"
XDG_RUNTIME_DIR=$runtime_dir "$weston" \
    --debug \
    --backend=x11 \
    --renderer=pixman \
    --socket="$wayland_socket" \
    --width=1000 \
    --height=650 \
    --idle-time=0 \
    --no-config \
    --log="$weston_log" &
weston_pid=$!

attempt=0
ready=false
while [ "$attempt" -lt 500 ]; do
    if [ -S "$runtime_dir/$wayland_socket" ]; then
        ready=true
        break
    fi
    if ! kill -0 "$weston_pid" 2>/dev/null; then
        echo "Weston exited before creating the interactive Wayland socket" >&2
        print_logs
        exit 1
    fi
    attempt=$((attempt + 1))
    sleep 0.01
done
if [ "$ready" != true ]; then
    echo "Weston did not create the interactive Wayland socket" >&2
    print_logs
    exit 1
fi

weston_window=
attempt=0
while [ "$attempt" -lt 500 ]; do
    weston_window=$(sed -n \
        's/.*x11 output [^,]*, window id \([0-9][0-9]*\)$/\1/p' \
        "$weston_log" | tail -n 1)
    if [ -n "$weston_window" ]; then
        break
    fi
    attempt=$((attempt + 1))
    sleep 0.01
done
if [ -z "$weston_window" ]; then
    echo "Weston X11 window was not created" >&2
    print_logs
    exit 1
fi
xdotool windowfocus --sync "$weston_window"

if [ -n "${BOXRIG_WAYLAND_TEST_DEBUG:-}" ]; then
    WAYLAND_DEBUG=$BOXRIG_WAYLAND_TEST_DEBUG
    export WAYLAND_DEBUG
fi
setsid env \
    XDG_RUNTIME_DIR="$runtime_dir" \
    WAYLAND_DISPLAY="$wayland_socket" \
    LIBDECOR_PLUGIN_DIR="$plugin_dir" \
    BOXRIG_WAYLAND_DECORATION=client \
    BOXRIG_BACKGROUND_OPACITY=100 \
    BOXRIG_GTK3_WAYLAND_READY_FD=3 \
    BOXRIG_GTK3_WAYLAND_CONTROL_FD=4 \
    "$sample_binary" -- "$child_binary" >"$sample_log" 2>&1 &
sample_pid=$!

phase=$(read_phase) || {
    echo "interactive sample child did not complete its PTY round trip" >&2
    print_logs
    exit 1
}
if [ "$phase" != pty-ready ]; then
    echo "interactive sample child reported an unexpected phase: $phase" >&2
    print_logs
    exit 1
fi
printf 'g' >&4
phase=$(read_phase) || {
    echo "interactive sample child did not receive initial geometry" >&2
    print_logs
    exit 1
}
if [ "$phase" != geometry-ready ]; then
    echo "interactive sample child reported an unexpected phase: $phase" >&2
    print_logs
    exit 1
fi

attempt=0
state=
mapped=false
while [ "$attempt" -lt 500 ]; do
    scene=$(scene_graph || :)
    state=$(view_state "$scene" || :)
    if [ -n "$state" ]; then
        set -- $state
        if [ "$1" -eq "$3" ] && [ "$2" -eq "$4" ]; then
            mapped=true
            break
        fi
    fi
    attempt=$((attempt + 1))
    sleep 0.01
done
if [ "$mapped" != true ]; then
    echo "interactive sample did not map synchronized content surfaces" >&2
    print_logs
    exit 1
fi
set -- $state
root_width=$1
root_height=$2
root_x=$9
root_y=${10}
xdotool mousemove --window "$weston_window" \
    $((root_x + 80)) $((root_y + 80)) click 1

printf 'z' >&4
phase=$(read_phase) || {
    echo "interactive sample child did not report initial cell geometry" >&2
    print_logs
    exit 1
}
set -- $phase
if [ "$1" != geometry ] || [ "$2" -le 0 ] || [ "$3" -le 0 ]; then
    echo "interactive sample child reported invalid cell geometry: $phase" >&2
    print_logs
    exit 1
fi
cell_height=$2
cell_width=$3

xdotool key --clearmodifiers super+shift+m
state=$(wait_for_larger_synchronized_view "$root_width") || {
    echo "interactive sample did not enter the maximized allocation" >&2
    scene_graph >&2 || :
    print_logs
    exit 1
}
xdotool key --clearmodifiers super+shift+m
state=$(wait_for_synchronized_view_geometry \
    "$root_width" "$root_height" "$root_x" "$root_y") || {
    echo "interactive sample did not restore its floating allocation" >&2
    scene_graph >&2 || :
    print_logs
    exit 1
}

sample_x=$((root_x + root_width / 2))
sample_y=$((root_y + root_height / 2))
set -- $("$pixel_probe" "$weston_window" "$sample_x" "$sample_y")
opaque_red=$1
opaque_green=$2
opaque_blue=$3

for opacity_step in 1 2 3 4 5 6; do
    xdotool key --clearmodifiers ctrl+less
done
state=$(wait_for_transparent_view) || {
    echo "startup opacity shortcut did not present an ARGB root and transparent terminal surface" >&2
    scene_graph >&2 || :
    print_logs
    exit 1
}
wait_for_opacity_pixel \
    "$weston_window" "$sample_x" "$sample_y" \
    "$opaque_red" "$opaque_green" "$opaque_blue" || {
    echo "startup opacity shortcut did not change the composited terminal pixel before zoom" >&2
    scene_graph >&2 || :
    print_logs
    exit 1
}

for zoom_step in 1 2 3; do
    xdotool key --clearmodifiers ctrl+plus
    state=$(wait_for_larger_synchronized_view "$root_width") || {
        echo "zoom $zoom_step did not synchronize root and terminal surfaces" >&2
        scene_graph >&2 || :
        print_logs
        exit 1
    }
    set -- $state
    root_width=$1
    printf 'z' >&4
    phase=$(read_phase) || {
        echo "zoom $zoom_step did not update cell geometry" >&2
        print_logs
        exit 1
    }
    set -- $phase
    if [ "$1" != geometry ] ||
        { [ "$2" -le "$cell_height" ] && [ "$3" -le "$cell_width" ]; }; then
        echo "zoom $zoom_step did not increase cell geometry: $phase" >&2
        print_logs
        exit 1
    fi
    cell_height=$2
    cell_width=$3
done

printf 'i' >&4
xdotool key --clearmodifiers x
phase=$(read_phase) || {
    echo "terminal input was not delivered before decoration interaction" >&2
    print_logs
    exit 1
}
if [ "$phase" != input-ready ]; then
    echo "interactive sample child reported an unexpected phase: $phase" >&2
    print_logs
    exit 1
fi

set -- $state
root_width=$1
root_x=$9
root_y=${10}
xdotool mousemove --window "$weston_window" \
    $((root_x + root_width / 2)) $((root_y - 12))
xdotool mousedown 1 mousemove_relative --sync 30 20 mouseup 1

printf 'i' >&4
xdotool key --clearmodifiers x
phase=$(read_phase) || {
    echo "terminal input was not delivered after libdecor titlebar drag" >&2
    scene_graph >&2 || :
    print_logs
    exit 1
}
if [ "$phase" != input-ready ]; then
    echo "interactive sample child reported an unexpected phase: $phase" >&2
    print_logs
    exit 1
fi

printf 'q' >&4
attempt=0
exited=false
while [ "$attempt" -lt 1500 ]; do
    state=$(ps -o stat= -p "$sample_pid" 2>/dev/null || :)
    case "$state" in
        '' | Z*)
            exited=true
            break
            ;;
    esac
    attempt=$((attempt + 1))
    sleep 0.01
done
if [ "$exited" != true ]; then
    echo "interactive sample did not exit after its controlled child" >&2
    print_logs
    exit 1
fi
sample_status=0
wait "$sample_pid" || sample_status=$?
sample_pid=
if [ "$sample_status" -ne 0 ]; then
    echo "interactive sample did not preserve the child exit status" >&2
    print_logs
    exit "$sample_status"
fi

echo "PASS native Wayland libdecor maximize, zoom, opacity, and focus interaction"
