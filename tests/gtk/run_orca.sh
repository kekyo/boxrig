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
mkfifo "$settings/orca-output" "$settings/client-input" "$settings/client-output"
exec 3<> "$settings/client-input"
exec 4<> "$settings/orca-output"
export ORCA_TEST_SETTINGS="$settings"
setsid script -q -e -c 'exec orca -u "$ORCA_TEST_SETTINGS" -d braille --debug --debug-file /dev/stdout' /dev/null > "$settings/orca-output" 2>&1 &
orca_pid=$!
wait_for_line() {
    local descriptor=$1 pattern=$2 log=$3 line remaining deadline=$((SECONDS + 180))
    # The timeout is only a failure watchdog. Success requires an actual Orca
    # event, regardless of how long startup or speech dispatch takes.
    while (( (remaining = deadline - SECONDS) > 0 )) && IFS= read -r -t "$remaining" -u "$descriptor" line; do
        printf '%s\n' "$line" >> "$log"
        if [[ "$line" == *"$pattern"* ]]; then return; fi
    done
    echo "FAIL accessibility test did not report: $pattern" >&2
    return 1
}
wait_for_line 4 'Starting Atspi main event loop' "$output/orca.log"
: > "$output/client.log"
"$client" "$driver" --speech <&3 > "$settings/client-output" 2>&1 &
client_pid=$!
exec 5< "$settings/client-output"
wait_for_line 5 'READY' "$output/client.log"
window=$(xdotool search --sync --onlyvisible --name '^boxrig-accessibility$')
xdotool windowfocus --sync "$window"
printf 'focus\n' >&3
wait_for_line 4 'Request to set locus of focus to [terminal]' "$output/orca.log"
wait_for_line 4 '^^^^^ PROCESS OBJECT EVENT object:state-changed:focused' "$output/orca.log"
printf 'output\n' >&3
wait_for_line 4 "SPEECH OUTPUT: 'Hello boxrig." "$output/orca.log"
printf 'speech\n' >&3
wait "$client_pid"
client_pid=
cat <&5 >> "$output/client.log"
echo 'PASS Orca generated Hello boxrig. speech output'
