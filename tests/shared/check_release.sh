#!/bin/bash
# Exercise the actual packaging entry point rather than an install-only SDK.
set -euo pipefail
make_command=${1:-make}
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
architecture=$(uname -m)
MAKE="$make_command" bash packaging/build_sdk.sh linux 1.2.3 "$root" 8
bash tests/shared/check_release_archive.sh \
    "$root/boxrig-1.2.3-linux-$architecture.tar.gz" 1.2.3 "$architecture"
