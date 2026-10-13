#!/bin/bash
# Run in a private D-Bus session; the owned nested compositor is one X11 window.
set -eu
if [ "${1:-}" != --private-session ]; then
    exec dbus-run-session -- bash "$0" --private-session "$@"
fi
shift
sample=$(realpath "$1")
scene=$(realpath "$2")
renderer=$3
output=$(realpath -m "$4")
driver=$(realpath "$(dirname "$0")/run_background_effect_video.mjs")
export BOXRIG_TEST_KWIN_CAPTURE
BOXRIG_TEST_KWIN_CAPTURE=$(realpath "${BOXRIG_TEST_KWIN_CAPTURE:-$(dirname "$sample")/kwin_background_capture}")
runtime=$(mktemp -d)
trap 'rm -rf "$runtime"' EXIT
export XDG_RUNTIME_DIR="$runtime" XDG_CONFIG_HOME="$runtime"
export XDG_DATA_HOME="$runtime/data" XDG_CACHE_HOME="$runtime/cache"
mkdir -p "$XDG_DATA_HOME/applications" "$XDG_CACHE_HOME"
# Register the helper through KWin's documented screenshot permission entry.
# Both the desktop file and service cache belong to this private test session.
node --input-type=module <<'JS'
import {writeFileSync} from 'node:fs';
const executable = process.env.BOXRIG_TEST_KWIN_CAPTURE.replaceAll('\\', '\\\\').replaceAll('"', '\\"').replaceAll('%', '%%');
writeFileSync(process.env.XDG_DATA_HOME + '/applications/boxrig-background-capture.desktop',
  '[Desktop Entry]\nType=Application\nName=boxrig background capture\nExec="' + executable + '"\nX-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2\n');
JS
kbuildsycoca6 --noincremental
export BOXRIG_TEST_CAPTURE_DISPLAY="$DISPLAY" BOXRIG_TEST_NESTED_WAYLAND=1
export BOXRIG_TEST_CAPTURE_XAUTHORITY="${XAUTHORITY:-}"
export BOXRIG_TEST_EXISTING_COMPOSITORS
BOXRIG_TEST_EXISTING_COMPOSITORS=$(xdotool search --onlyvisible --name '^KDE Wayland Compositor' || test "$?" -eq 1)
export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}" KWIN_COMPOSE=O2 LANG=C.UTF-8
cat > "$runtime/kwinrc" <<'EOF'
[Plugins]
blurEnabled=true
slidingpopupsEnabled=false
fadeEnabled=false
fadingpopupsEnabled=false
sheetEnabled=false
[Effect-blur]
BlurStrength=15
NoiseStrength=0
EOF
printf '[KDE]\nAnimationDurationFactor=0\n' > "$runtime/kdeglobals"
mkdir -p "$output"
rm -f "$output/passed.json"
printf -v session_command '%q ' node "$driver" "$sample" "$scene" "$renderer" "$output"
"${BOXRIG_TEST_KWIN_WAYLAND:-kwin_wayland}" --x11-display "$DISPLAY" \
    --width 1200 --height 800 --socket boxrig-background-test --xwayland \
    --no-lockscreen --no-global-shortcuts --no-kactivities \
    --exit-with-session "$session_command"
test -f "$output/passed.json"
