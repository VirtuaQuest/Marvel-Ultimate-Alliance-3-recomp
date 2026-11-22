#!/usr/bin/env bash
# Optional AMD compression candidate; standard play-steam.sh keeps safer defaults.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
export LARECOMP_VK_LEAN_USAGE=1
export LARECOMP_GPU_OVERLAP=0
export LARECOMP_NO_FRAME_CLEAR=0
exec bash "$PWD/play-steam.sh" "$@"
