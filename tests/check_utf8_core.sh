#!/bin/sh
# A source-only UTF-8 consumer must have no built-in legacy codec to select.
set -eu
if printf '%s\n' '#include <boxrig/terminal.h>' 'int codec = BOXRIG_ENCODING_SHIFT_JIS;' |
    "${CC:-cc}" -x c -Iinclude -fsyntax-only - >/dev/null 2>&1; then
    echo 'the retired built-in codec is still public' >&2
    exit 1
fi
if nm "$1" | grep -E 'shift_jis_(decode|encode)'; then
    exit 1
fi
