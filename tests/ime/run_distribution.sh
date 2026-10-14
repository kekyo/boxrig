#!/bin/bash
# Build and run the packaged IME in a private distribution environment.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
case "${1:-}" in
    debian13) base=docker.io/amd64/debian:trixie; version=5.1.12 ;;
    ubuntu2404) base=docker.io/amd64/ubuntu:24.04; version=5.1.7 ;;
    ubuntu2604) base=docker.io/amd64/ubuntu:26.04; version=5.1.19 ;;
    *) echo 'usage: bash tests/ime/run_distribution.sh debian13|ubuntu2404|ubuntu2604 [fcitx5|fcitx4]' >&2; exit 2 ;;
esac
distribution=$1
generation=${2:-fcitx5}
case "$generation" in
    fcitx5)
        packages='fcitx5 fcitx5-mozc fcitx5-frontend-gtk3 fcitx5-frontend-gtk4'
        daemon_package=fcitx5
        targets='test-fcitx5-real test-fcitx5-panels test-fcitx5-auto-real'
        ;;
    fcitx4)
        version=4.2.9.9
        packages='fcitx fcitx-mozc fcitx-ui-classic fcitx-frontend-gtk3'
        daemon_package=fcitx-bin
        targets='test-fcitx4 test-fcitx4-focus test-fcitx4-address test-fcitx4-discovery test-fcitx4-auto test-fcitx4-native test-gtk-fcitx4 test-fcitx4-real test-fcitx4-auto-real'
        ;;
    *) echo 'IME generation must be fcitx5 or fcitx4' >&2; exit 2 ;;
esac
image="localhost/boxrig-ime:$distribution-$generation"
results="$PWD/build/test-results/ime-distributions/$distribution-$generation"
mkdir -p "$results"
podman build --build-arg "BASE_IMAGE=$base" --build-arg "IME_PACKAGES=$packages" -f tests/ime/Containerfile -t "$image" tests/ime \
    > "$results/image.log" 2>&1
# Include current edits but never share a writable checkout with the container.
tar --exclude=.git --exclude=node_modules --exclude=build -cf "$results/source.tar" \
    Makefile include src deps tests examples tools packaging
podman run --rm --userns=keep-id:uid=2000,gid=2000 -v "$results/source.tar:/source.tar:ro" -v "$results:/results" \
    -e "BOXRIG_TEST_FCITX_VERSION=$version" -e "BOXRIG_TEST_FCITX_PACKAGES=$packages" \
    -e "BOXRIG_TEST_FCITX_PACKAGE=$daemon_package" -e "BOXRIG_TEST_FCITX_TARGETS=$targets" \
    -e "JOBS=${JOBS:-4}" "$image" bash -c '
    set -euo pipefail
    tar -xf /source.tar
    dpkg-query -W $BOXRIG_TEST_FCITX_PACKAGES "$BOXRIG_TEST_FCITX_PACKAGE" libgtk-3-0t64 libgtk-4-1 > /results/packages.txt
    version=$(dpkg-query -W -f="\${Version}" "$BOXRIG_TEST_FCITX_PACKAGE")
    [[ "${version#*:}" == "$BOXRIG_TEST_FCITX_VERSION"-* ]]
    make -j"$JOBS" $BOXRIG_TEST_FCITX_TARGETS \
        > /results/tests.log 2>&1
    ' || { tail -80 "$results/tests.log" >&2; exit 1; }
cat "$results/packages.txt"
echo "PASS $distribution $generation $version: $targets"
