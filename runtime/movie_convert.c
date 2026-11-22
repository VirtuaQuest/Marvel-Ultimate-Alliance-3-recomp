#include "movie_convert.h"
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <stdlib.h>

struct MovieConverter {
    struct SwsContext *sws;
    AVFrame *src, *dst;
    int width, height, threads;
};

// The game's output slot remains owned by the decoder until ReleaseOutputBufferIndex. sws_scale_frame completes
// its workers before returning; the reference prevents FFmpeg from copying the non-refcounted input frame.
static void borrowed_buffer_free(void *opaque, uint8_t *data) { (void)opaque; (void)data; }

MovieConverter *movie_converter_create(int w, int h, int threads) {
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1) || threads < 1 || threads > 4) return NULL;
    MovieConverter *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->width = w; c->height = h; c->threads = threads;
    c->sws = sws_alloc_context();
    c->src = av_frame_alloc(); c->dst = av_frame_alloc();
    if (!c->sws || !c->src || !c->dst) goto fail;
    const char *keys[] = {"srcw", "srch", "src_format", "dstw", "dsth", "dst_format", "sws_flags", "threads"};
    int values[] = {w, h, AV_PIX_FMT_NV12, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, threads};
    for (int i = 0; i < 8; i++)
        if (av_opt_set_int(c->sws, keys[i], values[i], 0) < 0) goto fail;
    if (sws_init_context(c->sws, NULL, NULL) < 0) goto fail;
    c->dst->format = AV_PIX_FMT_RGBA;
    c->dst->width = w; c->dst->height = h;
    if (av_frame_get_buffer(c->dst, 32) < 0) goto fail;
    return c;
fail:
    movie_converter_free(c);
    return NULL;
}

const AVFrame *movie_converter_run(MovieConverter *c, uint8_t *nv12, int stride) {
    if (!c || !nv12 || stride < c->width) return NULL;
    AVFrame *s = c->src;
    av_frame_unref(s);
    s->format = AV_PIX_FMT_NV12;
    s->width = c->width; s->height = c->height;
    s->buf[0] = av_buffer_create(nv12, (size_t)stride * (c->height + c->height / 2),
                                 borrowed_buffer_free, NULL, AV_BUFFER_FLAG_READONLY);
    if (!s->buf[0]) return NULL;
    s->data[0] = nv12; s->data[1] = nv12 + (size_t)stride * c->height;
    s->linesize[0] = s->linesize[1] = stride;
    int r;
    if (c->threads == 1) {
        r = sws_scale(c->sws, (const uint8_t *const *)s->data, s->linesize, 0, c->height,
                      c->dst->data, c->dst->linesize);
        if (r != c->height) r = -1;
    } else {
        // sws_scale always uses only the first slice context, even when threads > 1. The frame API dispatches
        // the slice workers. Explicitly initialized options preserve the old conversion's color/filter rules.
        r = sws_scale_frame(c->sws, c->dst, s);
    }
    av_frame_unref(s);
    return r < 0 ? NULL : c->dst;
}

void movie_converter_free(MovieConverter *c) {
    if (!c) return;
    sws_freeContext(c->sws);
    av_frame_free(&c->src); av_frame_free(&c->dst);
    free(c);
}
