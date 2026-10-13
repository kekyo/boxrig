#!/bin/sh

set -eu

if [ "$#" -ne 5 ]; then
    echo "usage: $0 FRONTEND WESTON SAMPLE_BINARY PTY_HELPER X11_PIXEL_PROBE" >&2
    exit 2
fi

frontend=$1
weston=$2
sample_binary=$3
pty_helper=$4
pixel_probe=$5
case "$frontend" in
    GTK3)
        native_frontend=false
        gtk_im_module=
        ;;
    GTK4)
        native_frontend=false
        gtk_im_module=${BOXRIG_TEST_GTK4_IM_MODULE:?BOXRIG_TEST_GTK4_IM_MODULE is required}
        ;;
    native-Wayland)
        native_frontend=true
        gtk_im_module=
        ;;
    *)
        echo "unknown Wayland sample frontend: $frontend" >&2
        exit 2
        ;;
esac
runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/boxrig-wayland-scenario.XXXXXX")
wayland_socket=boxrig-wayland-scenario-test
weston_log=$runtime_dir/weston.log
sample_log=$runtime_dir/sample.log
plugin_dir=$runtime_dir/libdecor-plugins
weston_pid=
sample_pid=
clipboard_peer_pid=
clipboard_peer_log=
clipboard_peer=${BOXRIG_TEST_CLIPBOARD_PEER:-}
pending_profile=${BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD:-0}
pending_clipboard=0
color_filter_demo=${BOXRIG_TEST_SAMPLE_COLORS:-0}
sample_renderer=${BOXRIG_TEST_SAMPLE_RENDERER:-}
current_scene=
current_state=
ready_title=
cell_height=
cell_width=
content_x=
content_y=
sample_wayland_debug=
if [ "$frontend" = "${BOXRIG_WAYLAND_TEST_DEBUG_FRONTEND:-native-Wayland}" ]; then
    sample_wayland_debug=${BOXRIG_WAYLAND_TEST_DEBUG:-}
fi
if [ "$frontend" = GTK3 ] && [ "${BOXRIG_TEST_SAMPLE_GEOMETRY:-0}" = 1 ]; then
    sample_wayland_debug=client
fi
if [ "$pending_profile" != 0 ]; then
    sample_wayland_debug=client
fi

cleanup() {
    if [ -n "$clipboard_peer_pid" ]; then
        kill -TERM "$clipboard_peer_pid" 2>/dev/null || :
        wait "$clipboard_peer_pid" 2>/dev/null || :
        clipboard_peer_pid=
        exec 9>&-
    fi
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
    if [ "${BOXRIG_KEEP_WAYLAND_SCENARIO_TEST:-0}" = 1 ]; then
        echo "$frontend Wayland scenario artifacts: $runtime_dir" >&2
    elif [ -d "$runtime_dir" ]; then
        find "$runtime_dir" -depth -delete 2>/dev/null || :
    fi
}

print_diagnostics() {
    if [ -n "$clipboard_peer_log" ] && [ -f "$clipboard_peer_log" ]; then
        echo "$frontend pending clipboard peer log:" >&2
        sed -n '1,100p' "$clipboard_peer_log" >&2
    fi
    if [ -n "$current_scene" ]; then
        echo "$frontend Wayland scenario scene graph:" >&2
        printf '%s\n' "$current_scene" >&2
    fi
    if [ -f "$sample_log" ]; then
        echo "$frontend Wayland scenario sample log:" >&2
        if [ -n "$sample_wayland_debug" ]; then
            tail -n 160 "$sample_log" >&2
        else
            sed -n '1,200p' "$sample_log" >&2
        fi
    fi
    if [ -f "$weston_log" ]; then
        echo "$frontend Wayland scenario Weston log:" >&2
        sed -n '1,200p' "$weston_log" >&2
    fi
}

fail() {
    echo "$1" >&2
    print_diagnostics
    exit 1
}

sample_is_live() {
    sample_state=$(ps -o stat= -p "$sample_pid" 2>/dev/null || :)
    case "$sample_state" in
        '' | Z*)
            return 1
            ;;
    esac
    return 0
}

wayland_log_has_keyboard_focus() {
    [ "$(sed -n \
        's/.*wl_keyboard@[0-9][0-9]*\.\(enter\|leave\)(.*/\1/p' \
        "$1" | tail -n 1)" = enter ]
}

wait_for_sample_keyboard_focus() {
    attempt=0
    while [ "$attempt" -lt 1000 ]; do
        if wayland_log_has_keyboard_focus "$sample_log"; then
            return 0
        fi
        sample_is_live || return 1
        attempt=$((attempt + 1))
        sleep 0.01
    done
    return 1
}

uncovered_clipboard_peer_point() {
    scene=$1
    peer=$2
    set -- $peer
    peer_width=$1
    peer_height=$2
    peer_x=$7
    peer_y=$8

    printf '%s\n' "$scene" | awk \
        -v target="$sample_pid" \
        -v x="$peer_x" -v y="$peer_y" \
        -v width="$peer_width" -v height="$peer_height" '
        BEGIN {
            candidate_x[1] = x + width / 2
            candidate_y[1] = y + height / 2
            candidate_x[2] = x + width - 40
            candidate_y[2] = candidate_y[1]
            candidate_x[3] = candidate_x[1]
            candidate_y[3] = y + height - 40
            candidate_x[4] = x + 40
            candidate_y[4] = candidate_y[1]
            candidate_x[5] = candidate_x[1]
            candidate_y[5] = y + 40
        }
        /^[[:space:]]*View [0-9]+ / {
            active = index($0, "PID " target ",") != 0
            next
        }
        active && /^[[:space:]]*position:/ {
            blocked += 1
            x1[blocked] = $2
            y1[blocked] = $3
            x2[blocked] = $5
            y2[blocked] = $6
            gsub(/[(),]/, "", x1[blocked])
            gsub(/[(),]/, "", y1[blocked])
            gsub(/[(),]/, "", x2[blocked])
            gsub(/[(),]/, "", y2[blocked])
            active = 0
        }
        END {
            for (candidate = 1; candidate <= 5; candidate += 1) {
                covered = 0
                for (view = 1; view <= blocked; view += 1) {
                    if (candidate_x[candidate] >= x1[view] &&
                        candidate_x[candidate] < x2[view] &&
                        candidate_y[candidate] >= y1[view] &&
                        candidate_y[candidate] < y2[view]) {
                        covered = 1
                        break
                    }
                }
                if (!covered) {
                    printf "%d %d\n", candidate_x[candidate], candidate_y[candidate]
                    exit
                }
            }
        }
    '
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
        active && /^[[:space:]]*format:/ && positioned {
            print role, x1, y1, x2, y2, $3
            active = 0
            positioned = 0
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
    if [ "$native_frontend" = false ]; then
        printf '%s %s %s %s %s %s %s %s\n' \
            "$root_width" "$root_height" "$root_width" "$root_height" \
            "$root_format" "$root_format" "$root_x" "$root_y"
        return 0
    fi
    child=$(printf '%s\n' "$records" | awk \
        -v x="$root_x" -v y="$root_y" \
        '$1 == "sub" && $2 == x && $3 == y { print; exit }')
    if [ -z "$child" ]; then
        return 1
    fi
    set -- $child
    child_width=$(($4 - $2))
    child_height=$(($5 - $3))
    printf '%s %s %s %s %s %s %s %s\n' \
        "$root_width" "$root_height" "$child_width" "$child_height" \
        "$root_format" "$6" "$root_x" "$root_y"
}

locate_terminal_viewport() {
    set -- $current_state
    root_width=$1
    root_height=$2
    root_x=$7
    root_y=$8
    if [ "$native_frontend" = true ]; then
        content_x=$root_x
        content_y=$root_y
        return 0
    fi

    attempt=0
    while [ "$attempt" -lt 1000 ]; do
        red_bounds=$("$pixel_probe" \
            --find-red "$weston_window" \
            "$root_x" "$root_y" "$root_width" "$root_height" || :)
        if [ -n "$red_bounds" ]; then
            set -- $red_bounds
            red_x=$1
            red_y=$2
            red_width=$3
            red_height=$4
            candidate_x=$red_x
            candidate_y=$((red_y - 5 * cell_height))
            if [ "$red_width" -le "$cell_width" ] &&
                [ "$red_height" -le "$cell_height" ] &&
                [ "$candidate_x" -ge "$root_x" ] &&
                [ "$candidate_y" -ge "$root_y" ] &&
                [ $((candidate_x + 80 * cell_width)) -le \
                    $((root_x + root_width)) ] &&
                [ $((candidate_y + 24 * cell_height)) -le \
                    $((root_y + root_height)) ]; then
                content_x=$candidate_x
                content_y=$candidate_y
                return 0
            fi
        fi
        if ! sample_is_live; then
            return 1
        fi
        attempt=$((attempt + 1))
        sleep 0.01
    done
    return 1
}

scenario_title() {
    printf '%s\n' "$1" | sed -n \
        "s/.*top-level window '\(boxrig-scenario-[^']*\)' of boxrig_term.*/\1/p" | \
        head -n 1
}

close_button_point() {
    scene=$1
    content_x=$2
    content_y=$3
    content_width=$4

    printf '%s\n' "$scene" | awk \
        -v target="$sample_pid" \
        -v content_x="$content_x" \
        -v content_y="$content_y" \
        -v content_width="$content_width" '
        /^[[:space:]]*View [0-9]+ / {
            active = index($0, "PID " target ",") != 0 &&
                     index($0, "role wl_subsurface") != 0
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
            # gsub produces strings; compare coordinates numerically even
            # when the titlebar crosses a decimal digit boundary.
            x1 += 0
            y1 += 0
            x2 += 0
            y2 += 0
            if (x1 == content_x && x2 == content_x + content_width &&
                y2 == content_y && y1 < y2) {
                print x2 - 16, y1 + (y2 - y1) / 2
                exit
            }
            active = 0
        }
    '
}

wait_for_ready_view() {
    attempt=0
    while [ "$attempt" -lt 1000 ]; do
        current_scene=$(scene_graph || :)
        ready_title=$(scenario_title "$current_scene")
        current_state=$(view_state "$current_scene" || :)
        case "$ready_title" in
            boxrig-scenario-ready-[0-9]*-[0-9]*)
                geometry=${ready_title#boxrig-scenario-ready-}
                cell_height=${geometry%%-*}
                cell_width=${geometry#*-}
                case "$cell_height:$cell_width" in
                    *[!0-9:]* | :* | *:)
                        ;;
                    *)
                        if [ "$cell_height" -gt 0 ] &&
                            [ "$cell_width" -gt 0 ] &&
                            [ -n "$current_state" ]; then
                            set -- $current_state
                            if [ "$1" -eq "$3" ] &&
                                [ "$2" -eq "$4" ] &&
                                [ "$1" -ge $((80 * cell_width)) ] &&
                                [ "$2" -ge $((24 * cell_height)) ]; then
                                return 0
                            fi
                        fi
                        ;;
                esac
                ;;
        esac
        if ! sample_is_live; then
            return 1
        fi
        attempt=$((attempt + 1))
        sleep 0.01
    done
    return 1
}

wait_for_title() {
    expected=$1
    attempt=0
    while [ "$attempt" -lt 1000 ]; do
        current_scene=$(scene_graph || :)
        observed=$(scenario_title "$current_scene")
        if [ "$observed" = "$expected" ]; then
            return 0
        fi
        case "$observed" in
            boxrig-scenario-unexpected-*)
                echo "$frontend Wayland scenario reported $observed" >&2
                return 1
                ;;
        esac
        if ! sample_is_live; then
            return 1
        fi
        attempt=$((attempt + 1))
        sleep 0.01
    done
    return 1
}

wait_for_larger_view() {
    previous_width=$1
    previous_height=$2
    attempt=0
    while [ "$attempt" -lt 1000 ]; do
        current_scene=$(scene_graph || :)
        current_state=$(view_state "$current_scene" || :)
        if [ -n "$current_state" ]; then
            set -- $current_state
            if [ "$1" -gt "$previous_width" ] &&
                [ "$2" -gt "$previous_height" ] &&
                [ "$1" -eq "$3" ] && [ "$2" -eq "$4" ]; then
                return 0
            fi
        fi
        if ! sample_is_live; then
            return 1
        fi
        attempt=$((attempt + 1))
        sleep 0.01
    done
    return 1
}

wait_for_view_dimensions() {
    expected_width=$1
    expected_height=$2
    attempt=0
    while [ "$attempt" -lt 1000 ]; do
        current_scene=$(scene_graph || :)
        current_state=$(view_state "$current_scene" || :)
        if [ -n "$current_state" ]; then
            set -- $current_state
            if [ "$1" -eq "$expected_width" ] &&
                [ "$2" -eq "$expected_height" ] &&
                [ "$1" -eq "$3" ] && [ "$2" -eq "$4" ]; then
                return 0
            fi
        fi
        if ! sample_is_live; then
            return 1
        fi
        attempt=$((attempt + 1))
        sleep 0.01
    done
    return 1
}

start_sample() {
    sample_log=$1
    setsid env \
        XDG_RUNTIME_DIR="$runtime_dir" \
        WAYLAND_DISPLAY="$wayland_socket" \
        WAYLAND_DEBUG="$sample_wayland_debug" \
        LIBDECOR_PLUGIN_DIR="$plugin_dir" \
        BOXRIG_WAYLAND_DECORATION=client \
        BOXRIG_BACKGROUND_OPACITY=100 \
        BOXRIG_COLOR_FILTER_DEMO="$color_filter_demo" \
        GDK_BACKEND=wayland \
        GSK_RENDERER=cairo \
        ${gtk_im_module:+GTK_IM_MODULE=$gtk_im_module} \
        NO_AT_BRIDGE=1 \
        BOXRIG_TEST_SAMPLE_PENDING_CLIPBOARD="$pending_clipboard" \
        "$sample_binary" ${sample_renderer:+--renderer=$sample_renderer} \
        -- "$pty_helper" --sample-scenario \
        >"$sample_log" 2>&1 &
    sample_pid=$!
}

wait_for_sample_exit() {
    attempt=0
    while [ "$attempt" -lt 1500 ]; do
        if [ "$native_frontend" = false ] && [ "$pending_clipboard" != 0 ] &&
            [ -n "$clipboard_peer_pid" ] &&
            [ "$(sed -n "/^READY sample pending $frontend clipboard callback after teardown\$/p" "$sample_log")" = \
                "READY sample pending $frontend clipboard callback after teardown" ]; then
            release_pending_clipboard || return 1
        fi
        if ! sample_is_live; then
            sample_status=0
            wait "$sample_pid" || sample_status=$?
            sample_pid=
            return "$sample_status"
        fi
        attempt=$((attempt + 1))
        sleep 0.01
    done
    return 1
}

move_pointer() {
    xdotool mousemove --sync --window "$weston_window" "$1" "$2"
}

position_sample_for_input() {
    set -- $current_state
    placement_width=$1
    placement_height=$2
    placement_x=$7
    placement_y=$8
    placement_pointer_x=$((placement_x + placement_width / 2))
    placement_pointer_y=$((placement_y + placement_height / 2))
    # A later toplevel can be initially placed partly outside the output.
    # Start on its visible portion and use Weston's native Super+left move.
    [ "$placement_pointer_x" -le 980 ] || placement_pointer_x=980
    [ "$placement_pointer_y" -le 620 ] || placement_pointer_y=620
    [ "$placement_pointer_x" -gt "$placement_x" ] &&
        [ "$placement_pointer_y" -gt "$placement_y" ] || return 1
    move_pointer "$placement_pointer_x" "$placement_pointer_y"
    xdotool keydown Super_L mousedown 1
    move_pointer $((placement_pointer_x + 40 - placement_x)) \
        $((placement_pointer_y + 70 - placement_y))
    placement_attempt=0
    placement_complete=false
    while [ "$placement_attempt" -lt 1000 ]; do
        current_scene=$(scene_graph || :)
        current_state=$(view_state "$current_scene" || :)
        if [ -n "$current_state" ]; then
            set -- $current_state
            if [ "$7" -eq 40 ] && [ "$8" -eq 70 ] &&
                [ "$1" -eq "$placement_width" ] &&
                [ "$2" -eq "$placement_height" ]; then
                placement_complete=true
                break
            fi
        fi
        sample_is_live || break
        placement_attempt=$((placement_attempt + 1))
        sleep 0.01
    done
    xdotool mouseup 1 keyup Super_L
    [ "$placement_complete" = true ]
}

drag_from_pointer() {
    modifier=$1
    end_x=$2
    end_y=$3

    if [ -n "$modifier" ]; then
        xdotool keydown "$modifier"
    fi
    xdotool mousedown 1
    xdotool mousemove --sync --window "$weston_window" "$end_x" "$end_y"
    xdotool mouseup 1
    if [ -n "$modifier" ]; then
        xdotool keyup "$modifier"
    fi
}

copy_and_paste() {
    expected=$1
    xdotool key --clearmodifiers ctrl+shift+c
    current_scene=$(scene_graph) || return 1
    xdotool key --clearmodifiers ctrl+shift+v
    wait_for_title "$expected"
}

start_pending_clipboard() {
    clipboard_peer_log=$sample_log.peer.log
    mkfifo "$sample_log.peer-control"
    exec 9<>"$sample_log.peer-control"
    env XDG_RUNTIME_DIR="$runtime_dir" WAYLAND_DISPLAY="$wayland_socket" \
        WAYLAND_DEBUG=client GDK_BACKEND=wayland NO_AT_BRIDGE=1 \
        "$clipboard_peer" hold clipboard late-paste \
        <"$sample_log.peer-control" >"$clipboard_peer_log" 2>&1 9>&- &
    clipboard_peer_pid=$!
    peer_state=
    attempt=0
    while [ "$attempt" -lt 1000 ]; do
        current_scene=$(scene_graph || :)
        peer_state=$(
            sample_pid=$clipboard_peer_pid
            native_frontend=false
            view_state "$current_scene" || :
        )
        [ -z "$peer_state" ] || break
        kill -0 "$clipboard_peer_pid" 2>/dev/null || return 1
        sample_is_live || return 1
        attempt=$((attempt + 1))
        sleep 0.01
    done
    [ -n "$peer_state" ] || return 1
    if ! wayland_log_has_keyboard_focus "$clipboard_peer_log"; then
        # Select only a visible part of the peer. Its center can be covered by
        # the sample, whose mouse-reporting child must not receive this click.
        peer_focus_point=$(uncovered_clipboard_peer_point \
            "$current_scene" "$peer_state")
        [ -n "$peer_focus_point" ] || return 1
        set -- $peer_focus_point
        xdotool mousemove --sync --window "$weston_window" "$1" "$2" click 1
    fi
    xdotool key --clearmodifiers c
    attempt=0
    while [ "$attempt" -lt 1000 ]; do
        current_scene=$(scene_graph || :)
        peer_state=$(
            sample_pid=$clipboard_peer_pid
            native_frontend=false
            view_state "$current_scene" || :
        )
        if [ -z "$peer_state" ] &&
            [ "$(sed -n '/^OWNED$/p' "$clipboard_peer_log")" = OWNED ]; then
            break
        fi
        kill -0 "$clipboard_peer_pid" 2>/dev/null || return 1
        sample_is_live || return 1
        attempt=$((attempt + 1))
        sleep 0.01
    done
    [ "$attempt" -lt 1000 ] || return 1
    # The peer has unmapped. If Weston has not restored keyboard focus, use its
    # task switcher without sending a click to the sample's mouse-reporting
    # child, then observe the corresponding Wayland keyboard event.
    if ! wayland_log_has_keyboard_focus "$sample_log"; then
        xdotool key --clearmodifiers super+Tab
        wait_for_sample_keyboard_focus || return 1
    fi
    xdotool key --clearmodifiers ctrl+shift+v
    attempt=0
    while [ "$attempt" -lt 1000 ]; do
        if [ "$(sed -n '/^REQUEST$/p' "$clipboard_peer_log")" = REQUEST ]; then
            current_scene=$(scene_graph || :)
            current_state=$(view_state "$current_scene" || :)
            [ -n "$current_state" ]
            return
        fi
        kill -0 "$clipboard_peer_pid" 2>/dev/null || return 1
        sample_is_live || return 1
        attempt=$((attempt + 1))
        sleep 0.01
    done
    return 1
}

release_pending_clipboard() {
    printf R >&9
    exec 9>&-
    attempt=0
    while [ "$attempt" -lt 1000 ]; do
        peer_state=$(ps -o stat= -p "$clipboard_peer_pid" 2>/dev/null || :)
        case "$peer_state" in
            '' | Z*) break ;;
        esac
        attempt=$((attempt + 1))
        sleep 0.01
    done
    [ "$attempt" -lt 1000 ] || return 1
    peer_status=0
    wait "$clipboard_peer_pid" || peer_status=$?
    clipboard_peer_pid=
    [ "$peer_status" -eq 0 ] &&
        [ "$(sed -n '/^RELEASED$/p' "$clipboard_peer_log")" = RELEASED ]
}

overlay_hash() {
    "$pixel_probe" --hash "$weston_window" \
        $((content_x + $1 * cell_width)) $((content_y + $2 * cell_height)) \
        $(($3 * cell_width)) "$cell_height"
}

overlay_pixel() {
    "$pixel_probe" "$weston_window" \
        $((content_x + $1 * cell_width + cell_width / 2)) \
        $((content_y + $2 * cell_height + cell_height / 2))
}

wait_for_overlay() {
    overlay_attempt=0
    while [ "$overlay_attempt" -lt 1500 ]; do
        if [ "$3" -eq 0 ]; then
            overlay_observed=$(overlay_pixel "$1" "$2" || :)
        else
            overlay_observed=$(overlay_hash "$1" "$2" "$3" || :)
        fi
        if [ -n "$overlay_observed" ]; then
            if [ "$5" = equal ] && [ "$overlay_observed" = "$4" ]; then
                return 0
            elif [ "$5" = different ] && [ "$overlay_observed" != "$4" ]; then
                return 0
            fi
        fi
        if ! sample_is_live; then
            return 1
        fi
        overlay_attempt=$((overlay_attempt + 1))
        sleep 0.01
    done
    echo "overlay $1,$2 width $3: $overlay_observed expected $5 $4" >&2
    return 1
}

run_color_demo() {
    # The key acknowledgement precedes this call, so the PTY scene is ready.
    # Sample a text row away from the pointer, cursor and native decoration.
    # The opaque image cell reaches exact red only after Weston finishes its
    # opening fade; a title acknowledgement alone can precede that frame.
    wait_for_overlay 0 5 0 '255 0 0' equal || return 1
    wait_for_overlay 30 18 0 '64 64 64' equal || return 1
    xdotool key --clearmodifiers ctrl+shift+F12
    wait_for_overlay 30 18 0 '16 16 16' equal || return 1
    plain_colors=$(overlay_hash 0 1 12) || return 1
    xdotool key --clearmodifiers ctrl+shift+F12
    wait_for_overlay 0 1 12 "$plain_colors" equal || return 1
    xdotool key --clearmodifiers ctrl+shift+F12
    wait_for_overlay 30 18 0 '64 64 64' equal || return 1
    filtered_colors=$(overlay_hash 0 1 12) || return 1
    [ "$plain_colors" != "$filtered_colors" ] || return 1
    xdotool key --clearmodifiers ctrl+shift+F12
    wait_for_overlay 0 1 12 "$plain_colors" equal || return 1
    xdotool key --clearmodifiers ctrl+shift+F12
    wait_for_overlay 0 1 12 "$plain_colors" equal || return 1
    xdotool key --clearmodifiers ctrl+shift+F12
    wait_for_overlay 0 1 12 "$filtered_colors" equal || return 1
    xdotool key --clearmodifiers ctrl+shift+F12
    wait_for_overlay 30 18 0 '16 16 16' equal || return 1
    color_filter_demo=0
    echo "PASS $frontend sample color filter toggle and identity pixels through Wayland"
}

run_overlays() {
    wait_for_overlay 14 11 0 '32 80 128' equal &&
        wait_for_overlay 0 14 0 '0 255 0' equal || return 1
    # The hover range excludes the native pointer's own pixels.
    overlay_link=$(overlay_hash 8 9 19) || return 1
    overlay_search=$(overlay_hash 0 11 14) || return 1
    overlay_preedit=$(overlay_hash 0 13 16) || return 1
    overlay_background=$(overlay_pixel 30 18) || return 1
    move_pointer $((content_x + cell_width + cell_width / 2)) \
        $((content_y + 9 * cell_height + cell_height / 2))
    wait_for_overlay 8 9 19 "$overlay_link" different || return 1
    move_pointer $((content_x + cell_width + cell_width / 2)) \
        $((content_y + 10 * cell_height + cell_height / 2))
    wait_for_overlay 8 9 19 "$overlay_link" equal || return 1
    move_pointer $((content_x + 14 * cell_width + cell_width / 2)) \
        $((content_y + 11 * cell_height + cell_height / 2))
    xdotool key --clearmodifiers v
    wait_for_title boxrig-scenario-overlay-ime-received || return 1
    # Weston composites its pointer into captures. The child acknowledgement
    # proves all hover targets were observed before moving it off the markers.
    move_pointer $((content_x + 40 * cell_width + cell_width / 2)) \
        $((content_y + 20 * cell_height + cell_height / 2))
    wait_for_overlay 14 11 0 '128 64 32' equal &&
        wait_for_overlay 0 14 0 '0 0 255' equal &&
        wait_for_overlay 0 11 14 "$overlay_search" different &&
        wait_for_overlay 0 13 16 "$overlay_preedit" different || return 1
    overlay_cursor=$(overlay_hash 0 13 16) || return 1
    xdotool key --clearmodifiers ctrl+less
    wait_for_overlay 30 18 0 "$overlay_background" different || return 1
    xdotool key --clearmodifiers v
    wait_for_title boxrig-scenario-overlay-opacity-received || return 1
    xdotool key --clearmodifiers ctrl+greater
    wait_for_overlay 30 18 0 "$overlay_background" equal || return 1
    xdotool key --clearmodifiers v
    wait_for_title boxrig-scenario-overlay-cleared || return 1
    wait_for_overlay 14 11 0 "$overlay_background" equal &&
        wait_for_overlay 0 14 0 "$overlay_background" equal &&
        wait_for_overlay 0 13 16 "$overlay_cursor" different || return 1
    xdotool key --clearmodifiers v
    wait_for_title boxrig-scenario-overlay-verified
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

required_commands="xdotool weston-debug awk sed ps"
if [ "$pending_profile" = 1 ]; then
    if [ ! -x "$clipboard_peer" ]; then
        fail "pending Wayland sample profile requires BOXRIG_TEST_CLIPBOARD_PEER"
    fi
    required_commands="$required_commands mkfifo"
fi
if [ "$native_frontend" = true ]; then
    required_commands="$required_commands pkg-config"
fi
for command in $required_commands; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "$frontend Wayland scenario test requires $command" >&2
        exit 1
    fi
done
if [ ! -x "$pixel_probe" ]; then
    echo "$frontend Wayland scenario test requires $pixel_probe" >&2
    exit 1
fi

if [ "$native_frontend" = true ]; then
    libdecor_libdir=$(pkg-config --variable=libdir 'libdecor-0 >= 0.2.0')
    cairo_plugin=$libdecor_libdir/libdecor/plugins-1/libdecor-cairo.so
    if [ ! -f "$cairo_plugin" ]; then
        echo "$frontend Wayland scenario test requires $cairo_plugin" >&2
        exit 1
    fi
fi

chmod 700 "$runtime_dir"
mkdir "$plugin_dir"
if [ "$native_frontend" = true ]; then
    ln -s "$cairo_plugin" "$plugin_dir/libdecor-cairo.so"
fi
XDG_RUNTIME_DIR=$runtime_dir "$weston" \
    --debug \
    --backend=x11 \
    --renderer="${BOXRIG_TEST_WESTON_RENDERER:-pixman}" \
    --socket="$wayland_socket" \
    --width=1000 \
    --height=650 \
    --idle-time=0 \
    --no-config \
    --log="$weston_log" &
weston_pid=$!

attempt=0
while [ "$attempt" -lt 500 ] && [ ! -S "$runtime_dir/$wayland_socket" ]; do
    if ! kill -0 "$weston_pid" 2>/dev/null; then
        fail "Weston exited before creating the $frontend scenario socket"
    fi
    attempt=$((attempt + 1))
    sleep 0.01
done
if [ ! -S "$runtime_dir/$wayland_socket" ]; then
    fail "Weston did not create the $frontend scenario socket"
fi

weston_window=
attempt=0
while [ "$attempt" -lt 500 ] && [ -z "$weston_window" ]; do
    weston_window=$(sed -n \
        's/.*x11 output [^,]*, window id \([0-9][0-9]*\)$/\1/p' \
        "$weston_log" | tail -n 1)
    attempt=$((attempt + 1))
    sleep 0.01
done
if [ -z "$weston_window" ]; then
    fail "Weston did not create the $frontend scenario X11 window"
fi
xdotool windowfocus --sync "$weston_window"

resize_view_by_drag() {
    delta_width=$1
    delta_height=$2
    current_scene=$(scene_graph) || return 1
    current_state=$(view_state "$current_scene") || return 1
    set -- $current_state
    resized_width=$(($1 + delta_width))
    resized_height=$(($2 + delta_height))
    drag_x=$(($7 + $1 - 30))
    drag_y=$(($8 + $2 - 30))
    # Weston chooses the right/bottom edges in the last third of the view.
    # Super+right-button is its native desktop-shell resize binding.
    # A grow drag ends exactly where the inverse drag starts. Force actual
    # motion so Weston refreshes pointer focus after the resized surface commit.
    move_pointer $((drag_x - 1)) "$drag_y"
    move_pointer "$drag_x" "$drag_y"
    xdotool keydown Super_L mousedown 3
    move_pointer $((drag_x + delta_width)) $((drag_y + delta_height))
    resized=false
    if wait_for_view_dimensions "$resized_width" "$resized_height"; then
        resized=true
    fi
    if [ "$resized" = true ] && [ "$frontend" = GTK3 ]; then
        # GTK3 saves the previous extent before a compositor resize, and
        # Weston sends 0x0 when releasing its grab. A final motion inside the
        # same resize increment saves the fully reached extent for release.
        # Observe the client's real acknowledgement instead of delaying.
        previous_configure=$(sed -n \
            's/.*xdg_surface[^ ]*\.ack_configure(\([0-9][0-9]*\)).*/\1/p' \
            "$sample_log" | tail -n 1)
        move_pointer $((drag_x + delta_width + 1)) $((drag_y + delta_height))
        attempt=0
        acknowledged=false
        while [ "$attempt" -lt 1000 ] && [ -n "$previous_configure" ]; do
            latest_configure=$(sed -n \
                's/.*xdg_surface[^ ]*\.ack_configure(\([0-9][0-9]*\)).*/\1/p' \
                "$sample_log" | tail -n 1)
            if [ -n "$latest_configure" ] &&
                [ "$latest_configure" != "$previous_configure" ]; then
                acknowledged=true
                break
            fi
            sample_is_live || break
            attempt=$((attempt + 1))
            sleep 0.01
        done
        [ "$acknowledged" = true ] || resized=false
    fi
    xdotool mouseup 3 keyup Super_L
    wait_for_view_dimensions "$resized_width" "$resized_height"
}

run_geometry() {
    resize_view_by_drag $((8 * cell_width)) $((4 * cell_height)) || return 1
    xdotool key --clearmodifiers r
    wait_for_title boxrig-scenario-geometry-resize-grid-28-88 || return 1
    resize_view_by_drag $((-8 * cell_width)) $((-4 * cell_height)) || return 1
    wait_for_view_dimensions "$initial_width" "$initial_height" || return 1
    xdotool key --clearmodifiers r
    wait_for_title boxrig-scenario-geometry-resize-reset-grid-24-80 || return 1

    xdotool key --clearmodifiers super+shift+m
    wait_for_larger_view "$initial_width" "$initial_height" || return 1
    xdotool key --clearmodifiers m
    attempt=0
    maximized_rows=
    maximized_columns=
    while [ "$attempt" -lt 1000 ]; do
        current_scene=$(scene_graph || :)
        observed=$(scenario_title "$current_scene")
        case "$observed" in
            boxrig-scenario-geometry-maximize-grid-[0-9]*-[0-9]*)
                grid=${observed#boxrig-scenario-geometry-maximize-grid-}
                rows=${grid%%-*}
                columns=${grid#*-}
                [ "$rows" -gt 24 ] && [ "$columns" -gt 80 ] || return 1
                maximized_rows=$rows
                maximized_columns=$columns
                break
                ;;
            boxrig-scenario-unexpected-*)
                echo "$frontend Wayland scenario reported $observed" >&2
                return 1
                ;;
        esac
        sample_is_live || return 1
        attempt=$((attempt + 1))
        sleep 0.01
    done
    [ "$attempt" -lt 1000 ] || return 1

    xdotool key --clearmodifiers ctrl+minus
    xdotool key --clearmodifiers z
    attempt=0
    last_pending=
    while [ "$attempt" -lt 1000 ]; do
        current_scene=$(scene_graph || :)
        observed=$(scenario_title "$current_scene")
        case "$observed" in
            boxrig-scenario-pending-geometry-maximize-zoom-[0-9]*)
                if [ "$observed" != "$last_pending" ]; then
                    xdotool key --clearmodifiers z
                    last_pending=$observed
                fi
                ;;
            boxrig-scenario-geometry-maximize-zoom-grid-[0-9]*-[0-9]*)
                grid=${observed#boxrig-scenario-geometry-maximize-zoom-grid-}
                rows=${grid%%-*}
                columns=${grid#*-}
                if [ "$rows" -le "$maximized_rows" ] &&
                    [ "$columns" -le "$maximized_columns" ]; then
                    return 1
                fi
                break
                ;;
            boxrig-scenario-unexpected-*)
                echo "$frontend Wayland scenario reported $observed" >&2
                return 1
                ;;
        esac
        sample_is_live || return 1
        attempt=$((attempt + 1))
        sleep 0.01
    done
    [ "$attempt" -lt 1000 ] || return 1

    xdotool key --clearmodifiers super+shift+m
    xdotool key --clearmodifiers m
    attempt=0
    last_pending=
    while [ "$attempt" -lt 1000 ]; do
        current_scene=$(scene_graph || :)
        observed=$(scenario_title "$current_scene")
        case "$observed" in
            boxrig-scenario-pending-geometry-maximize-reset-[0-9]*)
                if [ "$observed" != "$last_pending" ]; then
                    xdotool key --clearmodifiers m
                    last_pending=$observed
                fi
                ;;
            boxrig-scenario-geometry-maximize-reset-grid-24-80)
                return 0
                ;;
            boxrig-scenario-unexpected-*)
                echo "$frontend Wayland scenario reported $observed" >&2
                return 1
                ;;
        esac
        sample_is_live || return 1
        attempt=$((attempt + 1))
        sleep 0.01
    done
    return 1
}

for child_profile in child-exit pending-child-exit; do
    if [ "$child_profile" = pending-child-exit ] && [ "$pending_profile" != 1 ]; then
        continue
    fi
    pending_clipboard=0
    if [ "$child_profile" = pending-child-exit ]; then
        pending_clipboard=1
    fi
    start_sample "$runtime_dir/$child_profile-sample.log"
    if ! wait_for_ready_view; then
        fail "$frontend Wayland sample did not publish the shared ready scene"
    fi
    if ! position_sample_for_input; then
        fail "$frontend Wayland sample did not reach an operable placement"
    fi
    set -- $current_state
    initial_width=$1
    initial_height=$2
    if ! locate_terminal_viewport; then
        fail "$frontend Wayland sample did not present the shared image cell"
    fi

    xdotool mousemove --window "$weston_window" \
        $((content_x + 20 * cell_width)) \
        $((content_y + 8 * cell_height)) click 1
    xdotool key --clearmodifiers k
    if ! wait_for_title boxrig-scenario-key-received; then
        fail "$frontend Wayland sample did not deliver the shared key"
    fi
    if [ "$color_filter_demo" = 1 ] && ! run_color_demo; then
        fail "$frontend Wayland sample did not satisfy the color filter profile"
    fi

    move_pointer \
        $((content_x + cell_width / 4)) \
        $((content_y + cell_height / 2))
    drag_from_pointer "" \
        $((content_x + 10 * cell_width + 3 * cell_width / 4)) \
        $((content_y + cell_height / 2))
    if ! copy_and_paste boxrig-scenario-linear-copy-received; then
        fail "$frontend Wayland sample did not round-trip the linear selection"
    fi
    xdotool key --clearmodifiers m
    if ! wait_for_title boxrig-scenario-mouse-reporting-ready; then
        fail "$frontend Wayland sample did not cross the mouse-reporting barrier"
    fi
    move_pointer \
        $((content_x + 20 * cell_width + cell_width / 2)) \
        $((content_y + 8 * cell_height + cell_height / 2))
    xdotool click 1
    if ! wait_for_title boxrig-scenario-mouse-received; then
        fail "$frontend Wayland sample did not deliver the shared mouse click"
    fi
    move_pointer \
        $((content_x + cell_width / 4)) \
        $((content_y + 2 * cell_height + cell_height / 2))
    drag_from_pointer Shift_L \
        $((content_x + 10 * cell_width + 3 * cell_width / 4)) \
        $((content_y + 2 * cell_height + cell_height / 2))
    if ! copy_and_paste boxrig-scenario-shift-copy-received; then
        fail "$frontend Wayland sample did not round-trip the Shift selection override"
    fi

    move_pointer \
        $((content_x + cell_width / 4)) \
        $((content_y + cell_height / 2))
    drag_from_pointer Control_L \
        $((content_x + 4 * cell_width + 3 * cell_width / 4)) \
        $((content_y + cell_height + cell_height / 2))
    if ! copy_and_paste boxrig-scenario-rectangle-copy-received; then
        fail "$frontend Wayland sample did not round-trip the rectangular selection override"
    fi

    xdotool key --clearmodifiers ctrl+plus
    if ! wait_for_larger_view "$initial_width" "$initial_height"; then
        fail "$frontend Wayland sample did not synchronize its zoomed surfaces"
    fi
    xdotool key --clearmodifiers g
    if ! wait_for_title boxrig-scenario-zoom-grid-24-80; then
        fail "$frontend Wayland sample did not preserve the grid while zooming"
    fi

    xdotool key --clearmodifiers ctrl+0
    if ! wait_for_view_dimensions "$initial_width" "$initial_height"; then
        fail "$frontend Wayland sample did not restore its initial surface geometry"
    fi
    xdotool key --clearmodifiers g
    reset_title=boxrig-scenario-reset-grid-24-80
    if [ "${BOXRIG_TEST_SAMPLE_OVERLAYS:-0}" = 1 ]; then
        reset_title=boxrig-scenario-overlay-ready
    fi
    if ! wait_for_title "$reset_title"; then
        fail "$frontend Wayland sample did not preserve the grid after zoom reset"
    fi
    if [ "${BOXRIG_TEST_SAMPLE_OVERLAYS:-0}" = 1 ] && ! run_overlays; then
        fail "$frontend Wayland sample did not satisfy the overlay profile"
    fi
    if [ "${BOXRIG_TEST_SAMPLE_GEOMETRY:-0}" = 1 ] && ! run_geometry; then
        fail "$frontend Wayland sample did not converge resize/maximize geometry"
    fi

    if [ "$pending_clipboard" = 1 ] && ! start_pending_clipboard; then
        fail "$frontend sample did not start an external clipboard read before child exit"
    fi
    xdotool key --clearmodifiers q
    if ! wait_for_sample_exit; then
        fail "$frontend Wayland sample did not preserve controlled-child shutdown"
    fi
    if [ "$pending_clipboard" = 1 ]; then
        if [ -n "$clipboard_peer_pid" ]; then
            release_pending_clipboard || fail "$frontend clipboard peer did not release after child exit"
        fi
        echo "PASS $frontend sample cancels a pending native clipboard read on child exit"
    fi
done

for close_profile in window-close copy-close pending-close; do
    if [ "$close_profile" = pending-close ] && [ "$pending_profile" != 1 ]; then
        continue
    fi
    pending_clipboard=0
    if [ "$close_profile" = pending-close ]; then
        pending_clipboard=close
    fi
    current_scene=
    current_state=
    ready_title=
    start_sample "$runtime_dir/$close_profile-sample.log"
    if ! wait_for_ready_view; then
        fail "$frontend Wayland sample did not publish the $close_profile ready scene"
    fi
    if ! position_sample_for_input; then
        fail "$frontend Wayland sample did not reach an operable close-path placement"
    fi
    if [ "$pending_clipboard" = close ]; then
        if ! locate_terminal_viewport; then
            fail "$frontend sample did not present its pending-close content"
        fi
        xdotool mousemove --window "$weston_window" \
            $((content_x + 20 * cell_width)) \
            $((content_y + 8 * cell_height)) click 1
        xdotool key --clearmodifiers k
        if ! wait_for_title boxrig-scenario-key-received || ! start_pending_clipboard; then
            fail "$frontend sample did not start an external clipboard read before window close"
        fi
    fi
    if [ "$close_profile" = copy-close ]; then
        if ! locate_terminal_viewport; then
            fail "$frontend Wayland sample did not present its copy-close content"
        fi
        xdotool mousemove --window "$weston_window" \
            $((content_x + 20 * cell_width)) \
            $((content_y + 8 * cell_height)) click 1
        xdotool key --clearmodifiers k
        if ! wait_for_title boxrig-scenario-key-received; then
            fail "$frontend Wayland sample did not deliver the copy-close key"
        fi
        move_pointer \
            $((content_x + cell_width / 4)) \
            $((content_y + cell_height / 2))
        drag_from_pointer "" \
            $((content_x + 10 * cell_width + 3 * cell_width / 4)) \
            $((content_y + cell_height / 2))
        if ! copy_and_paste boxrig-scenario-linear-copy-received; then
            fail "$frontend Wayland sample did not finish paste before native close"
        fi
    fi
    if [ "$native_frontend" = true ]; then
        # Match the decoration and root coordinates from the same scene after
        # clipboard completion, not the earlier ready-scene placement.
        current_state=$(view_state "$current_scene" || :)
        if [ -z "$current_state" ]; then
            fail "$frontend sample did not expose its current close-path surface"
        fi
        set -- $current_state
        root_width=$1
        root_x=$7
        root_y=$8
        close_point=$(close_button_point \
            "$current_scene" "$root_x" "$root_y" "$root_width")
        if [ -z "$close_point" ]; then
            fail "$frontend sample did not expose its client decoration close button"
        fi
        set -- $close_point
        xdotool mousemove --window "$weston_window" "$1" "$2" click 1
    else
        if ! locate_terminal_viewport; then
            fail "$frontend Wayland sample did not present its close-path content"
        fi
        set -- $current_state
        root_width=$1
        root_x=$7
        root_y=$8
        titlebar_height=$((content_y - root_y))
        if [ "$titlebar_height" -le 0 ] ||
            [ "$titlebar_height" -ge "$root_width" ]; then
            fail "$frontend sample did not expose its client decoration titlebar"
        fi
        xdotool mousemove --window "$weston_window" \
            $((root_x + root_width - titlebar_height / 2)) \
            $((root_y + titlebar_height / 2)) click 1
    fi
    if ! wait_for_sample_exit; then
        fail "$frontend Wayland sample did not converge after $close_profile"
    fi
    if [ "$pending_clipboard" = close ]; then
        if [ -n "$clipboard_peer_pid" ]; then
            release_pending_clipboard || fail "$frontend clipboard peer did not release after window close"
        fi
        echo "PASS $frontend sample cancels a pending native clipboard read on window close"
    fi
done

echo "PASS $frontend sample closes after native clipboard copy and completed paste through Wayland"
echo "PASS $frontend sample follows shared PTY scene input selection geometry and shutdown through Wayland"
