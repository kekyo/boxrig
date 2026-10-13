#!/bin/bash
set -euo pipefail
build_dir=$(realpath "${1:-build}")
result_dir=$(realpath -m "${2:-build/test-results/step25/25-1}")
mkdir -p "$result_dir"
probe_runtime=$(mktemp -d "${TMPDIR:-/tmp}/boxrig-surrounding.XXXXXX")
ibus_pid=
engine_pid=
cleanup() {
    for pid in "$engine_pid" "$ibus_pid"; do
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || :
            wait "$pid" 2>/dev/null || :
        fi
    done
    # Mozc may daemonize. Stop only processes using this run's private profile.
    node --input-type=module - "$probe_runtime/config" <<'NODE'
import fs from 'node:fs';
const profile = process.argv[2];
for (const entry of fs.readdirSync('/proc')) {
    if (!/^\d+$/.test(entry)) continue;
    try {
        const name = fs.readFileSync(`/proc/${entry}/comm`, 'utf8').trim();
        if (!['mozc_server', 'mozc_renderer', 'ibus-engine-moz'].includes(name)) continue;
        const env = fs.readFileSync(`/proc/${entry}/environ`, 'utf8').split('\0');
        if (env.includes(`XDG_CONFIG_HOME=${profile}`)) process.kill(Number(entry), 'SIGTERM');
    } catch { /* An already exited process needs no cleanup. */ }
}
NODE
    rm -r "$probe_runtime"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
export IBUS_ADDRESS="unix:path=$probe_runtime/ibus.sock"
export XDG_CONFIG_HOME="$probe_runtime/config"
export XDG_CACHE_HOME="$probe_runtime/cache"
export XDG_DATA_HOME="$probe_runtime/data"
export GSETTINGS_BACKEND=memory GTK_IM_MODULE=ibus XMODIFIERS=@im=ibus
export NO_AT_BRIDGE=1 GTK_A11Y=none
unset IBUS_USE_PORTAL
if [ -d "$HOME/.mozc" ] && [ "${BOXRIG_PROBE_WIDGET:-0}" = 1 ]; then
    echo 'A legacy ~/.mozc profile overrides XDG_CONFIG_HOME; an isolated Mozc profile is required.' >&2
    exit 1
fi
mkdir -p "$XDG_CONFIG_HOME/mozc"
# https://github.com/google/mozc/blob/master/docs/configurations.md
cat > "$XDG_CONFIG_HOME/mozc/ibus_config.textproto" <<'EOF'
engines {
  name: "mozc-jp"
  longname: "Mozc"
  layout: "default"
  layout_variant: ""
  layout_option: ""
  rank: 80
}
active_on_launch: True
EOF
ibus-daemon --xim --single --panel=disable --config=disable \
    --emoji-extension=disable --address="$IBUS_ADDRESS" >"$result_dir/ibus.log" 2>&1 &
ibus_pid=$!
for ((attempt = 0; attempt < 3000; attempt++)); do
    if [ -S "$probe_runtime/ibus.sock" ]; then break; fi
    kill -0 "$ibus_pid"
    sleep 0.01
done
"$build_dir/tests/surrounding_engine" "$probe_runtime/engine-ready" \
    >"$result_dir/engine.log" 2>&1 &
engine_pid=$!
for ((attempt = 0; attempt < 3000; attempt++)); do
    if [ -f "$probe_runtime/engine-ready" ]; then break; fi
    kill -0 "$engine_pid"
    sleep 0.01
done
test -f "$probe_runtime/engine-ready"
if [ "${GDK_BACKEND:-x11}" = x11 ]; then
    xim_ready=0
    for ((attempt = 0; attempt < 3000; attempt++)); do
        if "$build_dir/tests/ime_capabilities" xim >"$result_dir/xim.log"; then
            xim_ready=1
            break
        fi
        kill -0 "$ibus_pid"
        sleep 0.01
    done
    test "$xim_ready" = 1
    cat "$result_dir/xim.log"
fi
if [ "${BOXRIG_PROBE_XIM_ONLY:-0}" = 1 ]; then exit 0; fi
for gtk in ${BOXRIG_PROBE_GTK_VERSIONS:-3 4}; do
    if [ "${BOXRIG_PROBE_WIDGET:-0}" = 1 ]; then
        probe_modes=${BOXRIG_PROBE_MODES:-none empty editable attach readonly reject busy mozc mozc-none}
        if [ "$gtk" = 4 ] && [ -z "${BOXRIG_PROBE_MODES:-}" ]; then
            probe_modes="$probe_modes mozc-default mozc-default-none"
        fi
        for renderer in ${BOXRIG_PROBE_RENDERERS:-cairo opengl}; do
            for mode in $probe_modes; do
                log="$result_dir/gtk$gtk-$renderer-$mode.log"
                probe_environment=()
                if [ "$gtk" = 4 ] && { [ "$mode" = mozc ] || [ "$mode" = mozc-none ]; }; then
                    probe_environment+=(IBUS_ENABLE_SYNC_MODE=2)
                fi
                if [[ "$mode" == mozc-default* ]]; then
                    probe_environment+=(IBUS_ENABLE_SYNC_MODE=1)
                fi
                if [ "${BOXRIG_PROBE_VIDEO:-0}" = 1 ]; then
                    probe_environment+=("BOXRIG_TEST_VIDEO=$result_dir/gtk$gtk-$renderer-$mode.mp4")
                fi
                timeout --kill-after=5 45 env "${probe_environment[@]}" "$build_dir/tests/gtk${gtk}_surrounding_widget" "$mode" "$renderer" >"$log" 2>&1 || {
                    cat "$log"
                    exit 1
                }
                if [ "$mode" = none ] || [ "$mode" = attach ] || [[ "$mode" == *-none ]]; then
                    rg -q 'has no capability of surrounding-text feature' "$log"
                elif rg -q 'has no capability of surrounding-text feature' "$log"; then
                    cat "$log"
                    exit 1
                fi
                cat "$log"
            done
        done
        continue
    fi
    for mode in ${BOXRIG_PROBE_MODES:-none empty screen editable}; do
        "$build_dir/tests/gtk${gtk}_surrounding_driver" "$mode" \
            >"$result_dir/gtk$gtk-$mode.log" 2>&1 || {
                cat "$result_dir/gtk$gtk-$mode.log"
                exit 1
            }
        if [ "$mode" = none ]; then
            rg -q 'has no capability of surrounding-text feature' \
                "$result_dir/gtk$gtk-$mode.log"
        elif rg -q 'has no capability of surrounding-text feature' \
                "$result_dir/gtk$gtk-$mode.log"; then
            cat "$result_dir/gtk$gtk-$mode.log"
            exit 1
        fi
        cat "$result_dir/gtk$gtk-$mode.log"
    done
done
