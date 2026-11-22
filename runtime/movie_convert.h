#pragma once
#include <stdint.h>
#include <libavutil/frame.h>

typedef struct MovieConverter MovieConverter;
// Synchronous conversion. The caller owns NV12; the converter owns the returned RGBA frame until its next call.
// Two slice workers by default in movie.c; one worker uses the original sws_scale path.
MovieConverter *movie_converter_create(int width, int height, int threads);
const AVFrame *movie_converter_run(MovieConverter *c, uint8_t *nv12, int stride);
void movie_converter_free(MovieConverter *c);
