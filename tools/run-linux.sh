#!/usr/bin/env bash
# Build and open the native MUA3 Recomp GUI on Linux.
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"

need=()
for tool in make clang clang++ pkg-config python3; do
    command -v "$tool" >/dev/null 2>&1 || need+=("$tool")
done

if command -v pkg-config >/dev/null 2>&1; then
    for package in sdl3 vulkan shaderc libavformat libavcodec libavutil libswscale; do
        pkg-config --exists "$package" || need+=("$package development files")
    done
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
  sudo dnf install clang lld make python3 python3-pip python3-devel SDL3-devel vulkan-headers vulkan-loader-devel libshaderc-devel ffmpeg-free-devel pkgconf-pkg-config

Then go to this shared project folder inside the Toolbox and run ./run-linux.sh.
EOF
            ;;
        fedora)
            echo 'Install the Fedora packages: clang lld make python3 python3-pip python3-devel SDL3-devel vulkan-headers vulkan-loader-devel libshaderc-devel ffmpeg-free-devel pkgconf-pkg-config' >&2
            ;;
        arch|manjaro|endeavouros)
            echo 'Install: clang lld make python python-pip sdl3 vulkan-headers vulkan-icd-loader shaderc ffmpeg pkgconf' >&2
            ;;
        ubuntu|debian|linuxmint|pop)
            echo 'Install: clang lld make python3 python3-venv libsdl3-dev libvulkan-dev libshaderc-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev pkg-config' >&2
            ;;
        *)
            echo 'Install clang, lld, make, Python 3, pkg-config, SDL3, Vulkan, shaderc and FFmpeg development packages for your distribution.' >&2
            ;;
    esac
    exit 1
fi

make -j"$(nproc 2>/dev/null || echo 4)" launcher
exec build/mua3launcher "$@"
