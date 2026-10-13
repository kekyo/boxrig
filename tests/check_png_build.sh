#!/bin/sh
# Exercise core archive assembly without compiling C sources.
set -eu
make_command=$1
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT HUP INT TERM
export MAKEFLAGS= MFLAGS= MAKEOVERRIDES=

# Record each source in its object file. Missing output directories still fail.
cat > "$root/compiler" <<'EOF'
#!/bin/sh
set -eu
input=''
output=''
while [ "$#" -gt 0 ]; do
    case "$1" in
        -c) input=$2; shift 2 ;;
        -o) output=$2; shift 2 ;;
        *) shift ;;
    esac
done
test -f "$input"
test -n "$output"
printf '%s\n' "$input" > "$output"
EOF
chmod +x "$root/compiler"

"$make_command" --no-print-directory -j4 \
    BUILD_DIR="$root/build" PNG_CONFIG_DIR="$root/generated/libpng" \
    CC="$root/compiler" MINGW_AMD64_CC="$root/compiler" \
    MINGW_I686_CC="$root/compiler" AR=ar MINGW_AMD64_AR=ar MINGW_I686_AR=ar \
    "$root/build/libboxrig.a" "$root/build/win32/amd64/libboxrig.a" \
    "$root/build/win32/i686/libboxrig.a" > "$root/make.log" 2>&1 || {
    cat "$root/make.log" >&2
    exit 1
}

for archive in "$root/build/libboxrig.a" \
    "$root/build/win32/amd64/libboxrig.a" "$root/build/win32/i686/libboxrig.a"; do
    for source in arm_init filter_neon_intrinsics palette_neon_intrinsics; do
        if [ "$(ar p "$archive" "$source.o")" != "deps/libpng/arm/$source.c" ]; then
            printf 'FAIL %s is missing libpng ARM implementation %s\n' "$archive" "$source" >&2
            exit 1
        fi
    done
done
printf '%s\n' 'PASS libpng ARM implementations reach native and Windows core archives'
