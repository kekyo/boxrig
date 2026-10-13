#!/usr/bin/env bash
set -euo pipefail
root=$1
gtk=$2
built=$3
compiler=$4
source=$5
export PKG_CONFIG_PATH="$root/usr/lib/pkgconfig"
package="boxrig-gtk$gtk"
requires=$(pkg-config --print-requires-private "$package")
if [[ $built == 1 ]]; then
    rg -qx epoxy <<<"$requires"
elif rg -qx epoxy <<<"$requires"; then
    echo 'FAIL Cairo-only package adds an epoxy dependency' >&2
    exit 1
fi
if [[ $built == 0 ]] && nm -u "$root/usr/lib/lib$package.a" | rg ' U (epoxy_|boxrig_ogl_|boxrig_gl_|boxrig_gtk_gl_)'; then
    echo 'FAIL Cairo-only archive retains OpenGL references' >&2
    exit 1
fi
cp "$source" "$root/consumer.c"
read -ra flags <<<"$(pkg-config --cflags "$package")"
read -ra libs <<<"$(pkg-config --libs "$package" $(pkg-config --print-requires-private "$package" | sed 's/[[:space:]].*//'))"
"$compiler" -std=c99 -O2 -Wall -Wextra -Werror "${flags[@]}" "$root/consumer.c" \
    -Wl,--wrap=gtk_gl_area_new "${libs[@]}" -o "$root/consumer"
for policy in 0 1 2; do
    for fault in 0 1; do
        "$root/consumer" "$built" "$policy" "$fault"
    done
done
