#!/usr/bin/env bash
# boxrig - Under MIT. A private bus is supplied by dbus-run-session.
set -euo pipefail
server=$1
driver=$2
coproc IME_SERVER { exec "$server"; }
server_pid=$IME_SERVER_PID
trap 'kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true' EXIT
read -r -t 60 ready <&"${IME_SERVER[0]}"
test "$ready" = READY
"$driver"
