#!/bin/bash
# Build the Linux and Windows SDKs and publish fresh archives in artifacts.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
repository=$(pwd)
source packaging/common.sh
target=all
distro_filter=''
release_filter=''
arch_filter=''
list_targets=0
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
while [ "$#" -gt 0 ]; do
    case "$1" in
        --distro|--release|--arch)
            [ "$#" -ge 2 ] || package_fail "Missing value for $1"
            value=$(package_normalize_filter "${1#--}" "$2")
            case "$1" in
                --distro) distro_filter=$value ;;
                --release) release_filter=$value ;;
                --arch) arch_filter=$value ;;
            esac
            shift 2 ;;
        --list-targets) list_targets=1; shift ;;
        --target|--jobs)
            [ "$#" -ge 2 ] || package_fail "Missing value for $1"
            if [ "$1" = --target ]; then target=$2; else jobs=$2; fi
            shift 2
            ;;
        --help)
            cat <<'EOF'
Usage: ./build_package.sh [options]

  --target <target> all (default), linux (Podman), win32, or native (host Linux).
  --distro <list>   Comma-separated Linux distribution filter.
  --release <list>  Comma-separated release filter.
  --arch <list>     Comma-separated Linux CPU architecture filter.
  --jobs <count>   Make jobs per target; defaults to JOBS or the host CPU count.
  --list-targets   List selected Linux targets without building packages.
  --help           Show this help.

Run ./prereq.sh with matching filters before building Linux container packages.
Targets run serially. Windows always includes both amd64 and i686.
EOF
            exit 0
            ;;
        *) package_fail "Unknown option: $1" ;;
    esac
done
case "$target" in all|linux|native|win32) ;; *) package_fail "Unsupported target: $target" ;; esac
package_positive_integer JOBS "$jobs"
selected=''
if [[ "$target" = all || "$target" = linux ]]; then
    selected=$(package_select_targets "$distro_filter" "$release_filter" "$arch_filter")
    [ -n "$selected" ] || package_fail 'No Linux targets matched the filters.'
elif [[ -n "$distro_filter$release_filter$arch_filter" || "$list_targets" -eq 1 ]]; then
    package_fail 'Linux filters and --list-targets require --target all or linux.'
fi
if [ "$list_targets" -eq 1 ]; then printf '%s\n' "$selected"; exit 0; fi
podman_command=${PODMAN:-podman}
if [ -n "$selected" ]; then
    package_require "$podman_command" tar
    while read -r distro release arch platform; do
        image=$(package_image "$distro" "$release" "$arch")
        "$podman_command" image exists "$image" ||
            package_fail "Missing image: $image. Run ./prereq.sh --distro $distro --release $release --arch $arch first."
    done <<< "$selected"
fi
package_require git npx
make_command=${MAKE:-make}
# Do not inherit a caller's make goals or installation paths.
unset MAKEFLAGS MFLAGS MAKEOVERRIDES
version=$(npx --yes screw-up format -f -e '{version}')
package_validate_version "$version"
git submodule update --init
if [ "$target" != win32 ]; then
    package_require "$make_command" npm node
    npm ci --prefix tools/codegen
    "$make_command" -f tools/codegen/rules.mk package-codegen
fi

packages="$repository/artifacts"
mkdir -p "$packages"
stage=$(mktemp -d "$packages/.stage.XXXXXX")
trap 'rm -rf "$stage"' EXIT
archives=()
if [ "$target" = native ]; then
    bash packaging/build_sdk.sh linux "$version" "$stage" "$jobs"
    archives+=("$stage/boxrig-$version-linux-$(uname -m).tar.gz")
fi
if [ -n "$selected" ]; then
    # Every target receives the same source snapshot, with no host objects,
    # Git internals, package outputs, or architecture-specific npm modules.
    snapshot="$stage/source.tar"
    tar -cf "$snapshot" --exclude='./build' --exclude='./artifacts' \
        --exclude='.git' --exclude='node_modules' -C "$repository" .
    tar -rf "$snapshot" -C "$repository" build/tools/codegen \
        build/generated/licenses/Wayland-protocols-NOTICES.md
    while read -r distro release arch platform; do
        image=$(package_image "$distro" "$release" "$arch")
        name="boxrig-$version-linux-$distro-$release-$arch"
        output="$stage/$distro-$release-$arch"
        mkdir -p "$output"
        # Extract on the host: newer tar versions use openat2, which older
        # QEMU user-mode emulators do not implement. Keep each target isolated.
        work=$(mktemp -d "$stage/source.XXXXXX")
        tar -xf "$snapshot" -C "$work"
        printf 'Building %s (%s)\n' "$name" "$platform"
        "$podman_command" run --rm --pull=never --platform "$platform" --userns=keep-id \
            --volume "$work:/workspace:Z" --volume "$output:/output:Z" --workdir /workspace \
            --env "BOXRIG_PACKAGE_VERSION=$version" --env "BOXRIG_PACKAGE_DISTRO=$distro" \
            --env "BOXRIG_PACKAGE_RELEASE=$release" --env "BOXRIG_PACKAGE_ARCH=$arch" \
            --env "JOBS=$jobs" "$image" bash packaging/build_linux_container.sh
        [ -s "$output/$name.tar.gz" ] || package_fail "Container did not create $name.tar.gz"
        archives+=("$output/$name.tar.gz")
        rm -rf "$work"
    done <<< "$selected"
fi
if [[ "$target" = all || "$target" = win32 ]]; then
    bash packaging/build_sdk.sh win32 "$version" "$stage" "$jobs"
    archives+=("$stage/boxrig-$version-win32-amd64.zip" "$stage/boxrig-$version-win32-i686.zip")
fi
# Replace archives only after every selected SDK has been built successfully.
mv "${archives[@]}" "$packages/"
printf 'Packages created in %s\n' "$packages"
