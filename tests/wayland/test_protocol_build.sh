#!/bin/sh

set -eu
export LC_ALL=C

make_command=$1
compiler=$2
test_root=$(mktemp -d "${TMPDIR:-/tmp}/boxrig-wayland-protocols.XXXXXX")
trap 'rm -rf "$test_root"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Keep nested builds isolated from the caller's targets and jobserver.
export MAKEFLAGS= MFLAGS= MAKEOVERRIDES=
# All submodule remotes in this fixture are local, including nested Git calls.
export GIT_ALLOW_PROTOCOL=file
project_root=$test_root/project
checkout_root=$test_root/checkout
build_root=$checkout_root/build
generated=$build_root/generated/wayland
source_root=$test_root/wayland-protocols
git init --quiet "$source_root"
git -C "$source_root" config user.name 'Boxrig test'
git -C "$source_root" config user.email 'test@boxrig.invalid'
git -C "$source_root" config commit.gpgsign false
for protocol in stable/viewporter/viewporter.xml \
    staging/fractional-scale/fractional-scale-v1.xml \
    stable/xdg-shell/xdg-shell.xml \
    unstable/xdg-decoration/xdg-decoration-unstable-v1.xml \
    unstable/text-input/text-input-unstable-v3.xml \
    unstable/primary-selection/primary-selection-unstable-v1.xml; do
    mkdir -p "$source_root/$(dirname "$protocol")"
    cat > "$source_root/$protocol" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<protocol name="boxrig_protocol_build_fixture">
  <interface name="boxrig_protocol_build_fixture" version="2">
    <event name="changed">
      <arg name="value" type="uint"/>
    </event>
  </interface>
</protocol>
EOF
done
git -C "$source_root" add .
git -C "$source_root" commit --quiet -m 'Pinned protocol definitions'
pinned_revision=$(git -C "$source_root" rev-parse HEAD)

mkdir -p "$project_root/packaging/licenses"
cp Makefile "$project_root/Makefile"
cp packaging/licenses/licenses.mk "$project_root/packaging/licenses/licenses.mk"
git init --quiet "$project_root"
git -C "$project_root" config user.name 'Boxrig test'
git -C "$project_root" config user.email 'test@boxrig.invalid'
git -C "$project_root" config commit.gpgsign false
git -C "$project_root" submodule add --quiet "file://$source_root" deps/wayland-protocols
git -C "$project_root" add Makefile packaging/licenses/licenses.mk
git -C "$project_root" commit --quiet -m 'Pin protocol submodule'

# Upstream can advance without changing the definitions pinned by the project.
cat > "$source_root/unstable/text-input/text-input-unstable-v3.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<protocol name="boxrig_protocol_build_fixture">
  <interface name="boxrig_protocol_build_fixture" version="3"/>
</protocol>
EOF
git -C "$source_root" add .
git -C "$source_root" commit --quiet -m 'Newer upstream definitions'
git clone --quiet "$project_root" "$checkout_root"
test ! -e "$checkout_root/deps/wayland-protocols/.git"

build_protocols() {
    "$make_command" --no-print-directory -C "$checkout_root" -j8 \
        BUILD_DIR="$build_root" "$@" \
        "$generated/viewporter-client-protocol.h" \
        "$generated/viewporter-server-protocol.h" \
        "$generated/viewporter-protocol.c" \
        "$generated/fractional-scale-v1-client-protocol.h" \
        "$generated/fractional-scale-v1-server-protocol.h" \
        "$generated/fractional-scale-v1-protocol.c" \
        "$generated/text-input-v3-client-protocol.h" \
        "$generated/text-input-v3-server-protocol.h" \
        "$generated/text-input-v3-protocol.c" \
        "$generated/primary-selection-v1-client-protocol.h" \
        "$generated/primary-selection-v1-server-protocol.h" \
        "$generated/primary-selection-v1-protocol.c" \
        "$generated/xdg-shell-client-protocol.h" \
        "$generated/xdg-shell-server-protocol.h" \
        "$generated/xdg-shell-protocol.c" \
        "$generated/xdg-decoration-v1-client-protocol.h" \
        "$generated/xdg-decoration-v1-protocol.c" > "$test_root/build.log" 2>&1 || {
        cat "$test_root/build.log" >&2
        exit 1
    }
}

# A clean parallel build must initialize the pinned submodule, even when the
# system package is missing or older. Exercise the resulting protocol metadata.
build_protocols
test "$(git -C "$checkout_root/deps/wayland-protocols" rev-parse HEAD)" = "$pinned_revision"
cat > "$test_root/consumer.c" <<'EOF'
#include <assert.h>
#include <string.h>
#include "text-input-v3-client-protocol.h"

int main(void)
{
    assert(boxrig_protocol_build_fixture_interface.version == 2);
    assert(boxrig_protocol_build_fixture_interface.event_count == 1);
    assert(strcmp(boxrig_protocol_build_fixture_interface.events[0].name,
        "changed") == 0);
    assert(strcmp(boxrig_protocol_build_fixture_interface.events[0].signature,
        "u") == 0);
    return 0;
}
EOF
"$compiler" -std=c99 -Wall -Wextra -Wpedantic -Werror -I"$generated" \
    $(pkg-config --cflags wayland-client) "$test_root/consumer.c" \
    "$generated/text-input-v3-protocol.c" $(pkg-config --libs wayland-client) \
    -o "$test_root/consumer"
"$test_root/consumer"

# Reuse generated outputs, then regenerate after clean without a remote.
mv "$source_root" "$test_root/offline-wayland-protocols"
build_protocols WAYLAND_SCANNER=false
"$make_command" --no-print-directory -C "$checkout_root" BUILD_DIR="$build_root" clean
build_protocols
build_protocols WAYLAND_SCANNER=false
test -z "$(git -C "$checkout_root/deps/wayland-protocols" status --porcelain)"

# An unavailable submodule must stop the build before protocol generation.
bad_root=$test_root/unavailable-checkout
git clone --quiet "$project_root" "$bad_root"
if "$make_command" --no-print-directory -C "$bad_root" -j8 \
    build/generated/wayland/text-input-v3-client-protocol.h \
    > "$test_root/bad.log" 2>&1; then
    echo "Wayland protocol generation succeeded without its submodule" >&2
    exit 1
fi
test ! -e "$bad_root/build/generated/wayland/text-input-v3-client-protocol.h"

echo "PASS Wayland protocol submodule pin, parallel initialization and offline clean rebuild"
