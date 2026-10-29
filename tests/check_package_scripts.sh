#!/bin/bash
# Exercise packaging with a fake toolchain; no compilers or containers are run.
set -euo pipefail
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
project="$root/project with spaces"
mkdir -p "$project" "$root/commands"
cp "$repository/build_package.sh" "$project/"
if [ -f "$repository/prereq.sh" ]; then cp "$repository/prereq.sh" "$project/"; fi
cp -a "$repository/packaging" "$project/"
cp "$repository/LICENSE" "$repository/README_pack.md" "$project/"
mkdir -p "$project/include/boxrig" "$project/tools/codegen"
printf 'header\n' > "$project/include/boxrig/session.h"
printf 'header\n' > "$project/include/boxrig/posix.h"
export BOXRIG_PACKAGE_TEST_LOG="$root/commands.log"
export PATH="$root/commands:$PATH"

cat > "$root/commands/npx" <<'EOF'
#!/bin/bash
set -eu
test "$*" = "--yes screw-up format -f -e {version}"
printf 'npx version\n' >> "$BOXRIG_PACKAGE_TEST_LOG"
printf '0.1.0'
EOF
cat > "$root/commands/git" <<'EOF'
#!/bin/bash
set -eu
test "$*" = 'submodule update --init'
EOF
cat > "$root/commands/npm" <<'EOF'
#!/bin/bash
printf 'npm %s\n' "$*" >> "$BOXRIG_PACKAGE_TEST_LOG"
EOF
cat > "$root/commands/make" <<'EOF'
#!/bin/bash
set -euo pipefail
printf 'make %s\n' "$*" >> "$BOXRIG_PACKAGE_TEST_LOG"
destination=''
prefix=''
for argument in "$@"; do
    case "$argument" in
        DESTDIR=*) destination=${argument#DESTDIR=} ;;
        PREFIX=*) prefix=${argument#PREFIX=} ;;
    esac
done
if [ -n "$destination" ]; then
    sdk="$destination$prefix"
    mkdir -p "$sdk/include/boxrig" "$sdk/lib/pkgconfig" "$sdk/bin" \
        "$sdk/share/licenses/boxrig" "$sdk/share/boxrig/examples"
    printf 'library\n' > "$sdk/lib/libboxrig.a"
    if [[ " $* " == *' install-shared '* ]]; then
        for name in boxrig boxrig-ui boxrig-session boxrig-posix boxrig-gtk3 boxrig-gtk4 \
            boxrig-x11 boxrig-wayland boxrig-render-ogl boxrig-x11-ogl boxrig-wayland-ogl \
            boxrig-notification boxrig-background-wayland boxrig-background-gtk3 boxrig-background-gtk4; do
            printf 'library\n' > "$sdk/lib/lib$name.a"
            printf 'shared library\n' > "$sdk/lib/lib$name.so.0"
            ln -s "lib$name.so.0" "$sdk/lib/lib$name.so"
        done
    fi
    printf 'license\n' > "$sdk/share/licenses/boxrig/README.md"
    printf 'Japanese documentation\n' > "$sdk/share/licenses/boxrig/README_ja.md"
    printf 'example\n' > "$sdk/share/boxrig/examples/example.c"
elif [[ " $* " == *' native '* ]]; then
    mkdir -p build/boxrig_emitter/posix
    printf 'emitter\n' > build/boxrig_emitter/posix/boxrig_emitter
    printf 'session\n' > build/libboxrig-session.a
    printf 'posix\n' > build/libboxrig-posix.a
    for frontend in x11 gtk3 gtk4 wayland; do
        mkdir -p "build/boxrig_term/$frontend"
        printf 'terminal\n' > "build/boxrig_term/$frontend/boxrig_term"
    done
elif [[ " $* " == *' win32 '* ]]; then
    for arch in amd64 i686; do
        mkdir -p "build/boxrig_term/win32/$arch" "build/boxrig_emitter/win32/$arch"
        printf 'terminal\n' > "build/boxrig_term/win32/$arch/boxrig_term.exe"
        printf 'launcher\n' > "build/boxrig_term/win32/$arch/boxrig_term_launcher.exe"
        printf 'emitter\n' > "build/boxrig_emitter/win32/$arch/boxrig_emitter.exe"
    done
elif [[ " $* " == *' package-codegen '* ]]; then
    mkdir -p build/tools/codegen build/generated/licenses build/generated/pkgconfig
    printf 'generated JavaScript\n' > build/tools/codegen/generate-wayland-notices.mjs
    printf 'generated JavaScript\n' > build/tools/codegen/generate-shift-jis.mjs
    printf 'generated JavaScript\n' > build/tools/codegen/generate-linux-metadata.mjs
    printf 'generated package metadata\n' > build/generated/pkgconfig/boxrig.pc
    printf 'protocol notices\n' > build/generated/licenses/Wayland-protocols-NOTICES.md
else
    printf 'Unexpected make invocation: %s\n' "$*" >&2
    exit 1
fi
EOF
for command in gcc g++ pkg-config wayland-scanner \
    x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ \
    i686-w64-mingw32-gcc i686-w64-mingw32-g++; do
    printf '#!/bin/bash\nprintf "Unexpected compiler invocation\\n" >&2\nexit 1\n' > "$root/commands/$command"
done
chmod +x "$root/commands/"*

cd "$root"
bash "$project/build_package.sh" --target native --jobs 2 > "$root/native.log" 2>&1 || {
    cat "$root/native.log"
    exit 1
}
linux_name="boxrig-0.1.0-linux-$(uname -m)"
tar -xzf "$project/artifacts/$linux_name.tar.gz" -C "$root"
test -x "$root/$linux_name/bin/boxrig_term_gtk4"
test -x "$root/$linux_name/bin/boxrig_emitter"
test -s "$root/$linux_name/lib/libboxrig-session.a"
test -s "$root/$linux_name/lib/libboxrig-gtk3.so.0"
test -L "$root/$linux_name/lib/libboxrig.so"
for name in x11 wayland render-ogl x11-ogl wayland-ogl notification \
    background-wayland background-gtk3 background-gtk4; do
    test -s "$root/$linux_name/lib/libboxrig-$name.so.0"
    test -L "$root/$linux_name/lib/libboxrig-$name.so"
done
test ! -e "$root/$linux_name/share/boxrig/examples"
test ! -e "$root/$linux_name/share/licenses/boxrig/README_ja.md"
test ! -e "$project/artifacts/boxrig-0.1.0-win32-amd64.zip"
if grep -q ' win32 ' "$BOXRIG_PACKAGE_TEST_LOG"; then
    echo 'FAIL native packaging invoked Windows builds' >&2
    exit 1
fi
printf '%s\n' 'PASS native SDK packaging without Windows builds'

: > "$BOXRIG_PACKAGE_TEST_LOG"
bash "$project/build_package.sh" --target win32 --jobs 2 > "$root/win32.log" 2>&1 || {
    cat "$root/win32.log"
    exit 1
}
for arch in amd64 i686; do
    name="boxrig-0.1.0-win32-$arch"
    unzip -q "$project/artifacts/$name.zip" -d "$root"
    cmp "$project/README_pack.md" "$root/$name/README.md"
    test -s "$root/$name/bin/boxrig_term.exe"
    test -s "$root/$name/bin/boxrig_term_launcher.exe"
    test -s "$root/$name/bin/boxrig_emitter.exe"
    test ! -e "$root/$name/share/boxrig/examples"
    test ! -e "$root/$name/share/licenses/boxrig/README_ja.md"
done
if grep -Eq '^npm | native | package-codegen ' "$BOXRIG_PACKAGE_TEST_LOG"; then
    echo 'FAIL Windows packaging invoked Linux preparation' >&2
    exit 1
fi
test -s "$project/artifacts/$linux_name.tar.gz"
printf '%s\n' 'PASS Windows SDK packaging without Linux builds'

cat > "$root/commands/podman" <<'EOF'
#!/bin/bash
set -euo pipefail
printf 'podman' >> "$BOXRIG_PACKAGE_TEST_LOG"
printf ' <%s>' "$@" >> "$BOXRIG_PACKAGE_TEST_LOG"
printf '\n' >> "$BOXRIG_PACKAGE_TEST_LOG"
case "$1" in
    image) test "${BOXRIG_TEST_NO_IMAGES:-0}" = 0; exit ;;
    build) exit 0 ;;
    run) shift ;;
    *) exit 1 ;;
esac
workspace=''
workdir=''
output=''
platform=''
verification=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --volume)
            case "$2" in
                *:/workspace:Z) workspace=${2%:/workspace:Z} ;;
                *:/output:Z) output=${2%:/output:Z} ;;
                *) echo 'Unexpected container mount' >&2; exit 1 ;;
            esac
            shift 2 ;;
        --platform) platform=$2; shift 2 ;;
        --workdir) workdir=$2; shift 2 ;;
        --env)
            case "$2" in
                BOXRIG_PACKAGE_VERSION=*) version=${2#*=} ;;
                BOXRIG_PACKAGE_DISTRO=*) distro=${2#*=} ;;
                BOXRIG_PACKAGE_RELEASE=*) release=${2#*=} ;;
                BOXRIG_PACKAGE_ARCH=*) arch=${2#*=} ;;
                JOBS=*) test "${2#*=}" = 2 ;;
                *) exit 1 ;;
            esac
            shift 2 ;;
        tests/shared/check_release_archive.sh)
            verification=1
            archive=$2
            verify_version=$3
            verify_arch=$4
            extracted_sdk=$5
            shift 5 ;;
        *) shift ;;
    esac
done
test -d "$workspace"
test "$workdir" = /workspace
test -d "$output"
target="$distro-$release-$arch"
if [ "$verification" -eq 1 ]; then
    test "$verify_version" = "$version"
    test "$verify_arch" = "$arch"
    test -s "$output/${archive##*/}"
    sdk="$workspace${extracted_sdk#/workspace}"
    test "$(cat "$sdk/platform.txt")" = "$platform"
    test "$(cat "$sdk/payload.txt")" = "${BOXRIG_TEST_ARCHIVE_TEXT:-initial}"
    test "${BOXRIG_TEST_FAIL_VERIFICATION_TARGET:-}" != "$target"
    exit 0
fi
for required in packaging/build_linux_container.sh \
    build/tools/codegen/generate-wayland-notices.mjs \
    build/generated/licenses/Wayland-protocols-NOTICES.md deps/fixture/source.c; do
    test -s "$workspace/$required"
done
entries=$(tar -cf - -C "$workspace" . | tar -tf -)
if grep -Eq '(^|/)\.git(/|$)|(^|/)node_modules/|^\./artifacts/|libboxrig.*\.a$|host-only\.o$' <<< "$entries"; then
    echo 'Host build products leaked into the container source' >&2
    exit 1
fi
test "$(cat "$workspace/deps/fixture/source.c")" = source
test -x "$workspace/deps/fixture/tool.sh"
test "$(stat -c %Y "$workspace/deps/fixture/source.c")" = 1700000000
test ! -e "$workspace/build/container-only.o"
# Each container may modify its source copy without affecting other targets
# or the original checkout.
printf 'container source\n' > "$workspace/deps/fixture/source.c"
printf 'container object\n' > "$workspace/build/container-only.o"
test "${BOXRIG_TEST_FAIL_TARGET:-}" != "$target"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
name="boxrig-$version-linux-$target"
mkdir "$stage/$name"
printf '%s\n' "$platform" > "$stage/$name/platform.txt"
printf '%s\n' "${BOXRIG_TEST_ARCHIVE_TEXT:-initial}" > "$stage/$name/payload.txt"
tar -czf "$output/$name.tar.gz" -C "$stage" "$name"
EOF
chmod +x "$root/commands/podman"

targets=$(bash "$project/build_package.sh" --list-targets)
test "$(wc -l <<< "$targets")" -eq 9
for row in 'debian trixie i686 linux/386' 'debian trixie armv7l linux/arm/v7' \
    'debian trixie riscv64 linux/riscv64' 'ubuntu 26.04 arm64 linux/arm64'; do
    grep -Fxq "$row" <<< "$targets"
done
selected=$(bash "$project/build_package.sh" --list-targets --distro debian --release 13 --arch i386,armhf,riscv64)
test "$(wc -l <<< "$selected")" -eq 3
test "$(bash "$project/build_package.sh" --list-targets --release noble --arch amd64)" = 'ubuntu 24.04 x86_64 linux/amd64'
for arguments in '--arch' '--arch unknown' '--release bookworm' '--target win32 --arch arm64' \
    '--distro ubuntu --arch i686' '--jobs 0' '--jobs -1'; do
    read -r -a options <<< "$arguments"
    if bash "$project/build_package.sh" "${options[@]}" > "$root/invalid.log" 2>&1; then
        echo "FAIL accepted invalid selection: $arguments" >&2
        exit 1
    fi
done
printf '%s\n' 'PASS target matrix, aliases and invalid selections'

: > "$BOXRIG_PACKAGE_TEST_LOG"
BOXRIG_TEST_NO_IMAGES=1 bash "$project/prereq.sh" --distro debian --arch i386,armhf,riscv64
test "$(grep -c '^podman <build>' "$BOXRIG_PACKAGE_TEST_LOG")" -eq 3
for platform in linux/386 linux/arm/v7 linux/riscv64; do
    grep -Fq "<--platform> <$platform>" "$BOXRIG_PACKAGE_TEST_LOG"
done
: > "$BOXRIG_PACKAGE_TEST_LOG"
bash "$project/prereq.sh" --release noble --arch amd64
if grep -q '^podman <build>' "$BOXRIG_PACKAGE_TEST_LOG"; then exit 1; fi
bash "$project/prereq.sh" --release noble --arch amd64 --force
grep -Fq '<--no-cache>' "$BOXRIG_PACKAGE_TEST_LOG"
printf '%s\n' 'PASS prerequisite image selection and reuse'

mkdir -p "$project/deps/fixture/.git" "$project/node_modules" "$project/.git"
printf 'source\n' > "$project/deps/fixture/source.c"
touch -d @1700000000 "$project/deps/fixture/source.c"
printf '#!/bin/bash\nexit 0\n' > "$project/deps/fixture/tool.sh"
chmod +x "$project/deps/fixture/tool.sh"
printf 'host object\n' > "$project/build/host-only.o"
: > "$BOXRIG_PACKAGE_TEST_LOG"
bash "$project/build_package.sh" --target linux --distro debian --arch i386,armhf,riscv64 --jobs 2
test "$(grep -c '^npx version' "$BOXRIG_PACKAGE_TEST_LOG")" -eq 1
for entry in 'i686 linux/386' 'armv7l linux/arm/v7' 'riscv64 linux/riscv64'; do
    read -r arch platform <<< "$entry"
    name="boxrig-0.1.0-linux-debian-trixie-$arch"
    test "$(tar -xOf "$project/artifacts/$name.tar.gz" "$name/platform.txt")" = "$platform"
done
if grep -Eq ' win32 | native ' "$BOXRIG_PACKAGE_TEST_LOG"; then
    echo 'FAIL container packaging invoked host compilation' >&2
    exit 1
fi
grep -Fq 'make -f tools/codegen/rules.mk package-codegen' "$BOXRIG_PACKAGE_TEST_LOG"
test "$(cat "$project/deps/fixture/source.c")" = source
test ! -e "$project/build/container-only.o"
printf '%s\n' 'PASS host-extracted isolated source and architecture-specific Linux archives'

archive="$project/artifacts/boxrig-0.1.0-linux-debian-trixie-i686.tar.gz"
cp "$archive" "$root/previous.tar.gz"
if BOXRIG_TEST_ARCHIVE_TEXT=replacement BOXRIG_TEST_FAIL_TARGET=debian-trixie-armv7l bash "$project/build_package.sh" \
    --target linux --distro debian --arch i686,armv7l --jobs 2 > "$root/failure.log" 2>&1; then
    echo 'FAIL container failure was ignored' >&2
    exit 1
fi
cmp "$archive" "$root/previous.tar.gz"
if BOXRIG_TEST_ARCHIVE_TEXT=replacement BOXRIG_TEST_FAIL_VERIFICATION_TARGET=debian-trixie-armv7l bash "$project/build_package.sh" \
    --target linux --distro debian --arch i686,armv7l --jobs 2 > "$root/verification-failure.log" 2>&1; then
    echo 'FAIL SDK verification failure was ignored' >&2
    exit 1
fi
cmp "$archive" "$root/previous.tar.gz"
: > "$BOXRIG_PACKAGE_TEST_LOG"
if BOXRIG_TEST_NO_IMAGES=1 bash "$project/build_package.sh" --target linux --arch amd64 > "$root/missing.log" 2>&1; then
    echo 'FAIL missing image was ignored' >&2
    exit 1
fi
grep -Fq './prereq.sh' "$root/missing.log"
if grep -Eq '^make |^npm |^podman <run>' "$BOXRIG_PACKAGE_TEST_LOG"; then exit 1; fi
printf '%s\n' 'PASS failed builds preserve previous archives and missing images fail early'

: > "$BOXRIG_PACKAGE_TEST_LOG"
bash "$project/build_package.sh" --jobs 2
test "$(grep -c '^podman <run>' "$BOXRIG_PACKAGE_TEST_LOG")" -eq 18
test "$(grep -c '^npx version' "$BOXRIG_PACKAGE_TEST_LOG")" -eq 1
for arch in amd64 i686; do
    test -s "$project/artifacts/boxrig-0.1.0-win32-$arch.zip"
done
printf '%s\n' 'PASS all-target packaging combines nine Linux and two Windows archives'

# Use an existing ELF executable; these checks do not compile a test program.
source "$repository/packaging/common.sh"
host_arch=$(package_normalize_filter arch "$(uname -m)")
package_check_elf /bin/bash "$host_arch"
cp /bin/bash "$root/wrong-machine"
# ELF e_machine is a 16-bit field at byte 18. EM_NONE is invalid for every target.
printf '\000\000' | dd of="$root/wrong-machine" bs=1 seek=18 count=2 conv=notrunc status=none
if (package_check_elf "$root/wrong-machine" "$host_arch") > "$root/elf.log" 2>&1; then
    echo 'FAIL mismatched ELF architecture was accepted' >&2
    exit 1
fi
ar qc "$root/mixed.a" /bin/bash "$root/wrong-machine"
if (package_check_elf "$root/mixed.a" "$host_arch") >> "$root/elf.log" 2>&1; then
    echo 'FAIL mixed ELF architectures in a static library were accepted' >&2
    exit 1
fi
printf '%s\n' 'PASS ELF architecture validation for executables and static libraries'
