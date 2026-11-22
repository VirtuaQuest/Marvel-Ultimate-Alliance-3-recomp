#!/usr/bin/env bash
# Add this file to Steam for Game Mode. Complete INSTALL in Desktop Mode first.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
exec >> "$PWD/game-mode.log" 2>&1
printf '\nMUA3 Recomp Game Mode start: %s\n' "$PWD"
export LARECOMP_DATA_ROOT="$PWD"
export MUA3_GAME_MODE=1
if [[ ! -x build/mua3recomp || ! -f build/mua3game.so || ! -d extracted ]]; then
    echo 'Game setup is incomplete. Open MUA3 Recomp.desktop in Desktop Mode and finish INSTALL first.'
    exit 1
fi
# Use the active Gamescope X server when available, keeping the Steam launch
# environment and process chain intact. Respect an explicit video backend.
if [[ -n ${DISPLAY:-} ]]; then
    export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-x11}
fi
# No setup container, detached session, GUI launcher or helper dialog here.
exec /bin/sh "$PWD/tools/start_game_linux.sh" extracted "$@"
