#!/bin/bash
# Prepare reusable Podman images for the selected Linux SDK targets.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
source packaging/common.sh
distro_filter=''
release_filter=''
arch_filter=''
force=0
list_targets=0
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
        --force) force=1; shift ;;
        --list-targets) list_targets=1; shift ;;
        --help)
            cat <<'EOF'
Usage: ./prereq.sh [options]

  --distro <list>   Comma-separated Linux distribution filter.
  --release <list>  Comma-separated release filter.
  --arch <list>     Comma-separated CPU architecture filter.
  --force          Refresh base images and rebuild without the build cache.
  --list-targets   List selected targets without building images.
  --help           Show this help.

Images are prepared serially. Foreign CPUs require registered QEMU/binfmt handlers.
EOF
            exit 0 ;;
        *) package_fail "Unknown option: $1" ;;
    esac
done
selected=$(package_select_targets "$distro_filter" "$release_filter" "$arch_filter")
[ -n "$selected" ] || package_fail 'No Linux targets matched the filters.'
if [ "$list_targets" -eq 1 ]; then printf '%s\n' "$selected"; exit 0; fi
podman_command=${PODMAN:-podman}
package_require "$podman_command"
while read -r distro release arch platform; do
    image=$(package_image "$distro" "$release" "$arch")
    if [ "$force" -eq 0 ] && "$podman_command" image exists "$image"; then
        printf 'Using existing image: %s\n' "$image"
        continue
    fi
    options=(--pull=missing)
    if [ "$force" -eq 1 ]; then options=(--pull=always --no-cache); fi
    printf 'Preparing %s %s %s (%s)\n' "$distro" "$release" "$arch" "$platform"
    "$podman_command" build "${options[@]}" --platform "$platform" \
        --build-arg "BASE_IMAGE=$(package_base_image "$distro" "$release" "$arch")" \
        --tag "$image" --file packaging/linux/Containerfile packaging/linux
done <<< "$selected"
