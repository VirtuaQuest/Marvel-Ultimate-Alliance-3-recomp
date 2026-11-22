#!/bin/sh
# Launch with bundled media libraries and the desktop's graphics driver.
set -eu
cd "$(dirname "$0")/.."
export LARECOMP_DATA_ROOT="$PWD"
if [ -d "$PWD/build/lib" ]; then
    export LD_LIBRARY_PATH="$PWD/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

# Steam Deck compatibility defaults. Explicit environment settings take priority.
steam_deck_defaults() {
    sh "$PWD/tools/deck_resolution_linux.sh"
    if [ "${MUA3_DECK_NATIVE:-1}" != 0 ]; then
        # The game's base targets are 1920x1080. Bound the actual allocation
        # scale to 1280x720 even if a stale menu setting still requests 2x/4K.
        export LARECOMP_SCALE=0.6666666667
        echo '[linux-start] Deck render scale fixed at 1280x720; window defaults 1280x800.'
    fi
    # Shader preparation at start: the shipped list and this player's own (cache/shader_seen.txt, every variant
    # built before), so areas and effects seen once don't stop the game to compile again. =0: off.
    export LARECOMP_SHADER_PREP=${LARECOMP_SHADER_PREP:-1}
    # First-use builds block (the black-screen workaround). =0: built in the background, the draws needing them
    # skipped meanwhile (no pause, effects missing for a moment). The runtime tests the variable for presence.
    sync_shaders=${LARECOMP_SYNC_SHADERS:-1}
    if [ "$sync_shaders" = 0 ]; then unset LARECOMP_SYNC_SHADERS; else export LARECOMP_SYNC_SHADERS="$sync_shaders"; fi
    export LARECOMP_LOG_SKIPS=${LARECOMP_LOG_SKIPS:-1}
    if [ "${MUA3_DECK_RENDER_SAFE:-1}" != 0 ]; then
        # Use the conservative render paths used on discrete GPUs: no overlap,
        # full image usage, and initialized render targets every frame. Keep
        # video conversion on the CPU to isolate the GPU's NV12 output path.
        # CPU and GPU overlapped, compressed images, no per-frame target clears: measured on a Steam Deck in the first
        # heavy fight (v7.7): 49.8 -> 55.5 fps median, 29.4 -> 44.9 fps p10, no visual issues. =0 each: the old safe path.
        export LARECOMP_GPU_OVERLAP=${LARECOMP_GPU_OVERLAP:-1}
        export LARECOMP_VK_LEAN_USAGE=${LARECOMP_VK_LEAN_USAGE:-1}
        export LARECOMP_NO_FRAME_CLEAR=${LARECOMP_NO_FRAME_CLEAR:-1}
        # Movies: GPU NV12 conversion by default (measured on a Steam Deck: 12 of 58 frames left out at a movie start,
        # 53 of 56 with the CPU path). =1: CPU conversion. The runtime tests the variable for presence: 0 unsets it.
        movie_cpu=${LARECOMP_MOVIE_CPU_CONVERT:-0}
        if [ "$movie_cpu" = 0 ]; then unset LARECOMP_MOVIE_CPU_CONVERT; else export LARECOMP_MOVIE_CPU_CONVERT="$movie_cpu"; fi
        echo "[linux-start] Deck conservative rendering: overlap=$LARECOMP_GPU_OVERLAP lean=$LARECOMP_VK_LEAN_USAGE skip-clears=$LARECOMP_NO_FRAME_CLEAR movie-cpu=$movie_cpu"
    fi
    echo "[linux-start] SteamOS shaders: prep=$LARECOMP_SHADER_PREP synchronous=$sync_shaders; first-use builds may pause rendering."
}
os_id=$( . /etc/os-release; printf '%s' "${ID:-}" )
case "$os_id" in steamos|holo) steam_deck_defaults ;; esac

# The host bridge does not necessarily inherit the container's display session.
# Pass populated display variables explicitly and translate host mount paths.
start_on_host() {
    set -- sh "$PWD/tools/start_game_linux.sh" "$@"
    if [ -n "${DISPLAY:-}" ]; then set -- "DISPLAY=$DISPLAY" "$@"; fi
    if [ -n "${WAYLAND_DISPLAY:-}" ]; then
        set -- "WAYLAND_DISPLAY=${WAYLAND_DISPLAY#/run/host}" "$@"
    fi
    if [ -n "${XDG_RUNTIME_DIR:-}" ]; then
        set -- "XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR#/run/host}" "$@"
    fi
    if [ -n "${XAUTHORITY:-}" ]; then
        set -- "XAUTHORITY=${XAUTHORITY#/run/host}" "$@"
    fi
    # Retain Steam's application identity when a container-to-host hop is needed.
    for name in SteamAppId SteamGameId SteamOverlayGameId STEAM_COMPAT_APP_ID SteamGamepadUI SDL_APP_ID SDL_VIDEODRIVER MUA3_GAME_MODE MUA3_DECK_RENDER_SAFE MUA3_DECK_NATIVE; do
        value=$(printenv "$name") || continue
        set -- "$name=$value" "$@"
    done
    exec distrobox-host-exec env "LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}" "$@"
}
if [ -f /run/.containerenv ] && command -v distrobox-host-exec >/dev/null 2>&1; then
    if host_libs=$(distrobox-host-exec env LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" ldd "$PWD/build/mua3recomp" 2>&1) &&
        ! printf '%s\n' "$host_libs" | grep -q 'not found'; then
        start_on_host "$@"
    else
        echo '[linux-start] Host libraries are incompatible; using the container graphics environment.'
    fi
fi
# When the Deck must run inside Fedora, use the same compatibility defaults.
if [ -f /run/.containerenv ] && [ -r /run/host/etc/os-release ]; then
    host_id=$( . /run/host/etc/os-release; printf '%s' "${ID:-}" )
    case "$host_id" in steamos|holo) steam_deck_defaults ;; esac
fi
if ldd build/mua3recomp 2>&1 | grep -q 'not found'; then
    if [ "${MUA3_GAME_MODE:-0}" != 1 ] && [ ! -f /run/.toolboxenv ] && [ ! -f /run/.containerenv ] && command -v toolbox >/dev/null 2>&1; then
        exec toolbox run --container mua3recomp sh "$PWD/tools/start_game_linux.sh" "$@"
    fi
    echo 'Game runtime libraries are missing in this environment:' >&2
    ldd build/mua3recomp >&2
    exit 1
fi
printf '[linux-start] Display session: DISPLAY=%s WAYLAND_DISPLAY=%s XDG_RUNTIME_DIR=%s\n' "${DISPLAY:-unset}" "${WAYLAND_DISPLAY:-unset}" "${XDG_RUNTIME_DIR:-unset}"
exec build/mua3recomp "$@"
