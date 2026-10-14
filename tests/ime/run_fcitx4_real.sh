#!/usr/bin/env bash
# boxrig - Under MIT. Run with a private session bus and Xvfb display.
set -euo pipefail
probe_root=$(mktemp -d "${TMPDIR:-/tmp}/boxrig-fcitx4-real.XXXXXX")
daemon_pid=
ready_pid=
cleanup() {
    if [[ -n "$ready_pid" ]]; then
        kill "$ready_pid" 2>/dev/null || :
        wait "$ready_pid" 2>/dev/null || :
    fi
    if [[ -n "$daemon_pid" ]]; then
        kill "$daemon_pid" 2>/dev/null || :
        wait "$daemon_pid" 2>/dev/null || :
    fi
    node --input-type=module - "$probe_root/config" <<'NODE'
import fs from 'node:fs';
const profile = process.argv[2];
for (const entry of fs.readdirSync('/proc')) {
    if (!/^\d+$/.test(entry)) continue;
    try {
        const name = fs.readFileSync(`/proc/${entry}/comm`, 'utf8').trim();
        if (!['mozc_server', 'mozc_renderer'].includes(name)) continue;
        const environment = fs.readFileSync(`/proc/${entry}/environ`, 'utf8').split('\0');
        if (environment.includes(`XDG_CONFIG_HOME=${profile}`)) process.kill(Number(entry), 'SIGTERM');
    } catch { /* Processes may exit while their environment is read. */ }
}
NODE
    rm -r "$probe_root"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
# Mozc's legacy profile overrides XDG_CONFIG_HOME; never use the desktop's
# dictionary. The distribution image supplies an unprivileged, clean home.
# https://github.com/google/mozc/blob/master/src/base/system_util.cc
if [[ -d "$HOME/.mozc" ]]; then
    echo 'A legacy ~/.mozc profile prevents this test from isolating Mozc.' >&2
    exit 1
fi
export XDG_CONFIG_HOME="$probe_root/config" XDG_CACHE_HOME="$probe_root/cache"
export XDG_DATA_HOME="$probe_root/data" XDG_RUNTIME_DIR="$probe_root/runtime"
export BOXRIG_TEST_FCITX_REAL=1
mkdir -p "$XDG_CONFIG_HOME/fcitx" "$XDG_CONFIG_HOME/mozc"
mkdir -m 700 "$XDG_RUNTIME_DIR"
# The 4.2.9.9 configuration descriptions define these keys and enum values:
# https://github.com/fcitx/fcitx/blob/4.2.9.9/data/profile.desc
# https://github.com/fcitx/fcitx/blob/4.2.9.9/data/config.desc
cat > "$XDG_CONFIG_HOME/fcitx/profile" <<'EOF'
[Profile]
IMName=mozc
EnabledIMList=fcitx-keyboard-us:True,mozc:True
PreeditStringInClientWindow=True
EOF
no_commit=False
if [[ -n ${BOXRIG_TEST_FCITX4_NO_FOCUS_COMMIT:-} ]]; then no_commit=True; fi
cat > "$XDG_CONFIG_HOME/fcitx/config" <<EOF
[Program]
DelayStart=0
DefaultInputMethodState=Active
[Output]
DontCommitPreeditWhenUnfocus=$no_commit
EOF
fcitx -D -s 0 > "$probe_root/daemon.log" 2>&1 &
daemon_pid=$!
display_number=${DISPLAY##*:}
service="org.fcitx.Fcitx-${display_number%%.*}"
gdbus wait --session --timeout 60 "$service" &
ready_pid=$!
if ! wait -n -p completed "$daemon_pid" "$ready_pid" || [[ "$completed" != "$ready_pid" ]]; then
    cat "$probe_root/daemon.log" >&2
    exit 1
fi
ready_pid=
if ! "$@"; then
    cat "$probe_root/daemon.log" >&2
    exit 1
fi
