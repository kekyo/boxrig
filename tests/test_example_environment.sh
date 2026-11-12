#!/bin/sh

set -eu

sample_binary=${BOXRIG_TEST_SAMPLE_BINARY:?}
renderer=${BOXRIG_TEST_SAMPLE_RENDERER:?}
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/boxrig-environment.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

for inherited in unset empty other; do
    for profile in default xterm; do
        (
            case "$inherited" in
                unset) unset COLORTERM ;;
                empty) export COLORTERM= ;;
                other) export COLORTERM=256color ;;
            esac
            case "$profile" in
                default) unset BOXRIG_TERM ;;
                xterm) export BOXRIG_TERM=xterm ;;
            esac
            export TERM=vt100 NO_COLOR=keep FORCE_COLOR=1 BOXRIG_TEST_INHERITED=preserved
            timeout 30 "$sample_binary" "--renderer=$renderer" -- /bin/sh -c '
                printf "%s\n" "$COLORTERM" "$TERM" "$NO_COLOR" "$FORCE_COLOR" \
                    "$BOXRIG_TEST_INHERITED" > "$1"
            ' sh "$test_dir/actual"
        ) >"$test_dir/sample.log" 2>&1 || {
            cat "$test_dir/sample.log" >&2
            exit 1
        }
        expected_term=xterm-256color
        if [ "$profile" = xterm ]; then expected_term=xterm; fi
        printf '%s\n' truecolor "$expected_term" keep 1 preserved >"$test_dir/expected"
        if ! cmp -s "$test_dir/expected" "$test_dir/actual"; then
            echo "FAIL $sample_binary: COLORTERM=$inherited, TERM profile=$profile" >&2
            diff -u "$test_dir/expected" "$test_dir/actual" >&2 || :
            exit 1
        fi
    done
done
echo "PASS $sample_binary: child color capabilities and inherited environment"
