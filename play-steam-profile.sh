#!/usr/bin/env bash
# Optional diagnostic entry: use only to collect a short GPU performance log.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
export LARECOMP_VK_PROF=1
export LARECOMP_GPUPROF_EVERY=120
export LARECOMP_LOG_MOVIE_OPEN=1
exec bash "$PWD/play-steam.sh" "$@"
