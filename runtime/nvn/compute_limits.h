#pragma once
#include <stdint.h>
#include <string.h>

#define NVN_COMPUTE_SHARED_MAX (48u * 1024u)

// NVN control: +0x20 section length, +0x748 workgroup xyz, +0x754 shared bytes.
// Unknown headers and unusable sizes keep the translator's previous 48 KB allocation.
static inline uint32_t nvn_compute_shared_bytes(const uint8_t *control) {
    uint32_t magic, length, bytes;
    if (!control) return 0;
    memcpy(&magic, control, 4);
    memcpy(&length, control + 0x20, 4);
    if (magic != 0x98761234u || length < 0x758 || length > 0x10000) return 0;
    memcpy(&bytes, control + 0x754, 4);
    return bytes && bytes <= NVN_COMPUTE_SHARED_MAX && !(bytes & 3) ? bytes : 0;
}

static inline uint32_t nvn_compute_shared_words(uint32_t bytes) {
    return bytes && bytes <= NVN_COMPUTE_SHARED_MAX && !(bytes & 3)
        ? bytes / 4 : NVN_COMPUTE_SHARED_MAX / 4;
}
