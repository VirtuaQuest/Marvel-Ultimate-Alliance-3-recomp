#!/bin/sh
# Compile inside the existing development Toolbox when the host lacks headers.
set -eu
cd "$(dirname "$0")/.."
missing=""
for tool in clang clang++ ld.lld make pkg-config python3; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
if command -v pkg-config >/dev/null 2>&1; then
    for package in sdl3 vulkan shaderc libavformat libavcodec libavutil libswscale; do
        pkg-config --exists "$package" || missing="$missing $package"
    done
    if pkg-config --exists sdl3 && command -v clang >/dev/null 2>&1; then
        if ! printf '#include <SDL3/SDL.h>\n' | clang $(pkg-config --cflags sdl3) -E -x c - >/dev/null 2>&1; then
            missing="$missing SDL3-headers"
        fi
    fi
fi
# VMA is header-only and has no pkg-config entry. Check the same include paths
# as the runtime so missing headers are caught before a long game build.
if command -v clang >/dev/null 2>&1; then
    if ! printf '#if __has_include(<vma/vk_mem_alloc.h>)\n#include <vma/vk_mem_alloc.h>\n#else\n#include <vk_mem_alloc.h>\n#endif\n' | clang -Ithird_party -E -x c - >/dev/null 2>&1; then
        missing="$missing VulkanMemoryAllocator-headers"
    fi
fi
if [ -n "$missing" ]; then
    if [ ! -f /run/.toolboxenv ] && [ ! -f /run/.containerenv ] && command -v toolbox >/dev/null 2>&1; then
        echo "Using the mua3recomp Toolbox for the native build."
        exec toolbox run --container mua3recomp sh "$PWD/tools/build_linux.sh" "$@"
    fi
    echo "ERROR: missing native build dependencies:$missing" >&2
    echo "In your mua3recomp Toolbox install: clang lld make SDL3-devel vulkan-headers vulkan-loader-devel VulkanMemoryAllocator-devel libshaderc-devel ffmpeg-free-devel pkgconf-pkg-config" >&2
    echo "Then reopen the launcher from that Toolbox, or start it on the host with Toolbox available." >&2
    exit 1
fi
if [ "${1:-}" = --check ]; then
    echo "Native compiler and development packages found."
    exit 0
fi
make -j"${MUA3_BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}" build/mua3recomp build/mua3game.so PY=.venv-linux/bin/python
python3 tools/bundle_runtime_linux.py
