#!/bin/bash
# Container targets consume host-generated metadata without a Node toolchain.
set -euo pipefail
make_command=${1:-make}
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
"$make_command" -f tools/codegen/rules.mk package-codegen
node build/tools/codegen/generate-linux-metadata.mjs "$root/metadata" 1.2.3 \
    /usr /usr/lib /usr/include /usr/lib/pkgconfig boxrig boxrig-ui boxrig-session boxrig-posix boxrig-gtk3 boxrig-gtk4
mkdir -p "$root/build/generated/licenses" "$root/commands"
cp build/generated/licenses/Wayland-protocols-NOTICES.md "$root/build/generated/licenses/"
for command in node npm; do
    printf '#!/bin/sh\necho "FAIL target attempted Node/npm code generation" >&2\nexit 1\n' > "$root/commands/$command"
    chmod +x "$root/commands/$command"
done
PATH="$root/commands:$PATH" "$make_command" -j8 install-shared \
    BUILD_DIR="$root/build" CODEGEN_DIR="$PWD/build/tools/codegen" \
    BOXRIG_PREGENERATED_PKGCONFIG="$root/metadata" BOXRIG_VERSION=1.2.3 \
    PREFIX=/usr DESTDIR="$root/stage"
export PKG_CONFIG_PATH="$root/stage/usr/lib/pkgconfig"
test "$(pkg-config --modversion boxrig)" = 1.2.3
cc -std=c99 -Wall -Wextra -Werror tests/shared/consumer.c \
    $(pkg-config --cflags --libs boxrig-ui boxrig-posix) -ldl -o "$root/consumer"
LD_LIBRARY_PATH="$root/stage/usr/lib" "$root/consumer"
for gtk in 3 4; do
    cc -std=c99 -Wall -Wextra -Werror "tests/gtk$gtk/test_gtk${gtk}_package.c" \
        $(pkg-config --cflags --libs "boxrig-gtk$gtk") -o "$root/gtk$gtk"
    LD_LIBRARY_PATH="$root/stage/usr/lib" "$root/gtk$gtk"
done
printf '%s\n' 'PASS shared SDK with prebuilt metadata and no target Node/npm execution'
