#!/bin/bash
# Run in the private source copy extracted by the package orchestrator.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
source packaging/common.sh
distro=${BOXRIG_PACKAGE_DISTRO:?}
release=${BOXRIG_PACKAGE_RELEASE:?}
arch=${BOXRIG_PACKAGE_ARCH:?}
version=${BOXRIG_PACKAGE_VERSION:?}
jobs=${JOBS:?}
source /etc/os-release
expected_release=$release
if [ "$distro" = debian ] && [ "$release" = trixie ]; then expected_release=13; fi
[[ "$ID" = "$distro" && "$VERSION_ID" = "$expected_release" ]] ||
    package_fail "Wrong container distribution: $ID $VERSION_ID (expected $distro $expected_release)"
[ "$(dpkg --print-architecture)" = "$(package_debian_arch "$arch")" ] ||
    package_fail "Wrong container architecture for $arch"
for generated in build/tools/codegen/generate-wayland-notices.mjs \
    build/tools/codegen/generate-shift-jis.mjs \
    build/generated/licenses/Wayland-protocols-NOTICES.md; do
    [ -s "$generated" ] || package_fail "Source preparation is missing: $generated"
done
bash packaging/build_sdk.sh linux "$version" /output "$jobs" "$distro-$release-$arch"
for library in build/libboxrig*.a; do
    package_check_elf "$library" "$arch"
done
for frontend in x11 gtk3 gtk4 wayland; do
    package_check_elf "build/boxrig_term/$frontend/boxrig_term" "$arch"
done
package_check_elf build/boxrig_emitter/posix/boxrig_emitter "$arch"
