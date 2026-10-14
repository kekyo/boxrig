#!/bin/bash
set -euo pipefail
client=$1
driver=$2
output=$3
mkdir -p "$output"
: > "$output/orca.log"
settings=$(mktemp -d "$XDG_CONFIG_HOME/boxrig-orca.XXXXXX")
orca_pid=
client_pid=
cleanup() {
    if [[ -n "$client_pid" ]]; then kill "$client_pid" 2>/dev/null || true; fi
    if [[ -n "$orca_pid" ]]; then kill -- "-$orca_pid" 2>/dev/null || true; fi
    wait 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
cat > "$settings/user-settings.conf" <<'EOF'
{
  "general": {
    "firstStart": false,
    "enableSpeech": true,
    "enableBraille": false,
    "startingProfile": ["Default", "default"],
    "speechServerFactory": null
  },
  "profiles": {
    "default": {
      "profile": ["Default", "default"],
      "pronunciations": {},
      "keybindings": {}
    }
  },
  "pronunciations": {},
  "keybindings": {}
}
EOF
mkfifo "$settings/orca-output" "$settings/client-input"
exec 3<> "$settings/client-input"
exec 4<> "$settings/orca-output"
export ORCA_TEST_SETTINGS="$settings"
setsid script -q -e -c 'exec orca -u "$ORCA_TEST_SETTINGS" -d braille --debug --debug-file /dev/stdout' /dev/null > "$settings/orca-output" 2>&1 &
orca_pid=$!
wait_for_log() {
    local pattern=$1 line remaining deadline=$((SECONDS + 180))
    # The timeout is only a failure watchdog. Success requires an actual Orca
    # event, regardless of how long startup or speech dispatch takes.
    while (( (remaining = deadline - SECONDS) > 0 )) && IFS= read -r -t "$remaining" line <&4; do
        printf '%s\n' "$line" >> "$output/orca.log"
        if [[ "$line" == *"$pattern"* ]]; then return; fi
    done
    echo "FAIL Orca did not report: $pattern" >&2
    return 1
}
wait_for_log 'Starting Atspi main event loop'
"$client" "$driver" --speech <&3 > "$output/client.log" 2>&1 &
client_pid=$!
window=$(xdotool search --sync --onlyvisible --name '^boxrig-accessibility$')
xdotool windowfocus --sync "$window"
wait_for_log '^^^^^ PROCESS OBJECT EVENT object:state-changed:focused'
printf 'focus\n' >&3
wait_for_log "SPEECH OUTPUT: 'Hello boxrig."
printf 'speech\n' >&3
wait "$client_pid"
client_pid=
echo 'PASS Orca generated Hello boxrig. speech output'
