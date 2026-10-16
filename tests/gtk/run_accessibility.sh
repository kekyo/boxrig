#!/bin/sh
set -eu
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
trap 'exit 1' HUP INT TERM
mkdir -m 700 "$root/run" "$root/config" "$root/cache"
export XDG_RUNTIME_DIR="$root/run" XDG_CONFIG_HOME="$root/config" XDG_CACHE_HOME="$root/cache"
export GDK_BACKEND=x11 GSETTINGS_BACKEND=memory GIO_USE_VFS=local
export ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
unset NO_AT_BRIDGE GTK_A11Y
dbus-run-session -- xvfb-run -a "$@"
