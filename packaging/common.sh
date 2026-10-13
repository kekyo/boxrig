#!/bin/bash
# Shared validation for the package entry points and SDK staging worker.

package_fail() {
    printf '%s\n' "$*" >&2
    exit 1
}

package_require() {
    local command
    for command in "$@"; do
        command -v "$command" >/dev/null 2>&1 ||
            package_fail "Install the missing build tool: $command (see docs/en/building.md)."
    done
}

package_positive_integer() {
    [[ "$2" =~ ^[1-9][0-9]*$ ]] || package_fail "$1 must be a positive integer."
}

package_validate_version() {
    [[ "$1" =~ ^[0-9][0-9A-Za-z.+-]*$ ]] ||
        package_fail "Invalid package version from screw-up: $1"
}

package_linux_targets() {
    cat <<'EOF'
debian trixie x86_64 linux/amd64
debian trixie i686 linux/386
debian trixie arm64 linux/arm64
debian trixie armv7l linux/arm/v7
debian trixie riscv64 linux/riscv64
ubuntu 24.04 x86_64 linux/amd64
ubuntu 24.04 arm64 linux/arm64
ubuntu 26.04 x86_64 linux/amd64
ubuntu 26.04 arm64 linux/arm64
EOF
}

package_normalize_filter() {
    local kind=$1 value=${2,,} item normalized='' resolved
    local -a items
    value=${value//[[:space:]]/}
    [[ -n "$value" && "$value" != ,* && "$value" != *, && "$value" != *,,* ]] ||
        package_fail "Invalid $kind filter: $2"
    IFS=',' read -r -a items <<< "$value"
    for item in "${items[@]}"; do
        case "$kind:$item" in
            distro:debian|distro:ubuntu) resolved=$item ;;
            release:13|release:trixie) resolved=trixie ;;
            release:24.04|release:noble) resolved=24.04 ;;
            release:26.04|release:resolute) resolved=26.04 ;;
            arch:x86_64|arch:amd64) resolved=x86_64 ;;
            arch:i686|arch:i386|arch:x86) resolved=i686 ;;
            arch:arm64|arch:aarch64) resolved=arm64 ;;
            arch:armv7l|arch:armv7|arch:armhf) resolved=armv7l ;;
            arch:riscv64) resolved=riscv64 ;;
            *) package_fail "Unsupported $kind filter: $item" ;;
        esac
        normalized+="${normalized:+,}$resolved"
    done
    printf '%s\n' "$normalized"
}

package_matches_filter() {
    [[ -z "$1" || ",$1," == *",$2,"* ]]
}

package_select_targets() {
    local distro release arch platform
    while read -r distro release arch platform; do
        package_matches_filter "$1" "$distro" || continue
        package_matches_filter "$2" "$release" || continue
        package_matches_filter "$3" "$arch" || continue
        printf '%s %s %s %s\n' "$distro" "$release" "$arch" "$platform"
    done < <(package_linux_targets)
}

package_image() {
    printf 'localhost/boxrig-package-%s-%s-%s:latest\n' "$1" "$2" "$3"
}

package_base_image() {
    local prefix
    case "$3" in
        x86_64) prefix=amd64 ;;
        i686) prefix=i386 ;;
        arm64) prefix=arm64v8 ;;
        armv7l) prefix=arm32v7 ;;
        riscv64) prefix=library ;;
        *) package_fail "Unsupported image architecture: $3" ;;
    esac
    printf 'docker.io/%s/%s:%s\n' "$prefix" "$1" "$2"
}

package_debian_arch() {
    case "$1" in
        x86_64) printf 'amd64\n' ;;
        i686) printf 'i386\n' ;;
        arm64) printf 'arm64\n' ;;
        armv7l) printf 'armhf\n' ;;
        riscv64) printf 'riscv64\n' ;;
        *) package_fail "Unsupported Debian architecture: $1" ;;
    esac
}

# Inspect every ELF member so a mixed-architecture static library is rejected.
package_check_elf() {
    local file=$1 arch=$2 class machine headers field value count=0
    case "$arch" in
        x86_64) class=ELF64; machine='Advanced Micro Devices X86-64' ;;
        i686) class=ELF32; machine='Intel 80386' ;;
        arm64) class=ELF64; machine=AArch64 ;;
        armv7l) class=ELF32; machine=ARM ;;
        riscv64) class=ELF64; machine=RISC-V ;;
        *) package_fail "Unsupported ELF architecture: $arch" ;;
    esac
    headers=$(LC_ALL=C readelf -h "$file") || package_fail "Cannot inspect ELF file: $file"
    while read -r field value; do
        case "$field" in
            Class:)
                [ "$value" = "$class" ] || package_fail "Wrong ELF class in $file: $value (expected $class)"
                count=$((count + 1)) ;;
            Machine:)
                [ "$value" = "$machine" ] || package_fail "Wrong ELF machine in $file: $value (expected $machine)"
                count=$((count + 1)) ;;
        esac
    done <<< "$headers"
    [ "$count" -ge 2 ] || package_fail "No ELF headers in $file"
}
