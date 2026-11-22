#!/usr/bin/env bash
# Build and open the native MUA3 Recomp GUI on Linux.
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")/.."

export LARECOMP_DATA_ROOT="$PWD"
# SteamOS needs a writable development environment for every INSTALL stage,
# even when the prebuilt GUI already runs on the host.
if [[ ! -f /run/.containerenv && ! -f /run/.toolboxenv ]]; then
    os_id=$( . /etc/os-release; printf '%s' "${ID:-}" )
    case "$os_id" in
        steamos|holo)
            exec bash "$PWD/tools/steamdeck_setup.sh" "$@"
            ;;
        *)
            # Immutable desktops (Bazzite, Silverblue, Kinoite, ...) have no compiler or development packages on the
            # host: INSTALL uses the same Fedora Distrobox as the Steam Deck. Hosts with the packages (or a ready
            # mua3recomp Toolbox, which tools/build_linux.sh uses itself) keep building on the host.
            if ! sh "$PWD/tools/build_linux.sh" --check >/dev/null 2>&1 && command -v distrobox >/dev/null 2>&1; then
                echo 'Development packages missing on this system: using the mua3recomp-deck Distrobox.'
                exec bash "$PWD/tools/steamdeck_setup.sh" "$@"
            fi
            ;;
    esac
fi
if [[ -x build/mua3launcher && build/mua3launcher -nt launcher/launcher.cpp ]]; then
    exec build/mua3launcher "$@"
fi

need=()
for tool in make pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || need+=("$tool")
done

if command -v pkg-config >/dev/null 2>&1; then
    for package in sdl3; do
        pkg-config --exists "$package" || need+=("$package development files")
    done
fi

if command -v clang++ >/dev/null 2>&1; then
    launcher_cxx=clang++
elif command -v g++ >/dev/null 2>&1; then
    launcher_cxx=g++
else
    need+=("clang++ or g++")
fi

if ((${#need[@]})); then
    printf 'Missing Linux build dependencies: %s\n\n' "${need[*]}" >&2
    . /etc/os-release 2>/dev/null || true
    case "${ID:-unknown}" in
        bazzite)
            cat >&2 <<'EOF'
Bazzite uses an immutable host. Build in a Toolbox so the compiler and development
packages stay separate from the operating system. In a terminal, run:

  toolbox create --container mua3recomp
  toolbox enter mua3recomp
  sudo dnf install clang lld make python3 python3-pip python3-devel SDL3-devel vulkan-headers vulkan-loader-devel VulkanMemoryAllocator-devel libshaderc-devel ffmpeg-free-devel pkgconf-pkg-config

Then go to this shared project folder inside the Toolbox and run ./run-linux.sh.
EOF
            ;;
        fedora)
            echo 'Install the Fedora packages: clang lld make python3 python3-pip python3-devel SDL3-devel vulkan-headers vulkan-loader-devel VulkanMemoryAllocator-devel libshaderc-devel ffmpeg-free-devel pkgconf-pkg-config' >&2
            ;;
        arch|manjaro|endeavouros)
            echo 'Install: clang lld make python python-pip sdl3 vulkan-headers vulkan-icd-loader vulkan-memory-allocator shaderc ffmpeg pkgconf' >&2
            ;;
        ubuntu|debian|linuxmint|pop)
            echo 'Install: clang lld make python3 python3-venv libsdl3-dev libvulkan-dev libvulkan-memory-allocator-dev libshaderc-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev pkg-config' >&2
            ;;
        *)
            echo 'Install clang, lld, make, Python 3, pkg-config, SDL3, Vulkan, shaderc and FFmpeg development packages for your distribution.' >&2
            ;;
    esac
    exit 1
fi

make -j"${MUA3_BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}" launcher CXX="$launcher_cxx"
exec build/mua3launcher "$@"
