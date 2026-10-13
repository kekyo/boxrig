#!/bin/sh
set -eu
make_command=$1
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT HUP INT TERM

# Exercise optional asset extraction without downloading or executing a runtime.
for arch in x86 x64 arm64; do
    mkdir -p "$root/input/runtimes/win-$arch/native" \
        "$root/input/build/native/runtimes/$arch"
    printf 'fixture DLL %s\n' "$arch" > "$root/input/runtimes/win-$arch/native/conpty.dll"
    printf 'fixture host %s\n' "$arch" > "$root/input/build/native/runtimes/$arch/OpenConsole.exe"
done
(cd "$root/input" && zip -qr "$root/conpty.nupkg" .)
digest=$(sha256sum "$root/conpty.nupkg")
digest=${digest%% *}
"$make_command" --no-print-directory conpty-runtime-assets \
    BUILD_DIR="$root/build" CONPTY_PACKAGE="$root/conpty.nupkg" \
    CONPTY_PACKAGE_SHA256="$digest" > "$root/extract.log" 2>&1 || {
    cat "$root/extract.log"
    exit 1
}
for directory in boxrig_term/win32/amd64 boxrig_term/win32/i686 \
    win32/amd64 win32/i686 win32/amd64/tests win32/i686/tests; do
    runtime="$root/build/$directory"
    test -s "$runtime/conpty.dll"
    cmp "$root/input/build/native/runtimes/x64/OpenConsole.exe" "$runtime/x64/OpenConsole.exe"
    cmp deps/conpty/LICENSE.txt "$runtime/LICENSE.conpty.txt"
    cmp packaging/licenses/ConPTY-NOTICE.md "$runtime/NOTICE.conpty.md"
done
printf '%s\n' 'PASS optional ConPTY assets retain license and third-party notices'
