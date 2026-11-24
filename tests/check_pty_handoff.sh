#!/bin/sh
set -eu
driver=$1
test "$("$driver")" = 'round trip: 3 replies from the same shell, resize observed, child reaped'
test "$("$driver" --rollback)" = 'rollback: 3 replies from the same shell, resize observed, child reaped'
echo 'PASS PTY checkpoints preserve the running shell across transfer and rollback'
