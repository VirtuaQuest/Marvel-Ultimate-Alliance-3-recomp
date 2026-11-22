// Component checks with synthetic frames only. No game, keys, display, or GPU required.
#include "movie_convert.h"
#include "nvn/compute_limits.h"
#include "nvn/shader_prep.h"
#include <libswscale/swscale.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void check_shared(void) {
    uint8_t control[0x848] = {0};
    assert(nvn_compute_shared_bytes(NULL) == 0);
    assert(nvn_compute_shared_bytes(control) == 0);
    put32(control, 0x98761234); put32(control + 0x20, sizeof control);
    const uint32_t sizes[] = {4, 512, 1024, 2816, 3072, 4224, 5628, 32832, 49152};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        put32(control + 0x754, sizes[i]);
        assert(nvn_compute_shared_bytes(control) == sizes[i]);
        assert(nvn_compute_shared_words(sizes[i]) == sizes[i] / 4);
        ShaderRec a = {.kind = 'C', .vs = 0x1234, .local = {64, 1, 1}, .shared_bytes = sizes[i], .stencil = 3}, b;
        char line[256]; shader_rec_format(&a, line, sizeof line);
        assert(shader_rec_parse(line, &b));
        assert(b.vs == a.vs && b.shared_bytes == a.shared_bytes && b.stencil == a.stencil);
        assert(!memcmp(a.local, b.local, sizeof a.local));
    }
    const uint32_t invalid[] = {0, 3, 49153, UINT32_MAX};
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        put32(control + 0x754, invalid[i]);
        assert(nvn_compute_shared_bytes(control) == 0);
        assert(nvn_compute_shared_words(invalid[i]) == 12288);
    }
    put32(control + 0x754, 512); put32(control + 0x20, 0x754);
    assert(nvn_compute_shared_bytes(control) == 0);
    ShaderRec old;
    assert(shader_rec_parse("C cs=1234 l=64,1,1", &old));
    assert(old.shared_bytes == 0 && nvn_compute_shared_words(old.shared_bytes) == 12288);
    puts("PASS: compute allocation bounds, record round trips, old-record fallback");
}
static void check_movie(int w, int h, int pad, int frames, int threads) {
    int stride = w + pad;
    size_t size = (size_t)stride * (h + h / 2);
    uint8_t *src = malloc(size), *expected = malloc((size_t)w * h * 4);
    assert(src && expected);
    struct SwsContext *old = sws_getContext(w, h, AV_PIX_FMT_NV12, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    MovieConverter *c = movie_converter_create(w, h, threads);
    assert(old && c);
    assert(!movie_converter_run(c, NULL, stride));
    assert(!movie_converter_run(c, src, w - 1));
    const uint8_t *sp[4] = {src, src + (size_t)stride * h, NULL, NULL};
    int sl[4] = {stride, stride, 0, 0}, dl[4] = {w * 4, 0, 0, 0};
    uint8_t *dp[4] = {expected, NULL, NULL, NULL};
    double old_s = 0, new_s = 0;
    for (int f = 0; f < frames; f++) {
        // Chroma extremes, changing frames and padded lines expose boundary/lifetime/stride mistakes.
        for (size_t i = 0; i < size; i++) src[i] = (uint8_t)(i * 13 + (i / stride) * 17 + f * 7);
        double t = now(); assert(sws_scale(old, sp, sl, 0, h, dp, dl) == h); old_s += now() - t;
        t = now(); const AVFrame *out = movie_converter_run(c, src, stride); new_s += now() - t;
        assert(out && out->width == w && out->height == h);
        for (int y = 0; y < h; y++)
            assert(!memcmp(expected + (size_t)y * w * 4, out->data[0] + (size_t)y * out->linesize[0], (size_t)w * 4));
    }
    printf("PASS: %dx%d stride %d, %d workers, %d frames identical; legacy %.3f ms, new %.3f ms/frame\n",
           w, h, stride, threads, frames, old_s * 1000 / frames, new_s * 1000 / frames);
    movie_converter_free(c); sws_freeContext(old); free(src); free(expected);
}
int main(void) {
    check_shared();
    assert(!movie_converter_create(1279, 720, 2));
    assert(!movie_converter_create(1280, 720, 0));
    assert(!movie_converter_run(NULL, NULL, 0));
    movie_converter_free(NULL);
    check_movie(1280, 720, 0, 6, 1);
    check_movie(1280, 720, 32, 6, 2);
    check_movie(1920, 1080, 0, 60, 2);
    check_movie(854, 480, 10, 6, 2);
    check_movie(640, 360, 0, 6, 4);
    return 0;
}
