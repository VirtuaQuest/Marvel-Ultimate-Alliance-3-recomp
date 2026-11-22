#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/mua3-performance.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
make -s build/inc/shader_code_locs.inc build/inc/shader_variants.inc
${CC:-clang} -O2 -g -ffunction-sections -fdata-sections -Iruntime -Ibuild/inc \
    $(pkg-config --cflags libswscale libavutil) \
    tools/checks/performance.c runtime/movie_convert.c runtime/nvn/shader_prep.c \
    -Wl,--gc-sections $(pkg-config --libs libswscale libavutil) -pthread -o "$tmp/performance"
"$tmp/performance"
${CC:-clang} -O2 -g -ffunction-sections -fdata-sections -Iruntime -Ithird_party \
    $(pkg-config --cflags sdl3 vulkan) tools/checks/upload_ownership.c runtime/nvn/vulkan/textures.c \
    -Wl,--gc-sections -lm -pthread -o "$tmp/upload_ownership"
"$tmp/upload_ownership"
