// MUA3's button icons in the controller's own style (runtime/nvn/mua3_prompts.c): the game's 64x64 BC3 glyphs from
// UI/screenlayout/80_button_icon/80_button_icon.ext.kslt, recognised at upload by content and relabelled in place.
// Called from runtime/nvn/texpatch.c (identify / apply), so both backends get it.
#pragma once
#include <stdint.h>

#include "nvn.h"

// Prompt styles (texpatch.c prompt_style uses the same numbers)
enum { MP_STYLE_NINTENDO, MP_STYLE_XBOX, MP_STYLE_PS, MP_STYLE_DECK, MP_STYLE_STEAMC, MP_NSTYLES };
// Labels: which button a Switch input's glyph names now (texpatch.c prompt_label, by the controller bindings).
// MP_A..MP_MINUS = that Switch button's own position (A = east, B = south, X = north, Y = west).
enum { MP_A, MP_B, MP_X, MP_Y, MP_L, MP_R, MP_ZL, MP_ZR, MP_PLUS, MP_MINUS, MP_NROLES,
       MP_LS = MP_NROLES, MP_RS, MP_DUP, MP_DDOWN, MP_DLEFT, MP_DRIGHT, MP_OTHER };

// The ids texpatch.c hands out for these: MP_ID_BASE + index (below TEXPATCH_RECOLOUR_BASE)
#define MP_ID_BASE 2048

// Which MUA3 glyph `t` is (index), or -1. Cheap for textures of other sizes / formats.
int mua3p_identify(const NvnTexture *t);
// The same for data the game copies to a texture from a buffer (nvnCommandBufferCopyBufferToTexture: how MUA3 fills
// these, so the texture's own memory never holds them): `lin` = level 0's BC3 blocks, `row` bytes per block row.
int mua3p_identify_blocks(const uint8_t *lin, uint32_t w, uint32_t h, uint32_t row);
// Relabels level `lv` (linear BC3 blocks, `row` bytes per block row) of glyph `id` for `style`; labels[MP_A..MP_MINUS]
// = the label each Switch input's glyph gets. No-op for the Nintendo style unless a button was remapped.
void mua3p_apply(int id, uint32_t lv, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t row, int style,
                 const int labels[MP_NROLES]);
