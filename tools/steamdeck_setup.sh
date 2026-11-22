#!/usr/bin/env bash
# Start the complete launcher/setup workflow in a writable Fedora Distrobox.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
root=$PWD
box=mua3recomp-deck

ready() {
    sh "$root/tools/build_linux.sh" --check >/dev/null 2>&1
}

# Movies: Fedora's ffmpeg-free has no H.264 decoder (they fall back to the slow openh264, choppy at 1080p), so the
# container gets the full FFmpeg from RPM Fusion here - downloaded by the player's own setup, not shipped with the
# release (codec patents). The game's bundled libraries (build/lib) are refreshed from it once. Same library versions,
# no rebuild. MUA3_FULL_FFMPEG=0: keep ffmpeg-free.
full_ffmpeg() {
    [[ ${MUA3_FULL_FFMPEG:-1} != 0 ]] || return 0
    if ! rpm -q ffmpeg-libs >/dev/null 2>&1; then
        echo 'Movies: installing the full FFmpeg (H.264, hardware video decoding) from RPM Fusion in the container...'
        if ! { rpm -q rpmfusion-free-release >/dev/null 2>&1 ||
               sudo dnf install -y "https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm"; } ||
           ! sudo dnf install -y --allowerasing ffmpeg-libs ffmpeg-devel; then
            echo 'Movies: full FFmpeg not installed (no network?): movies use the slower decoder; the next start tries again.'
            return 0
        fi
    fi
    if [[ -x "$root/build/mua3recomp" && ! -f "$root/build/lib/.full-ffmpeg" ]]; then
        echo 'Movies: copying the full FFmpeg into the game'"'"'s libraries...'
        python3 "$root/tools/bundle_runtime_linux.py" && touch "$root/build/lib/.full-ffmpeg"
    fi
}

if [[ ${1:-} == --inside ]]; then
    shift
    # This mode is called only after entering the development container.
    if [[ ! -f /run/.containerenv && ! -f /run/.toolboxenv ]]; then
        echo 'ERROR: dependency installation must run inside the development container.' >&2
        exit 1
    fi
    if ! ready; then
        echo 'Installing the Linux compiler and development libraries in the container...'
        sudo dnf install -y clang lld make python3 python3-pip python3-devel \
            SDL3-devel vulkan-headers vulkan-loader-devel VulkanMemoryAllocator-devel libshaderc-devel \
            ffmpeg-free-devel pkgconf-pkg-config mesa-vulkan-drivers
    fi
    ready || { echo 'ERROR: development packages are still missing.' >&2; exit 1; }
    full_ffmpeg
    echo 'Development environment ready. Opening MUA3 Recomp...'
    export MUA3_BUILD_JOBS=${MUA3_BUILD_JOBS:-2}
    exec bash "$root/run-linux.sh" --launcher "$@"
fi

terminal=0
if [[ ${1:-} == --terminal ]]; then
    terminal=1
    shift
fi
# Keep first-time setup errors visible when launched by clicking the script.
exec > >(tee -a "$root/setup-linux.log") 2>&1
if ((terminal)); then
    trap 'result=$?; if ((result)); then echo; echo "Setup failed (exit $result). See setup-linux.log in the project folder."; read -r -p "Press Enter to close..." reply || true; fi' EXIT
fi

if ! command -v distrobox >/dev/null 2>&1; then
    echo 'ERROR: Distrobox is missing. Update SteamOS in Desktop Mode (or install distrobox), then try again.' >&2
    echo 'Manual installation guide: https://github.com/89luca89/distrobox/blob/main/docs/posts/steamdeck_guide.md' >&2
    if ((!terminal)) && command -v kdialog >/dev/null 2>&1; then
        kdialog --error 'Distrobox is missing. Update SteamOS, then try again. See STEAMOS.md for the manual installation guide.' || true
    fi
    exit 1
fi

# Distrobox lists ID | NAME | STATUS | IMAGE. Match the full name.
listing=$(distrobox list --no-color)
exists=0
if awk -F '|' -v name="$box" '{gsub(/^[ \t]+|[ \t]+$/, "", $2); if ($2 == name) found=1} END {exit !found}' <<< "$listing"; then
    exists=1
fi
state_dir=${XDG_STATE_HOME:-$HOME/.local/state}/mua3recomp
# An existing initialized container needs no setup terminal on later launches.
if ((exists)); then
    if distrobox enter --name "$box" --no-tty -- sh "$root/tools/build_linux.sh" --check; then
        exec distrobox enter --name "$box" --no-tty -- bash "$root/tools/steamdeck_setup.sh" --inside "$@"
    fi
fi

if [[ ! -t 0 ]] && ((!terminal)); then
    if command -v konsole >/dev/null 2>&1; then
        exec konsole -e bash "$root/tools/steamdeck_setup.sh" --terminal "$@"
    elif command -v ptyxis >/dev/null 2>&1; then
        exec ptyxis -- bash "$root/tools/steamdeck_setup.sh" --terminal "$@"
    elif command -v gnome-terminal >/dev/null 2>&1; then
        exec gnome-terminal --wait -- bash "$root/tools/steamdeck_setup.sh" --terminal "$@"
    elif command -v xterm >/dev/null 2>&1; then
        exec xterm -e bash "$root/tools/steamdeck_setup.sh" --terminal "$@"
    fi
    echo 'Open Desktop Mode, then run ./run.sh from Konsole for first-time setup.' >&2
    exit 1
fi

echo 'MUA3 Recomp: preparing the development environment (a Fedora Distrobox).'
echo 'First setup downloads Fedora and development packages. Keep this window open.'
if ((!exists)); then
    distrobox create --yes --name "$box" --image docker.io/library/fedora:44
fi
# Check and install packages before marking this container ready. Pass the
# project path as an argument so spaces and punctuation remain literal.
distrobox enter --name "$box" -- bash -c '
    set -e
    cd "$1"
    sudo dnf install -y clang lld make python3 python3-pip python3-devel SDL3-devel vulkan-headers vulkan-loader-devel VulkanMemoryAllocator-devel libshaderc-devel ffmpeg-free-devel pkgconf-pkg-config mesa-vulkan-drivers
' bash "$root"
mkdir -p "$state_dir"
touch "$state_dir/steamdeck-ready"
# Do not exec here: retain the terminal error handler if launch fails.
distrobox enter --name "$box" -- bash "$root/tools/steamdeck_setup.sh" --inside "$@"
