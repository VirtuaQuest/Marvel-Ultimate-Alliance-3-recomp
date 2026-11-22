// MUA3's button icons in the controller's own style: Xbox, PlayStation, Steam Deck, Steam Controller (or Nintendo
// letters for a remapped button). Edits made to the game's own glyph textures as they're uploaded; no game art ships.
//
// The glyphs: UI/screenlayout/80_button_icon/80_button_icon.ext.kslt (Koei Tecmo "TLSK3100" texture list: a header,
// 20-byte entries whose first word is the texture's file offset, the names, then each texture: a 0x48-byte header -
// format 3 = BC3, u16 width / height at +4, row pitch at +0x20 - and its linear blocks). Other layouts carry
// byte-identical copies of some (btn_L_icon in 00_chara_info, ...), so matching by content covers them too.
// Each texture the game uploads at a glyph's size and a 4x4 / 16-byte block format is compared with them (level 0,
// deswizzled), once per new content (the backends cache it).
//
// The art: a grey button with a yellow label (orange on the "_hold" ones) that has a black outline, and a black rim.
// Relabelling: the label (saturated pixels) and its outline (pixels near it, apart from the rim, which is the dark
// region connected to the outside) are painted over with the button's grey; the new label is drawn with a small
// built-in stroke font (round-capped strokes), its outline being the same strokes drawn wider, in black. The
// Plus / Minus "JOY" glyphs (a bare grey + / -) get the round Plus / Minus button as their base. btn_dir_all_ABXY_icon
// (four yellow discs with black letters) has each disc's letter redrawn.
#include "mua3_prompts.h"

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "../rt.h"
#include "tegra.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define KSLT_PATH "UI/screenlayout/80_button_icon/80_button_icon.ext.kslt"

enum { K_LABEL, K_JOY, K_ABXY };

typedef struct {
    char name[40];
    int role, kind;
    bool hold;
    uint16_t w, h;
    uint8_t *lin;  // level 0's BC3 blocks, rows of (w + 3) / 4 blocks
} Ref;

static Ref g_refs[24];
static int g_n;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static const struct { const char *name; int role, kind; bool hold; } k_glyphs[] = {
    {"btn_A_icon", MP_A, K_LABEL, false},       {"btn_A_icon_hold", MP_A, K_LABEL, true},
    {"btn_B_icon", MP_B, K_LABEL, false},       {"btn_X_icon", MP_X, K_LABEL, false},
    {"btn_Y_icon", MP_Y, K_LABEL, false},       {"btn_Y_icon_hold", MP_Y, K_LABEL, true},
    {"btn_L_icon", MP_L, K_LABEL, false},       {"btn_R_icon", MP_R, K_LABEL, false},
    {"btn_ZL_icon", MP_ZL, K_LABEL, false},     {"btn_ZR_icon", MP_ZR, K_LABEL, false},
    {"btn_Plus_icon", MP_PLUS, K_LABEL, false}, {"btn_Minus_icon", MP_MINUS, K_LABEL, false},
    {"btn_Plus_JOY_icon", MP_PLUS, K_JOY, false}, {"btn_Minus_JOY_icon", MP_MINUS, K_JOY, false},
    {"btn_dir_all_ABXY_icon", -1, K_ABXY, false},
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// ------------------------------------------------------------------ loading
bool romfs_file_find(const char *rel, uint64_t *off, uint64_t *size);  // runtime/services/fs.c
ssize_t romfs_image_read(void *dst, uint64_t n, uint64_t off);

static uint8_t *read_game_file(const char *rel, size_t *len) {  // (an extracted romfs folder, else the romfs image)
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", g_romfs_dir ? g_romfs_dir : "extracted/romfs", rel);
    FILE *f = fopen(p, "rb");
    if (f) {
        uint8_t *d = NULL;
        if (!fseek(f, 0, SEEK_END)) {
            long n = ftell(f);
            if (n > 0 && n < (64 << 20) && !fseek(f, 0, SEEK_SET) && (d = malloc((size_t)n))) {
                if (fread(d, 1, (size_t)n, f) == (size_t)n) *len = (size_t)n;
                else { free(d); d = NULL; }
            }
        }
        fclose(f);
        if (d) return d;
    }
    uint64_t off, size;
    if (!romfs_file_find(rel, &off, &size) || !size || size > (64u << 20)) return NULL;
    uint8_t *d = malloc(size);
    if (!d) return NULL;
    if (romfs_image_read(d, size, off) != (ssize_t)size) { free(d); return NULL; }
    *len = (size_t)size;
    return d;
}

static void load(void) {
    size_t len = 0;
    uint8_t *k = read_game_file(KSLT_PATH, &len);
    if (!k) { LOG("[prompts] %s not found: the game's own button icons\n", KSLT_PATH); return; }
    uint32_t count = len >= 0x40 && !memcmp(k, "TLSK3100", 8) ? rd32(k + 8) : 0, nsz = len >= 0x40 ? rd32(k + 0x14) : 0;
    int64_t tbl = -1;
    if (count && count < 4096 && nsz < len) {
        uint64_t g = (uint64_t)rd32(k + 0x10) + 0x40;  // (where the entries are in every list seen)
        if (g + 20ull * count + nsz + 4 <= len && rd32(k + g) == g + 20ull * count + nsz) tbl = (int64_t)g;
        for (uint64_t s = 20ull * count; tbl < 0 && s + nsz + 4 <= len; s++)  // (else: the entries end where the names start)
            if (rd32(k + s - 20ull * count) == s + nsz) tbl = (int64_t)(s - 20ull * count);
    }
    if (tbl < 0) { LOG("[prompts] %s: not a texture list this version knows\n", KSLT_PATH); free(k); return; }
    const char *nm = (const char *)k + tbl + 20ull * count, *nend = nm + nsz;
    for (uint32_t i = 0; i < count && nm < nend; i++) {
        size_t nl = strnlen(nm, (size_t)(nend - nm));
        char name[64];
        snprintf(name, sizeof name, "%.*s", (int)(nl < 63 ? nl : 63), nm);
        nm += nl + 1;
        int gi = -1;
        for (int j = 0; j < (int)(sizeof k_glyphs / sizeof *k_glyphs) && gi < 0; j++)
            if (!strcmp(name, k_glyphs[j].name)) gi = j;
        uint64_t o = rd32(k + tbl + 20ull * i);
        if (gi < 0 || o + 0x48 > len || g_n == (int)(sizeof g_refs / sizeof *g_refs)) continue;
        uint32_t fmt = rd32(k + o), w = rd16(k + o + 4), h = rd16(k + o + 6), pitch = rd32(k + o + 0x20);
        uint32_t wb = (w + 3) / 4, hb = (h + 3) / 4;
        if (fmt != 3 || !w || !h || w > 256 || h > 256 || pitch < wb * 16 || o + 0x48 + (uint64_t)pitch * (hb - 1) + wb * 16 > len) {
            LOG("[prompts] %s: format %u %ux%u not handled\n", name, fmt, w, h);
            continue;
        }
        Ref *r = &g_refs[g_n];
        if (!(r->lin = malloc((size_t)wb * hb * 16))) continue;
        for (uint32_t y = 0; y < hb; y++) memcpy(r->lin + (size_t)y * wb * 16, k + o + 0x48 + (uint64_t)y * pitch, wb * 16);
        snprintf(r->name, sizeof r->name, "%s", name);
        r->role = k_glyphs[gi].role, r->kind = k_glyphs[gi].kind, r->hold = k_glyphs[gi].hold;
        r->w = (uint16_t)w, r->h = (uint16_t)h;
        g_n++;
    }
    free(k);
    LOG("[prompts] %d MUA3 button icons known\n", g_n);
}

static const Ref *ref_named(const char *name) {
    for (int i = 0; i < g_n; i++) if (!strcmp(g_refs[i].name, name)) return &g_refs[i];
    return NULL;
}

static int match_blocks(const uint8_t *lin, uint32_t w, uint32_t h, uint32_t row) {
    uint32_t wb = (w + 3) / 4, hb = (h + 3) / 4;
    for (int i = 0; i < g_n; i++) {
        if (g_refs[i].w != w || g_refs[i].h != h) continue;
        bool same = true;
        for (uint32_t y = 0; y < hb && same; y++) same = !memcmp(lin + (size_t)y * row, g_refs[i].lin + (size_t)y * wb * 16, wb * 16);
        if (same) return i;
    }
    return -1;
}

int mua3p_identify_blocks(const uint8_t *lin, uint32_t w, uint32_t h, uint32_t row) {
    pthread_once(&g_once, load);
    if (!g_n || !lin || row < (w + 3) / 4 * 16) return -1;
    int found = match_blocks(lin, w, h, row);
    static int nlog, ncand, logall = -1;
    if (logall < 0) logall = getenv("LARECOMP_LOG_PROMPTS") ? 1 : 0;
    if (found >= 0 && nlog++ < 64) LOG("[prompts] %s copied in (%ux%u)\n", g_refs[found].name, w, h);
    else if (found < 0 && logall && ncand++ < 200) LOG("[prompts] copy candidate %ux%u: no match\n", w, h);
    return found;
}

int mua3p_identify(const NvnTexture *t) {
    pthread_once(&g_once, load);
    if (!g_n || !t->addr || t->depth > 1) return -1;
    const FormatInfo *fi = t->layout.fmt;
    if (!fi || fi->bpb != 16 || fi->bw != 4 || fi->bh != 4) return -1;
    bool sized = false;
    for (int i = 0; i < g_n && !sized; i++) sized = g_refs[i].w == t->width && g_refs[i].h == t->height;
    if (!sized) return -1;
    LevelLayout l = tegra_level(&t->layout, 0);
    uint64_t total = tegra_total_size(&t->layout);
    if (l.wb != (t->width + 3) / 4 || l.hb != (t->height + 3) / 4 || l.offset >= total) return -1;
    size_t n = (size_t)l.wb * l.hb * 16;
    uint8_t *lin = malloc(n);
    if (!lin) return -1;
    const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr + l.offset;
    if (t->flags & 0x10) {  // (linear: rows `stride` apart)
        if (!t->stride || l.offset + t->stride * (l.hb - 1) + l.wb * 16 > total) { free(lin); return -1; }
        for (uint32_t y = 0; y < l.hb; y++) memcpy(lin + (size_t)y * l.wb * 16, src + t->stride * y, (size_t)l.wb * 16);
    } else {
        tegra_deswizzle(src, lin, l.wb, l.hb, 1, 16, l.block_height_log2, l.block_depth_log2);
    }
    int found = -1;
    for (int i = 0; i < g_n && found < 0; i++)
        if (g_refs[i].w == t->width && g_refs[i].h == t->height && !memcmp(lin, g_refs[i].lin, n)) found = i;
    free(lin);
    static int nlog, ncand, logall = -1;
    if (logall < 0) logall = getenv("LARECOMP_LOG_PROMPTS") ? 1 : 0;
    if (found >= 0 && nlog++ < 64)
        LOG("[prompts] %s uploaded (%ux%u fmt %#x %s)\n", g_refs[found].name, t->width, t->height, t->format,
            t->flags & 0x10 ? "linear" : "block-linear");
    else if (found < 0 && logall && ncand++ < 200)
        LOG("[prompts] candidate %ux%u fmt %#x %s: no match\n", t->width, t->height, t->format,
            t->flags & 0x10 ? "linear" : "block-linear");
    return found;
}

// ------------------------------------------------------------------ BC3
static void rgb565(uint16_t c, int o[3]) {
    int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    o[0] = r << 3 | r >> 2, o[1] = g << 2 | g >> 4, o[2] = b << 3 | b >> 2;
}

static void bc3_decode(const uint8_t *b, uint8_t px[16][4]) {
    int a0 = b[0], a1 = b[1], pal[8] = {a0, a1};
    if (a0 > a1) for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
    else { for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5; pal[6] = 0; pal[7] = 255; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    int c[4][3];
    rgb565(rd16(b + 8), c[0]);
    rgb565(rd16(b + 10), c[1]);
    for (int k = 0; k < 3; k++) c[2][k] = (2 * c[0][k] + c[1][k]) / 3, c[3][k] = (c[0][k] + 2 * c[1][k]) / 3;
    uint32_t idx = rd32(b + 12);
    for (int i = 0; i < 16; i++) {
        for (int k = 0; k < 3; k++) px[i][k] = (uint8_t)c[(idx >> (2 * i)) & 3][k];
        px[i][3] = (uint8_t)pal[(bits >> (3 * i)) & 7];
    }
}

static uint16_t to565(const uint8_t *p) { return (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3); }

static void bc3_encode(const uint8_t px[16][4], uint8_t *b) {
    int lo = 255, hi = 0;  // alpha: BC4, 8-value mode
    for (int i = 0; i < 16; i++) { if (px[i][3] < lo) lo = px[i][3]; if (px[i][3] > hi) hi = px[i][3]; }
    uint64_t bits = 0;
    b[0] = (uint8_t)hi, b[1] = (uint8_t)lo;
    if (hi != lo) {
        int pal[8] = {hi, lo};
        for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * hi + i * lo + 3) / 7;
        for (int i = 0; i < 16; i++) {
            int best = 0, be = 1 << 30;
            for (int q = 0; q < 8; q++) { int e = abs(pal[q] - px[i][3]); if (e < be) be = e, best = q; }
            bits |= (uint64_t)best << (3 * i);
        }
    }
    for (int i = 0; i < 6; i++) b[2 + i] = (uint8_t)(bits >> (8 * i));
    int bi = 0, bj = 0, bd = -1;  // colour: the block's two most distant (visible) colours as endpoints
    for (int i = 0; i < 16; i++)
        for (int j = i + 1; j < 16; j++) {
            int d = 0;
            for (int k = 0; k < 3; k++) d += (px[i][k] - px[j][k]) * (px[i][k] - px[j][k]);
            d = d * (px[i][3] > 8 && px[j][3] > 8 ? 4 : 1);  // (fully transparent texels matter less)
            if (d > bd) bd = d, bi = i, bj = j;
        }
    uint16_t c0 = to565(px[bi]), c1 = to565(px[bj]);
    if (c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
    int c[4][3];
    rgb565(c0, c[0]);
    rgb565(c1, c[1]);
    for (int k = 0; k < 3; k++) c[2][k] = (2 * c[0][k] + c[1][k]) / 3, c[3][k] = (c[0][k] + 2 * c[1][k]) / 3;
    uint32_t idx = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, be = 1 << 30;
        for (int q = 0; q < 4; q++) {
            int e = 0;
            for (int k = 0; k < 3; k++) e += (c[q][k] - px[i][k]) * (c[q][k] - px[i][k]);
            if (e < be) be = e, best = q;
        }
        idx |= (uint32_t)best << (2 * i);
    }
    b[8] = (uint8_t)c0, b[9] = (uint8_t)(c0 >> 8), b[10] = (uint8_t)c1, b[11] = (uint8_t)(c1 >> 8);
    for (int i = 0; i < 4; i++) b[12 + i] = (uint8_t)(idx >> (8 * i));
}

// ------------------------------------------------------------------ stroke font (cap height 100 units, y down)
typedef struct { float ax, ay, bx, by; } Seg;
typedef struct { Seg s[200]; int n; } Path;

static void ln(Path *p, float ax, float ay, float bx, float by) {
    if (p->n < (int)(sizeof p->s / sizeof *p->s)) p->s[p->n++] = (Seg){ax, ay, bx, by};
}
static void arc(Path *p, float cx, float cy, float r, float t0, float t1, int steps) {  // (degrees; 90 = down)
    float px = cx + r * cosf(t0 * (float)M_PI / 180), py = cy + r * sinf(t0 * (float)M_PI / 180);
    for (int i = 1; i <= steps; i++) {
        float t = (t0 + (t1 - t0) * i / steps) * (float)M_PI / 180, x = cx + r * cosf(t), y = cy + r * sinf(t);
        ln(p, px, py, x, y);
        px = x, py = y;
    }
}

static float glyph(Path *p, char ch, float x) {  // adds character `ch` at x; returns its width
    Path g = {.n = 0};
    float w = 0;
    switch (ch) {
    case 'A': w = 80; ln(&g, 0, 100, 40, 0); ln(&g, 40, 0, 80, 100); ln(&g, 17, 70, 63, 70); break;
    case 'B':
        w = 56;
        ln(&g, 26, 50, 0, 50); ln(&g, 0, 50, 0, 0); ln(&g, 0, 0, 26, 0); arc(&g, 26, 25, 25, -90, 90, 10);
        ln(&g, 0, 50, 0, 100); ln(&g, 0, 100, 30, 100); arc(&g, 30, 75, 25, 90, -90, 10);
        break;
    case 'X': w = 64; ln(&g, 0, 0, 64, 100); ln(&g, 64, 0, 0, 100); break;
    case 'Y': w = 68; ln(&g, 0, 0, 34, 52); ln(&g, 34, 52, 68, 0); ln(&g, 34, 52, 34, 100); break;
    case 'L': w = 46; ln(&g, 0, 0, 0, 100); ln(&g, 0, 100, 46, 100); break;
    case 'R':
        w = 56;
        ln(&g, 0, 100, 0, 0); ln(&g, 0, 0, 28, 0); arc(&g, 28, 26, 26, -90, 90, 10); ln(&g, 28, 52, 0, 52);
        ln(&g, 24, 52, 56, 100);
        break;
    case 'Z': w = 56; ln(&g, 0, 0, 56, 0); ln(&g, 56, 0, 0, 100); ln(&g, 0, 100, 56, 100); break;
    case 'T': w = 60; ln(&g, 0, 0, 60, 0); ln(&g, 30, 0, 30, 100); break;
    case 'S': w = 54; arc(&g, 27, 25, 25, -20, -270, 14); arc(&g, 27, 75, 25, -90, 160, 14); break;
    case '1': w = 30; ln(&g, 0, 20, 26, 0); ln(&g, 26, 0, 26, 100); break;
    case '2': {
        w = 56;
        arc(&g, 27, 27, 27, -170, 20, 12);
        float ex = 27 + 27 * cosf(20 * (float)M_PI / 180), ey = 27 + 27 * sinf(20 * (float)M_PI / 180);
        ln(&g, ex, ey, 0, 100); ln(&g, 0, 100, 56, 100);
        break;
    }
    case '3': w = 54; arc(&g, 27, 25, 25, -160, 90, 12); arc(&g, 27, 75, 25, -90, 160, 12); break;
    default: break;
    }
    for (int i = 0; i < g.n; i++) ln(p, g.s[i].ax + x, g.s[i].ay, g.s[i].bx + x, g.s[i].by);
    return w;
}

enum { SY_CIRCLE, SY_CROSS, SY_TRIANGLE, SY_SQUARE, SY_MENU, SY_VIEW, SY_CREATE, SY_START, SY_BACK, SY_PLUS, SY_MINUS,
       SY_UP, SY_DOWN, SY_LEFT, SY_RIGHT, SY_DOT };

static void symbol(Path *p, int sy, float *gh) {  // in a 100 x 100 box (the minus: 100 x 24)
    *gh = 100;
    switch (sy) {
    case SY_CIRCLE: arc(p, 50, 50, 42, 0, 360, 32); break;
    case SY_CROSS: ln(p, 10, 10, 90, 90); ln(p, 90, 10, 10, 90); break;
    case SY_TRIANGLE: ln(p, 50, 6, 96, 88); ln(p, 96, 88, 4, 88); ln(p, 4, 88, 50, 6); break;
    case SY_SQUARE: ln(p, 8, 8, 92, 8); ln(p, 92, 8, 92, 92); ln(p, 92, 92, 8, 92); ln(p, 8, 92, 8, 8); break;
    case SY_MENU: ln(p, 8, 14, 92, 14); ln(p, 8, 50, 92, 50); ln(p, 8, 86, 92, 86); break;
    case SY_VIEW:  // two overlapping windows
        ln(p, 34, 8, 92, 8); ln(p, 92, 8, 92, 60); ln(p, 92, 60, 70, 60); ln(p, 34, 8, 34, 40);
        ln(p, 8, 40, 66, 40); ln(p, 66, 40, 66, 92); ln(p, 66, 92, 8, 92); ln(p, 8, 92, 8, 40);
        break;
    case SY_CREATE: ln(p, 50, 50, 50, 6); ln(p, 38, 58, 8, 30); ln(p, 62, 58, 92, 30); ln(p, 50, 88, 50, 88); break;
    case SY_START: ln(p, 20, 6, 88, 50); ln(p, 88, 50, 20, 94); ln(p, 20, 94, 20, 6); break;
    case SY_BACK: ln(p, 80, 6, 12, 50); ln(p, 12, 50, 80, 94); ln(p, 80, 94, 80, 6); break;
    case SY_PLUS: ln(p, 50, 4, 50, 96); ln(p, 4, 50, 96, 50); break;
    case SY_MINUS: ln(p, 4, 12, 96, 12); *gh = 24; break;
    case SY_DOT: ln(p, 50, 50, 50, 50); break;
    default: {  // arrows: drawn pointing up, then turned
        static const float up[3][4] = {{50, 96, 50, 8}, {50, 6, 10, 46}, {50, 6, 90, 46}};
        for (int i = 0; i < 3; i++) {
            float q[4];
            for (int k = 0; k < 4; k += 2) {
                float x = up[i][k], y = up[i][k + 1];
                q[k] = sy == SY_UP ? x : sy == SY_DOWN ? 100 - x : sy == SY_LEFT ? y : 100 - y;
                q[k + 1] = sy == SY_UP ? y : sy == SY_DOWN ? 100 - y : sy == SY_LEFT ? 100 - x : x;
            }
            ln(p, q[0], q[1], q[2], q[3]);
        }
    }
    }
}

// The label `lab` (MP_*) in `style`: its strokes (units), size, stroke radius (units), whether it is text, and the
// controller's own colour for it (0xRRGGBB + 1; 0: the game's label colour).
typedef struct { Path p; float w, h, rad; bool text; int chars; uint32_t colour; } Label;

static void make_label(int lab, int style, Label *L) {
    memset(L, 0, sizeof *L);
    const char *txt = NULL;
    int sy = -1;
    bool xlike = style == MP_STYLE_XBOX || style == MP_STYLE_DECK || style == MP_STYLE_STEAMC;
    if (lab >= MP_A && lab <= MP_Y) {  // face buttons: east, south, north, west
        int i = lab - MP_A;
        if (style == MP_STYLE_PS) {
            static const int ps[4] = {SY_CIRCLE, SY_CROSS, SY_TRIANGLE, SY_SQUARE};
            static const uint32_t pc[4] = {0xff576e, 0x8ca6ff, 0x3fd0b0, 0xf28fe3};
            sy = ps[i], L->colour = pc[i] + 1;
        } else if (xlike) {
            static const char *const xb[4] = {"B", "A", "Y", "X"};
            static const uint32_t xc[4] = {0xf03a30, 0x5cd13e, 0xffc810, 0x2f8cf5};
            txt = xb[i];
            if (style == MP_STYLE_XBOX) L->colour = xc[i] + 1;
        } else {
            static const char *const nin[4] = {"A", "B", "X", "Y"};
            txt = nin[i];
        }
    } else if (lab >= MP_L && lab <= MP_ZR) {
        static const char *const nin[4] = {"L", "R", "ZL", "ZR"}, *const xb[4] = {"LB", "RB", "LT", "RT"},
                                 *const ps[4] = {"L1", "R1", "L2", "R2"};
        int i = lab - MP_L;
        txt = style == MP_STYLE_NINTENDO ? nin[i] : style == MP_STYLE_XBOX || style == MP_STYLE_STEAMC ? xb[i] : ps[i];
    } else if (lab == MP_PLUS) {
        sy = style == MP_STYLE_NINTENDO ? SY_PLUS : style == MP_STYLE_STEAMC ? SY_START : SY_MENU;
    } else if (lab == MP_MINUS) {
        sy = style == MP_STYLE_NINTENDO ? SY_MINUS : style == MP_STYLE_STEAMC ? SY_BACK : style == MP_STYLE_PS ? SY_CREATE : SY_VIEW;
    } else if (lab == MP_LS || lab == MP_RS) {
        bool three = style == MP_STYLE_PS || style == MP_STYLE_DECK;
        txt = lab == MP_LS ? (three ? "L3" : "LS") : (three ? "R3" : "RS");
    } else if (lab >= MP_DUP && lab <= MP_DRIGHT) {
        sy = SY_UP + (lab - MP_DUP);
    } else {
        sy = SY_DOT;
    }
    if (txt) {
        float x = 0;
        for (const char *c = txt; *c; c++) { x += glyph(&L->p, *c, x); if (c[1]) x += 26; }
        L->w = x, L->h = 100, L->text = true, L->chars = (int)strlen(txt);
        L->rad = L->chars == 1 ? 12 : 10;
    } else {
        symbol(&L->p, sy, &L->h);
        L->w = 100;
        L->rad = sy == SY_DOT ? 34 : 10;
    }
}

// Coverage (0..1) of the strokes `s` (pixels) with radius r: 4x4 samples per pixel
static void cover(float *cv, int W, int H, const Seg *s, int n, float r) {
    memset(cv, 0, sizeof(float) * (size_t)W * H);
    if (!n) return;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < n; i++) {
        x0 = fminf(x0, fminf(s[i].ax, s[i].bx)), x1 = fmaxf(x1, fmaxf(s[i].ax, s[i].bx));
        y0 = fminf(y0, fminf(s[i].ay, s[i].by)), y1 = fmaxf(y1, fmaxf(s[i].ay, s[i].by));
    }
    int X0 = (int)fmaxf(0, floorf(x0 - r - 1)), X1 = (int)fminf((float)W - 1, ceilf(x1 + r + 1));
    int Y0 = (int)fmaxf(0, floorf(y0 - r - 1)), Y1 = (int)fminf((float)H - 1, ceilf(y1 + r + 1));
    float r2 = r * r;
    for (int y = Y0; y <= Y1; y++)
        for (int x = X0; x <= X1; x++) {
            int hit = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float px = x + (sx + 0.5f) / 4, py = y + (sy + 0.5f) / 4;
                    for (int i = 0; i < n; i++) {
                        float vx = s[i].bx - s[i].ax, vy = s[i].by - s[i].ay, wx = px - s[i].ax, wy = py - s[i].ay;
                        float l2 = vx * vx + vy * vy, t = l2 > 1e-9f ? (wx * vx + wy * vy) / l2 : 0;
                        t = t < 0 ? 0 : t > 1 ? 1 : t;
                        float dx = wx - t * vx, dy = wy - t * vy;
                        if (dx * dx + dy * dy <= r2) { hit++; break; }
                    }
                }
            cv[y * W + x] = hit / 16.0f;
        }
}

// The label placed: units -> pixels (scale sc, top-left at ox, oy)
static int place(const Label *L, float sc, float ox, float oy, Seg *out) {
    for (int i = 0; i < L->p.n; i++)
        out[i] = (Seg){ox + L->p.s[i].ax * sc, oy + L->p.s[i].ay * sc, ox + L->p.s[i].bx * sc, oy + L->p.s[i].by * sc};
    return L->p.n;
}

static int quant(const uint8_t *p) { return (p[0] >> 4) << 12 | (p[1] >> 4) << 8 | (p[2] >> 4) << 4 | p[3] >> 4; }

// The most common colour (by 4-bit buckets, averaged inside the winner) of the pixels with mask[i]; false if none
static bool mode_colour(const uint8_t *img, const uint8_t *mask, int N, float out[4]) {
    uint16_t *hist = calloc(1 << 16, sizeof *hist);
    if (!hist) return false;
    int best = -1, bc = 0;
    for (int i = 0; i < N; i++)
        if (mask[i]) { int q = quant(img + i * 4); if (++hist[q] > bc) bc = hist[q], best = q; }
    free(hist);
    if (best < 0) return false;
    float acc[4] = {0};
    int n = 0;
    for (int i = 0; i < N; i++)
        if (mask[i] && quant(img + i * 4) == best) { for (int k = 0; k < 4; k++) acc[k] += img[i * 4 + k]; n++; }
    for (int k = 0; k < 4; k++) out[k] = acc[k] / n;
    return true;
}

static bool saturated(const uint8_t *p) {
    int mx = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
    int mn = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
    return p[3] > 200 && mx - mn > 80 && mx > 120;
}

static void relabel(uint8_t *img, int W, int H, int lab, int style, bool keep_colour) {
    int N = W * H;
    float s = W / 64.0f;
    uint8_t *Lp = calloc((size_t)N, 1), *outer = calloc((size_t)N, 1), *m = calloc((size_t)N, 1);
    int *stack = malloc(sizeof(int) * (size_t)N * 4);
    float *O = malloc(sizeof(float) * (size_t)N), *C = malloc(sizeof(float) * (size_t)N);
    Seg *segs = malloc(sizeof(Seg) * 200);
    Label *L = malloc(sizeof *L);
    if (!Lp || !outer || !m || !stack || !O || !C || !segs || !L) goto done;
    int lx0 = W, ly0 = H, lx1 = -1, ly1 = -1, nl = 0;
    float lab_col[3] = {0};
    for (int i = 0; i < N; i++)
        if (saturated(img + i * 4)) {
            Lp[i] = 1, nl++;
            for (int k = 0; k < 3; k++) lab_col[k] += img[i * 4 + k];
            int x = i % W, y = i / W;
            if (x < lx0) lx0 = x;
            if (x > lx1) lx1 = x;
            if (y < ly0) ly0 = y;
            if (y > ly1) ly1 = y;
        }
    if (nl < 8) goto done;
    for (int k = 0; k < 3; k++) lab_col[k] /= nl;
    // the rim: transparent pixels and the dark ones connected to them
    int sp = 0;
    for (int i = 0; i < N; i++) if (img[i * 4 + 3] <= 200) outer[i] = 1, stack[sp++] = i;
    while (sp) {
        int i = stack[--sp], x = i % W, y = i / W;
        const int nb[4] = {x > 0 ? i - 1 : -1, x < W - 1 ? i + 1 : -1, y > 0 ? i - W : -1, y < H - 1 ? i + W : -1};
        for (int k = 0; k < 4; k++) {
            int j = nb[k];
            if (j < 0 || outer[j]) continue;
            const uint8_t *p = img + j * 4;
            if (p[3] <= 200 || p[0] + p[1] + p[2] < 150) outer[j] = 1, stack[sp++] = j;
        }
    }
    int ix0 = W, iy0 = H, ix1 = -1, iy1 = -1;
    for (int i = 0; i < N; i++) {
        const uint8_t *p = img + i * 4;
        m[i] = !outer[i] && !Lp[i] && p[3] > 200 && p[0] + p[1] + p[2] >= 150;
        if (!outer[i]) {
            int x = i % W, y = i / W;
            if (x < ix0) ix0 = x;
            if (x > ix1) ix1 = x;
            if (y < iy0) iy0 = y;
            if (y > iy1) iy1 = y;
        }
    }
    float body[4];
    if (!mode_colour(img, m, N, body)) goto done;
    uint8_t bodyc[4];
    for (int k = 0; k < 4; k++) bodyc[k] = (uint8_t)lroundf(body[k]);
    // paint the old label and its outline over with the button's grey
    float er = 3.5f * s;
    int R = (int)ceilf(er);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            if (outer[i]) continue;
            bool near = false;
            for (int dy = -R; dy <= R && !near; dy++) {
                int yy = y + dy;
                if (yy < 0 || yy >= H) continue;
                for (int dx = -R; dx <= R && !near; dx++) {
                    int xx = x + dx;
                    near = xx >= 0 && xx < W && dx * dx + dy * dy <= er * er && Lp[yy * W + xx];
                }
            }
            if (near) memcpy(img + i * 4, bodyc, 4);
        }
    // the new label
    make_label(lab, style, L);
    float col[3];
    if (L->colour && !keep_colour) for (int k = 0; k < 3; k++) col[k] = (float)((L->colour - 1) >> (16 - 8 * k) & 255);
    else memcpy(col, lab_col, sizeof col);
    float ow = 2.0f * s, lw = (float)(lx1 - lx0 + 1), lh = (float)(ly1 - ly0 + 1);
    float iw = (float)(ix1 - ix0 + 1), ih = (float)(iy1 - iy0 + 1), sc, cy;
    float cx = (ix0 + ix1 + 1) / 2.0f;
    if (L->text) {
        float avail = iw - 2 * (ow + 2 * s);
        sc = lh / (100 + 2 * L->rad);
        if ((L->w + 2 * L->rad) * sc > avail) sc = avail / (L->w + 2 * L->rad);
        cy = (ly0 + ly1 + 1) / 2.0f;
    } else {
        float box = L->h < 50 ? lw : fmaxf(lw, lh);
        box = fminf(box, fminf(iw, ih) * 0.62f);
        sc = box / (100 + 2 * L->rad);
        cy = (iy0 + iy1 + 1) / 2.0f;
    }
    int n = place(L, sc, cx - L->w * sc / 2, cy - L->h * sc / 2, segs);
    float r = L->rad * sc;
    cover(O, W, H, segs, n, r + ow);
    cover(C, W, H, segs, n, r);
    for (int i = 0; i < N; i++) {
        uint8_t *p = img + i * 4;
        if (outer[i] && p[3] <= 200) continue;
        if (O[i] > 0) for (int k = 0; k < 3; k++) p[k] = (uint8_t)lroundf(p[k] * (1 - O[i]));
        if (C[i] > 0) for (int k = 0; k < 3; k++) p[k] = (uint8_t)lroundf(p[k] + (col[k] - p[k]) * C[i]);
    }
done:
    free(Lp), free(outer), free(m), free(stack), free(O), free(C), free(segs), free(L);
}

// btn_dir_all_ABXY_icon: four yellow discs with black letters (positions in 64ths: top = X, left = Y, right = A,
// bottom = B)
static void relabel_abxy(uint8_t *img, int W, int H, int style, const int labels[MP_NROLES]) {
    static const struct { float x, y; int role; } discs[4] = {{29, 11, MP_X}, {12, 29, MP_Y}, {44, 29, MP_A}, {28, 45, MP_B}};
    int N = W * H;
    float s = W / 64.0f;
    uint8_t *m = calloc((size_t)N, 1);
    float *C = malloc(sizeof(float) * (size_t)N);
    Seg *segs = malloc(sizeof(Seg) * 200);
    Label *L = malloc(sizeof *L);
    if (!m || !C || !segs || !L) goto done;
    for (int d = 0; d < 4; d++) {
        if (style == MP_STYLE_NINTENDO && labels[discs[d].role] == discs[d].role) continue;
        float cx = discs[d].x * s, cy = discs[d].y * s, rin = 8.5f * s;
        memset(m, 0, (size_t)N);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                m[y * W + x] = dx * dx + dy * dy <= rin * rin && saturated(img + (y * W + x) * 4);
            }
        float yel[4];
        if (!mode_colour(img, m, N, yel)) continue;
        uint8_t yc[4];
        for (int k = 0; k < 4; k++) yc[k] = (uint8_t)lroundf(yel[k]);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                if (dx * dx + dy * dy <= rin * rin) memcpy(img + (y * W + x) * 4, yc, 4);
            }
        make_label(labels[discs[d].role], style, L);
        float rad = L->rad, box = 10.5f * s;
        float sc = box / (fmaxf(L->w, L->h) + 2 * rad);
        int n = place(L, sc, cx - L->w * sc / 2, cy - L->h * sc / 2, segs);
        cover(C, W, H, segs, n, rad * sc);
        for (int i = 0; i < N; i++)
            if (C[i] > 0) for (int k = 0; k < 3; k++) img[i * 4 + k] = (uint8_t)lroundf(img[i * 4 + k] * (1 - C[i]));
    }
done:
    free(m), free(C), free(segs), free(L);
}

void mua3p_apply(int id, uint32_t lv, uint8_t *blocks, uint32_t w, uint32_t h, uint32_t row, int style,
                 const int labels[MP_NROLES]) {
    if (id < 0 || id >= g_n || lv) return;
    const Ref *r = &g_refs[id];
    uint32_t wb = (w + 3) / 4, hb = (h + 3) / 4;
    if (w != r->w || h != r->h || row < wb * 16) return;
    bool change = style != MP_STYLE_NINTENDO;
    if (!change && r->kind == K_ABXY)
        for (int i = MP_A; i <= MP_Y; i++) change |= labels[i] != i;
    else if (!change)
        change = labels[r->role] != r->role;
    if (!change) return;
    const uint8_t *src = blocks;
    uint32_t srow = row;
    bool all = false;
    if (r->kind == K_JOY) {  // the round button as the base
        const Ref *b = ref_named(r->role == MP_PLUS ? "btn_Plus_icon" : "btn_Minus_icon");
        if (b && b->w == w && b->h == h) src = b->lin, srow = wb * 16, all = true;
    }
    int W = (int)wb * 4, H = (int)hb * 4;
    uint8_t *img = malloc((size_t)W * H * 4), *was = malloc((size_t)W * H * 4);
    if (!img || !was) { free(img), free(was); return; }
    for (uint32_t by = 0; by < hb; by++)
        for (uint32_t bx = 0; bx < wb; bx++) {
            uint8_t px[16][4];
            bc3_decode(src + by * srow + bx * 16, px);
            for (int i = 0; i < 16; i++) memcpy(img + ((by * 4 + (i >> 2)) * W + bx * 4 + (i & 3)) * 4, px[i], 4);
        }
    memcpy(was, img, (size_t)W * H * 4);
    if (r->kind == K_ABXY) relabel_abxy(img, W, H, style, labels);
    else relabel(img, W, H, labels[r->role], style, r->hold);
    int nchanged = 0;
    for (uint32_t by = 0; by < hb; by++)
        for (uint32_t bx = 0; bx < wb; bx++) {
            uint8_t px[16][4];
            bool dirty = all;
            for (int i = 0; i < 16; i++) {
                size_t at = ((by * 4 + (i >> 2)) * W + bx * 4 + (i & 3)) * 4;
                memcpy(px[i], img + at, 4);
                dirty |= memcmp(img + at, was + at, 4) != 0;
            }
            if (dirty) bc3_encode(px, blocks + by * row + bx * 16), nchanged++;
        }
    static int nlog;
    if (nlog++ < 64) LOG("[prompts] %s restyled (style %d, %d blocks)\n", r->name, style, nchanged);
    free(img), free(was);
}
