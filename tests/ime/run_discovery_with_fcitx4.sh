#!/usr/bin/env bash
# boxrig - Under MIT. The IBus runner passes a font argument first.
set -euo pipefail
shift
exec bash tests/ime/run_fcitx4.sh "$@"
