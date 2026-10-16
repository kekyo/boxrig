#!/usr/bin/env bash
# boxrig - Under MIT. Outer bus has no IME; the inner bus owns the daemon.
set -euo pipefail
export GIO_USE_VFS=local
if [[ ${1:-} != --private ]]; then
    export BOXRIG_TEST_FCITX4_SESSION_ADDRESS=$DBUS_SESSION_BUS_ADDRESS
    exec dbus-run-session --config-file=tests/ime/session.conf -- bash "$0" --private "$@"
fi
shift
server=$1
driver=$2
shift 2
probe_root=$(mktemp -d "${TMPDIR:-/tmp}/boxrig-fcitx4-address.XXXXXX")
export XDG_CONFIG_HOME="$probe_root/config"
mkdir -p "$XDG_CONFIG_HOME/fcitx/dbus"
machine_id=$(cat /var/lib/dbus/machine-id 2>/dev/null || cat /etc/machine-id)
display_number=${BOXRIG_TEST_FCITX4_DISPLAY_NUMBER:-72}
export BOXRIG_TEST_FCITX4_ADDRESS_PATH="$XDG_CONFIG_HOME/fcitx/dbus/$machine_id-$display_number"
export BOXRIG_TEST_FCITX4_PRIVATE_ADDRESS=$DBUS_SESSION_BUS_ADDRESS
coproc IME_SERVER { exec "$server"; }
server_pid=$IME_SERVER_PID
trap 'kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true; rm -r "$probe_root"' EXIT
read -r -t 60 ready <&"${IME_SERVER[0]}"
test "$ready" = READY
export DBUS_SESSION_BUS_ADDRESS=$BOXRIG_TEST_FCITX4_SESSION_ADDRESS DISPLAY="example:$display_number.3"
status=0
if (( $# == 0 )); then
    set -- explicit file explicit-stale malformed oversized unterminated invalid-address dead-pid zero-pid \
        bad-display empty-bus watch watch-parent owner watch-pending
fi
for scenario in "$@"; do
    "$driver" "$scenario" || status=1
done
exit "$status"
