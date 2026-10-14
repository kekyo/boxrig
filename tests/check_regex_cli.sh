#!/bin/sh
set -eu
cli=$1
directory=$2
mkdir -p "$directory"
output=$(printf 'zaaa' | "$cli" 'a|aa' 0 1)
case "$output" in 'match 1 3 work='*) ;; *) echo "Unexpected match: $output" >&2; exit 1 ;; esac
if output=$(printf 'aaaa' | "$cli" 'a+b' 0 3); then
    echo 'Expected a non-match exit status' >&2
    exit 1
else
    test "$?" -eq 1
    case "$output" in 'not-found work='*) ;; *) exit 1 ;; esac
fi
printf 'a\000b' > "$directory/pattern"
output=$(printf 'xa\000by' | "$cli" --pattern-file "$directory/pattern" 0 1)
case "$output" in 'match 1 4 work='*) ;; *) exit 1 ;; esac
if printf '\343\201' | "$cli" '.' 2> "$directory/error"; then exit 1; else test "$?" -eq 2; fi
test "$(cat "$directory/error")" = 'input error at byte 0 (status 1)'
if printf a | "$cli" '[' 2> "$directory/error"; then exit 1; else test "$?" -eq 2; fi
test "$(cat "$directory/error")" = 'pattern error at byte 0 (code 3, status 1)'
output=$(printf 'zb' | "$cli" --captures '(?<missing>a)?(a*)b' 0 1)
printf '%s\n' "$output" > "$directory/captures"
tail -n 3 "$directory/captures" > "$directory/actual"
printf 'group 0 0 1\ngroup 1 absent\ngroup 2 0 0\n' > "$directory/expected"
cmp "$directory/expected" "$directory/actual"
echo 'PASS standalone regex CLI'
