#!/usr/bin/env bash
# boxrig - Under MIT. Compose the existing isolated IBus and fcitx runners.
set -euo pipefail
shift # The shared IBus runner passes a font argument first.
exec bash tests/ime/run_fcitx5.sh "$@"
