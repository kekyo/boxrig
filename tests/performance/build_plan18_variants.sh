#!/usr/bin/env bash
set -euo pipefail
if (( $# != 1 )); then
    echo "usage: $0 NEW_OUTPUT_DIRECTORY" >&2
    exit 2
fi
root=$(git rev-parse --show-toplevel)
output=$(realpath -m "$1")
if [[ -e $output ]]; then
    echo "output directory already exists: $output" >&2
    exit 2
fi
scratch=$(mktemp -d)
rmdir "$scratch"
cleanup() {
    git -C "$root" worktree remove --force "$scratch" 2>/dev/null || true
}
trap cleanup EXIT
git -C "$root" worktree add --detach "$scratch" HEAD
for dependency in cardio libpng utf8proc wayland-protocols zlib; do
    rmdir "$scratch/deps/$dependency"
    ln -s "$root/deps/$dependency" "$scratch/deps/$dependency"
done
mkdir -p "$output"
for variant in baseline step2 step3; do
    case $variant in
        baseline) revision=89548bf ;;
        step2) revision=e1909c5 ;;
        step3) revision=aae9d33 ;;
    esac
    git -C "$root" show "$revision:src/opengl/runtime.c" > "$scratch/src/opengl/runtime.c"
    git -C "$root" show "$revision:src/opengl/renderer.c" > "$scratch/src/opengl/renderer.c"
    destination="$output/$variant"
    mkdir -p "$destination"
    printf 'renderer_revision=%s\nworktree_revision=%s\n' \
        "$revision" "$(git -C "$scratch" rev-parse HEAD)" > "$destination/provenance.txt"
    for flavor in plain trace; do
        build_dir="build-$variant-$flavor"
        flags=
        if [[ $flavor == trace ]]; then flags=-DBOXRIG_PLAN18_TRACE_BUILD=1; fi
        targets=("$build_dir/boxrig_term/gtk3/boxrig_term")
        if [[ $flavor == plain ]]; then
            targets+=("$build_dir/tests/performance/plan18_region_bench")
        fi
        make -C "$scratch" -j2 BUILD_DIR="$build_dir" \
            CFLAGS='-O2 -g -ffunction-sections -fdata-sections' \
            CPPFLAGS="$flags" "${targets[@]}" > "$destination/$flavor-build.log" 2>&1
        if [[ $flavor == plain ]]; then
            cp "$scratch/$build_dir/boxrig_term/gtk3/boxrig_term" "$destination/boxrig_term"
            cp "$scratch/$build_dir/tests/performance/plan18_region_bench" \
                "$destination/region_bench"
        else
            cp "$scratch/$build_dir/boxrig_term/gtk3/boxrig_term" \
                "$destination/boxrig_term_traced"
        fi
    done
    sha256sum "$destination/boxrig_term" "$destination/boxrig_term_traced" \
        "$destination/region_bench" > "$destination/sha256.txt"
done
