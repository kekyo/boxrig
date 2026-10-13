#!/bin/bash
set -euo pipefail

frontend=${1:-${BOXRIG_TEST_NOTIFICATION_FRONTEND:-wayland}}
renderer=${2:-${BOXRIG_TEST_NOTIFICATION_RENDERER:-wayland}}
work=$(mktemp -d)
sample_pid=
service_pid=
cleanup() {
    if [[ -n "$sample_pid" ]]; then
        kill -- "-$sample_pid" 2>/dev/null || true
        wait "$sample_pid" 2>/dev/null || true
    fi
    if [[ -n "$service_pid" ]]; then
        kill "$service_pid" 2>/dev/null || true
        wait "$service_pid" 2>/dev/null || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

exec 3< <(exec build/tests/fake_notification_service)
service_pid=$!
read -r -n 1 -t 20 ready <&3
[[ "$ready" = R ]]
mkfifo "$work/finish"
setsid env BOXRIG_ALLOW_NOTIFICATIONS=1 \
    "build/boxrig_term/$frontend/boxrig_term" "--renderer=$renderer" -- \
    /bin/sh -c 'printf "\033]99;i=plan27;notification sample\033\\"; read -r finish < "$1"' \
    sh "$work/finish" >"$work/sample.log" 2>&1 &
sample_pid=$!
if ! read -r -n 1 -t 20 notification <&3 || [[ "$notification" != N ]]; then
    cat "$work/sample.log" >&2
    echo "FAIL $frontend sample did not deliver its notification" >&2
    exit 1
fi
printf 'finish\n' >"$work/finish"
wait "$sample_pid"
sample_pid=
echo "PASS $frontend sample notification reaches the desktop provider"
