#!/bin/bash
# Build and run the packaged IME in a private distribution environment.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
case "${1:-}" in
    debian13) base=docker.io/amd64/debian:trixie; version=5.1.12 ;;
    ubuntu2404) base=docker.io/amd64/ubuntu:24.04; version=5.1.7 ;;
    ubuntu2604) base=docker.io/amd64/ubuntu:26.04; version=5.1.19 ;;
    *) echo 'usage: bash tests/ime/run_distribution.sh debian13|ubuntu2404|ubuntu2604' >&2; exit 2 ;;
esac
distribution=$1
image="localhost/boxrig-ime:$distribution-fcitx5"
results="$PWD/build/test-results/ime-distributions/$distribution-fcitx5"
mkdir -p "$results"
podman build --build-arg "BASE_IMAGE=$base" -f tests/ime/Containerfile -t "$image" tests/ime \
    > "$results/image.log" 2>&1
# Include current edits but never share a writable checkout with the container.
tar --exclude=.git --exclude=node_modules --exclude=build -cf "$results/source.tar" \
    Makefile include src deps tests examples tools packaging
podman run --rm --userns=keep-id:uid=2000,gid=2000 -v "$results/source.tar:/source.tar:ro" -v "$results:/results" \
    -e "BOXRIG_TEST_FCITX_VERSION=$version" -e "JOBS=${JOBS:-4}" "$image" bash -c '
    set -euo pipefail
    tar -xf /source.tar
    dpkg-query -W fcitx5 fcitx5-mozc libgtk-3-0t64 libgtk-4-1 > /results/packages.txt
    version=$(dpkg-query -W -f="\${Version}" fcitx5)
    [[ "$version" == "$BOXRIG_TEST_FCITX_VERSION"-* ]]
    make -j"$JOBS" test-fcitx5-real test-fcitx5-panels test-fcitx5-auto-real \
        > /results/tests.log 2>&1
    ' || { tail -80 "$results/tests.log" >&2; exit 1; }
cat "$results/packages.txt"
echo "PASS $distribution fcitx $version: explicit input, candidates, and automatic selection"
