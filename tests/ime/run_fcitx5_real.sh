#!/usr/bin/env bash
# boxrig - Under MIT. Run under dbus-run-session, with an isolated display.
set -euo pipefail
probe_root=$(mktemp -d "${TMPDIR:-/tmp}/boxrig-fcitx5-real.XXXXXX")
daemon_pid=
ready_pid=
cleanup() {
    if [ -n "$ready_pid" ]; then
        kill "$ready_pid" 2>/dev/null || :
        wait "$ready_pid" 2>/dev/null || :
    fi
    if [ -n "$daemon_pid" ]; then
        kill "$daemon_pid" 2>/dev/null || :
        wait "$daemon_pid" 2>/dev/null || :
    fi
    # Mozc daemonizes. Identify only children using this private profile.
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
# Mozc's documented legacy directory takes priority over XDG_CONFIG_HOME.
# https://github.com/google/mozc/blob/master/src/base/system_util.cc
if [ -d "$HOME/.mozc" ]; then
    echo 'A legacy ~/.mozc profile prevents this test from isolating Mozc.' >&2
    exit 1
fi
export XDG_CONFIG_HOME="$probe_root/config" XDG_CACHE_HOME="$probe_root/cache"
export XDG_DATA_HOME="$probe_root/data" BOXRIG_TEST_FCITX_REAL=1
mkdir -p "$XDG_CONFIG_HOME/fcitx5" "$XDG_CONFIG_HOME/mozc"
cat > "$XDG_CONFIG_HOME/fcitx5/profile" <<'EOF'
[Groups/0]
Name=Test
Default Layout=us
DefaultIM=mozc
[Groups/0/Items/0]
Name=keyboard-us
Layout=
[Groups/0/Items/1]
Name=mozc
Layout=
[GroupOrder]
0=Test
EOF
cat > "$XDG_CONFIG_HOME/fcitx5/config" <<'EOF'
[Behavior]
ActiveByDefault=True
EOF
fcitx5 -D --keep --disable all --enable dbus,dbusfrontend,keyboard,mozc \
    > "$probe_root/daemon.log" 2>&1 &
daemon_pid=$!
# Name ownership is the readiness signal, without activating a second daemon.
gdbus wait --session --timeout 60 org.fcitx.Fcitx5 &
ready_pid=$!
if ! wait -n -p completed "$daemon_pid" "$ready_pid" || [ "$completed" != "$ready_pid" ]; then
    cat "$probe_root/daemon.log" >&2
    exit 1
fi
ready_pid=
if ! "$@"; then
    cat "$probe_root/daemon.log" >&2
    exit 1
fi
