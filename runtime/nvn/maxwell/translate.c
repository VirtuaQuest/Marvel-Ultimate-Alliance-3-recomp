// Maxwell (SM 5.x) shader binary -> Metal Shading Language, or Vulkan GLSL (key->glsl; contract in shader.h).
//
// The binary is an NVN header (0x30), the shader program header (SPH, 0x50; graphics stages), then
// instructions in groups of four 64-bit words (one scheduling word + three instructions).
// Registers are emitted as `uint` variables; float ops use as_type<> conversions. Shaders with
// branches become a `switch (pc)` state machine with explicit SSY/PBK/PCNT stacks.
//
// GLSL: the instruction body is generated in the MSL spelling and converted by glsl_rewrite() (a token-aware
// pass: as_type<T>() -> as_u/as_f overloads, float4 -> vec4, ulong -> uint64_t, char()/short() casts -> sx8/sx16,
// in.x / out.x stage I/O -> GLSL globals and built-ins), helpers with MSL names (saturate, fmin, clz, mulhi, ...)
// are defined in the GLSL prelude, and the structurally different constructs (texture lookups, global memory,
// half-float packing, subgroup shuffles, barriers, discard, declarations and entry point) are emitted per dialect.
// The MSL text is byte-identical to the MSL-only translator (tools/test_translate.c checks it).
#if defined(_WIN32)
#include <malloc.h>  // alloca
#endif
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shader.h"

uint64_t maxwell_gmem_base, maxwell_gmem_size;
uint32_t maxwell_gmem_chunk_shift;  // 0: one buffer for the whole heap (GLSL; shader.h)
bool maxwell_gmem_lazy;
bool maxwell_gmem_fs_miss;
bool maxwell_glsl_minpc;
const uint8_t *maxwell_dbg_code;
uint32_t maxwell_dbg_pc;
int maxwell_dbg_reg;
const uint8_t *maxwell_dbg_vs;

#define RZ 255
#define LMEM_WORDS 256  // local memory per invocation (1 KB)
#define SMEM_WORDS 12288  // shared memory per workgroup (48 KB: the most a Maxwell compute shader has)
#define PT 7

typedef struct {
    char *buf;
    size_t len, cap;
} Str;

static void sp(Str *s, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (s->len + n + 1 > s->cap) {
        s->cap = (s->len + n + 1) * 2;
        s->buf = realloc(s->buf, s->cap);
    }
    vsnprintf(s->buf + s->len, n + 1, fmt, ap2);
    va_end(ap2);
    s->len += n;
}

// The per-draw tint's recolour mode (ltint[0].w < 0; runtime/nvn/recolour.c, Cosmetics tab): an HSV change of the
// output colour, in its gamma encoding: for colours whose hue is within ltint[1].y of ltint[1].x (>= 0.5: any) and
// saturation >= ltint[1].z, the hue goes to ltint[1].x + ltint[0].x + (its distance from ltint[1].x) * ltint[1].w
// (turns: .w = 1 a plain shift, < 1 also pulls the hues together), saturation * .y, value * .z (kept <= 1 unless it
// was above). The shading stays: only the colour moves. ltint[2] / [3] (ltint[2].w < 0): a second such change with
// its own hue window, made from the same colour (a draw blending two recoloured maps, e.g. grass and sand: each keeps
// its own; where both windows take a colour, the two results are averaged by weight). `c` is the colour target's
// expression ("c0" / "o.c0"), v3 / v4 the vector types ("vec3" / "float3").
static void emit_recolour_window(Str *s, const char *v3, const char *v4, int k, const char *r, const char *m) {
    // (rc_hh0 / rc_ss0 / rc_vv0: the colour's HSV) -> r: the changed colour (linear), m: its weight
    sp(s, "    float rc_sd%d = fract(rc_hh0 - ltint[%d].x + 0.5f) - 0.5f, rc_dh%d = abs(rc_sd%d);\n", k, k + 1, k, k);
    sp(s, "    float %s = (ltint[%d].y >= 0.5f ? 1.0f : 1.0f - smoothstep(ltint[%d].y * 0.6f, ltint[%d].y, rc_dh%d)) *\n"
          "              smoothstep(ltint[%d].z * 0.5f, ltint[%d].z + 1e-4f, rc_ss0);\n", m, k + 1, k + 1, k + 1, k, k + 1, k + 1);
    sp(s, "    float rc_hh%d = fract(ltint[%d].x + ltint[%d].x + rc_sd%d * ltint[%d].w), rc_ss%d = clamp(rc_ss0 * ltint[%d].y, 0.0f, 1.0f);\n",
       k + 1, k + 1, k, k, k + 1, k + 1, k);
    sp(s, "    float rc_vv%d = min(rc_vv0 * ltint[%d].z, max(rc_vv0, 1.0f));\n", k + 1, k);
    sp(s, "    %s rc_k%d = clamp(abs(fract(%s(rc_hh%d) + %s(1.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - %s(3.0f)) - %s(1.0f), "
          "%s(0.0f), %s(1.0f));\n", v3, k, v3, k + 1, v3, v3, v3, v3, v3);
    sp(s, "    %s %s = pow(rc_vv%d * mix(%s(1.0f), rc_k%d, %s(rc_ss%d)), %s(2.2f));\n", v3, r, k + 1, v3, k, v3, k + 1, v3);
    (void)v4;
}

static void emit_recolour(Str *s, const char *c, const char *v3, const char *v4) {
    sp(s, "  else if (ltint[0].w < 0.0f) {\n");
    sp(s, "    %s rc_g = pow(max(%s.rgb, %s(0.0f)), %s(1.0f / 2.2f));\n", v3, c, v3, v3);
    sp(s, "    %s rc_K = %s(0.0f, -1.0f / 3.0f, 2.0f / 3.0f, -1.0f);\n", v4, v4);
    sp(s, "    %s rc_p = mix(%s(rc_g.b, rc_g.g, rc_K.w, rc_K.z), %s(rc_g.g, rc_g.b, rc_K.x, rc_K.y), %s(step(rc_g.b, rc_g.g)));\n", v4, v4, v4, v4);
    sp(s, "    %s rc_q = mix(%s(rc_p.x, rc_p.y, rc_p.w, rc_g.r), %s(rc_g.r, rc_p.y, rc_p.z, rc_p.x), %s(step(rc_p.x, rc_g.r)));\n", v4, v4, v4, v4);
    sp(s, "    float rc_d = rc_q.x - min(rc_q.w, rc_q.y);\n");
    sp(s, "    float rc_hh0 = abs(rc_q.z + (rc_q.w - rc_q.y) / (6.0f * rc_d + 1e-6f)), rc_ss0 = rc_d / (rc_q.x + 1e-6f), rc_vv0 = rc_q.x;\n");
    emit_recolour_window(s, v3, v4, 0, "rc_r", "rc_m");
    sp(s, "    if (ltint[2].w < 0.0f) {\n");
    emit_recolour_window(s, v3, v4, 2, "rc_r2", "rc_m2");
    sp(s, "      float rc_w = rc_m + rc_m2;\n");
    sp(s, "      if (rc_w > 0.0f) { rc_r = (rc_r * rc_m + rc_r2 * rc_m2) / rc_w; rc_m = min(rc_w, 1.0f); }\n    }\n");
    sp(s, "    %s.rgb = mix(%s.rgb, rc_r, %s(rc_m));\n  }\n", c, c, v3);
}

typedef struct Construct Construct;  // (structure_flow)
typedef struct VCtx VCtx;            // (expand_calls)
typedef struct {
    const uint8_t *code;   // first instruction group
    size_t ninsts;         // number of 8-byte words to scan
    int stage;
    const ShaderKey *key;
    ShaderInfo *info;
    Str body;
    uint8_t reg_used[256];
    uint8_t pred_used[8];
    uint32_t in_attr_mask, out_attr_mask;
    uint32_t vs_in_comp[32];
    bool uses_branches, uses_cc, uses_local, uses_shared, uses_vid, uses_iid, uses_frag_coord, uses_front_facing, uses_lane;
    bool uses_rscale;  // resolution-scale table (fragment/compute only)
    uint32_t flat_mask;           // fragment inputs using constant (flat) interpolation
    uint32_t noperspective_mask;  // fragment inputs using pass (linear) interpolation
    uint32_t omap_target, omap_depth;
    int tex_slot[8192];           // texture index -> Metal slot + 1
    bool tex_weak[64];            // slot type only guessed (from a size query): the next sample decides
    bool fail;
    bool branchy;  // program uses the pc state machine
    int ipa_mode;  // interpolation mode of the IPA being translated (-1 for plain loads)
    int ipa_mul;   // its multiplier register (mode 1: the result is the "pass" value attr/w times this register)
    // GLSL (fields kept last: the MSL path's layout, including its out-of-range reg_used writes, is unchanged)
    bool glsl;               // emit Vulkan GLSL: dialect-specific constructs here, the rest via glsl_rewrite()
    bool uses_gmem_store;    // STG present: the gmem buffer is not readonly
    uint32_t cbuf_need[18];  // highest constant vec4 index + 1 per cbuf (array size of the GLSL block)
    bool img_read[64], img_write[64];  // image slots loaded from (SULD) / stored to (SUST)
    int8_t *ssy_depth, *pbk_depth;     // (GLSL) static SSY / PBK stack depth before each instruction: static_stacks
    int ssy_max, pbk_max;              // their deepest levels (slots ssy0.. / pbk0..); ssy_depth NULL: dynamic arrays
    uint32_t *pop_tgt;                 // (static_stacks) where each SYNC / BRK goes; NULL: not known
    bool structured;                   // (structure_flow) loops and blocks instead of the pc dispatcher
    Construct *cons;                   // its constructs, in opening order
    int ncons, nstack;
    int *cons_at, *stack;              // construct + 1 by target ([2 l] block, [2 l + 1] loop); the open ones
    const uint8_t *blob;               // the program and its constants (BRX tables)
    size_t blob_size;
    uint64_t *vcode;                   // (expand_calls) the program with its subroutines copied in; code points here
    uint32_t *vorig;                   // its instructions' indices in the original program
    uint16_t *vctx;                    // and their copies (vctxs: 0 the program itself)
    VCtx *vctxs;
    int nvctx;
    const uint8_t *ocode;              // the original program
    size_t oend;
} Ctx;

static uint64_t bits(uint64_t w, int lo, int n) { return (w >> lo) & ((n == 64) ? ~0ull : ((1ull << n) - 1)); }
static int64_t sbits(uint64_t w, int lo, int n) {
    int64_t v = (int64_t)bits(w, lo, n);
    return (v & (1ll << (n - 1))) ? v - (1ll << n) : v;
}

// LARECOMP_SHADER_SURVEY=1: a shader with unknown instructions logs every one to stderr (translation goes on past
// the first; the result is still a failure).
static bool survey_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_SHADER_SURVEY") != NULL;
    return on;
}

static void fail(Ctx *c, const char *fmt, ...) {
    if (c->fail) return;
    c->fail = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->info->error, sizeof c->info->error, fmt, ap);
    va_end(ap);
}

// Branch targets may point at a scheduling word; execution continues at the next instruction.
static uint32_t norm_target(int64_t t) {
    if (t >= 0 && ((t / 8) % 4) == 0) t += 8;
    return (uint32_t)t;
}

// ------------------------------------------------------------------ operands
static const char *reg(Ctx *c, int r, char *buf) {
    if (r == RZ) return "0u";
    c->reg_used[r] = 1;
    sprintf(buf, "r%d", r);
    return buf;
}

#define R(r) reg(c, (r), alloca(16))
#define F(expr) fmt_f(alloca(512), (expr))
static char *fmt_f(char *b, const char *e) { sprintf(b, "as_type<float>(%s)", e); return b; }

static const char *pred(Ctx *c, int p, bool neg, char *buf) {
    if (p == PT) return neg ? "false" : "true";
    c->pred_used[p] = 1;
    sprintf(buf, "%sp%d", neg ? "!" : "", p);
    return buf;
}
#define P(p, n) pred(c, (p), (n), alloca(16))

static const char *cbuf_ref(Ctx *c, int idx, int64_t off, const char *dyn, char *buf) {
    c->info->cbuf_mask |= 1u << idx;
    if (idx == 0) {  // a storage buffer binding: which slot (any, when indexed with a register)
        if (dyn) c->info->ssbo_read = 0xFFFF;
        else if (off >= 0x110 && off < 0x210) c->info->ssbo_read |= (uint16_t)(1u << ((off - 0x110) >> 4));
        else if (off >= 0x510 && off < 0x610) c->info->ssbo_read |= (uint16_t)(1u << ((off - 0x510) >> 4));
    }
    if (off + 16 > (int64_t)c->info->cbuf_size[idx]) c->info->cbuf_size[idx] = (uint32_t)((off + 16 + 255) & ~255);
    if (dyn) c->info->cbuf_dynamic |= 1u << idx;
    if (!dyn && idx < 18 && off >= 0 && (uint32_t)(off >> 4) + 1 > c->cbuf_need[idx]) c->cbuf_need[idx] = (uint32_t)(off >> 4) + 1;
    if (dyn) sprintf(buf, "cbr(cb%d, (%s) + %lldu)", idx, dyn, (long long)off);
    else sprintf(buf, "cb%d[%lld][%lld]", idx, (long long)(off >> 4), (long long)((off >> 2) & 3));
    return buf;
}

// Operand B in its register / constant buffer / immediate forms, selected by the opcode class.
typedef enum { B_REG, B_CBUF, B_IMM } BForm;

static BForm b_form(uint64_t w) {
    uint64_t top = w >> 60;
    if (top == 0x5) return B_REG;
    if (top == 0x4) return B_CBUF;
    return B_IMM;
}

static const char *operand_b(Ctx *c, uint64_t w, bool float_imm, char *buf) {
    switch (b_form(w)) {
    case B_REG: return reg(c, (int)bits(w, 20, 8), buf);
    case B_CBUF: return cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, buf);
    default: {
        uint32_t v = (uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19);
        if (float_imm) v <<= 12;
        else if (v & 0x80000u) v |= 0xFFF00000u;  // sign-extend 20 bits
        sprintf(buf, "0x%08xu", v);
        return buf;
    }
    }
}
#define B(fi) operand_b(c, w, (fi), alloca(128))

// The "C" operand of three-source ops: REG_39, or the cbuf/reg swap form (0x51/0x53/... classes).
static const char *operand_c_for_ffma(Ctx *c, uint64_t w, char *buf, const char **b_out, char *bbuf) {
    uint64_t op8 = w >> 56;
    if ((op8 & 0xF8) == 0x50 && (op8 & 7) != 0) {  // e.g. 0x51: B = REG_39, C = cbuf
        *b_out = reg(c, (int)bits(w, 39, 8), bbuf);
        return cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, buf);
    }
    *b_out = operand_b(c, w, true, bbuf);
    return reg(c, (int)bits(w, 39, 8), buf);
}

static void set_reg(Ctx *c, int r, const char *expr) {
    if (r == RZ) return;
    c->reg_used[r] = 1;
    sp(&c->body, "    r%d = %s;\n", r, expr);
}

static void set_dreg(Ctx *c, int r, const char *expr) {
    if (r == RZ) return;
    c->reg_used[r] = c->reg_used[r + 1] = 1;
    sp(&c->body, "    { uvec2 dv_ = unpackDouble2x32(%s); r%d = dv_.x; r%d = dv_.y; }\n", expr, r, r + 1);
}

static const char *fneg_abs(const char *e, bool neg, bool abs_, char *buf) {
    if (abs_ && neg) sprintf(buf, "(-abs(%s))", e);
    else if (abs_) sprintf(buf, "abs(%s)", e);
    else if (neg) sprintf(buf, "(-%s)", e);
    else return e;
    return buf;
}
#define NA(e, n, a) fneg_abs((e), (n), (a), alloca(600))

// double precision (GLSL): a register pair, low word first
static const char *dreg(Ctx *c, int r, char *buf) {
    if (r == RZ) return "0.0lf";
    c->reg_used[r] = c->reg_used[r + 1] = 1;
    sprintf(buf, "packDouble2x32(uvec2(r%d, r%d))", r, r + 1);
    return buf;
}
#define DR(r) dreg(c, (r), alloca(64))
// a double B operand: a register pair, a cbuf pair (bit 20 set: unaligned, the low word is zero) or the top 20 bits
static const char *double_b(Ctx *c, uint64_t w, char *buf) {
    char lo[256], hi[256];
    switch (b_form(w)) {
    case B_REG: return dreg(c, (int)bits(w, 20, 8), buf);
    case B_CBUF: {
        int ix = (int)bits(w, 34, 5);
        uint32_t off = (uint32_t)bits(w, 20, 14) * 4;
        if (off & 4) sprintf(buf, "packDouble2x32(uvec2(0u, %s))", cbuf_ref(c, ix, off, NULL, hi));
        else sprintf(buf, "packDouble2x32(uvec2(%s, %s))", cbuf_ref(c, ix, off, NULL, lo), cbuf_ref(c, ix, off + 4, NULL, hi));
        return buf;
    }
    default:
        sprintf(buf, "packDouble2x32(uvec2(0u, 0x%08xu))", ((uint32_t)bits(w, 20, 19) | (uint32_t)bits(w, 56, 1) << 19) << 12);
        return buf;
    }
}
#define DB() double_b(c, w, alloca(600))

static const char *fresult(const char *e, bool sat, char *buf) {
    if (sat) sprintf(buf, "as_type<uint>(saturate(%s))", e);
    else sprintf(buf, "as_type<uint>(%s)", e);
    return buf;
}
#define FRES(e, sat) fresult((e), (sat), alloca(700))

static const char *cmp_op(int op) {
    static const char *const ops[] = {"false", "<", "==", "<=", ">", "!=", ">=", "num", "nan",
                                      "<", "==", "<=", ">", "!=", ">=", "true"};
    return ops[op & 15];
}

// float comparison including unordered variants
static void fcmp_expr(char *out, int op, const char *a, const char *b) {
    switch (op) {
    case 0: strcpy(out, "false"); break;
    case 15: strcpy(out, "true"); break;
    case 7: sprintf(out, "(!isnan(%s) && !isnan(%s))", a, b); break;
    case 8: sprintf(out, "(isnan(%s) || isnan(%s))", a, b); break;
    default:
        if (op < 7) sprintf(out, "(%s %s %s)", a, cmp_op(op), b);
        else sprintf(out, "((%s %s %s) || isnan(%s) || isnan(%s))", a, cmp_op(op), b, a, b);
    }
}

static const char *bool_op(int op) { return op == 0 ? "&&" : op == 1 ? "||" : "!="; }

static void icmp_expr(char *out, int op, const char *a, const char *b, bool is_signed) {
    static const char *const ops[] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
    if (op == 0) strcpy(out, "false");
    else if (op == 7) strcpy(out, "true");
    else if (is_signed) sprintf(out, "(int(%s) %s int(%s))", a, ops[op], b);
    else sprintf(out, "(%s %s %s)", a, ops[op], b);
}

// ------------------------------------------------------------------ attributes
// Maxwell attribute space: 0x70 position, 0x80 + 16*i generic attribute i, 0x2F8 instance id,
// 0x2FC vertex id, 0x3FC front facing, 0x6C point size.
static void attr_load(Ctx *c, int dst, uint32_t addr, const char *idx) {
    char e[256];
    if (idx) { fail(c, "indexed attribute load"); return; }
    if (c->stage == STAGE_VERTEX) {
        if (addr >= 0x80 && addr < 0x280) {
            int a = (addr - 0x80) >> 4, comp = (addr >> 2) & 3;
            c->in_attr_mask |= 1u << a;
            c->vs_in_comp[a] |= 1u << comp;
            int kind = c->key ? c->key->attr_kind[a] : 0;
            if (kind == 0) sprintf(e, "as_type<uint>(in.a%d[%d])", a, comp);
            else sprintf(e, "as_type<uint>(in.a%d[%d])", a, comp);
        } else if (addr == 0x2F8) { c->uses_iid = true; strcpy(e, "iid"); }
        else if (addr == 0x2FC) { c->uses_vid = true; strcpy(e, "vid"); }
        else { sprintf(e, "0u /* attr %#x */", addr); }
    } else if (c->stage == STAGE_FRAGMENT) {
        if (addr >= 0x70 && addr < 0x80) {
            c->uses_frag_coord = true;
            int comp = (addr >> 2) & 3;
            if (comp == 0 && c->key && c->key->flip_x) {  // mirror mode's composite: x mirrored (rscale[65].x = width)
                c->uses_rscale = true;
                strcpy(e, "as_type<uint>(rscale[65][0] - (in.pos[0] - rscale[0][2]) / rscale[0][0])");
            } else if (comp < 2) {  // guest pixel units: the render target may be allocated larger
                c->uses_rscale = true;
                sprintf(e, "as_type<uint>((in.pos[%d] - rscale[0][%d]) / rscale[0][%d])", comp, comp + 2, comp);
            } else {
                sprintf(e, "as_type<uint>(in.pos[%d])", comp);
            }
        } else if (addr >= 0x80 && addr < 0x280) {
            int a = (addr - 0x80) >> 4, comp = (addr >> 2) & 3;
            // (MSL: a mask of 0 means unknown. GLSL: the renderer always passes the vertex shader's outputs, and
            // Vulkan requires every fragment input to be a vertex output, so 0 means none.)
            if (c->key && (c->key->vs_out_mask || c->key->glsl) && !(c->key->vs_out_mask & (1u << a))) {
                strcpy(e, comp == 3 ? "0x3f800000u" : "0u");  // not provided by the vertex shader
            } else {
                c->in_attr_mask |= 1u << a;
                // IPA "pass" yields attr/w (the shader multiplies by w itself); Metal gives attr. "multiply" is the
                // pass value times a register - usually w (from the pass of position w: Metal's attr again), but a
                // shader may pass its own (the Mysterious Woods mist: 1/(v4.w * pos.w), a projective screen lookup)
                if (c->ipa_mode == 0) sprintf(e, "as_type<uint>(in.v%d[%d] * in.pos.w)", a, comp);
                else if (c->ipa_mode == 1 && c->ipa_mul != RZ) {
                    char mb[16];
                    sprintf(e, "as_type<uint>(in.v%d[%d] * in.pos.w * as_type<float>(%s))", a, comp, reg(c, c->ipa_mul, mb));
                } else sprintf(e, "as_type<uint>(in.v%d[%d])", a, comp);
            }
        } else if (addr == 0x3FC) { c->uses_front_facing = true; strcpy(e, "(front_facing ? 0xFFFFFFFFu : 0u)"); }
        else if (addr == 0x2E0 || addr == 0x2E4) { sprintf(e, "as_type<uint>(point_coord[%d])", (addr >> 2) & 1); }
        else { sprintf(e, "0u /* attr %#x */", addr); }
    } else {
        sprintf(e, "0u /* attr %#x */", addr);
    }
    set_reg(c, dst, e);
}

static void attr_store(Ctx *c, int src, uint32_t addr) {
    char b[16];
    const char *v = reg(c, src, b);
    if (c->glsl && c->stage != STAGE_VERTEX) {  // no vertex outputs in this stage (MSL: undeclared `out`)
        if (addr >= 0x80 && addr < 0x280) c->out_attr_mask |= 1u << ((addr - 0x80) >> 4);
        if (addr == 0x6C) c->info->writes_point_size = true;
        sp(&c->body, "    /* store attr %#x */\n", addr);
        return;
    }
    if (addr >= 0x70 && addr < 0x80) {
        sp(&c->body, "    out.pos[%d] = as_type<float>(%s);\n", (addr >> 2) & 3, v);
    } else if (addr >= 0x80 && addr < 0x280) {
        int a = (addr - 0x80) >> 4;
        c->out_attr_mask |= 1u << a;
        sp(&c->body, "    out.v%d[%d] = as_type<float>(%s);\n", a, (addr >> 2) & 3, v);
    } else if (addr == 0x6C) {
        c->info->writes_point_size = true;
        sp(&c->body, "    out.psize = as_type<float>(%s);\n", v);
    } else {
        sp(&c->body, "    /* store attr %#x */\n", addr);
    }
}


// ------------------------------------------------------------------ half precision (SM53)
// Swizzle: 0 H1_H0 (both halves), 1 F32 (one f32 used for both lanes), 2 H0_H0, 3 H1_H1.
// Merge: 0 H1_H0 (pack both), 1 F32 (lane 0 as f32), 2 MRG_H0 (replace low half), 3 MRG_H1 (replace high half).
static const char *hsrc(Ctx *c, const char *v, int swz, bool neg, bool abs_, char *buf) {
    (void)c;
    char t[300];
    sprintf(t, "hx(%s, %du)", v, swz);
    if (abs_ && neg) sprintf(buf, "(-abs(%s))", t);
    else if (abs_) sprintf(buf, "abs(%s)", t);
    else if (neg) sprintf(buf, "(-%s)", t);
    else strcpy(buf, t);
    return buf;
}
#define HS(v, sw, n, ab) hsrc(c, (v), (sw), (n), (ab), alloca(400))

static void hstore(Ctx *c, int d, const char *expr, int merge, bool sat) {
    char t[2048], e[1700];
    if (sat) sprintf(e, "saturate(%s)", expr); else strcpy(e, expr);
    if (d == RZ) return;
    c->reg_used[d] = 1;
    sprintf(t, "hpack(%s, %du, r%d)", e, merge, d);
    set_reg(c, d, t);
}

// immediate halves: 9 bits of each half's top (the low 6 mantissa bits are zero) plus separate signs
static uint32_t himm(uint64_t w) {
    uint32_t lo = ((uint32_t)bits(w, 20, 9) << 6) | ((uint32_t)bits(w, 29, 1) << 15);
    uint32_t hi = ((uint32_t)bits(w, 30, 9) << 6) | ((uint32_t)bits(w, 56, 1) << 15);
    return lo | (hi << 16);
}

static bool emit_half(Ctx *c, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    char t[1800], ib[32], cb[256];
    uint16_t op = (uint16_t)(w >> 48);
    // ---- HADD2 / HMUL2 (reg 5d10/5d08, cbuf 7a8x/788x, imm 7a0x/780x, 32I 2c/2a)
    int kind = -1;  // 0 add, 1 mul
    int form = -1;  // 0 reg, 1 cbuf, 2 imm, 3 32i
    if ((op & 0xfff8) == 0x5d10) { kind = 0; form = 0; }
    else if ((op & 0xfff8) == 0x5d08) { kind = 1; form = 0; }
    else if ((op & 0xfe80) == 0x7a80) { kind = 0; form = 1; }
    else if ((op & 0xfe80) == 0x7a00) { kind = 0; form = 2; }
    else if ((op & 0xfe80) == 0x7880) { kind = 1; form = 1; }
    else if ((op & 0xfe80) == 0x7800) { kind = 1; form = 2; }
    else if ((op & 0xfe00) == 0x2c00) { kind = 0; form = 3; }
    else if ((op & 0xfe00) == 0x2a00) { kind = 1; form = 3; }
    if (kind >= 0) {
        int merge, swa, swb = 0;
        bool sat, nega, absa = false, negb = false, absb = false;
        const char *bv;
        if (form == 3) {
            merge = 0; sat = bits(w, 52, 1); swa = (int)bits(w, 53, 2); nega = kind == 0 ? bits(w, 56, 1) : false;
            sprintf(ib, "0x%08xu", (uint32_t)bits(w, 20, 32)); bv = ib;
        } else {
            // field layout per Ryujinx's InstDecoders (InstHadd2*/InstHmul2*)
            merge = (int)bits(w, 49, 2); swa = (int)bits(w, 47, 2); nega = bits(w, 43, 1); absa = bits(w, 44, 1);
            if (form == 0) {
                sat = bits(w, 32, 1); absb = bits(w, 30, 1); swb = (int)bits(w, 28, 2);
                if (kind == 0) negb = bits(w, 31, 1);
                else { nega = bits(w, 31, 1); negb = false; }  // HMUL2 reg: the negate bit is A's at 31
                bv = R((int)bits(w, 20, 8));
            } else if (form == 1) {
                sat = bits(w, 52, 1); negb = kind == 0 ? bits(w, 56, 1) : false; absb = bits(w, 54, 1); swb = 1;
                bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            } else {
                sat = bits(w, 52, 1); swb = 0;
                sprintf(ib, "0x%08xu", himm(w)); bv = ib;
            }
        }
        sprintf(t, kind == 0 ? "(%s + %s)" : "(%s * %s)", HS(R(a), swa, nega, absa), HS(bv, swb, negb, absb));
        hstore(c, d, t, merge, sat);
        return true;
    }
    // ---- HFMA2 (reg 5d00, cr 70xx|0x80, rc 60xx|0x80, imm 70xx, 32I 28)
    form = -1;
    if ((op & 0xfff8) == 0x5d00) form = 0;
    else if ((op & 0xf880) == 0x7080) form = 1;  // b = cbuf, c = reg
    else if ((op & 0xf880) == 0x6080) form = 2;  // b = reg, c = cbuf
    else if ((op & 0xf880) == 0x7000) form = 3;  // b = imm, c = reg
    else if ((op & 0xfe00) == 0x2800) form = 4;  // 32I: c = d
    if (form >= 0) {
        int merge, swa = (int)bits(w, 47, 2), swb = 0, swc = 0;
        bool sat, negb, negc;
        const char *bv, *cv;
        char cb2[256];
        switch (form) {  // field layout per Ryujinx's InstDecoders (InstHfma2*); NegA is applied to B (same product)
        case 0:
            merge = (int)bits(w, 49, 2); sat = bits(w, 32, 1); negb = bits(w, 31, 1); negc = bits(w, 30, 1);
            swb = (int)bits(w, 28, 2); swc = (int)bits(w, 35, 2);
            bv = R((int)bits(w, 20, 8)); cv = R((int)bits(w, 39, 8));
            break;
        case 1:  // b = cbuf, c = reg
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = bits(w, 56, 1); negc = bits(w, 51, 1);
            swb = 1; swc = (int)bits(w, 53, 2);
            bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); cv = R((int)bits(w, 39, 8));
            break;
        case 2:  // b = reg (field at 39), c = cbuf
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = bits(w, 56, 1); negc = bits(w, 51, 1);
            swb = (int)bits(w, 53, 2); swc = 1;
            bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb2);
            break;
        case 3:  // b = imm, c = reg
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = false; negc = bits(w, 51, 1);
            swb = 0; swc = (int)bits(w, 53, 2);
            sprintf(ib, "0x%08xu", himm(w)); bv = ib; cv = R((int)bits(w, 39, 8));
            break;
        default:  // 32I: c = d
            merge = 0; sat = false; negb = false; negc = bits(w, 51, 1);
            swa = (int)bits(w, 47, 2); swb = 0; swc = 0;
            sprintf(ib, "0x%08xu", (uint32_t)bits(w, 20, 32)); bv = ib; cv = R(d);
            break;
        }
        sprintf(t, "fma(%s, %s, %s)", HS(R(a), swa, false, false), HS(bv, swb, negb, false), HS(cv, swc, negc, false));
        hstore(c, d, t, merge, sat);
        return true;
    }
    // ---- HSETP2 / HSET2 (reg forms), HSETP2 cbuf / imm (7e80 / 7e00: B is a constant as one f32 for both halves, or
    // two immediate halves; the comparison moves to 49 and .H_AND to 53)
    bool setp_ci = (op & 0xfe80) == 0x7e80 || (op & 0xfe80) == 0x7e00;
    if ((op & 0xfff8) == 0x5d20 || (op & 0xfff8) == 0x5d18 || setp_ci) {
        bool setp = (op & 0xfff8) == 0x5d20 || setp_ci;
        const char *x = HS(R(a), (int)bits(w, 47, 2), bits(w, 43, 1), bits(w, 44, 1));
        const char *y;
        if (!setp_ci) y = HS(R((int)bits(w, 20, 8)), (int)bits(w, 28, 2), bits(w, 31, 1), bits(w, 30, 1));
        else if (op & 0x80) y = HS(cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb), 1, bits(w, 56, 1), bits(w, 54, 1));
        else { sprintf(ib, "0x%08xu", himm(w)); y = HS(ib, 0, false, false); }
        int cop = (int)bits(w, setp_ci ? 49 : 35, 4), bop = (int)bits(w, 45, 2);
        const char *pp = P((int)bits(w, 39, 3), bits(w, 42, 1));
        char lx[512], ly[512];
        sp(&c->body, "    { float2 hx_ = %s, hy_ = %s; bool pp = %s;\n", x, y, pp);
        fcmp_expr(lx, cop, "hx_.x", "hy_.x");
        fcmp_expr(ly, cop, "hx_.y", "hy_.y");
        sp(&c->body, "      bool l = %s, h = %s;\n", lx, ly);
        if (setp) {
            int pa = (int)bits(w, 3, 3), pb = (int)bits(w, 0, 3);
            if (bits(w, setp_ci ? 53 : 49, 1)) {  // .H_AND: each half is combined with the predicate first, then ANDed
                sp(&c->body, "      l = (l %s pp) && (h %s pp); h = !l;\n", bool_op(bop), bool_op(bop));
                if (pa != PT) { c->pred_used[pa] = 1; sp(&c->body, "      p%d = l;\n", pa); }
                if (pb != PT) { c->pred_used[pb] = 1; sp(&c->body, "      p%d = h;\n", pb); }
            } else {
                if (pa != PT) { c->pred_used[pa] = 1; sp(&c->body, "      p%d = l %s pp;\n", pa, bool_op(bop)); }
                if (pb != PT) { c->pred_used[pb] = 1; sp(&c->body, "      p%d = h %s pp;\n", pb, bool_op(bop)); }
            }
        } else {
            bool bf = bits(w, 49, 1);
            if (d != RZ) {
                c->reg_used[d] = 1;
                sp(&c->body, "      l = l %s pp; h = h %s pp;\n", bool_op(bop), bool_op(bop));
                sp(&c->body, bf ? "      r%d = (l ? 0x3c00u : 0u) | (h ? 0x3c000000u : 0u);\n"
                                : "      r%d = (l ? 0xFFFFu : 0u) | (h ? 0xFFFF0000u : 0u);\n", d);
            }
        }
        sp(&c->body, "    }\n");
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ textures
static int tex_slot(Ctx *c, uint32_t index, int type, bool shadow, bool array) {
    if (index >= 8192) return 0;
    if (c->tex_slot[index]) {
        int s = c->tex_slot[index] - 1;
        if (type >= 0 && c->tex_weak[s]) {
            ShaderTexture *t = &c->info->tex[s];
            t->type = (uint8_t)type;
            t->shadow = shadow;
            t->is_array = array;
            c->tex_weak[s] = false;
        } else if (c->glsl && type >= 0 && (c->info->tex[s].type != type || c->info->tex[s].shadow != shadow)) {
            fail(c, "texture %u used as two types", index);  // one GLSL sampler type per binding
        }
        return s;
    }
    bool weak = type < 0;
    if (weak) type = TEXTYPE_2D;
    int s = c->info->ntex++;
    if (s >= 31) { fail(c, "too many textures"); return 0; }
    c->tex_slot[index] = s + 1;
    c->tex_weak[s] = weak;
    ShaderTexture *t = &c->info->tex[s];
    t->handle_offset = index * 4;
    t->type = (uint8_t)type;
    t->shadow = shadow;
    t->is_array = array;
    return s;
}

static const char *tex_type_name(int t) {
    switch (t) {
    case TEXTYPE_1D: return "1d";
    case TEXTYPE_3D: return "3d";
    case TEXTYPE_CUBE: return "cube";
    case TEXTYPE_2D_ARRAY: return "2d_array";
    case TEXTYPE_CUBE_ARRAY: return "cube_array";
    case TEXTYPE_1D_ARRAY: return "1d_array";
    default: return "2d";
    }
}

// Write texture results: component mask selects which of r,g,b,a go to consecutive registers.
static void write_tex_result(Ctx *c, int slot, const char *call, uint32_t mask, int d0, int d1, bool texs, bool f16) {
    sp(&c->body, "    { float4 t = %s;\n", call);
    (void)slot;
    if (f16) {
        // TEXS/TLDS .F16: components (in mask order) packed as half pairs into d0 (lo, hi) and d1 (lo, hi)
        int comps[4], n = 0;
        for (int comp = 0; comp < 4; comp++) if (mask & (1u << comp)) comps[n++] = comp;
        char v[4][16];
        for (int i = 0; i < 4; i++) { if (i < n) sprintf(v[i], "t[%d]", comps[i]); else strcpy(v[i], "0.0f"); }
        const char *pk = c->glsl ? "      r%d = packHalf2x16(vec2(%s, %s));\n" : "      r%d = as_type<uint>(half2(half(%s), half(%s)));\n";
        if (d0 != RZ) { c->reg_used[d0] = 1; sp(&c->body, pk, d0, v[0], v[1]); }
        if (d1 != RZ) { c->reg_used[d1] = 1; sp(&c->body, pk, d1, v[2], v[3]); }
        sp(&c->body, "    }\n");
        return;
    }
    int regs[4], n = 0;
    if (texs) {
        // TEXS: up to two components in d0/d0+1, two more in d1/d1+1
        int out[4] = {d0, d0 == RZ ? RZ : d0 + 1, d1, d1 == RZ ? RZ : d1 + 1};
        for (int i = 0; i < 4; i++) regs[i] = out[i];
    } else {
        for (int i = 0; i < 4; i++) regs[i] = d0 == RZ ? RZ : d0 + i;
    }
    for (int comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        int r = regs[n++];
        if (r != RZ) { c->reg_used[r] = 1; sp(&c->body, "      r%d = as_type<uint>(t[%d]);\n", r, comp); }
    }
    sp(&c->body, "    }\n");
}

// Integer texel coordinates in guest units -> the (possibly larger) allocation of texture slot `slot`.
static const char *RSC2(Ctx *c, int slot, const char *fmt, ...) {
    static char bufs[4][1024];
    static int bi;
    char *o = bufs[bi++ & 3];
    char inner[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(inner, sizeof inner, fmt, ap);
    va_end(ap);
    if (c->stage == STAGE_VERTEX) { snprintf(o, 1024, "%s", inner); return o; }
    c->uses_rscale = true;
    snprintf(o, 1024, "rsc(%s, rscale[%d])", inner, slot + 1);
    return o;
}

// TEXS / TLDS: texture_info selects target and coordinate layout
static const char *alayer(Ctx *c, const char *r, int slot, char *buf) {
    if (c->glsl) sprintf(buf, "min(%s, uint(tsize%d(0).z) - 1u)", r, slot);
    else sprintf(buf, "min(%s, t%d.get_array_size() - 1u)", r, slot);
    return buf;
}
#define ALAYER(r, slot) alayer(c, (r), (slot), alloca(96))

// 2D normalized coordinates: a scaled texture whose contents occupy its top-left part (Dynamic resolution) is sampled
// there (prelude uvs(); rscale[1 + slot].zw). Vertex shaders have no rscale table: their lookups stay as they are.
static const char *uv2(Ctx *c, int slot, const char *x, const char *y, char *buf) {
    if (c->stage == STAGE_VERTEX) { snprintf(buf, 700, "%s2(%s, %s)", c->glsl ? "vec" : "float", x, y); return buf; }
    c->uses_rscale = true;
    snprintf(buf, 700, "uvs(%s2(%s, %s), rscale[%d])", c->glsl ? "vec" : "float", x, y, slot + 1);
    return buf;
}
#define UV2(slot, x, y) uv2(c, (slot), (x), (y), alloca(700))

// GLSL texture lookup on slot `slot` (its type is final here: a conflicting use fails in tex_slot).
// co: coordinate expressions (1 for 1D, 2 for 2D, 3 for 3D / cube), layer: uint expression or NULL, dref: depth
// reference or NULL, lod_kind 0 implicit / 1 level / 2 bias (lod), off: ivec2 texel offset or NULL.
// GLSL offsets must be constants, so register offsets move the coordinates by offset / size of the level
// (level 0 for implicit-LOD lookups). Non-fragment stages have no implicit LOD: level 0 (+ bias).
static void glsl_sample(Ctx *c, char *out, int slot, const char *const *co, const char *layer, const char *dref,
                        int lod_kind, const char *lod, const char *off) {
    if (c->fail) { strcpy(out, "vec4(0.0f)"); return; }  // e.g. a type conflict: tex[slot] is not this lookup's type
    const ShaderTexture *t = &c->info->tex[slot];
    int type = t->type;
    bool cube = type == TEXTYPE_CUBE || type == TEXTYPE_CUBE_ARRAY;
    int n = (type == TEXTYPE_1D || type == TEXTYPE_1D_ARRAY) ? 1 : (type == TEXTYPE_2D || type == TEXTYPE_2D_ARRAY) ? 2 : 3;
    if (c->stage != STAGE_FRAGMENT && lod_kind != 1 && lod_kind != 3) {  // (3: explicit gradients, any stage)
        if (lod_kind == 0) lod = "0.0f";
        lod_kind = 1;
    }
    if (t->shadow && type == TEXTYPE_3D) { fail(c, "3d shadow texture"); strcpy(out, "vec4(0.0f)"); return; }
    if (layer && type != TEXTYPE_1D_ARRAY && type != TEXTYPE_2D_ARRAY && type != TEXTYPE_CUBE_ARRAY) {
        fail(c, "array lookup on a 3d texture");  // TEX .3D with the array bit
        strcpy(out, "vec4(0.0f)");
        return;
    }
    char L[600], xs[1600], P[2000];
    if (lod_kind == 1) snprintf(L, sizeof L, "int(%s)", lod); else strcpy(L, "0");
    if (n == 1) {
        if (off) snprintf(xs, sizeof xs, "(%s + float((%s).x) / float(tsize%d(%s).x))", co[0], off, slot, L);
        else snprintf(xs, sizeof xs, "%s", co[0]);
    } else if (n == 2) {
        if (off) snprintf(xs, sizeof xs, "(vec2(%s, %s) + vec2(%s) / vec2(tsize%d(%s).xy))", co[0], co[1], off, slot, L);
        else snprintf(xs, sizeof xs, "vec2(%s, %s)", co[0], co[1]);
        if (c->stage != STAGE_VERTEX) { char w[1700]; snprintf(w, sizeof w, "uvs(%s, rscale[%d])", xs, slot + 1); strcpy(xs, w); c->uses_rscale = true; }
    } else {
        if (off && !cube) snprintf(xs, sizeof xs, "vec3(vec2(%s, %s) + vec2(%s) / vec2(tsize%d(%s).xy), %s)", co[0], co[1], off, slot, L, co[2]);
        else snprintf(xs, sizeof xs, "vec3(%s, %s, %s)", co[0], co[1], co[2]);  // cube lookups take no offset
    }
    const char *extra = NULL;  // separate compare argument (cube array shadow)
    if (!t->shadow) {
        if (layer) snprintf(P, sizeof P, "vec%d(%s, float(%s))", n + 1, xs, layer);
        else snprintf(P, sizeof P, "%s", xs);
    } else if (type == TEXTYPE_CUBE_ARRAY) {
        snprintf(P, sizeof P, "vec4(%s, float(%s))", xs, layer ? layer : "0u");
        extra = dref;
    } else if (type == TEXTYPE_1D) {
        snprintf(P, sizeof P, "vec3(%s, 0.0f, %s)", xs, dref);
    } else if (layer) {
        snprintf(P, sizeof P, "vec%d(%s, float(%s), %s)", n + 2, xs, layer, dref);
    } else {
        snprintf(P, sizeof P, "vec%d(%s, %s)", n + 1, xs, dref);
    }
    char e[2000], ex[300] = "";
    if (extra) snprintf(ex, sizeof ex, ", %s", extra);
    if (lod_kind == 0) snprintf(e, sizeof e, "texture(t%d, %s%s)", slot, P, ex);
    else if (lod_kind == 1) snprintf(e, sizeof e, "textureLod(t%d, %s%s, %s)", slot, P, ex, lod);
    else if (lod_kind == 3) snprintf(e, sizeof e, "textureGrad(t%d, %s%s, %s)", slot, P, ex, lod);  // (lod: "dPdx, dPdy")
    else snprintf(e, sizeof e, "texture(t%d, %s%s, %s)", slot, P, ex, lod);
    if (t->shadow) sprintf(out, "vec4(%s)", e);
    else strcpy(out, e);
}

// TEXS / TLDS in GLSL (same slot allocation order and result layout as the MSL switch in emit_texs)
static bool glsl_texs(Ctx *c, uint64_t w, bool fetch, int *slot_out, char *call, const char *A, const char *A1,
                      const char *Bv, const char *B1) {
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int info = (int)bits(w, 53, 4);
    int slot;
    const char *co[3];
    char fa[64], fa1[64], fb[64], fb1[64];
    sprintf(fa, "as_type<float>(%s)", A); sprintf(fa1, "as_type<float>(%s)", A1);
    sprintf(fb, "as_type<float>(%s)", Bv); sprintf(fb1, "as_type<float>(%s)", B1);
    if (!fetch) {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false); co[0] = fa;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_2D, false, false); co[0] = fa; co[1] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false); co[0] = fa; co[1] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, "0.0f", NULL); break;
        case 3: slot = tex_slot(c, index, TEXTYPE_2D, false, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, fb, NULL); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, true, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, fb, 0, NULL, NULL); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, true, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, fb1, 1, fb, NULL); break;
        case 6: slot = tex_slot(c, index, TEXTYPE_2D, true, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, fb, 1, "0.0f", NULL); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true); co[0] = fa1; co[1] = fb;
            glsl_sample(c, call, slot, co, ALAYER(A, slot), NULL, 0, NULL, NULL); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true); co[0] = fa1; co[1] = fb;
            glsl_sample(c, call, slot, co, ALAYER(A, slot), NULL, 1, "0.0f", NULL); break;
        case 9: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, true, true); co[0] = fa1; co[1] = fb;
            glsl_sample(c, call, slot, co, A, fb1, 1, "0.0f", NULL); break;
        case 10: slot = tex_slot(c, index, TEXTYPE_3D, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 11: slot = tex_slot(c, index, TEXTYPE_3D, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, "0.0f", NULL); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 13: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, fb1, NULL); break;
        default: fail(c, "texs info %d", info); return false;
        }
    } else {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "texelFetch(t%d, int(%s), 0)", slot, A); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "texelFetch(t%d, int(%s), int(%s))", slot, A, Bv); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), 0)", slot, RSC2(c, slot, "uint2(%s, %s)", A, Bv)); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), 0)", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, Bv, Bv)); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), int(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A, A1), Bv); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "texelFetch(t%d, ivec3(%s, %s, %s), 0)", slot, A, A1, Bv); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "texelFetch(t%d, ivec3(ivec2(%s), int(%s)), 0)", slot, RSC2(c, slot, "uint2(%s, %s)", A1, Bv), A); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), int(%s))", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, B1, B1), Bv); break;
        default: fail(c, "tlds info %d", info); return false;
        }
    }
    *slot_out = slot;
    return true;
}

static void emit_texs(Ctx *c, uint64_t w, bool fetch) {
    int d0 = (int)bits(w, 0, 8), d1 = (int)bits(w, 28, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int info = (int)bits(w, 53, 4);
    int msel = (int)bits(w, 50, 3);
    uint32_t mask;
    if (d1 == RZ) {
        static const uint32_t m2[] = {1, 2, 4, 8, 3, 9, 10, 12};
        mask = m2[msel];
    } else {
        static const uint32_t m4[] = {7, 11, 13, 14, 15, 15, 15, 15};
        mask = m4[msel];
    }
    char ra[16], ra1[16], rb[16], rb1[16];
    const char *A = reg(c, a, ra), *A1 = reg(c, a == RZ ? RZ : a + 1, ra1), *Bv = reg(c, b, rb),
               *B1 = reg(c, b == RZ ? RZ : b + 1, rb1);
    char call[1024];
    int slot;
    if (c->glsl) {
        char gcall[4096];
        if (!glsl_texs(c, w, fetch, &slot, gcall, A, A1, Bv, B1)) return;
        write_tex_result(c, slot, gcall, mask, d0, d1, true, !bits(w, 59, 1));
        return;
    }
    if (!fetch) {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "t%d.sample(s%d, %s)", slot, slot, F(A)); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.sample(s%d, %s)", slot, slot, UV2(slot, F(A), F(Bv))); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.sample(s%d, %s, level(0))", slot, slot, UV2(slot, F(A), F(Bv))); break;
        case 3: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.sample(s%d, %s, level(%s))", slot, slot, UV2(slot, F(A), F(A1)), F(Bv)); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, true, false);
            sprintf(call, "float4(t%d.sample_compare(s%d, %s, %s))", slot, slot, UV2(slot, F(A), F(A1)), F(Bv)); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, true, false);
            sprintf(call, "float4(t%d.sample_compare(s%d, %s, %s, level(%s)))", slot, slot, UV2(slot, F(A), F(A1)), F(B1), F(Bv)); break;
        case 6: slot = tex_slot(c, index, TEXTYPE_2D, true, false);
            sprintf(call, "float4(t%d.sample_compare(s%d, %s, %s, level(0)))", slot, slot, UV2(slot, F(A), F(A1)), F(Bv)); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "t%d.sample(s%d, %s, %s)", slot, slot, UV2(slot, F(A1), F(Bv)), ALAYER(A, slot)); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "t%d.sample(s%d, %s, %s, level(0))", slot, slot, UV2(slot, F(A1), F(Bv)), ALAYER(A, slot)); break;
        case 9: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, true, true);
            sprintf(call, "float4(t%d.sample_compare(s%d, %s, %s, %s, level(0)))", slot, slot, UV2(slot, F(A1), F(Bv)), A, F(B1)); break;
        case 10: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 11: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s), level(0))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 13: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s), level(%s))", slot, slot, F(A), F(A1), F(Bv), F(B1)); break;
        default: fail(c, "texs info %d", info); return;
        }
    } else {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "t%d.read(uint(%s))", slot, A); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "t%d.read(uint(%s), uint(%s))", slot, A, Bv); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s)", slot, RSC2(c, slot, "uint2(%s, %s)", A, Bv)); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s)", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, Bv, Bv)); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A, A1), Bv); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "t%d.read(uint3(%s, %s, %s))", slot, A, A1, Bv); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A1, Bv), A); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, B1, B1), Bv); break;
        default: fail(c, "tlds info %d", info); return;
        }
    }
    write_tex_result(c, slot, call, mask, d0, d1, true, !bits(w, 59, 1));  // 0xd0/0xd2: .F16 results
}

// TLD4S: a 2D gather (component at 52, or a depth compare with .DC at 50), results as TEXS's (all four: d0, d0 + 1,
// d1, d1 + 1). Coordinates: x in a and y in b, or with .AOFFI (51) / .DC: x, y in a, a + 1 and the packed offset
// (6-bit signed x, y bytes), then the reference in b, b + 1. .F16 at 55. GLSL only (the Vulkan backend).
static void emit_tld4s(Ctx *c, uint64_t w) {
    int d0 = (int)bits(w, 0, 8), d1 = (int)bits(w, 28, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int comp = (int)bits(w, 52, 2);
    bool dc = bits(w, 50, 1), aoffi = bits(w, 51, 1), f16 = bits(w, 55, 1);
    if (!c->glsl) { fail(c, "tld4s (msl)"); return; }
    char ra[16], ra1[16], rb[16], rb1[16];
    const char *A = reg(c, a, ra), *A1 = reg(c, a == RZ ? RZ : a + 1, ra1), *Bv = reg(c, b, rb),
               *B1 = reg(c, b == RZ ? RZ : b + 1, rb1);
    const char *y = aoffi || dc ? A1 : Bv, *off = aoffi ? Bv : NULL, *dref = dc ? (aoffi ? B1 : Bv) : NULL;
    int slot = tex_slot(c, index, TEXTYPE_2D, dc, false);
    if (c->fail) return;
    char xy[700], P[900], call[1200];
    if (off)
        snprintf(xy, sizeof xy, "(vec2(as_f(%s), as_f(%s)) + vec2(ivec2(int(%s << 26) >> 26, int(%s << 18) >> 26)) / "
                 "vec2(tsize%d(0).xy))", A, y, off, off, slot);
    else snprintf(xy, sizeof xy, "vec2(as_f(%s), as_f(%s))", A, y);
    if (c->stage != STAGE_VERTEX) { snprintf(P, sizeof P, "uvs(%s, rscale[%d])", xy, slot + 1); c->uses_rscale = true; }
    else snprintf(P, sizeof P, "%s", xy);
    if (dc) snprintf(call, sizeof call, "textureGather(t%d, %s, as_f(%s))", slot, P, dref);
    else snprintf(call, sizeof call, "textureGather(t%d, %s, %d)", slot, P, comp);
    write_tex_result(c, slot, call, 0xF, d0, d1, true, f16);
}

// TXD (GLSL): a lookup with explicit derivatives. Coordinates from gpr8, then (array or offsets) one register packing
// the layer (low 16 bits) and the offsets (4-bit x at 16, y at 20); derivatives from gpr20, interleaved per
// coordinate: dx.x, dy.x, dx.y, dy.y. 1D / 2D (and arrays) only.
static void emit_txd(Ctx *c, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8), target = (int)bits(w, 29, 2);
    bool array = bits(w, 28, 1), aoffi = bits(w, 35, 1);
    uint32_t mask = (uint32_t)bits(w, 31, 4), index = (uint32_t)bits(w, 36, 13);
    if (!c->glsl || target > 1) { fail(c, "txd %s", c->glsl ? "3d / cube" : "(msl)"); return; }
    int n = target == 0 ? 1 : 2;
    int type = target == 0 ? (array ? TEXTYPE_1D_ARRAY : TEXTYPE_1D) : (array ? TEXTYPE_2D_ARRAY : TEXTYPE_2D);
    int slot = tex_slot(c, index, type, false, array);
    char cs[2][40], lay[96], offv[96], grad[400];
    const char *co[3] = {cs[0], cs[1], NULL}, *layp = NULL, *offp = NULL;
    for (int i = 0; i < n; i++) { sprintf(cs[i], "as_type<float>(r%d)", a + i); c->reg_used[a + i] = 1; }
    int pk = a + n;
    if (array) { sprintf(lay, "min(r%d & 0xffffu, uint(tsize%d(0).z) - 1u)", pk, slot); c->reg_used[pk] = 1; layp = lay; }
    if (aoffi) { sprintf(offv, "int2(int(r%d << 12) >> 28, int(r%d << 8) >> 28)", pk, pk); c->reg_used[pk] = 1; offp = offv; }
    for (int i = 0; i < 2 * n; i++) c->reg_used[b + i] = 1;
    if (n == 1) {
        sprintf(grad, "as_type<float>(r%d), as_type<float>(r%d)", b, b + 1);
    } else {
        // (resolution-scaled textures: the coordinates go through uvs(), the derivatives scale with them)
        const char *sc = c->stage != STAGE_VERTEX ? "rscale" : NULL;
        if (sc) {
            sprintf(grad, "vec2(as_type<float>(r%d), as_type<float>(r%d)) * rscale[%d].zw, "
                          "vec2(as_type<float>(r%d), as_type<float>(r%d)) * rscale[%d].zw", b, b + 2, slot + 1, b + 1, b + 3, slot + 1);
            c->uses_rscale = true;
        } else {
            sprintf(grad, "vec2(as_type<float>(r%d), as_type<float>(r%d)), vec2(as_type<float>(r%d), as_type<float>(r%d))",
                    b, b + 2, b + 1, b + 3);
        }
    }
    char call[4096];
    glsl_sample(c, call, slot, co, layp, NULL, 3, grad, offp);
    write_tex_result(c, slot, call, mask, d, RZ, false, false);
}

// TEX (full form): coordinates from consecutive registers at gpr8, extra parameters at gpr20.
static void emit_tex(Ctx *c, uint64_t w, bool bindless) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = bindless ? 0 : (uint32_t)bits(w, 36, 13);
    int target = (int)bits(w, 29, 2);
    bool array = bits(w, 28, 1), dc = bits(w, 50, 1), aoffi = bindless ? bits(w, 36, 1) : bits(w, 54, 1);
    uint32_t mask = (uint32_t)bits(w, 31, 4);
    int lod_mode = bindless ? (int)bits(w, 37, 3) : (int)bits(w, 55, 3);  // 0 none, 1 lz, 2 lb, 3 ll, 6 lba, 7 lla
    if (bindless) { fail(c, "bindless tex"); return; }
    int type = target == 0 ? (array ? TEXTYPE_1D_ARRAY : TEXTYPE_1D)
             : target == 1 ? (array ? TEXTYPE_2D_ARRAY : TEXTYPE_2D)
             : target == 2 ? TEXTYPE_3D
                           : (array ? TEXTYPE_CUBE_ARRAY : TEXTYPE_CUBE);
    int ncoord = target == 0 ? 1 : target == 1 ? 2 : 3;
    int slot = tex_slot(c, index, type, dc, array);
    if (c->glsl) {  // same register layout as below
        char cs[3][40], lay[96], lodv[48], offv[96], drv[48], gcall[4096];
        const char *co[3], *layp = NULL, *lodp = NULL, *offp = NULL, *drp = NULL;
        int ar2 = a, p2 = b, lk = 0;
        if (array) { sprintf(lay, "min(uint(r%d), uint(tsize%d(0).z) - 1u)", a, slot); c->reg_used[a] = 1; ar2 = a + 1; layp = lay; }
        for (int i = 0; i < ncoord; i++) { sprintf(cs[i], "as_type<float>(r%d)", ar2 + i); c->reg_used[ar2 + i] = 1; co[i] = cs[i]; }
        if (lod_mode == 2 || lod_mode == 3 || lod_mode == 6 || lod_mode == 7) {
            c->reg_used[p2] = 1;
            sprintf(lodv, "as_type<float>(r%d)", p2);
            lodp = lodv;
            lk = lod_mode == 2 || lod_mode == 6 ? 2 : 1;
            p2++;
        } else if (lod_mode == 1) {
            lk = 1;
            lodp = "0.0f";
        }
        if (aoffi) { c->reg_used[p2] = 1; sprintf(offv, "int2(int(r%d << 28) >> 28, int(r%d << 24) >> 28)", p2, p2); offp = offv; p2++; }
        if (dc) { c->reg_used[p2] = 1; sprintf(drv, "as_type<float>(r%d)", p2); drp = drv; p2++; }
        glsl_sample(c, gcall, slot, co, layp, drp, lk, lodp, offp);
        write_tex_result(c, slot, gcall, mask, d, RZ, false, false);
        return;
    }
    char coords[256] = "", layer[64] = "", tmp[64];
    int ar = a;
    // the hardware clamps the array index to the layer count
    if (array) { sprintf(layer, "min(uint(r%d), t%d.get_array_size() - 1u)", a, slot); c->reg_used[a] = 1; ar = a + 1; }
    for (int i = 0; i < ncoord; i++) {
        sprintf(tmp, "%sas_type<float>(r%d)", i ? ", " : "", ar + i);
        c->reg_used[ar + i] = 1;
        strcat(coords, tmp);
    }
    int pr = b;
    char lod[96] = "", dref[64] = "", off[128] = "";
    if (lod_mode == 2 || lod_mode == 3 || lod_mode == 6 || lod_mode == 7) {
        c->reg_used[pr] = 1;
        sprintf(lod, lod_mode == 2 || lod_mode == 6 ? ", bias(as_type<float>(r%d))" : ", level(as_type<float>(r%d))", pr);
        pr++;
    } else if (lod_mode == 1) {
        strcpy(lod, ", level(0)");
    }
    if (aoffi) {
        c->reg_used[pr] = 1;
        sprintf(off, ", int2(int(r%d << 28) >> 28, int(r%d << 24) >> 28)", pr, pr);
        pr++;
    }
    if (dc) { c->reg_used[pr] = 1; sprintf(dref, "as_type<float>(r%d)", pr); pr++; }
    const char *vec = ncoord == 1 ? "" : ncoord == 2 ? "float2" : "float3";
    char cexpr[420], call[1024];
    if (ncoord == 2 && c->stage != STAGE_VERTEX) { snprintf(cexpr, sizeof cexpr, "uvs(float2(%s), rscale[%d])", coords, slot + 1); c->uses_rscale = true; }
    else snprintf(cexpr, sizeof cexpr, "%s(%s)", vec, coords);
    if (dc)
        sprintf(call, "float4(t%d.sample_compare(s%d, %s%s%s, %s%s%s))", slot, slot, cexpr, array ? ", " : "",
                layer, dref, lod, off);
    else
        sprintf(call, "t%d.sample(s%d, %s%s%s%s%s)", slot, slot, cexpr, array ? ", " : "", layer, lod, off);
    write_tex_result(c, slot, call, mask, d, RZ, false, false);
}

// ------------------------------------------------------------------ surfaces (SULD / SUST: image load / store)
// GLSL only. A bound image (the handle in the driver constants at 0x120 + 8 * k: a texture pool index, like a
// texture's) takes a texture slot of its own with is_image set: binding 32 + slot, an image<type> without a format
// qualifier (shaderStorageImageRead / WriteWithoutFormat; the view is the texture's, in its non-sRGB format).
// Formatted (.P) access only, as float components: integer-format images are not handled yet.
static int img_slot(Ctx *c, uint32_t index, int type) {
    if (index >= 8192) return 0;
    if (c->tex_slot[index]) {
        int s = c->tex_slot[index] - 1;
        if (c->tex_weak[s] && !c->info->tex[s].is_image) {  // (imageSize() is a TXQ on the image's handle)
            ShaderTexture *t = &c->info->tex[s];
            t->type = (uint8_t)type;
            t->is_image = true;
            t->is_array = type == TEXTYPE_1D_ARRAY || type == TEXTYPE_2D_ARRAY;
            c->tex_weak[s] = false;
        }
        if (!c->info->tex[s].is_image || c->info->tex[s].type != type) fail(c, "image %u used as two types (%s %d, then image %d)", index, c->info->tex[s].is_image ? "image" : "texture", c->info->tex[s].type, type);
        return s;
    }
    int s = c->info->ntex++;
    if (s >= 31) { fail(c, "too many textures"); return 0; }
    c->tex_slot[index] = s + 1;
    ShaderTexture *t = &c->info->tex[s];
    t->handle_offset = index * 4;
    t->type = (uint8_t)type;
    t->is_image = true;
    t->is_array = type == TEXTYPE_1D_ARRAY || type == TEXTYPE_2D_ARRAY;
    return s;
}

// SULD (store = false) / SUST. Fields: data / destination gpr0, coordinates from gpr8 (consecutive registers: x, y,
// z or layer - its low 16 bits), component mask 20..23 (.P), surface type 33..35, bound (51) with the handle offset
// / 4 at 36..48, raw (.D) mode 52.
static void emit_surface(Ctx *c, uint64_t w, bool store) {
    if (!c->glsl) { fail(c, "%s (MSL)", store ? "SUST" : "SULD"); return; }
    if (!bits(w, 51, 1)) { fail(c, "%s bindless", store ? "SUST" : "SULD"); return; }
    if (bits(w, 52, 1)) { fail(c, "%s.D (raw)", store ? "SUST" : "SULD"); return; }
    static const int types[8] = {TEXTYPE_1D, TEXTYPE_BUFFER, TEXTYPE_1D_ARRAY, TEXTYPE_2D, TEXTYPE_2D_ARRAY, TEXTYPE_3D, -1, -1};
    int type = types[bits(w, 33, 3)];
    if (type < 0 || type == TEXTYPE_BUFFER) { fail(c, "%s surface type %d", store ? "SUST" : "SULD", (int)bits(w, 33, 3)); return; }
    int slot = img_slot(c, (uint32_t)bits(w, 36, 13), type);
    int dr = (int)bits(w, 0, 8), cr = (int)bits(w, 8, 8);
    uint32_t mask = (uint32_t)bits(w, 20, 4);
    char co[3][16];
    for (int i = 0; i < 3; i++) {
        if (cr == RZ || cr + i > 254) strcpy(co[i], "0");
        else { c->reg_used[cr + i] = 1; snprintf(co[i], sizeof co[i], "int(r%d)", cr + i); }
    }
    char lay[32];
    snprintf(lay, sizeof lay, "(%s & 0xFFFF)", type == TEXTYPE_1D_ARRAY ? co[1] : co[2]);
    char cexpr[96], inb[160];
    switch (type) {
    case TEXTYPE_1D:
        snprintf(cexpr, sizeof cexpr, "%s", co[0]);
        snprintf(inb, sizeof inb, "uint(cs_) < uint(imageSize(t%d))", slot);
        break;
    case TEXTYPE_1D_ARRAY:
        snprintf(cexpr, sizeof cexpr, "ivec2(%s, %s)", co[0], lay);
        snprintf(inb, sizeof inb, "all(lessThan(uvec2(cs_), uvec2(imageSize(t%d))))", slot);
        break;
    case TEXTYPE_2D:
        snprintf(cexpr, sizeof cexpr, "ivec2(%s, %s)", co[0], co[1]);
        snprintf(inb, sizeof inb, "all(lessThan(uvec2(cs_), uvec2(imageSize(t%d))))", slot);
        break;
    case TEXTYPE_2D_ARRAY:
        snprintf(cexpr, sizeof cexpr, "ivec3(%s, %s, %s)", co[0], co[1], lay);
        snprintf(inb, sizeof inb, "all(lessThan(uvec3(cs_), uvec3(imageSize(t%d))))", slot);
        break;
    default:
        snprintf(cexpr, sizeof cexpr, "ivec3(%s, %s, %s)", co[0], co[1], co[2]);
        snprintf(inb, sizeof inb, "all(lessThan(uvec3(cs_), uvec3(imageSize(t%d))))", slot);
        break;
    }
    const char *ct = type == TEXTYPE_1D ? "int" : (type == TEXTYPE_2D || type == TEXTYPE_1D_ARRAY) ? "ivec2" : "ivec3";
    if (store) {
        c->img_write[slot] = true;
        c->info->tex[slot].image_write = true;
        sp(&c->body, "    { %s cs_ = %s; vec4 sv_ = vec4(0.0f, 0.0f, 0.0f, 1.0f);\n", ct, cexpr);
        int n = 0;
        for (int i = 0; i < 4; i++)
            if (mask & (1u << i)) {
                int r = dr == RZ ? RZ : dr + n;
                n++;
                if (r == RZ || r > 254) sp(&c->body, "      sv_[%d] = 0.0f;\n", i);
                else { c->reg_used[r] = 1; sp(&c->body, "      sv_[%d] = uintBitsToFloat(r%d);\n", i, r); }
            }
        sp(&c->body, "      if (%s) imageStore(t%d, cs_, sv_); }\n", inb, slot);
    } else {
        c->img_read[slot] = true;
        sp(&c->body, "    { %s cs_ = %s; vec4 lv_ = %s ? imageLoad(t%d, cs_) : vec4(0.0f);\n", ct, cexpr, inb, slot);
        int n = 0;
        for (int i = 0; i < 4; i++)
            if (mask & (1u << i)) {
                int r = dr + n++;
                if (dr == RZ || r > 254) continue;
                c->reg_used[r] = 1;
                sp(&c->body, "      r%d = floatBitsToUint(lv_[%d]);\n", r, i);
            }
        sp(&c->body, "    }\n");
    }
}

// ------------------------------------------------------------------ instruction translation
// The game's light-culling compute shader (a 30x17 grid of 64-pixel tiles for the tiled lighting of interiors and
// caves) turns a tile's pixel column into a view direction with the screen height alone: x = px * 2 / height -
// aspect, which assumes square pixels (width / height = aspect). With a widened camera the image keeps its 1920x1080
// target, so pixels are no longer square: at 32:9 the tiles right of the screen centre got the lights meant for
// half as far out, and the lighting changed along a hard vertical line where a light's tiles ended (the floor
// right of it lighter, MarthaUkuku at 2/3 of the window). The two column steps (pc 0x198: tile x, 0x1a8: tile x +
// 1) are scaled by aspect * height / width, which is 1 at 16:9: the original shader's result there.
static void light_tiles_fix(Ctx *c, const uint64_t *code, size_t end, uint32_t pc) {
    static const uint64_t sig[][2] = {{0x188, 0x5c68100000970309ull}, {0x198, 0x5c68100000d7030dull},
                                      {0x1a8, 0x5c6810000087030bull}, {0x1b0, 0x5c68100000470303ull}};
    if (c->stage != STAGE_COMPUTE || (pc != 0x198 && pc != 0x1a8) || end <= 0x1b0 / 8) return;
    for (size_t i = 0; i < sizeof sig / sizeof sig[0]; i++)
        if (code[sig[i][0] / 8] != sig[i][1]) return;
    int d = (int)(code[pc / 8] & 0xff);  // (the destination: r13 / r11)
    char a[48], h[48], w[48];
    cbuf_ref(c, 4, 0x1bc, NULL, a);  // camera aspect
    cbuf_ref(c, 4, 0x1c4, NULL, h);  // target height
    cbuf_ref(c, 4, 0x1c0, NULL, w);  // target width
    sp(&c->body, "    r%d = as_type<uint>(as_type<float>(r%d) * (as_type<float>(%s) * as_type<float>(%s) / as_type<float>(%s)));\n", d, d, a, h, w);  // (GLSL: as_type is rewritten)
}

// A CAL to a short subroutine without control flow of its own (a texture fetch helper, an empty one) is inlined: its
// RET would otherwise make the program a pc state machine with a case for every return address, which the driver
// compiles very slowly (the big effect compute programs: ~3000 calls, minutes per compile). Returns the index of the
// subroutine's RET, 0 if it isn't one of those.
static size_t cal_inline_ret(const Ctx *c, uint32_t target) {
    const uint64_t *code = (const uint64_t *)c->code;
    if (!code) return 0;
    for (size_t i = target / 8, n = 0; i < c->ninsts && n < 64; i++) {
        if (i % 4 == 0) continue;
        uint64_t w = code[i], top12 = w & 0xfff0000000000000ull;
        if (top12 == 0xe320000000000000ull) return bits(w, 16, 4) == PT ? i : 0;  // (an unconditional RET)
        if (((w >> 56) == 0xe2 || (w >> 56) == 0xe3) && top12 != 0xe330000000000000ull) return 0;  // (flow, but KIL)
        if ((w & 0xfff8000000000000ull) == 0xf0f8000000000000ull) return 0;                     // (SYNC)
        n++;
    }
    return 0;
}

static void structure_goto(Ctx *c, size_t from, size_t l);
static void structure_exit(Ctx *c);
static size_t vmap(const Ctx *c, int ctx, size_t k);
static bool brx_marks(Ctx *c, size_t i, uint8_t *mark);
static bool brx_table_labels(const uint8_t *code, size_t i, size_t end, const uint8_t *blob, size_t const_offset,
                             uint8_t *label, bool clamped);

static void emit_insn(Ctx *c, uint64_t w, uint32_t pc) {
    char t[2048], u[2048];
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    int pr = (int)bits(w, 16, 3);
    bool prn = bits(w, 19, 1);
    uint16_t op13 = (uint16_t)(w >> 51);  // top 13 bits
    // predicate guard. SSY/PBK/PCNT/PRET take no predicate: those bits are zero in the encoding,
    // which would otherwise read as @P0.
    uint64_t top12 = w & 0xfff0000000000000ull;
    if (top12 == 0xe290000000000000ull || top12 == 0xe2a0000000000000ull || top12 == 0xe2b0000000000000ull ||
        top12 == 0xe270000000000000ull) {
        pr = PT;
        prn = false;
    }
    bool guarded = !(pr == PT && !prn);
    if (pr == PT && prn) return;  // @!pt: never executes
    if (guarded) sp(&c->body, "  if (%s) {\n", P(pr, prn));

#define OPM(val, mask) ((w & (uint64_t)(mask)) == (uint64_t)(val))
    if (OPM(0xe300000000000000ull, 0xfff0000000000000ull)) {  // EXIT
        if (c->structured) structure_exit(c);
        else sp(&c->body, c->branchy ? "    exit_now = true; break;\n" : "    break;\n");
    } else if (OPM(0xe330000000000000ull, 0xfff0000000000000ull)) {  // KIL
        c->info->uses_discard = true;
        if (getenv("LARECOMP_NO_DISCARD")) {
        } else if (!c->glsl) sp(&c->body, "    discard_fragment();\n");
        else if (c->stage == STAGE_FRAGMENT) sp(&c->body, "    discard;\n");
        else sp(&c->body, "    /* kil */\n");  // GLSL has discard in fragment shaders only
    } else if (OPM(0xe240000000000000ull, 0xfff0000000000020ull)) {  // BRA
        uint32_t target = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
        c->uses_branches = true;
        if (c->structured) structure_goto(c, pc / 8, target / 8);
        else sp(&c->body, "    pc = %uu; continue;\n", target);
    } else if (OPM(0xe290000000000000ull, 0xfff0000000000020ull)) {  // SSY
        c->uses_branches = true;
        if (c->structured) {
        } else if (!c->ssy_depth) sp(&c->body, "    ssy[ssy_n++] = %uu;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
        else if (c->ssy_depth[pc / 8] >= 0)
            sp(&c->body, "    ssy%d = %uu;\n", c->ssy_depth[pc / 8], norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xe260000000000000ull, 0xfff0000000000020ull)) {  // CAL: a subroutine (its RET comes back here)
        uint32_t target = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
        size_t ret = cal_inline_ret(c, target);
        if (ret) {
            for (size_t i = target / 8; i < ret; i++)
                if (i % 4) emit_insn(c, ((const uint64_t *)c->code)[i], (uint32_t)(i * 8));
        } else {
            c->uses_branches = true;
            sp(&c->body, "    cst[cst_n++] = %uu; pc = %uu; continue;\n", norm_target((int64_t)pc + 8), target);
        }
    } else if (OPM(0xe320000000000000ull, 0xfff0000000000000ull)) {  // RET (from the program itself: EXIT)
        if (c->structured) structure_exit(c);  // (structure_flow: no CAL that isn't inlined)
        else sp(&c->body, c->branchy ? "    if (cst_n == 0u) { exit_now = true; break; } pc = cst[--cst_n]; continue;\n" : "    break;\n");
    } else if (OPM(0xe2a0000000000000ull, 0xfff0000000000020ull)) {  // PBK
        c->uses_branches = true;
        if (c->structured) {
        } else if (!c->pbk_depth) sp(&c->body, "    pbk[pbk_n++] = %uu;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
        else if (c->pbk_depth[pc / 8] >= 0)
            sp(&c->body, "    pbk%d = %uu;\n", c->pbk_depth[pc / 8], norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xe2b0000000000000ull, 0xfff0000000000020ull)) {  // PCNT
        c->uses_branches = true;
        sp(&c->body, "    pcnt[pcnt_n++] = %uu;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xf0f8000000000000ull, 0xfff8000000000000ull)) {  // SYNC
        if (c->structured) structure_goto(c, pc / 8, c->pop_tgt[pc / 8] / 8);
        else if (!c->ssy_depth) sp(&c->body, "    pc = ssy[--ssy_n]; continue;\n");
        else if (c->ssy_depth[pc / 8] > 0) sp(&c->body, "    pc = ssy%d; continue;\n", c->ssy_depth[pc / 8] - 1);
        else sp(&c->body, "    exit_now = true; break;\n");  // (never reached: static_stacks)
    } else if (OPM(0xe340000000000000ull, 0xfff0000000000000ull)) {  // BRK
        if (c->structured) structure_goto(c, pc / 8, c->pop_tgt[pc / 8] / 8);
        else if (!c->pbk_depth) sp(&c->body, "    pc = pbk[--pbk_n]; continue;\n");
        else if (c->pbk_depth[pc / 8] > 0) sp(&c->body, "    pc = pbk%d; continue;\n", c->pbk_depth[pc / 8] - 1);
        else sp(&c->body, "    exit_now = true; break;\n");  // (never reached: static_stacks)
    } else if (OPM(0xe350000000000000ull, 0xfff0000000000000ull)) {  // CONT
        sp(&c->body, "    pc = pcnt[pcnt_n - 1]; continue;\n");
    } else if (OPM(0xf0f0000000000000ull, 0xfff8000000000000ull) || OPM(0x50b0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0xef98000000000000ull, 0xfff8000000000000ull)) {
        // DEPBAR / NOP / MEMBAR
    } else if (OPM(0xe00000000000ff00ull, 0xff0000400000ff00ull) || OPM(0xe000004000000000ull, 0xff00004000000000ull)) {
        // IPA: interpolate fragment input
        bool idx = bits(w, 38, 1);
        uint32_t addr = (uint32_t)bits(w, 28, 10);
        int mode = (int)bits(w, 54, 2);  // 0 pass, 1 multiply, 2 constant, 3 sc
        if (idx) { fail(c, "ipa idx"); }
        if (addr >= 0x80 && addr < 0x280) {
            int at = (addr - 0x80) >> 4;
            if (mode == 2) c->flat_mask |= 1u << at;
        }
        c->ipa_mode = mode;
        c->ipa_mul = (int)bits(w, 20, 8);
        attr_load(c, d, addr, NULL);
        c->ipa_mode = -1;
        if (bits(w, 51, 1) && d != RZ) sp(&c->body, "    r%d = as_type<uint>(saturate(as_type<float>(r%d)));\n", d, d);
    } else if (OPM(0xefd8000000000000ull, 0xfff8000000000000ull)) {  // LD attribute
        int n = 1 + (int)bits(w, 47, 2);
        uint32_t addr = (uint32_t)bits(w, 20, 10);
        int ia = (int)bits(w, 8, 8);
        if (ia != RZ) { fail(c, "indexed ld a[]"); }
        for (int i = 0; i < n; i++) attr_load(c, d + i, addr + 4 * i, NULL);
    } else if (OPM(0xeff0000000000000ull, 0xfff8000000000000ull)) {  // ST attribute
        int n = 1 + (int)bits(w, 47, 2);
        uint32_t addr = (uint32_t)bits(w, 20, 10);
        for (int i = 0; i < n; i++) attr_store(c, d + i, addr + 4 * i);
    } else if (OPM(0xef90000000000000ull, 0xfff8000000000000ull)) {  // LD c[idx][reg + imm]
        int idx = (int)bits(w, 36, 5);
        int64_t off = (int64_t)bits(w, 20, 16);  // unsigned (Ryujinx InstLdc.CbufOffset)
        int sz = (int)bits(w, 48, 3);
        int n = sz == 5 ? 2 : 1;
        char rb[16];
        const char *dyn = a == RZ ? NULL : reg(c, a, rb);
        // LDC.64 R4, c[i][R4 + x]: the address register is also a destination. Read it once before the writes, or
        // the second word comes from c[i][<first word> + x + 4] (fix from letsgo-recomp)
        bool overlap = dyn && n > 1 && a >= d && a < d + n;
        if (overlap) { sp(&c->body, "    { uint ldc_addr = %s;\n", dyn); dyn = "ldc_addr"; }
        for (int i = 0; i < n; i++) {
            char cb[256];
            cbuf_ref(c, idx, off + 4 * i, dyn, cb);
            if (dyn) c->info->cbuf_size[idx] = 0x10000;
            set_reg(c, d + i, cb);
        }
        if (overlap) sp(&c->body, "    }\n");
    } else if (OPM(0xd000000000000000ull, 0xf600000000000000ull)) {  // TEXS
        emit_texs(c, w, false);
    } else if (OPM(0xd200000000000000ull, 0xf600000000000000ull)) {  // TLDS
        emit_texs(c, w, true);
    } else if (OPM(0xc038000000000000ull, 0xfc38000000000000ull)) {  // TEX
        emit_tex(c, w, false);
    } else if (OPM(0xdeb8000000000000ull, 0xfff8000000000000ull)) {  // TEX.B (bindless)
        emit_tex(c, w, true);
    } else if (OPM(0xde38000000000000ull, 0xfff8000000000000ull)) {  // TXD
        emit_txd(c, w);
    } else if (OPM(0xeb18000000000000ull, 0xfff8000000000000ull)) {  // SULD
        emit_surface(c, w, false);
    } else if (OPM(0xeb28000000000000ull, 0xfff8000000000000ull)) {  // SUST
        emit_surface(c, w, true);
    } else if (OPM(0xdf00000000000000ull, 0xffc0000000000000ull)) {  // TLD4S
        emit_tld4s(c, w);
    } else if (OPM(0xdf48000000000000ull, 0xfff8000000000000ull)) {  // TXQ
        uint32_t index = (uint32_t)bits(w, 36, 13);
        int qt = (int)bits(w, 22, 6);
        uint32_t mask = (uint32_t)bits(w, 31, 4);
        int slot = tex_slot(c, index, -1, false, false);  // the type comes from a sample, if any
        if (qt == 1 && c->glsl) {  // tsize<slot>() is emitted with the slot's final type
            if (c->stage != STAGE_VERTEX) {
                c->uses_rscale = true;
                sp(&c->body, "    { uint lv = %s; uvec4 q = uvec4(uint(floor(float(tsize%d(int(lv)).x) * rscale[%d].z / rscale[%d].x + 0.5f)), "
                   "uint(floor(float(tsize%d(int(lv)).y) * rscale[%d].w / rscale[%d].y + 0.5f)), uint(tsize%d(int(lv)).z), uint(tlevels%d()));\n",
                   R(a), slot, slot + 1, slot + 1, slot, slot + 1, slot + 1, slot, slot);
            } else {
                sp(&c->body, "    { uint lv = %s; uvec4 q = uvec4(uint(tsize%d(int(lv)).x), uint(tsize%d(int(lv)).y), uint(tsize%d(int(lv)).z), uint(tlevels%d()));\n",
                   R(a), slot, slot, slot, slot);
            }
            int n = 0;
            for (int i = 0; i < 4; i++)
                if (mask & (1u << i)) { c->reg_used[d + n] = 1; sp(&c->body, "      r%d = q[%d];\n", d + n, i); n++; }
            sp(&c->body, "    }\n");
        } else if (qt == 1) {  // dimension
            if (c->stage != STAGE_VERTEX) {  // report the guest size of scaled allocations
                c->uses_rscale = true;
                sp(&c->body, "    { uint lv = %s; uint4 q = uint4(uint(round(float(t%d.get_width(lv)) * rscale[%d].z / rscale[%d].x)), "
                   "uint(round(float(t%d.get_height(lv)) * rscale[%d].w / rscale[%d].y)), 1, t%d.get_num_mip_levels());\n",
                   R(a), slot, slot + 1, slot + 1, slot, slot + 1, slot + 1, slot);
            } else {
                sp(&c->body, "    { uint lv = %s; uint4 q = uint4(t%d.get_width(lv), t%d.get_height(lv), 1, t%d.get_num_mip_levels());\n",
                   R(a), slot, slot, slot);
            }
            int n = 0;
            for (int i = 0; i < 4; i++)
                if (mask & (1u << i)) { c->reg_used[d + n] = 1; sp(&c->body, "      r%d = q[%d];\n", d + n, i); n++; }
            sp(&c->body, "    }\n");
        } else {
            fail(c, "txq type %d", qt);
        }
    } else if (OPM(0x5080000000000000ull, 0xfff8000000000000ull)) {  // MUFU
        int fn = (int)bits(w, 20, 4);
        const char *x = NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1));
        switch (fn) {
        case 0: sprintf(t, "cos(%s)", x); break;
        case 1: sprintf(t, "sin(%s)", x); break;
        case 2: sprintf(t, "exp2(%s)", x); break;
        case 3: sprintf(t, "log2(%s)", x); break;
        case 4: sprintf(t, "(1.0f / %s)", x); break;
        case 5: sprintf(t, "rsqrt(%s)", x); break;
        case 8: sprintf(t, "sqrt(%s)", x); break;
        default: fail(c, "mufu %d", fn); return;
        }
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x5c90000000000000ull, 0xfff8000000000000ull) || OPM(0x4c90000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3890000000000000ull, 0xfef8000000000000ull)) {  // RRO: range reduction (no-op for us)
        const char *x = NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1));
        set_reg(c, d, FRES(x, false));
    } else if (OPM(0x5c58000000000000ull, 0xfff8000000000000ull) || OPM(0x4c58000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3858000000000000ull, 0xfef8000000000000ull)) {  // FADD
        sprintf(t, "(%s + %s)", NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1)), NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1)));
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x0800000000000000ull, 0xfc00000000000000ull)) {  // FADD32I
        sprintf(u, "as_type<float>(0x%08xu)", (uint32_t)bits(w, 20, 32));
        sprintf(t, "(%s + %s)", NA(F(R(a)), bits(w, 56, 1), bits(w, 54, 1)), NA(u, bits(w, 53, 1), bits(w, 57, 1)));
        set_reg(c, d, FRES(t, false));
    } else if (OPM(0x5c68000000000000ull, 0xfff8000000000000ull) || OPM(0x4c68000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3868000000000000ull, 0xfef8000000000000ull)) {  // FMUL
        static const char *const scale[] = {"", " * 0.5f", " * 0.25f", " * 0.125f", " * 8.0f", " * 4.0f", " * 2.0f", ""};
        sprintf(t, "(%s * %s%s)", F(R(a)), NA(F(B(true)), bits(w, 48, 1), false), scale[bits(w, 41, 3)]);
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x1e00000000000000ull, 0xff00000000000000ull)) {  // FMUL32I
        sprintf(t, "(%s * as_type<float>(0x%08xu))", F(R(a)), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, FRES(t, bits(w, 55, 1)));
    } else if (OPM(0x5980000000000000ull, 0xff80000000000000ull) || OPM(0x4980000000000000ull, 0xff80000000000000ull) ||
               OPM(0x5180000000000000ull, 0xff80000000000000ull) || OPM(0x3280000000000000ull, 0xfe80000000000000ull)) {  // FFMA
        const char *bv;
        char cbuf_c[256], bbuf[256];
        const char *cv = operand_c_for_ffma(c, w, cbuf_c, &bv, bbuf);
        sprintf(t, "fma(%s, %s, %s)", F(R(a)), NA(F(bv), bits(w, 48, 1), false), NA(F(cv), bits(w, 49, 1), false));
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x0c00000000000000ull, 0xfc00000000000000ull)) {  // FFMA32I
        sprintf(u, "as_type<float>(0x%08xu)", (uint32_t)bits(w, 20, 32));
        sprintf(t, "fma(%s, %s, %s)", NA(F(R(a)), bits(w, 56, 1), false), u, NA(F(R(d)), bits(w, 57, 1), false));
        set_reg(c, d, FRES(t, bits(w, 55, 1)));
    } else if (OPM(0x5c60000000000000ull, 0xfff8000000000000ull) || OPM(0x4c60000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3860000000000000ull, 0xfef8000000000000ull)) {  // FMNMX
        const char *x = NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1)), *y = NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1));
        sprintf(t, "(%s ? fmin(%s, %s) : fmax(%s, %s))", P((int)bits(w, 39, 3), bits(w, 42, 1)), x, y, x, y);
        set_reg(c, d, FRES(t, false));
    } else if (OPM(0x5bb0000000000000ull, 0xfff0000000000000ull) || OPM(0x4bb0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36b0000000000000ull, 0xfef0000000000000ull)) {  // FSETP
        const char *x = NA(F(R(a)), bits(w, 43, 1), bits(w, 7, 1)), *y = NA(F(B(true)), bits(w, 6, 1), bits(w, 44, 1));
        char cmp[512];
        fcmp_expr(cmp, (int)bits(w, 48, 4), x, y);
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pc_ = P((int)bits(w, 39, 3), bits(w, 42, 1));
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n      p%d = cmp %s pp;\n", cmp, pc_, p, bool_op(bo)); }
        else sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cmp, pc_);
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5800000000000000ull, 0xff00000000000000ull) || OPM(0x4800000000000000ull, 0xfe00000000000000ull) ||
               OPM(0x3000000000000000ull, 0xfe00000000000000ull)) {  // FSET
        const char *x = NA(F(R(a)), bits(w, 43, 1), bits(w, 54, 1)), *y = NA(F(B(true)), bits(w, 53, 1), bits(w, 44, 1));
        char cmp[512];
        fcmp_expr(cmp, (int)bits(w, 48, 4), x, y);
        sprintf(t, "((%s) %s %s)", cmp, bool_op((int)bits(w, 45, 2)), P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 52, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0x5ba0000000000000ull, 0xfff0000000000000ull) || OPM(0x4ba0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36a0000000000000ull, 0xfef0000000000000ull) || OPM(0x53a0000000000000ull, 0xfff0000000000000ull)) {  // FCMP
        char cmp[512], cb[256], bb[256];
        const char *bv, *cv;
        if (OPM(0x53a0000000000000ull, 0xfff0000000000000ull)) {
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
        } else {
            bv = operand_b(c, w, true, bb);
            cv = R((int)bits(w, 39, 8));
        }
        fcmp_expr(cmp, (int)bits(w, 48, 4), F(cv), "0.0f");
        sprintf(t, "(%s ? %s : %s)", cmp, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c98000000000000ull, 0xfff8000000000000ull) || OPM(0x4c98000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3898000000000000ull, 0xfef8000000000000ull)) {  // MOV
        set_reg(c, d, B(false));
    } else if (OPM(0x0100000000000000ull, 0xfff0000000000000ull)) {  // MOV32I
        sprintf(t, "0x%08xu", (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
    } else if (OPM(0x5ca0000000000000ull, 0xfff8000000000000ull) || OPM(0x4ca0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38a0000000000000ull, 0xfef8000000000000ull)) {  // SEL
        sprintf(t, "(%s ? %s : %s)", P((int)bits(w, 39, 3), bits(w, 42, 1)), R(a), B(false));
        set_reg(c, d, t);
    } else if (OPM(0x5cb0000000000000ull, 0xfff8000000000000ull) || OPM(0x4cb0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38b0000000000000ull, 0xfef8000000000000ull)) {  // F2I (src fmt bits 10-11: 1 F16; dst size bits 8-9)
        char srcv[512];
        if (bits(w, 10, 2) == 1 && c->glsl) sprintf(srcv, "h2f((%s >> %du) & 0xFFFFu)", B(false), 16 * (int)bits(w, 41, 1));
        else if (bits(w, 10, 2) == 1) sprintf(srcv, "float(as_type<half2>(%s)[%d])", B(false), (int)bits(w, 41, 1));
        else strcpy(srcv, F(B(true)));
        const char *x = NA(srcv, bits(w, 45, 1), bits(w, 49, 1));
        int rnd = (int)bits(w, 39, 2);
        bool sgn = bits(w, 12, 1), i16 = bits(w, 8, 2) == 1;
        static const char *const rf[] = {"rint", "floor", "ceil", "trunc"};
        if (i16)
            sprintf(t, sgn ? "uint(int(clamp(%s(%s), -32768.0f, 32767.0f)))" : "uint(clamp(%s(%s), 0.0f, 65535.0f))", rf[rnd], x);
        else
            sprintf(t, sgn ? "uint(int(clamp(%s(%s), -2147483648.0f, 2147483520.0f)))" : "uint(clamp(%s(%s), 0.0f, 4294967040.0f))", rf[rnd], x);
        set_reg(c, d, t);
    } else if (OPM(0x5cb8000000000000ull, 0xfff8000000000000ull) || OPM(0x4cb8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38b8000000000000ull, 0xfef8000000000000ull)) {  // I2F
        bool sgn = bits(w, 13, 1);
        int sz = (int)bits(w, 10, 2), sel = (int)bits(w, 41, 2);
        char src[256];
        const char *bv = B(false);
        if (sz == 0) sprintf(src, sgn ? "int(char((%s >> %d) & 0xFF))" : "((%s >> %d) & 0xFFu)", bv, sel * 8);
        else if (sz == 1) sprintf(src, sgn ? "int(short((%s >> %d) & 0xFFFF))" : "((%s >> %d) & 0xFFFFu)", bv, sel * 8);
        else sprintf(src, sgn ? "int(%s)" : "%s", bv);
        sprintf(u, "float(%s)", src);
        if (bits(w, 8, 2) == 1) {  // F16 result in the low half
            char h[700];
            snprintf(h, sizeof h, c->glsl ? "(packHalf2x16(vec2(%s, 0.0f)))" : "(uint(as_type<ushort>(half(%s))))",
                     NA(u, bits(w, 45, 1), bits(w, 49, 1)));
            set_reg(c, d, h);
        } else {
            set_reg(c, d, FRES(NA(u, bits(w, 45, 1), bits(w, 49, 1)), false));
        }
    } else if (OPM(0x5ca8000000000000ull, 0xfff8000000000000ull) || OPM(0x4ca8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38a8000000000000ull, 0xfef8000000000000ull)) {  // F2F (formats: 1 F16, 2 F32; bit 41 = high half)
        int dfmt = (int)bits(w, 8, 2), sfmt = (int)bits(w, 10, 2);
        char srcv[512];
        if (sfmt == 1 && c->glsl) sprintf(srcv, "h2f((%s >> %du) & 0xFFFFu)", B(false), 16 * (int)bits(w, 41, 1));
        else if (sfmt == 1) sprintf(srcv, "float(as_type<half2>(%s)[%d])", B(false), (int)bits(w, 41, 1));
        else strcpy(srcv, F(B(true)));
        const char *x = NA(srcv, bits(w, 45, 1), bits(w, 49, 1));
        int mode = (int)((bits(w, 39, 2)) | (bits(w, 42, 1) << 2));
        switch (mode) {
        case 4: sprintf(t, "rint(%s)", x); break;
        case 5: sprintf(t, "floor(%s)", x); break;
        case 6: sprintf(t, "ceil(%s)", x); break;
        case 7: sprintf(t, "trunc(%s)", x); break;
        default: sprintf(t, "%s", x);
        }
        if (dfmt == 1) {  // F16 result in the low half, high half zero
            char h[1100];
            snprintf(h, sizeof h, c->glsl ? "(packHalf2x16(vec2(%s, 0.0f)))" : "(uint(as_type<ushort>(half(%s))))",
                     bits(w, 50, 1) ? (sprintf(u, "saturate(%s)", t), u) : t);
            set_reg(c, d, h);
        } else {
            set_reg(c, d, FRES(t, bits(w, 50, 1)));
        }
    } else if (OPM(0x5c10000000000000ull, 0xfff8000000000000ull) || OPM(0x4c10000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3810000000000000ull, 0xfef8000000000000ull)) {  // IADD
        if (bits(w, 43, 1) || bits(w, 47, 1)) c->uses_cc = true;
        sprintf(t, "(%s%s + %s%s%s)", bits(w, 49, 1) ? "-" : "", R(a), bits(w, 48, 1) ? "-" : "", B(false),
                bits(w, 43, 1) ? " + cc" : "");
        if (bits(w, 47, 1)) {
            sp(&c->body, "    { uint s = %s%s; uint t2 = %s%s; uint res = s + t2%s; cc = (res < s) ? 1u : 0u;\n", bits(w, 49, 1) ? "-" : "",
               R(a), bits(w, 48, 1) ? "-" : "", B(false), bits(w, 43, 1) ? " + cc" : "");
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "    }\n");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x1c00000000000000ull, 0xfe00000000000000ull)) {  // IADD32I (CC bit 52, X bit 53)
        bool wcc = bits(w, 52, 1), x = bits(w, 53, 1);
        if (wcc || x) c->uses_cc = true;
        sp(&c->body, "    { ulong sum = ulong(%s%s) + ulong(0x%08xu)%s;%s\n", bits(w, 56, 1) ? "-" : "", R(a), (uint32_t)bits(w, 20, 32),
           x ? " + ulong(cc)" : "", wcc ? " cc = uint(sum >> 32) & 1u;" : "");
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = uint(sum);\n", d); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5cc0000000000000ull, 0xfff0000000000000ull) || OPM(0x4cc0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x38c0000000000000ull, 0xfef0000000000000ull)) {  // IADD3
        bool wcc3 = bits(w, 47, 1), x3 = bits(w, 48, 1);
        if (wcc3 || x3) {  // carry in/out (64-bit address math)
            c->uses_cc = true;
            sp(&c->body, "    { ulong sum = ulong(%s%s) + ulong(%s%s) + ulong(%s%s)%s;\n", bits(w, 51, 1) ? "-" : "", R(a),
               bits(w, 50, 1) ? "-" : "", B(false), bits(w, 49, 1) ? "-" : "", R((int)bits(w, 39, 8)), x3 ? " + ulong(cc)" : "");
            if (wcc3) sp(&c->body, "      cc = uint(sum >> 32) & 1u;\n");
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = uint(sum);\n", d); }
            sp(&c->body, "    }\n");
        } else {
            sprintf(t, "(%s%s + %s%s + %s%s)", bits(w, 51, 1) ? "-" : "", R(a), bits(w, 50, 1) ? "-" : "", B(false),
                    bits(w, 49, 1) ? "-" : "", R((int)bits(w, 39, 8)));
            set_reg(c, d, t);
        }
    } else if (OPM(0x5c18000000000000ull, 0xfff8000000000000ull) || OPM(0x4c18000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3818000000000000ull, 0xfef8000000000000ull)) {  // ISCADD
        sprintf(t, "((%s%s << %du) + %s%s)", bits(w, 49, 1) ? "-" : "", R(a), (int)bits(w, 39, 5), bits(w, 48, 1) ? "-" : "",
                B(false));
        if (bits(w, 47, 1)) {  // .CC: carry out for a following IADD.X (64-bit address math)
            c->uses_cc = true;
            sp(&c->body, "    { uint s = %s%s << %du; uint t2 = %s%s; uint res = s + t2; cc = (res < s) ? 1u : 0u;\n",
               bits(w, 49, 1) ? "-" : "", R(a), (int)bits(w, 39, 5), bits(w, 48, 1) ? "-" : "", B(false));
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "    }\n");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x1400000000000000ull, 0xfc00000000000000ull)) {  // ISCADD32I
        sprintf(t, "((%s << %du) + 0x%08xu)", R(a), (int)bits(w, 53, 5), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
    } else if (OPM(0x5bd0000000000000ull, 0xfff0000000000000ull) || OPM(0x4bd0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36d0000000000000ull, 0xfef8000000000000ull) || OPM(0x1800000000000000ull, 0xfc00000000000000ull)) {
        // LEA: B + (A << scale); LEA.HI (0x5bd8 reg, 0x18 cbuf): B + ((C:A) >> (32 - scale)), the high word of a
        // scaled 64-bit index. .X adds the carry in, .CC sets it (64-bit address math); a predicate result isn't modelled.
        bool hi = OPM(0x5bd8000000000000ull, 0xfff8000000000000ull) || OPM(0x1800000000000000ull, 0xfc00000000000000ull);
        bool hcb = OPM(0x1800000000000000ull, 0xfc00000000000000ull);
        int scale = hcb ? (int)bits(w, 51, 5) : hi ? (int)bits(w, 28, 5) : (int)bits(w, 39, 5);
        bool neg = hcb ? bits(w, 56, 1) : hi ? bits(w, 37, 1) : bits(w, 45, 1);
        bool x = hcb ? bits(w, 57, 1) : hi ? bits(w, 38, 1) : bits(w, 46, 1), wcc = bits(w, 47, 1);
        if (bits(w, 48, 3) != PT) { fail(c, "lea predicate result"); return; }
        char cb[256];
        const char *bv = hcb ? cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb) : B(false);
        char off[300];
        if (hi) snprintf(off, sizeof off, "uint((%s(ulong(%s) << 32 | ulong(%s))) >> %du)", neg ? "-" : "",
                         R((int)bits(w, 39, 8)), R(a), 32 - scale);
        else snprintf(off, sizeof off, "((%s%s) << %du)", neg ? "-" : "", R(a), scale);
        if (x || wcc) {
            c->uses_cc = true;
            sp(&c->body, "    { ulong sum = ulong(%s) + ulong(%s)%s;%s\n", off, bv, x ? " + ulong(cc)" : "",
               wcc ? " cc = uint(sum >> 32) & 1u;" : "");
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = uint(sum);\n", d); }
            sp(&c->body, "    }\n");
        } else {
            sprintf(t, "(%s + %s)", off, bv);
            set_reg(c, d, t);
        }
    } else if (OPM(0x5b60000000000000ull, 0xfff0000000000000ull) || OPM(0x4b60000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3660000000000000ull, 0xfef0000000000000ull)) {  // ISETP
        char cmp[512];
        icmp_expr(cmp, (int)bits(w, 49, 3), R(a), B(false), bits(w, 48, 1));
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pp = P((int)bits(w, 39, 3), bits(w, 42, 1));
        sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cmp, pp);
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5b50000000000000ull, 0xfff0000000000000ull) || OPM(0x4b50000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3650000000000000ull, 0xfef0000000000000ull)) {  // ISET
        char cmp[512];
        icmp_expr(cmp, (int)bits(w, 49, 3), R(a), B(false), bits(w, 48, 1));
        sprintf(t, "((%s) %s %s)", cmp, bool_op((int)bits(w, 45, 2)), P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 44, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0x5c20000000000000ull, 0xfff8000000000000ull) || OPM(0x4c20000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3820000000000000ull, 0xfef8000000000000ull)) {  // IMNMX
        bool sgn = bits(w, 48, 1);
        const char *x = R(a), *y = B(false), *p = P((int)bits(w, 39, 3), bits(w, 42, 1));
        if (sgn) sprintf(t, "uint(%s ? min(int(%s), int(%s)) : max(int(%s), int(%s)))", p, x, y, x, y);
        else sprintf(t, "(%s ? min(%s, %s) : max(%s, %s))", p, x, y, x, y);
        set_reg(c, d, t);
    } else if (OPM(0x5c48000000000000ull, 0xfff8000000000000ull) || OPM(0x4c48000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3848000000000000ull, 0xfef8000000000000ull)) {  // SHL
        const char *bv = B(false);
        if (bits(w, 39, 1)) sprintf(t, "(%s << (%s & 31u))", R(a), bv);
        else sprintf(t, "((%s) >= 32u ? 0u : (%s << (%s)))", bv, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c28000000000000ull, 0xfff8000000000000ull) || OPM(0x4c28000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3828000000000000ull, 0xfef8000000000000ull)) {  // SHR
        const char *bv = B(false);
        bool sgn = bits(w, 48, 1);
        if (sgn) sprintf(t, "uint(int(%s) >> min(%s, 31u))", R(a), bv);
        else sprintf(t, "((%s) >= 32u ? 0u : (%s >> (%s)))", bv, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c40000000000000ull, 0xfff8000000000000ull) || OPM(0x4c40000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3840000000000000ull, 0xfef8000000000000ull)) {  // LOP
        char x[64], y[256];
        sprintf(x, bits(w, 39, 1) ? "(~%s)" : "%s", R(a));
        sprintf(y, bits(w, 40, 1) ? "(~%s)" : "%s", B(false));
        static const char *const ops[] = {"&", "|", "^", ""};
        int op = (int)bits(w, 41, 2);
        if (op == 3) sprintf(t, "%s", y);
        else sprintf(t, "(%s %s %s)", x, ops[op], y);
        int pd = (int)bits(w, 48, 3);
        if (pd != PT) {
            c->pred_used[pd] = 1;
            int pop = (int)bits(w, 44, 2);
            sp(&c->body, "    { uint res = %s;\n", t);
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "      p%d = %s; }\n", pd, pop == 2 ? "(res == 0u)" : pop == 3 ? "(res != 0u)" : pop == 1 ? "true" : "false");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x0400000000000000ull, 0xfc00000000000000ull)) {  // LOP32I
        char x[64], y[64];
        sprintf(x, bits(w, 55, 1) ? "(~%s)" : "%s", R(a));
        sprintf(y, bits(w, 56, 1) ? "(~0x%08xu)" : "0x%08xu", (uint32_t)bits(w, 20, 32));
        static const char *const ops[] = {"&", "|", "^", ""};
        int op = (int)bits(w, 53, 2);
        if (op == 3) sprintf(t, "%s", y);
        else sprintf(t, "(%s %s %s)", x, ops[op], y);
        set_reg(c, d, t);
    } else if (OPM(0x5be0000000000000ull, 0xfff8000000000000ull) || OPM(0x3c00000000000000ull, 0xfc00000000000000ull) ||
               OPM(0x0200000000000000ull, 0xfe00000000000000ull)) {  // LOP3
        uint32_t lut;
        const char *bv, *cv;
        char bb[256];
        if ((w >> 56) == 0x5b) { lut = (uint32_t)bits(w, 28, 8); bv = R((int)bits(w, 20, 8)); cv = R((int)bits(w, 39, 8)); }
        else if ((w >> 58) == 0xf) { lut = (uint32_t)bits(w, 48, 8); sprintf(bb, "0x%08xu", (uint32_t)((uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19) | (bits(w, 56, 1) ? 0xFFF00000u : 0))); bv = bb; cv = R((int)bits(w, 39, 8)); }
        else { lut = (uint32_t)bits(w, 48, 8); bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, bb); cv = R((int)bits(w, 39, 8)); }
        char e[1024] = "0u";
        // sum of minterms
        int first = 1;
        for (int i = 0; i < 8; i++) {
            if (!(lut & (1u << i))) continue;
            char term[256];
            sprintf(term, "(%s%s & %s%s & %s%s)", (i & 4) ? "" : "~", R(a), (i & 2) ? "" : "~", bv, (i & 1) ? "" : "~", cv);
            if (first) { strcpy(e, term); first = 0; }
            else { strcat(e, " | "); strcat(e, term); }
        }
        sprintf(t, "(%s)", e);
        set_reg(c, d, t);
    } else if (OPM(0x5c00000000000000ull, 0xfff8000000000000ull) || OPM(0x4c00000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3800000000000000ull, 0xfef8000000000000ull)) {  // BFE
        const char *bv = B(false);
        bool sgn = bits(w, 48, 1);
        if (bits(w, 40, 1)) fail(c, "bfe brev");
        sprintf(t, sgn ? "bfe_s(%s, %s)" : "bfe_u(%s, %s)", R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5bf0000000000000ull, 0xfff8000000000000ull) || OPM(0x4bf0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x36f0000000000000ull, 0xfef8000000000000ull) || OPM(0x53f0000000000000ull, 0xfff8000000000000ull)) {  // BFI
        const char *bv, *cv;
        char cb[256];
        if (OPM(0x53f0000000000000ull, 0xfff8000000000000ull)) {
            bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            sprintf(t, "bfi(%s, %s, %s)", R(a), cv, bv);  // insert a into base (cbuf) with b = position/length
            // bfi with cbuf form: B = REG_39 as position/length, C = cbuf as base
            sprintf(t, "bfi(%s, %s, %s)", R(a), bv, cv);
        } else {
            bv = B(false); cv = R((int)bits(w, 39, 8));
            sprintf(t, "bfi(%s, %s, %s)", R(a), bv, cv);
        }
        set_reg(c, d, t);
    } else if (OPM(0x5b00000000000000ull, 0xffc0000000000000ull) || OPM(0x4e00000000000000ull, 0xfe00000000000000ull) ||
               OPM(0x3600000000000000ull, 0xfec0000000000000ull) || OPM(0x5100000000000000ull, 0xff80000000000000ull)) {  // XMAD
        // per Ryujinx's InstXmad* decoders and EmitXmad
        const char *bv, *cv;
        char cb[256], bb[256];
        bool hb, psl, mrg, ext, ha = bits(w, 53, 1);
        int cmode;
        uint64_t top = w >> 56;
        if (top == 0x5b) {  // reg, reg
            bv = R((int)bits(w, 20, 8)); cv = R((int)bits(w, 39, 8));
            hb = bits(w, 35, 1); psl = bits(w, 36, 1); mrg = bits(w, 37, 1); ext = bits(w, 38, 1); cmode = (int)bits(w, 50, 3);
        } else if ((top & 0xfe) == 0x4e) {  // cbuf B
            bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); cv = R((int)bits(w, 39, 8));
            hb = bits(w, 52, 1); psl = bits(w, 55, 1); mrg = bits(w, 56, 1); ext = bits(w, 54, 1); cmode = (int)bits(w, 50, 2);
        } else if ((top & 0xfe) == 0x36) {  // imm16 B
            sprintf(bb, "0x%xu", (uint32_t)bits(w, 20, 16)); bv = bb; cv = R((int)bits(w, 39, 8));
            hb = false; psl = bits(w, 36, 1); mrg = bits(w, 37, 1); ext = bits(w, 38, 1); cmode = (int)bits(w, 50, 3);
        } else {  // 0x51: B = reg39, C = cbuf
            bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            hb = bits(w, 52, 1); psl = false; mrg = false; ext = bits(w, 54, 1); cmode = (int)bits(w, 50, 2);
        }
        bool sa = bits(w, 48, 1), sb = bits(w, 49, 1), wcc = bits(w, 47, 1);
        if (ext || wcc) c->uses_cc = true;
        sp(&c->body, "    { uint xa = (%s >> %d) & 0xFFFFu; uint xb = (%s >> %d) & 0xFFFFu; uint xbu = %s;\n", R(a), ha ? 16 : 0, bv, hb ? 16 : 0, bv);
        if (sa) sp(&c->body, "      xa = uint(int(short(xa)));\n");
        if (sb) sp(&c->body, "      xb = uint(int(short(xb)));\n");
        sp(&c->body, "      uint prod = xa * xb;%s\n", psl ? " prod <<= 16;" : "");
        switch (cmode) {
        case 1: sp(&c->body, "      uint cc_ = %s & 0xFFFFu;\n", cv); break;                                  // clo
        case 2: sp(&c->body, "      uint cc_ = %s >> 16;\n", cv); break;                                      // chi
        case 3: sp(&c->body, "      uint cc_ = %s - (((xa >> 31) << 16) + ((xb >> 31) << 16));\n", cv); break;  // csfu
        case 4: sp(&c->body, "      uint cc_ = %s + (xbu << 16);\n", cv); break;                              // cbcc
        default: sp(&c->body, "      uint cc_ = %s;\n", cv);                                                  // cfull
        }
        if (ext) sp(&c->body, "      uint res = prod + (cc & 1u);\n");
        else sp(&c->body, "      uint res = prod + cc_;\n");
        if (wcc) sp(&c->body, "      cc = (res < prod) ? 1u : 0u;\n");
        if (mrg) sp(&c->body, "      res = (res & 0xFFFFu) | (xbu << 16);\n");
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5090000000000000ull, 0xfff8000000000000ull)) {  // PSETP
        const char *pa = P((int)bits(w, 12, 3), bits(w, 15, 1)), *pb = P((int)bits(w, 29, 3), bits(w, 32, 1));
        const char *pcc = P((int)bits(w, 39, 3), bits(w, 42, 1));
        int bo1 = (int)bits(w, 24, 2), bo2 = (int)bits(w, 45, 2);
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3);
        sp(&c->body, "    { bool v = (%s %s %s); bool pp = %s;\n", pa, bool_op(bo1), pb, pcc);
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = v %s pp;\n", p, bool_op(bo2)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !v %s pp;\n", q, bool_op(bo2)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5088000000000000ull, 0xfff8000000000000ull)) {  // PSET
        const char *pa = P((int)bits(w, 12, 3), bits(w, 15, 1)), *pb = P((int)bits(w, 29, 3), bits(w, 32, 1));
        sprintf(t, "(((%s %s %s)) %s %s)", pa, bool_op((int)bits(w, 24, 2)), pb, bool_op((int)bits(w, 45, 2)),
                P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 44, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0xf0c8000000000000ull, 0xfff8000000000000ull)) {  // S2R
        int sr = (int)bits(w, 20, 8);
        switch (sr) {
        case 0x12: set_reg(c, d, "0xbf800000u"); break;  // y_direction: -1 (lower-left)
        case 0x21: set_reg(c, d, "tid.x"); break;
        case 0x22: set_reg(c, d, "tid.y"); break;
        case 0x23: set_reg(c, d, "tid.z"); break;
        case 0x25: set_reg(c, d, "ctaid.x"); break;
        case 0x26: set_reg(c, d, "ctaid.y"); break;
        case 0x27: set_reg(c, d, "ctaid.z"); break;
        // lane id and the lane masks (EQ LT LE GT GE): the subgroup's (32 lanes on NVIDIA, like a Maxwell warp);
        // a vertex stage is one lane (as in VOTE / SHFL)
        case 0x00: set_reg(c, d, c->glsl && c->stage != STAGE_VERTEX ? "gl_SubgroupInvocationID" : "0u"); break;
        case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c:
            if (c->glsl && c->stage != STAGE_VERTEX) {
                static const char *const m[] = {"gl_SubgroupEqMask.x", "gl_SubgroupLtMask.x", "gl_SubgroupLeMask.x",
                                                "gl_SubgroupGtMask.x", "gl_SubgroupGeMask.x"};
                set_reg(c, d, m[sr - 0x38]);
            } else {
                static const char *const m[] = {"1u", "0u", "1u", "0u", "1u"};
                set_reg(c, d, m[sr - 0x38]);
            }
            break;
        default: sprintf(t, "0u /* sr %#x */", sr); set_reg(c, d, t);
        }
    } else if (OPM(0x5c30000000000000ull, 0xfff8000000000000ull) || OPM(0x4c30000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3830000000000000ull, 0xfef8000000000000ull)) {  // FLO
        const char *bv = B(false);
        sprintf(t, bits(w, 48, 1) ? "(int(%s) < 0 ? (31u - clz(~%s)) : (31u - clz(%s)))" : "(31u - clz(%s))", bv, bv, bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c08000000000000ull, 0xfff8000000000000ull) || OPM(0x4c08000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3808000000000000ull, 0xfef8000000000000ull)) {  // POPC
        sprintf(t, bits(w, 40, 1) ? "popcount(~%s)" : "popcount(%s)", B(false));
        set_reg(c, d, t);
    } else if (OPM(0x5c38000000000000ull, 0xfff8000000000000ull) || OPM(0x4c38000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3838000000000000ull, 0xfef8000000000000ull)) {  // IMUL
        if (bits(w, 39, 1)) sprintf(t, bits(w, 40, 1) ? "uint(mulhi(int(%s), int(%s)))" : "mulhi(%s, %s)", R(a), B(false));
        else sprintf(t, "(%s * %s)", R(a), B(false));
        set_reg(c, d, t);
    } else if (OPM(0x1f00000000000000ull, 0xff00000000000000ull)) {  // IMUL32I
        sprintf(t, "(%s * 0x%08xu)", R(a), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
    } else if (OPM(0x5ce0000000000000ull, 0xfff8000000000000ull) || OPM(0x4ce0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38e0000000000000ull, 0xfef8000000000000ull)) {  // I2I
        int dt = (int)((bits(w, 8, 2)) | (bits(w, 12, 1) << 2)), st = (int)((bits(w, 10, 2)) | (bits(w, 13, 1) << 2));
        int sel = (int)bits(w, 41, 2);
        char src[256];
        const char *bv = B(false);
        int ssz = st & 3;
        bool ssg = st & 4;
        if (ssz == 0) sprintf(src, ssg ? "int(char((%s >> %d) & 0xFFu))" : "int((%s >> %d) & 0xFFu)", bv, sel * 8);
        else if (ssz == 1) sprintf(src, ssg ? "int(short((%s >> %d) & 0xFFFFu))" : "int((%s >> %d) & 0xFFFFu)", bv, sel * 8);
        else sprintf(src, "int(%s)", bv);
        if (bits(w, 49, 1)) { sprintf(u, "abs(%s)", src); strcpy(src, u); }
        if (bits(w, 45, 1)) { sprintf(u, "(-%s)", src); strcpy(src, u); }
        int dsz = dt & 3;
        bool dsg = dt & 4, sat = bits(w, 50, 1);
        if (sat && !ssg) {  // .SAT from an unsigned source: compare unsigned (a U32 value above 0x7FFFFFFF isn't negative)
            static const char *const hi[2][3] = {{"0xFFu", "0xFFFFu", ""}, {"0x7Fu", "0x7FFFu", "0x7FFFFFFFu"}};
            if (dsz < 2 || dsg) sprintf(t, "min(uint(%s), %s)", src, hi[dsg][dsz < 2 ? dsz : 2]);
            else sprintf(t, "uint(%s)", src);
        } else if (dsz == 0) sprintf(t, sat ? (dsg ? "uint(clamp(%s, -128, 127))" : "uint(clamp(%s, 0, 255))") : "(uint(%s) & 0xFFu)", src);
        else if (dsz == 1) sprintf(t, sat ? (dsg ? "uint(clamp(%s, -32768, 32767))" : "uint(clamp(%s, 0, 65535))") : "(uint(%s) & 0xFFFFu)", src);
        else if (sat && !dsg) sprintf(t, "uint(max(%s, 0))", src);  // S32 -> U32 .SAT: negatives become 0
        else sprintf(t, "uint(%s)", src);
        set_reg(c, d, t);
        if (bits(w, 47, 1)) {  // .CC: zero/sign flags of the result
            c->uses_cc = true;
            sp(&c->body, "    { uint ccv = %s; ccz = ccv == 0u; ccs = int(ccv) < 0; }\n", t);
        }
    } else if (OPM(0x5bc0000000000000ull, 0xfff0000000000000ull) || OPM(0x4bc0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36c0000000000000ull, 0xfef0000000000000ull) || OPM(0x53c0000000000000ull, 0xfff0000000000000ull)) {  // PRMT
        const char *bv, *cv;
        char cb[256];
        if (OPM(0x53c0000000000000ull, 0xfff0000000000000ull)) { bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); }
        else { bv = B(false); cv = R((int)bits(w, 39, 8)); }
        if (bits(w, 48, 4)) fail(c, "prmt mode");
        sprintf(t, "prmt(%s, %s, %s)", R(a), cv, bv);
        set_reg(c, d, t);
    } else if (OPM(0x5cf8000000000000ull, 0xfff8000000000000ull) || OPM(0x38f8000000000000ull, 0xfef8000000000000ull) ||
               OPM(0x5bf8000000000000ull, 0xfff8000000000000ull) || OPM(0x36f8000000000000ull, 0xfef8000000000000ull)) {  // SHF
        // funnel shift of hi:lo (C:A). .W (bit 50) wraps the shift amount, otherwise it's clamped to the width;
        // bits 37-38: 2 .U64 / 3 .S64 shift the full 64 bits (width 64), else 32. The immediate is 6 bits (20-25:
        // bits 37-38 of the 19-bit B immediate are the width field). .CC / .X / .HI (47-49) aren't modelled.
        bool right = (w >> 56) == 0x5c || (w >> 57) == (0x38 >> 1);
        if (bits(w, 47, 3)) { fail(c, "shf cc/x/hi"); return; }
        char shb[16];
        const char *sh = ((w >> 60) == 3) ? (sprintf(shb, "%uu", (unsigned)bits(w, 20, 6)), shb) : R((int)bits(w, 20, 8));
        const char *hi = R((int)bits(w, 39, 8));
        int width = bits(w, 38, 1) ? 64 : 32;
        bool s64 = bits(w, 37, 2) == 3, wrap = bits(w, 50, 1);
        sp(&c->body, wrap ? "    { uint s = %s & %du; ulong v = (ulong(%s) << 32) | ulong(%s);\n"
                          : "    { uint s = min(%s, %du); ulong v = (ulong(%s) << 32) | ulong(%s);\n",
           sh, wrap ? width - 1 : width, hi, R(a));
        if (right && width == 64)
            sprintf(t, "s >= 64u ? %s : uint(%s >> s)", s64 ? "uint(int(v >> 32) >> 31)" : "0u", s64 ? "long(v)" : "v");
        else if (right) sprintf(t, "uint(v >> s)");
        else if (width == 64) sprintf(t, "s >= 64u ? 0u : uint((v << s) >> 32)");
        else sprintf(t, "uint((v << s) >> 32)");
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = %s;\n", d, t); }
        sp(&c->body, "    }\n");
    } else if (OPM(0xeed0000000000000ull, 0xfff8000000000000ull) || OPM(0xeed8000000000000ull, 0xfff8000000000000ull)) {  // LDG / STG
        bool store = bits(w, 51, 1);
        bool wide = bits(w, 45, 1);
        int size = (int)bits(w, 48, 3);
        int64_t off = sbits(w, 20, 24);
        c->info->uses_gmem = true;
        char addr[256];
        if (a == RZ) sprintf(addr, "ulong(%lld)", (long long)off);
        else if (wide) sprintf(addr, "((ulong(%s) << 32 | ulong(%s)) + ulong(%lld))", R(a + 1), R(a), (long long)off);
        else sprintf(addr, "(ulong(%s) + ulong(%lld))", R(a), (long long)off);
        sp(&c->body, "    { ulong ga = %s - 0x%llxul; bool ok = ga < 0x%llxul;\n", addr,
           (unsigned long long)maxwell_gmem_base, (unsigned long long)maxwell_gmem_size);
        static const char *const ld_small[] = {"uint(gmem[ga])", "uint(int(char(gmem[ga])))",
                                               "uint(*(device ushort *)(gmem + ga))", "uint(int(*(device short *)(gmem + ga)))"};
        int nw = size == 5 ? 2 : size >= 6 ? 4 : 1;
        bool lazy_wide = c->glsl && maxwell_gmem_chunk_shift && maxwell_gmem_lazy && nw > 1;  // (one chunk check per access)
        if (c->glsl && c->stage == STAGE_COMPUTE) {
            // Compute (Vulkan): the guest heap is host memory across PCIe on a discrete GPU, and a shader storing to
            // it in a loop crawls (the little chain chomps in Madam MeowMeow's house: ~4 ms a dispatch, ~0.07 ms without
            // the heap stores). The dispatch's storage buffers are copied to a device-local buffer (gfast_, address
            // table gmap: exec.c gfast_prepare) and back after it: loads and stores inside them use that copy.
            // Anything outside the table goes to the heap as before.
            c->info->uses_gmem_map = true;
            static const char *const gl_small[] = {
                "(%s >> (uint(ga & 3ul) * 8u)) & 0xFFu", "uint(sx8(%s >> (uint(ga & 3ul) * 8u)))",
                "(%s >> (uint(ga & 2ul) * 8u)) & 0xFFFFu", "uint(sx16(%s >> (uint(ga & 2ul) * 8u)))"};
            sp(&c->body, "      uint fw = ok ? %s(uint(ga)) : 0xFFFFFFFFu;\n", store ? "gfindw" : "gfind");  // (a store: flags its range)
            if (!store) {
                if (size < 4) {
                    if (d != RZ) {
                        char fast[160], slow[160];
                        snprintf(fast, sizeof fast, gl_small[size], "gfast_.w[fw]");
                        snprintf(slow, sizeof slow, gl_small[size], "GMR(uint(ga >> 2))");
                        c->reg_used[d] = 1;
                        sp(&c->body, "      r%d = fw != 0xFFFFFFFFu ? %s : ok ? %s : 0u;\n", d, fast, slow);
                    }
                } else if (lazy_wide) {
                    sp(&c->body, "      { uvec%d t = fw != 0xFFFFFFFFu ? uvec%d(", nw, nw);
                    for (int k = 0; k < nw; k++) sp(&c->body, "%sgfast_.w[fw + %du]", k ? ", " : "", k);
                    sp(&c->body, ") : ok ? GMR%d(uint(ga >> 2)) : uvec%d(0u);\n", nw, nw);
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "        r%d = t[%d];\n", d + k, k); }
                    sp(&c->body, "      }\n");
                } else {
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) {
                            c->reg_used[d + k] = 1;
                            sp(&c->body, "      r%d = fw != 0xFFFFFFFFu ? gfast_.w[fw + %du] : ok ? GMR(uint(ga >> 2) + %du) : 0u;\n",
                               d + k, k, k);
                        }
                }
            } else {
                c->uses_gmem_store = true;
                c->info->uses_gmem_store = true;
                if (size < 4) {
                    uint32_t m = size < 2 ? 0xFFu : 0xFFFFu;
                    const char *bm = size < 2 ? "3ul" : "2ul";
                    sp(&c->body, "      if (ok) { uint sh = uint(ga & %s) * 8u; uint v = (%s & 0x%xu) << sh;\n", bm, R(d), m);
                    sp(&c->body, "        if (fw != 0xFFFFFFFFu) { atomicAnd(gfast_.w[fw], ~(0x%xu << sh)); atomicOr(gfast_.w[fw], v); }\n", m);
                    sp(&c->body, "        else { GMAND(uint(ga >> 2), ~(0x%xu << sh)); GMOR(uint(ga >> 2), v); } }\n", m);
                } else if (lazy_wide) {
                    for (int k = 0; k < nw; k++) sp(&c->body, "      if (fw != 0xFFFFFFFFu) gfast_.w[fw + %du] = %s;\n", k, R(d == RZ ? RZ : d + k));
                    sp(&c->body, "      if (fw == 0xFFFFFFFFu && ok) GMW%d(uint(ga >> 2), uvec%d(", nw, nw);
                    for (int k = 0; k < nw; k++) sp(&c->body, "%s%s", k ? ", " : "", R(d == RZ ? RZ : d + k));
                    sp(&c->body, "));\n");
                } else {
                    for (int k = 0; k < nw; k++)
                        sp(&c->body, "      if (fw != 0xFFFFFFFFu) gfast_.w[fw + %du] = %s; else if (ok) GMW(uint(ga >> 2) + %du, %s);\n", k,
                           R(d == RZ ? RZ : d + k), k, R(d == RZ ? RZ : d + k));
                }
            }
        } else if (c->glsl) {  // 32-bit words (GMR / GMW / GMAND / GMOR: the prelude); sub-word accesses within the containing word
            static const char *const gl_small[] = {
                "(GMR(uint(ga >> 2)) >> (uint(ga & 3ul) * 8u)) & 0xFFu",
                "uint(sx8(GMR(uint(ga >> 2)) >> (uint(ga & 3ul) * 8u)))",
                "(GMR(uint(ga >> 2)) >> (uint(ga & 2ul) * 8u)) & 0xFFFFu",
                "uint(sx16(GMR(uint(ga >> 2)) >> (uint(ga & 2ul) * 8u)))"};
            if (!store) {
                if (size < 4) {
                    if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      if (ok) r%d = %s; else r%d = 0u;\n", d, gl_small[size], d); }
                } else if (lazy_wide) {  // (one bounds check and address for the whole aligned access)
                    sp(&c->body, "      { uvec%d t = ok ? GMR%d(uint(ga >> 2)) : uvec%d(0u);\n", nw, nw, nw);
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "        r%d = t[%d];\n", d + k, k); }
                    sp(&c->body, "      }\n");
                } else {
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) {
                            c->reg_used[d + k] = 1;
                            sp(&c->body, "      if (ok) r%d = GMR(uint(ga >> 2) + %du); else r%d = 0u;\n", d + k, k, d + k);
                        }
                }
            } else {
                c->uses_gmem_store = true;
                c->info->uses_gmem_store = true;  // (the draw marks the pages it stores to: vertex cache copies go stale)
                if (size < 4)
                    sp(&c->body, "      if (ok) { uint wi = uint(ga >> 2); uint sh = uint(ga & %s) * 8u; GMAND(wi, ~(0x%xu << sh)); "
                       "GMOR(wi, (%s & 0x%xu) << sh); }\n", size < 2 ? "3ul" : "2ul", size < 2 ? 0xFFu : 0xFFFFu, R(d),
                       size < 2 ? 0xFFu : 0xFFFFu);
                else if (lazy_wide) {
                    sp(&c->body, "      if (ok) GMW%d(uint(ga >> 2), uvec%d(", nw, nw);
                    for (int k = 0; k < nw; k++) sp(&c->body, "%s%s", k ? ", " : "", R(d == RZ ? RZ : d + k));
                    sp(&c->body, "));\n");
                } else
                    for (int k = 0; k < nw; k++)
                        sp(&c->body, "      if (ok) GMW(uint(ga >> 2) + %du, %s);\n", k, R(d == RZ ? RZ : d + k));
            }
        } else if (!store) {
            if (size < 4) {
                if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = ok ? %s : 0u;\n", d, ld_small[size]); }
            } else {
                for (int k = 0; k < nw; k++)
                    if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "      r%d = ok ? *(device uint *)(gmem + ga + %du) : 0u;\n", d + k, k * 4); }
            }
        } else {
            if (size == 0 || size == 1) sp(&c->body, "      if (ok) gmem[ga] = uchar(%s);\n", R(d));
            else if (size == 2 || size == 3) sp(&c->body, "      if (ok) *(device ushort *)(gmem + ga) = ushort(%s);\n", R(d));
            else
                for (int k = 0; k < nw; k++)
                    sp(&c->body, "      if (ok) *(device uint *)(gmem + ga + %du) = %s;\n", k * 4, R(d == RZ ? RZ : d + k));
        }
        sp(&c->body, "    }\n");
    } else if (emit_half(c, w)) {
    } else if (OPM(0x50f8000000000000ull, 0xfff8000000000000ull)) {  // FSWZADD
        // per quad lane (laneid & 3), 2 bits of the swizzle pick: a+b, b-a, a-b, b
        uint32_t swz = (uint32_t)bits(w, 28, 8);
        if (c->stage != STAGE_FRAGMENT) {
            sprintf(t, "as_type<uint>(%s + %s)", F(R(a)), F(R((int)bits(w, 20, 8))));
        } else {
            c->uses_lane = true;
            sprintf(t, "as_type<uint>(fswzadd(%s, %s, 0x%02xu, lane))", F(R(a)), F(R((int)bits(w, 20, 8))), swz);
        }
        set_reg(c, d, t);
    } else if (OPM(0x5b40000000000000ull, 0xfff0000000000000ull) || OPM(0x4b40000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3640000000000000ull, 0xfef0000000000000ull) || OPM(0x5340000000000000ull, 0xfff0000000000000ull)) {  // ICMP
        char cmp[512], cb[256], bb[256];
        const char *bv, *cv;
        if (OPM(0x5340000000000000ull, 0xfff0000000000000ull)) {
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
        } else {
            bv = operand_b(c, w, false, bb);
            cv = R((int)bits(w, 39, 8));
        }
        icmp_expr(cmp, (int)bits(w, 49, 3), cv, "0u", bits(w, 48, 1));
        sprintf(t, "(%s ? %s : %s)", cmp, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0xed00000000000000ull, 0xff00000000000000ull) || OPM(0xebf8000000000000ull, 0xfff8000000000000ull)) {
        // ATOM: atomic on global memory at Ra (+ Ra+1 << 32 with .E, bit 48) + imm20 (bits 28-47, signed); size at 49
        // (U32 only here), op at 52 (ADD MIN MAX INC DEC AND OR XOR EXCH); d = the old value. Compute (GLSL) only: the
        // dispatch's storage buffers are in gfast_ (exec.c gfast_prepare), where it is a real atomic; outside them the
        // heap word is read, changed and written back (not atomic)
        // RED: the same without a result: the value is Rd's field (bits 0-7), size at 20 (U32, S32: the same for ADD and
        // the bit ops), op at 23
        bool red = OPM(0xebf8000000000000ull, 0xfff8000000000000ull);
        int size = red ? (int)bits(w, 20, 3) : (int)bits(w, 49, 3), op = red ? (int)bits(w, 23, 3) : (int)bits(w, 52, 4);
        int b = red ? d : (int)bits(w, 20, 8);
        if (red) {
            if (size == 1 && (op == 0 || op >= 5)) size = 0;
            d = RZ;
        }
        int64_t off = sbits(w, 28, 20);
        static const char *const fn[] = {"atomicAdd", "atomicMin", "atomicMax", NULL, NULL, "atomicAnd", "atomicOr",
                                         "atomicXor", "atomicExchange"};
        static const char *const slow[] = {"o_ + bv_", "min(o_, bv_)", "max(o_, bv_)", NULL, NULL, "o_ & bv_", "o_ | bv_",
                                           "o_ ^ bv_", "bv_"};
        if (!c->glsl || c->stage != STAGE_COMPUTE) fail(c, "ATOM outside a GLSL compute shader");
        else if (size != 0 || op > 8 || !fn[op]) fail(c, "ATOM size %d op %d", size, op);
        else {
            c->info->uses_gmem = c->info->uses_gmem_map = true;
            c->uses_gmem_store = c->info->uses_gmem_store = true;
            char addr[256];
            if (a == RZ) sprintf(addr, "ulong(%lld)", (long long)off);
            else if (bits(w, 48, 1)) sprintf(addr, "((ulong(%s) << 32 | ulong(%s)) + ulong(%lld))", R(a + 1), R(a), (long long)off);
            else sprintf(addr, "(ulong(%s) + ulong(%lld))", R(a), (long long)off);
            sp(&c->body, "    { ulong ga = %s - 0x%llxul; bool ok = ga < 0x%llxul; uint bv_ = %s, o_ = 0u;\n", addr,
               (unsigned long long)maxwell_gmem_base, (unsigned long long)maxwell_gmem_size, R(b));
            sp(&c->body, "      uint fw = ok ? gfindw(uint(ga)) : 0xFFFFFFFFu;\n");
            sp(&c->body, "      if (fw != 0xFFFFFFFFu) o_ = %s(gfast_.w[fw], bv_);\n", fn[op]);
            sp(&c->body, "      else if (ok) { o_ = GMR(uint(ga >> 2)); GMW(uint(ga >> 2), %s); }\n", slow[op]);
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = o_;\n", d); }
            sp(&c->body, "    }\n");
        }
    } else if (OPM(0xec00000000000000ull, 0xff00000000000000ull)) {
        // ATOMS: atomic on the workgroup's shared memory, word index (Ra >> 2) + imm22 (bits 30-51); size at 28 (U32,
        // S32, U64, S64), op at 52 (ADD MIN MAX INC DEC AND OR XOR EXCH); d = the old value
        int size = (int)bits(w, 28, 2), op = (int)bits(w, 52, 4), b = (int)bits(w, 20, 8);
        uint32_t imm = (uint32_t)bits(w, 30, 22);
        if (!c->glsl || c->stage != STAGE_COMPUTE) fail(c, "ATOMS outside a GLSL compute shader");
        if (size >= 2 || op > 8) fail(c, "ATOMS size %d op %d", size, op);
        c->uses_shared = true;
        char ix[128];
        if (a == RZ) sprintf(ix, "%uu", imm);
        else sprintf(ix, "((%s >> 2) + %uu)", R(a), imm);
        static const char *const fn[] = {"atomicAdd", "atomicMin", "atomicMax", NULL, NULL, "atomicAnd", "atomicOr",
                                         "atomicXor", "atomicExchange"};
        char e[256];
        const char *bv = R(b);
        if (op == 3 || op == 4) {  // INC / DEC wrap at b: a compare-and-swap loop
            sp(&c->body, "    { uint ai = min(%s, smem_words_ - 1u), bv = %s, o = smem[ai], n;\n"
                         "      for (;;) { n = %s; uint p = atomicCompSwap(smem[ai], o, n); if (p == o) break; o = p; }\n",
               ix, bv, op == 3 ? "o >= bv ? 0u : o + 1u" : "(o == 0u || o > bv) ? bv : o - 1u");
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = o;\n", d); }
            sp(&c->body, "    }\n");
        } else {
            if (size == 1 && (op == 1 || op == 2)) fail(c, "ATOMS signed min/max");  // (smem is uint: not seen yet)
            sprintf(e, "%s(smem[min(%s, smem_words_ - 1u)], %s)", fn[op], ix, bv);
            if (d != RZ) set_reg(c, d, e);
            else sp(&c->body, "    %s;\n", e);
        }
    } else if (OPM(0xef40000000000000ull, 0xffe8000000000000ull) || OPM(0xef48000000000000ull, 0xffe8000000000000ull)) {
        // LDL / STL: thread-private local memory; LDS / STS: the workgroup's shared memory (compute). Both modelled
        // as word arrays (byte/short accesses within a word); the encodings differ only in bit 51 (shared) and 52 (store)
        bool store = bits(w, 52, 1), shared = bits(w, 51, 1);
        int size = (int)bits(w, 48, 3);
        int32_t off = (int32_t)sbits(w, 20, 24);
        if (shared && (!c->glsl || c->stage != STAGE_COMPUTE)) fail(c, "shared memory outside a GLSL compute shader");
        if (shared) c->uses_shared = true;
        else c->uses_local = true;
        const char *mem = shared ? "smem" : "lmem";
        char ixf[64];  // word index wrapper: local memory masks (power of two), shared memory clamps
        if (shared) strcpy(ixf, "min(%s, smem_words_ - 1u)");
        else sprintf(ixf, "(%%s) & %du", LMEM_WORDS - 1);
        char addr[128];
        if (a == RZ) sprintf(addr, "%du", off);
        else sprintf(addr, "(%s + %du)", R(a), (uint32_t)off);
        int nw = size == 5 ? 2 : size >= 6 ? 4 : 1;
        char wi[96], tmp[64];
        sp(&c->body, "    { uint la = %s;\n", addr);
        if (!store) {
            if (size < 4) {
                static const char *const ext[] = {"(v >> sh) & 0xFFu", "uint(int(char((v >> sh) & 0xFFu)))", "(v >> sh) & 0xFFFFu",
                                                  "uint(int(short((v >> sh) & 0xFFFFu)))"};
                snprintf(wi, sizeof wi, ixf, "la >> 2");
                if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      uint v = %s[%s]; uint sh = (la & 3u) * 8u; r%d = %s;\n", mem, wi, d, ext[size]); }
            } else {
                for (int k = 0; k < nw; k++) {
                    snprintf(tmp, sizeof tmp, "(la >> 2) + %du", k);
                    snprintf(wi, sizeof wi, ixf, tmp);
                    if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "      r%d = %s[%s];\n", d + k, mem, wi); }
                }
            }
        } else {
            if (size < 4) {
                uint32_t m = size < 2 ? 0xFFu : 0xFFFFu;
                snprintf(wi, sizeof wi, ixf, "la >> 2");
                sp(&c->body, "      uint sh = (la & 3u) * 8u; uint i = %s; %s[i] = (%s[i] & ~(0x%xu << sh)) | ((%s & 0x%xu) << sh);\n",
                   wi, mem, mem, m, R(d), m);
            } else {
                for (int k = 0; k < nw; k++) {
                    snprintf(tmp, sizeof tmp, "(la >> 2) + %du", k);
                    snprintf(wi, sizeof wi, ixf, tmp);
                    sp(&c->body, "      %s[%s] = %s;\n", mem, wi, R(d == RZ ? RZ : d + k));
                }
            }
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0xef10000000000000ull, 0xfff8000000000000ull)) {  // SHFL
        int mode = (int)bits(w, 30, 2);
        char bb[32];
        const char *bv = bits(w, 28, 1) ? (sprintf(bb, "%uu", (unsigned)bits(w, 20, 5)), bb) : R((int)bits(w, 20, 8));
        int pd = (int)bits(w, 48, 3);
        if (c->stage == STAGE_VERTEX) {
            set_reg(c, d, R(a));  // no SIMD-group functions in vertex functions: lane-local value
        } else if (c->glsl) {  // a quad xor (lane mask < 4) is the subgroup xor within the quad
            static const char *const fn[] = {"subgroupShuffle", "subgroupShuffleUp", "subgroupShuffleDown", "subgroupShuffleXor"};
            sprintf(t, "%s(%s, uint(%s))", fn[mode], R(a), bv);
            set_reg(c, d, t);
        } else if (mode == 3) {
            bool quad = bits(w, 28, 1) && bits(w, 20, 5) < 4;
            sprintf(t, quad ? "quad_shuffle_xor(%s, ushort(%s))" : "simd_shuffle_xor(%s, ushort(%s))", R(a), bv);
            set_reg(c, d, t);
        } else {
            static const char *const fn[] = {"simd_shuffle", "simd_shuffle_up", "simd_shuffle_down"};
            sprintf(t, "%s(%s, ushort(%s))", fn[mode], R(a), bv);
            set_reg(c, d, t);
        }
        if (pd != PT) { c->pred_used[pd] = 1; sp(&c->body, "    p%d = true;\n", pd); }
    } else if (OPM(0x50d8000000000000ull, 0xfff8000000000000ull)) {  // VOTE (MUA3's compute shaders: VOTE.ALL R, PT, PT)
        // d = the ballot of the source predicate (the active lanes for PT); the predicate destination at 45 = .ALL /
        // .ANY / .EQ of it. Maxwell's warp is 32 lanes, as NVIDIA's subgroups (.x of the ballot). Vertex stages: one lane.
        int mode = (int)bits(w, 48, 2), vp = (int)bits(w, 45, 3);
        const char *src = P((int)bits(w, 39, 3), bits(w, 42, 1));
        if (mode == 3) { fail(c, "vote mode 3"); }
        else if (!c->glsl) { fail(c, "vote (msl)"); }
        else {
            bool lane = c->stage == STAGE_VERTEX;
            sp(&c->body, "    { bool vs_ = %s;\n", src);
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, lane ? "      r%d = vs_ ? 1u : 0u;\n" : "      r%d = subgroupBallot(vs_).x;\n", d); }
            if (vp != PT) {
                static const char *const fn[] = {"subgroupAll(vs_)", "subgroupAny(vs_)", "subgroupAllEqual(vs_)"};
                c->pred_used[vp] = 1;
                sp(&c->body, "      p%d = %s;\n", vp, lane ? (mode == 2 ? "true" : "vs_") : fn[mode]);
            }
            sp(&c->body, "    }\n");
        }
    } else if (OPM(0x38f0000000000000ull, 0xfef8000000000000ull) || OPM(0x5cf0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x4cf0000000000000ull, 0xfff8000000000000ull)) {  // R2P
        if (bits(w, 40, 1)) { fail(c, "r2p cc"); }
        int shift = (int)bits(w, 41, 2) * 8;
        const char *mask = B(false);
        for (int i = 0; i < 7; i++) {
            c->pred_used[i] = 1;
            sp(&c->body, c->glsl ? "    if (((%s >> %du) & 1u) != 0u) p%d = ((%s >> %du) & 1u) != 0u;\n"
                                 : "    if ((%s >> %du) & 1u) p%d = ((%s >> %du) & 1u) != 0u;\n", mask, i, i, R(a), shift + i);
        }
    } else if (OPM(0x38e8000000000000ull, 0xfef8000000000000ull) || OPM(0x5ce8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x4ce8000000000000ull, 0xfff8000000000000ull)) {  // P2R
        int shift = (int)bits(w, 41, 2) * 8;
        const char *mask = B(false);
        if (bits(w, 40, 1)) {  // .CC: the flags Z, S, C, O as bits 0-3 (overflow isn't tracked: 0)
            c->uses_cc = true;
            sp(&c->body, "    { uint pr = (ccz ? 1u : 0u) | (ccs ? 2u : 0u) | ((cc & 1u) << 2);\n");
            sp(&c->body, "      uint m = (%s & 0xFu) << %du;\n", mask, shift);
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = (%s & ~m) | ((pr << %du) & m);\n", d, R(a), shift); }
            sp(&c->body, "    }\n");
            goto p2r_done;
        }
        sp(&c->body, "    { uint pr = 0u;\n");
        for (int i = 0; i < 7; i++) { c->pred_used[i] = 1; sp(&c->body, "      pr |= (p%d ? 1u : 0u) << %du;\n", i, i); }
        sp(&c->body, "      uint m = (%s & 0x7Fu) << %du;\n", mask, shift);
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = (%s & ~m) | ((pr << %du) & m);\n", d, R(a), shift); }
        sp(&c->body, "    }\n");
    p2r_done:;
    } else if (OPM(0xe250000000000000ull, 0xfff0000000000020ull)) {  // BRX reg + imm (relative)
        // computed targets can name a scheduling word; execution continues at the next slot (as for BRA)
        if (c->structured) {  // (each target of the table: structure_flow; any other: the end, as the dispatcher's default)
            size_t vi = pc / 8;  // (expand_calls: the table and the targets in the original program, mapped)
            uint32_t opc = c->vorig ? c->vorig[vi] * 8 : pc;
            const uint8_t *oc = c->vorig ? c->ocode : c->code;
            size_t on = c->vorig ? c->oend : c->ninsts;
            sp(&c->body, "    { uint t = %uu + %s + 0x%08xu; if (((t >> 3) & 3u) == 0u) t += 8u;\n", opc + 8, R(a),
               (uint32_t)sbits(w, 20, 24));
            uint8_t *mark = calloc(on + 4, 1);
            if (mark && brx_table_labels(oc, opc / 8, on, c->blob, c->blob_size, mark, true))
                for (size_t k = 0; k < on; k++)
                    if (mark[k]) {
                        size_t nk = c->vorig ? vmap(c, c->vctx[vi], k) : k;
                        sp(&c->body, "    if (t == %zuu) {\n", k * 8);
                        structure_goto(c, vi, nk == SIZE_MAX ? c->ninsts : nk);
                        sp(&c->body, "    }\n");
                    }
            free(mark);
            structure_exit(c);
            sp(&c->body, "    }\n");
        } else
            sp(&c->body, "    { uint t = %uu + %s + 0x%08xu; if (((t >> 3) & 3u) == 0u) t += 8u; pc = t; continue; }\n", pc + 8, R(a),
               (uint32_t)sbits(w, 20, 24));
    } else if (OPM(0x50a0000000000000ull, 0xfff8000000000000ull)) {  // CSETP (zero/sign flags only)
        int test = (int)bits(w, 8, 5);
        const char *cond;
        switch (test) {
        case 0: cond = "false"; break;
        case 15: cond = "true"; break;
        case 2: case 10: cond = "ccz"; break;
        case 5: case 13: cond = "!ccz"; break;
        case 1: case 9: cond = "ccs"; break;
        case 6: case 14: cond = "!ccs"; break;
        case 4: case 12: cond = "(!ccz && !ccs)"; break;
        case 3: case 11: cond = "(ccz || ccs)"; break;
        default: cond = "false"; fail(c, "csetp test %d", test);
        }
        c->uses_cc = true;
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pc_ = P((int)bits(w, 39, 3), bits(w, 42, 1));
        sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cond, pc_);
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5c80000000000000ull, 0xfff8000000000000ull) || OPM(0x4c80000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3880000000000000ull, 0xfef8000000000000ull) || OPM(0x5c70000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x4c70000000000000ull, 0xfff8000000000000ull) || OPM(0x3870000000000000ull, 0xfef8000000000000ull) ||
               OPM(0x5b70000000000000ull, 0xfff0000000000000ull) || OPM(0x4b70000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3670000000000000ull, 0xfef0000000000000ull) || OPM(0x5370000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x5b80000000000000ull, 0xfff0000000000000ull) || OPM(0x4b80000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3680000000000000ull, 0xfef0000000000000ull)) {
        // DMUL (-A at 48), DADD (|A| 46, -A 48, |B| 49, -B 45), DFMA (A * -B (48) + -C (49), C = R39; 0x53: B = R39,
        // C = cbuf), DSETP (as FSETP): doubles in register pairs. The rounding modes and .CC aren't modelled.
        if (!c->glsl) { fail(c, "double precision (MSL)"); return; }
        if (OPM(0x5c80000000000000ull, 0xfff8000000000000ull) || OPM(0x4c80000000000000ull, 0xfff8000000000000ull) ||
            OPM(0x3880000000000000ull, 0xfef8000000000000ull)) {
            sprintf(t, "(%s * %s)", NA(DR(a), bits(w, 48, 1), false), DB());
            set_dreg(c, d, t);
        } else if (OPM(0x5c70000000000000ull, 0xfff8000000000000ull) || OPM(0x4c70000000000000ull, 0xfff8000000000000ull) ||
                   OPM(0x3870000000000000ull, 0xfef8000000000000ull)) {
            sprintf(t, "(%s + %s)", NA(DR(a), bits(w, 48, 1), bits(w, 46, 1)), NA(DB(), bits(w, 45, 1), bits(w, 49, 1)));
            set_dreg(c, d, t);
        } else if (OPM(0x5b80000000000000ull, 0xfff0000000000000ull) || OPM(0x4b80000000000000ull, 0xfff0000000000000ull) ||
                   OPM(0x3680000000000000ull, 0xfef0000000000000ull)) {
            const char *x = NA(DR(a), bits(w, 43, 1), bits(w, 7, 1)), *y = NA(DB(), bits(w, 6, 1), bits(w, 44, 1));
            char cmp[1400];
            fcmp_expr(cmp, (int)bits(w, 48, 4), x, y);
            int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
            const char *pc_ = P((int)bits(w, 39, 3), bits(w, 42, 1));
            sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cmp, pc_);
            if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo)); }
            if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
            sp(&c->body, "    }\n");
        } else {  // DFMA
            const char *bv, *cv;
            char cb[600];
            if (OPM(0x5370000000000000ull, 0xfff0000000000000ull)) {
                bv = DR((int)bits(w, 39, 8));
                int ix = (int)bits(w, 34, 5);
                uint32_t off = (uint32_t)bits(w, 20, 14) * 4;
                char lo[256], hi[256];
                sprintf(cb, "packDouble2x32(uvec2(%s, %s))", cbuf_ref(c, ix, off & ~7u, NULL, lo), cbuf_ref(c, ix, (off & ~7u) + 4, NULL, hi));
                cv = cb;
            } else {
                bv = DB();
                cv = DR((int)bits(w, 39, 8));
            }
            sprintf(t, "fma(%s, %s, %s)", DR(a), NA(bv, bits(w, 48, 1), false), NA(cv, bits(w, 49, 1), false));
            set_dreg(c, d, t);
        }
    } else if (OPM(0x5098000000000000ull, 0xfff8000000000000ull)) {  // CSET
        fail(c, "cset");
    } else if (OPM(0xf0a8000000000000ull, 0xfff8000000000000ull)) {  // BAR
        if (!c->glsl) sp(&c->body, "    threadgroup_barrier(mem_flags::mem_threadgroup);\n");
        else if (c->stage == STAGE_COMPUTE) sp(&c->body, "    barrier();\n");
        else sp(&c->body, "    /* bar */\n");  // GLSL barrier() is compute / tessellation only
    } else {
        fail(c, "unknown instruction %016llx at %#x", (unsigned long long)w, pc);
        if (survey_on()) fprintf(stderr, "[survey] code %p stage %d: unknown %016llx at %#x\n", (const void *)c->code, c->stage,
                            (unsigned long long)w, pc);
    }
    (void)op13;
    if (guarded) sp(&c->body, "  }\n");
}

// ------------------------------------------------------------------ program assembly
static bool is_branch_target_op(uint64_t w) {
    return ((w & 0xfff0000000000020ull) == 0xe240000000000000ull);
}

// ------------------------------------------------------------------ GLSL output
static bool id_start(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }
static bool id_char(char ch) { return id_start(ch) || (ch >= '0' && ch <= '9'); }

// MSL-spelled instruction body -> GLSL (token-aware; comments and numbers are copied as they are).
static void glsl_rewrite(Ctx *c, Str *o, const char *src) {
    static const char *const map[][2] = {
        {"float2", "vec2"}, {"float3", "vec3"}, {"float4", "vec4"}, {"int2", "ivec2"}, {"int3", "ivec3"},
        {"int4", "ivec4"}, {"uint2", "uvec2"}, {"uint3", "uvec3"}, {"uint4", "uvec4"}, {"bool2", "bvec2"},
        {"ulong", "uint64_t"}, {"long", "int64_t"}, {"rsqrt", "inversesqrt"}, {"rint", "roundEven"},
        {"front_facing", "gl_FrontFacing"}, {"point_coord", "gl_PointCoord"}, {"lane", "gl_SubgroupInvocationID"},
        {"vid", "uint(gl_VertexIndex)"}, {"iid", "uint(gl_InstanceIndex)"},
    };
    const char *p = src;
    while (*p) {
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            e = e ? e + 2 : p + strlen(p);
            sp(o, "%.*s", (int)(e - p), p);
            p = e;
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            const char *e = strchr(p, '\n');
            if (!e) e = p + strlen(p);
            sp(o, "%.*s", (int)(e - p), p);
            p = e;
            continue;
        }
        if (*p >= '0' && *p <= '9') {  // number (0x..u, 1.0f, ...)
            const char *e = p;
            while (id_char(*e) || *e == '.') e++;
            sp(o, "%.*s", (int)(e - p), p);
            p = e;
            continue;
        }
        if (!id_start(*p)) { sp(o, "%c", *p); p++; continue; }
        const char *e = p;
        while (id_char(*e)) e++;
        int n = (int)(e - p);
        char id[64];
        if (n >= (int)sizeof id || (p > src && p[-1] == '.')) {  // member / swizzle
            sp(o, "%.*s", n, p);
            p = e;
            continue;
        }
        memcpy(id, p, n);
        id[n] = 0;
        p = e;
        if (!strcmp(id, "as_type") && *p == '<') {
            const char *gt = strchr(p, '>');
            if (!gt) { fail(c, "glsl: bad as_type"); return; }
            int tn = (int)(gt - p - 1);
            if (tn == 5 && !strncmp(p + 1, "float", 5)) sp(o, "as_f");
            else if (tn == 4 && !strncmp(p + 1, "uint", 4)) sp(o, "as_u");
            else if (tn == 3 && !strncmp(p + 1, "int", 3)) sp(o, "as_i");
            else if (tn == 5 && !strncmp(p + 1, "half2", 5)) sp(o, "unpack_h2");
            else { fail(c, "glsl: as_type<%.*s>", tn, p + 1); return; }
            p = gt + 1;
            continue;
        }
        if ((!strcmp(id, "in") || !strcmp(id, "out")) && *p == '.') {  // stage I/O struct members
            const char *m = p + 1, *me = m;
            while (id_char(*me)) me++;
            char mem[32];
            snprintf(mem, sizeof mem, "%.*s", (int)(me - m), m);
            if (!strcmp(mem, "pos")) sp(o, c->stage == STAGE_FRAGMENT ? "gl_FragCoord" : "gl_Position");
            else if (!strcmp(mem, "psize")) sp(o, "gl_PointSize");
            else sp(o, "%s", mem);  // a<i> (vertex input), v<i> (varying)
            p = me;
            continue;
        }
        if ((!strcmp(id, "char") || !strcmp(id, "short")) && *p == '(') { sp(o, id[0] == 'c' ? "sx8" : "sx16"); continue; }
        if (!strcmp(id, "tid") || !strcmp(id, "ctaid")) {
            sp(o, c->stage != STAGE_COMPUTE ? "uvec3(0u)" : id[0] == 't' ? "gl_LocalInvocationID" : "gl_WorkGroupID");
            continue;
        }
        if (id[0] == 'r' && n > 1 && n < 6) {  // registers beyond r255 (RZ) come from out-of-range encodings
            bool num = true;
            for (int i = 1; i < n; i++) num = num && id[i] >= '0' && id[i] <= '9';
            if (num && atoi(id + 1) > 255) { fail(c, "register %s out of range", id); return; }
        }
        const char *rep = id;
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (!strcmp(id, map[i][0])) { rep = map[i][1]; break; }
        sp(o, "%s", rep);
    }
}

static const char glsl_prelude[] =
    "#version 460\n"
    "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require\n"
    "#extension GL_KHR_shader_subgroup_basic : require\n"
    "#extension GL_KHR_shader_subgroup_shuffle : require\n"
    "#extension GL_KHR_shader_subgroup_shuffle_relative : require\n"
    "#extension GL_KHR_shader_subgroup_vote : require\n"
    "#extension GL_KHR_shader_subgroup_ballot : require\n"
    "#extension GL_EXT_texture_shadow_lod : require\n"
    "#define cbr(cb, off) cb[min((off) >> 4, uint(cb.length() - 1))][((off) >> 2) & 3u]\n"
    "uint as_u(float x) { return floatBitsToUint(x); }\n"
    "uint as_u(uint x) { return x; }\n"
    "uint as_u(int x) { return uint(x); }\n"
    "float as_f(uint x) { return uintBitsToFloat(x); }\n"
    "float as_f(int x) { return intBitsToFloat(x); }\n"
    "float as_f(float x) { return x; }\n"
    "int as_i(uint x) { return int(x); }\n"
    "int as_i(float x) { return floatBitsToInt(x); }\n"
    "int sx8(uint v) { return bitfieldExtract(int(v), 0, 8); }\n"
    "int sx8(int v) { return bitfieldExtract(v, 0, 8); }\n"
    "int sx16(uint v) { return bitfieldExtract(int(v), 0, 16); }\n"
    "int sx16(int v) { return bitfieldExtract(v, 0, 16); }\n"
    // MSL semantics: saturate(NaN) = 0, fmin/fmax return the non-NaN operand
    "float saturate(float x) { return isnan(x) ? 0.0f : clamp(x, 0.0f, 1.0f); }\n"
    "vec2 saturate(vec2 x) { return vec2(saturate(x.x), saturate(x.y)); }\n"
    "float fmin(float a, float b) { return isnan(a) ? b : (isnan(b) ? a : min(a, b)); }\n"
    "float fmax(float a, float b) { return isnan(a) ? b : (isnan(b) ? a : max(a, b)); }\n"
    "uint clz(uint v) { return v == 0u ? 32u : uint(31 - findMSB(v)); }\n"
    "uint popcount(uint v) { return uint(bitCount(v)); }\n"
    "uint mulhi(uint a, uint b) { uint hi, lo; umulExtended(a, b, hi, lo); return hi; }\n"
    "int mulhi(int a, int b) { int hi, lo; imulExtended(a, b, hi, lo); return hi; }\n"
    "uint bfe_u(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return 0u; return bitfieldExtract(v, int(pos), int(min(len, 32u - pos))); }\n"
    "uint bfe_s(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return (int(v) < 0) ? 0xFFFFFFFFu : 0u; return uint(bitfieldExtract(int(v), int(pos), int(min(len, 32u - pos)))); }\n"
    "uint bfi(uint ins, uint ctl, uint base) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u || pos >= 32u) return base; return bitfieldInsert(base, ins, int(pos), int(min(len, 32u - pos))); }\n"
    "float fswzadd(float a, float b, uint swz, uint lane) { uint m = (swz >> ((lane & 3u) * 2u)) & 3u; float ma = m == 1u ? -1.0f : (m == 3u ? 0.0f : 1.0f); float mb = m == 2u ? -1.0f : 1.0f; return a * ma + b * mb; }\n"
    "uvec2 rsc(uvec2 c, vec4 s) { return (s.x == 1.0f && s.y == 1.0f) ? c : uvec2(floor((vec2(c) + 0.5f) * s.xy)); }\n"
    // a scaled texture whose contents occupy its top-left part (Dynamic resolution): sample that part
    "vec2 uvs(vec2 uv, vec4 s) { return (s.z == 1.0f && s.w == 1.0f) ? uv : min(uv, 1.0f) * s.zw; }\n";
// The half <-> float helpers (glsl_finish: glsl_h2f_exact or glsl_h2f_native, then glsl_prelude_half).
// Half -> float in vertex (and other pre-raster) shaders: by bits, not unpackHalf2x16. NVIDIA's driver recognises
// unpacked halves and runs some of the fp32 math after them in native fp16, differently per shader, so a depth
// pre-pass and its colour pass (EQUAL test) placed wind-animated foliage apart: black slashes / flicker (notes/PLAN.md
// 2026-10-08).
static const char glsl_h2f_exact[] =
    "float h2f(uint h) { uint s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;"
    " if (e == 0u) { float d = float(m) * 5.9604644775390625e-8; return s != 0u ? -d : d; }"
    " if (e == 31u) return uintBitsToFloat(s | 0x7F800000u | (m << 13));"
    " return uintBitsToFloat(s | ((e + 112u) << 23) | (m << 13)); }\n"
    "vec2 unpack_h2(uint v) { return vec2(h2f(v & 0xFFFFu), h2f(v >> 16)); }\n";
// ... and in fragment / compute shaders the native conversion: positions don't depend on them, and the by-bits
// version (~12 integer ops and two branches per half) was half of the main lighting pass's GPU time on an Intel UHD
// 770 and a third on an RTX 3080 Ti (the game's shaders use packed halves heavily). LARECOMP_VK_EXACT_H2F=1: by bits
// in every stage.
static const char glsl_h2f_native[] =
    "float h2f(uint h) { return unpackHalf2x16(h).x; }\n"
    "vec2 unpack_h2(uint v) { return unpackHalf2x16(v); }\n";
static const char glsl_prelude_half[] =
    "vec2 hx(uint v, uint swz) { if (swz == 1u) return vec2(uintBitsToFloat(v)); vec2 h = unpack_h2(v); return swz == 2u ? h.xx : (swz == 3u ? h.yy : h); }\n"
    "uint hpack(vec2 r, uint merge, uint old) { if (merge == 1u) return floatBitsToUint(r.x); uint lo = packHalf2x16(vec2(r.x, 0.0f)), hi = packHalf2x16(vec2(r.y, 0.0f)); if (merge == 2u) return (old & 0xFFFF0000u) | lo; if (merge == 3u) return (old & 0xFFFFu) | (hi << 16); return lo | (hi << 16); }\n"
    "uint prmt(uint a, uint b, uint sel) { uint r = 0u; for (uint i = 0u; i < 4u; i++) { uint s = (sel >> (i * 4u)) & 7u; uint bt = s < 4u ? (a >> (s * 8u)) & 0xFFu : (b >> ((s - 4u) * 8u)) & 0xFFu; r |= bt << (i * 8u); } return r; }\n";

static const char *glsl_sampler_type(const ShaderTexture *t) {
    static const char *const names[][3] = {
        {"sampler1D", "sampler1DShadow", "usampler1D"}, {"sampler2D", "sampler2DShadow", "usampler2D"},
        {"sampler3D", "sampler3D", "usampler3D"}, {"samplerCube", "samplerCubeShadow", "usamplerCube"},
        {"sampler2DArray", "sampler2DArrayShadow", "usampler2DArray"},
        {"samplerCubeArray", "samplerCubeArrayShadow", "usamplerCubeArray"},
        {"sampler1DArray", "sampler1DArrayShadow", "usampler1DArray"}};
    int ty = t->type <= TEXTYPE_1D_ARRAY ? t->type : TEXTYPE_2D;
    return names[ty][t->stencil ? 2 : t->shadow ? 1 : 0];
}

// Stencil slots (usampler, ShaderTexture.stencil): every lookup on one becomes uintBitsToFloat(lookup), the float
// whose bits are the value - what the body's float lookups expect (they take the bits back with as_u). Lookups are
// texture* / texelFetch* calls whose first argument is t<slot>; the size and level queries return ints and stay.
static void glsl_wrap_stencil(Ctx *c, Str *body) {
    bool any = false;
    for (int i = 0; i < c->info->ntex; i++) any = any || c->info->tex[i].stencil;
    if (!any || !body->buf) return;
    Str o = {0};
    const char *p = body->buf;
    while (*p) {
        bool start = (p == body->buf || !id_char(p[-1])) && (!strncmp(p, "texture", 7) || !strncmp(p, "texelFetch", 10));
        if (start) {
            const char *e = p;
            while (id_char(*e)) e++;
            int n = (int)(e - p), slot = -1;
            bool query = (n == 11 && !strncmp(p, "textureSize", 11)) || !strncmp(p, "textureQuery", 12) || !strncmp(p, "textureSamples", 14);
            if (!query && e[0] == '(' && e[1] == 't' && e[2] >= '0' && e[2] <= '9') slot = atoi(e + 2);
            if (slot >= 0 && slot < c->info->ntex && c->info->tex[slot].stencil) {
                const char *q = e;
                int depth = 0;
                do { if (*q == '(') depth++; else if (*q == ')') depth--; q++; } while (*q && depth > 0);
                sp(&o, "uintBitsToFloat(%.*s)", (int)(q - p), p);
                p = q;
                continue;
            }
            sp(&o, "%.*s", n, p);
            p = e;
            continue;
        }
        sp(&o, "%c", *p);
        p++;
    }
    free(body->buf);
    *body = o;
}

// A texture function's call at p (its name, then "(t<slot>"): the slot, or -1; *end: past the name
static int tex_call_slot(const char *p, const char **end) {
    const char *e = p;
    while (id_char(*e)) e++;
    *end = e;
    return e[0] == '(' && e[1] == 't' && e[2] >= '0' && e[2] <= '9' ? atoi(e + 2) : -1;
}

// Buffer texture slots (ShaderKey.tex_buffer): a 1D slot whose every lookup is a texelFetch becomes TEXTYPE_BUFFER
// (samplerBuffer), its fetches lose the level argument (texelFetch(t, x, l) -> texelFetch(t, x)), and an integer
// one's are wrapped to the float whose bits are the value (as the stencil slots). A slot looked up another way stays 1D.
static void glsl_buffer_slots(Ctx *c, Str *body) {
    const ShaderKey *key = c->key;
    if (!key || !key->tex_buffer || !body->buf) return;
    bool cand[64] = {0}, any = false;
    for (int i = 0; i < c->info->ntex && i < 64; i++) {
        ShaderTexture *t = &c->info->tex[i];
        int k = ((int)t->handle_offset - 0x20) / 8;
        if (t->is_image || t->bindless || t->shadow || t->stencil || t->type != TEXTYPE_1D || k < 0 || k >= 32) continue;
        cand[i] = ((key->tex_buffer >> (2 * k)) & 3) != 0;
    }
    for (const char *p = body->buf; *p; p++) {  // (a slot looked up other than by texelFetch: not a buffer)
        if (!(p == body->buf || !id_char(p[-1])) || (strncmp(p, "texture", 7) && strncmp(p, "texelFetch", 10))) continue;
        const char *e;
        int slot = tex_call_slot(p, &e);
        if (slot >= 0 && slot < 64 && (e - p != 10 || strncmp(p, "texelFetch", 10))) cand[slot] = false;
        p = e - 1;
    }
    for (int i = 0; i < c->info->ntex && i < 64; i++) {
        if (!cand[i]) continue;
        ShaderTexture *t = &c->info->tex[i];
        t->type = TEXTYPE_BUFFER;
        t->buffer_kind = (uint8_t)((key->tex_buffer >> (2 * (((int)t->handle_offset - 0x20) / 8))) & 3);
        any = true;
    }
    if (!any) return;
    Str o = {0};
    const char *p = body->buf;
    while (*p) {
        bool start = (p == body->buf || !id_char(p[-1])) && !strncmp(p, "texelFetch(", 11);
        const char *e;
        int slot = start ? tex_call_slot(p, &e) : -1;
        if (slot < 0 || slot >= 64 || !cand[slot]) {
            if (start) { sp(&o, "%.*s", (int)(e - p), p); p = e; continue; }
            sp(&o, "%c", *p);
            p++;
            continue;
        }
        const char *q = e, *last = NULL;  // (the call's end; its last top-level comma)
        int depth = 0;
        do {
            if (*q == '(') depth++;
            else if (*q == ')') depth--;
            else if (*q == ',' && depth == 1) last = q;
            q++;
        } while (*q && depth > 0);
        int kind = c->info->tex[slot].buffer_kind;
        const char *wrap = kind == 2 ? "uintBitsToFloat(" : kind == 3 ? "intBitsToFloat(" : "";
        if (last) sp(&o, "%s%.*s)%s", wrap, (int)(last - p), p, *wrap ? ")" : "");
        else sp(&o, "%s%.*s%s", wrap, (int)(q - p), p, *wrap ? ")" : "");
        p = q;
    }
    free(body->buf);
    *body = o;
}

// (GLSL fragment, lazy heap chunks) Early depth / stencil tests for a shader that counts chunk misses (GMiss atomics):
// only where the tests' outcome can't depend on the shader - no discard, no depth output (a sample mask output
// isn't translated) - and the shader has no side effect of its own (stores), so whether a hidden fragment runs it is
// not observable. (Alpha to
// coverage with early tests would write depth for samples it drops: the renderer uses this shader's module without
// the execution mode for those pipelines, vulkan/exec.c fs_late.)
static bool glsl_early_tests(const Ctx *c) {
    return c->glsl && c->stage == STAGE_FRAGMENT && c->info->uses_gmem && maxwell_gmem_chunk_shift && maxwell_gmem_lazy &&
           !maxwell_gmem_fs_miss && !c->info->uses_discard && !c->omap_depth && !c->uses_gmem_store;
}

// Declarations + entry point around the (rewritten) body; mirrors the MSL assembly in maxwell_translate.
static ShaderInfo *glsl_finish(Ctx *c, bool any_branch, bool dbg) {
    ShaderInfo *info = c->info;
    const ShaderKey *key = c->key;
    int stage = c->stage;
    Str body = {0};
    glsl_rewrite(c, &body, c->body.buf ? c->body.buf : "");
    for (int i = 0; i < info->ntex; i++) {  // stencil slots (the key's bound slots): usampler, lookups wrapped
        ShaderTexture *t = &info->tex[i];
        int k = ((int)t->handle_offset - 0x20) / 8;
        if (!key || t->is_image || k < 0 || k >= 32 || !((key->tex_stencil_mask >> k) & 1)) continue;
        if (t->shadow) { fail(c, "stencil texture %d with a depth compare", k); continue; }
        t->stencil = true;
    }
    glsl_wrap_stencil(c, &body);
    glsl_buffer_slots(c, &body);
    if (info->uses_gmem && (info->cbuf_mask & (1u << GMEM_BUFFER_INDEX))) fail(c, "cbuf %d clashes with gmem", GMEM_BUFFER_INDEX);
    if (info->cbuf_mask >> 18) fail(c, "cbuf index out of range");
    for (int i = 0; i < info->ntex; i++)
        if (info->tex[i].shadow && info->tex[i].type == TEXTYPE_3D) fail(c, "3d shadow texture");
    if (c->fail) { free(body.buf); free(c->body.buf); free(c); return info; }
    int set = stage == STAGE_FRAGMENT ? 1 : 0;
    Str s = {0};
    sp(&s, "#version 460\n");  // (the prelude's first line; an extension directive has to follow it)
    if (info->uses_gmem && maxwell_gmem_chunk_shift && maxwell_gmem_lazy) sp(&s, "#extension GL_EXT_buffer_reference : require\n");
    if (stage == STAGE_COMPUTE && maxwell_glsl_minpc) sp(&s, "#extension GL_KHR_shader_subgroup_arithmetic : require\n");
    sp(&s, "%s", glsl_prelude + sizeof "#version 460\n" - 1);
    static int exact = -1;
    if (exact < 0) exact = getenv("LARECOMP_VK_EXACT_H2F") && atoi(getenv("LARECOMP_VK_EXACT_H2F")) != 0;
    sp(&s, "%s%s", !exact && (stage == STAGE_FRAGMENT || stage == STAGE_COMPUTE) ? glsl_h2f_native : glsl_h2f_exact,
       glsl_prelude_half);
    for (int i = 0; i < 18; i++) {
        if (!(info->cbuf_mask & (1u << i))) continue;
        uint32_t n = info->cbuf_size[i] / 16;
        if (c->cbuf_need[i] > n) n = c->cbuf_need[i];
        if ((info->cbuf_dynamic & (1u << i)) && n < 4096) n = 4096;
        if (!n) n = 1;
        sp(&s, "layout(std140, set = %d, binding = %d) uniform CB%d { uvec4 cb%d[%u]; };\n", set, i, i, i, n);
    }
    if (info->uses_gmem && !maxwell_gmem_chunk_shift) {
        sp(&s, "layout(std430, set = %d, binding = %d) %sbuffer GMem { uint w[]; } gmem_;\n", set, GMEM_BUFFER_INDEX,
           c->uses_gmem_store ? "" : "readonly ");
        sp(&s, "#define GMR(i) gmem_.w[i]\n");
        if (c->uses_gmem_store)
            sp(&s, "#define GMW(i, v) gmem_.w[i] = (v)\n#define GMAND(i, m) atomicAnd(gmem_.w[i], m)\n"
                   "#define GMOR(i, m) atomicOr(gmem_.w[i], m)\n");
    } else if (info->uses_gmem && !maxwell_gmem_lazy) {  // the heap in windows, one binding each, plain branches (shader.h)
        int n = maxwell_gmem_chunks(), ws = (int)maxwell_gmem_chunk_shift - 2, cs = GMEM_CHUNK_SET(stage);
        for (int k = 0; k < n; k++)
            sp(&s, "layout(std430, set = %d, binding = %d) %sbuffer GMem%d { uint w[]; } gmem%d_;\n", cs, GMEM_CHUNK_BINDING + k,
               c->uses_gmem_store ? "" : "readonly ", k, k);
        sp(&s, "uint GMR_(uint i) { uint c = i >> %du, o = i & 0x%xu;\n", ws, (1u << ws) - 1);
        for (int k = 0; k < n; k++) sp(&s, "  if (c == %du) return gmem%d_.w[o];\n", k, k);
        sp(&s, "  return 0u; }\n#define GMR(i) GMR_(i)\n");
        if (c->uses_gmem_store) {
            static const char *const ops[3][2] = {{"GMW_", "gmem%d_.w[o] = v"}, {"GMAND_", "atomicAnd(gmem%d_.w[o], v)"},
                                                  {"GMOR_", "atomicOr(gmem%d_.w[o], v)"}};
            for (int op = 0; op < 3; op++) {
                sp(&s, "void %s(uint i, uint v) { uint c = i >> %du, o = i & 0x%xu;\n", ops[op][0], ws, (1u << ws) - 1);
                for (int k = 0; k < n; k++) {
                    char body[64];
                    snprintf(body, sizeof body, ops[op][1], k);
                    sp(&s, "  if (c == %du) { %s; return; }\n", k, body);
                }
                sp(&s, "}\n");
            }
            sp(&s, "#define GMW(i, v) GMW_(i, v)\n#define GMAND(i, m) GMAND_(i, m)\n#define GMOR(i, m) GMOR_(i, m)\n");
        }
    } else if (info->uses_gmem) {  // chunks imported lazily: a table of device addresses, buffer references (shader.h)
        int ws = (int)maxwell_gmem_chunk_shift - 2, cs = GMEM_CHUNK_SET(stage);
        bool direct = stage != STAGE_COMPUTE;  // (compute: its storage buffers are in gfast_ already)
        sp(&s, "layout(std140, set = %d, binding = %d) uniform GChunks { uvec4 gchunk[%d]; };\n", cs, GMEM_CHUNK_BINDING, GMEM_CHUNKS_MAX);
        sp(&s, "layout(buffer_reference, std430, buffer_reference_align = 4) %sbuffer GW { uint w[]; };\n", c->uses_gmem_store ? "" : "readonly ");
        // Misses are counted with atomics on GMiss: a side effect, which in a fragment shader turns early depth / stencil
        // tests off - every overdrawn fragment ran in full (UHD 770 main pass 13.3 ms vs 8.4). So a fragment shader
        // whose own result can't depend on late tests (no discard or depth output, no stores) declares
        // early tests (glsl_early_tests) and keeps its counter; one that can't (discard / depth, no stores)
        // counts nothing - it would cost those tests - and relies on the runtime bringing its chunks in before the draw:
        // the bound storage buffers' chunks (gmem_chunk_bind), pointers in uniform buffers, the chunks other shaders
        // missed (cache/gmem_chunks.txt). LARECOMP_VK_GMEM_FS_MISS=1 (maxwell_gmem_fs_miss): every shader counts, no
        // early tests (as before; to find a chunk only such a shader reaches).
        bool count = stage != STAGE_FRAGMENT || maxwell_gmem_fs_miss || c->uses_gmem_store || glsl_early_tests(c);
        if (count) {
            sp(&s, "layout(std430, set = %d, binding = %d) buffer GMiss { uint n[%d]; uint w[%d]; } gmiss_;\n", cs, GMEM_MISS_BINDING,
               GMEM_CHUNKS_MAX, GMEM_CHUNKS_MAX);
            sp(&s, "void gmiss(uint c, uint i) { atomicAdd(gmiss_.n[c], 1u); atomicMin(gmiss_.w[c], i); }\n");
        } else {
            sp(&s, "void gmiss(uint c, uint i) { }\n");
        }
        // (gchunk[c]: x, y = device address, z = words; the first word's chunk holds the whole aligned access)
        sp(&s, "#define GMC(i) ((i) >> %du)\n#define GMO(i) ((i) & 0x%xu)\n", ws, (1u << ws) - 1);
        if (direct) {  // (gd[k]: x = the chunk's first word, y = its words (0: unused), z, w = its device address; GDir<k>: it)
            sp(&s, "layout(push_constant) uniform GDirect { uvec4 gd[%d]; };\n", GMEM_DIRECT_MAX);
            for (int k = 0; k < GMEM_DIRECT_MAX; k++)
                sp(&s, "layout(std430, set = %d, binding = %d) %sbuffer GDir%d { uint w[]; } gdir%d_;\n", cs, GMEM_DIRECT_BINDING + k,
                   c->uses_gmem_store ? "" : "readonly ", k, k);
        }
        // Each access: the draw's storage buffer chunks first (gd, uniform: no table read, no miss path), then the table.
        static const struct { const char *head, *act, *miss; int last; bool store; } fn[] = {
            {"uint GMR1(uint i)", "return p.w[o];", "return 0u;", 0, false},
            {"uvec2 GMR2(uint i)", "return uvec2(p.w[o], p.w[o + 1u]);", "return uvec2(0u);", 1, false},
            {"uvec4 GMR4(uint i)", "return uvec4(p.w[o], p.w[o + 1u], p.w[o + 2u], p.w[o + 3u]);", "return uvec4(0u);", 3, false},
            {"void GMW1(uint i, uint v)", "p.w[o] = v; return;", "", 0, true},
            {"void GMW2(uint i, uvec2 v)", "p.w[o] = v.x; p.w[o + 1u] = v.y; return;", "", 1, true},
            {"void GMW4(uint i, uvec4 v)", "p.w[o] = v.x; p.w[o + 1u] = v.y; p.w[o + 2u] = v.z; p.w[o + 3u] = v.w; return;", "", 3, true},
            {"void GMAND1(uint i, uint v)", "atomicAnd(p.w[o], v); return;", "", 0, true},
            {"void GMOR1(uint i, uint v)", "atomicOr(p.w[o], v); return;", "", 0, true},
        };
        for (int f = 0; f < (int)(sizeof fn / sizeof fn[0]); f++) {
            if (fn[f].store && !c->uses_gmem_store) continue;
            sp(&s, "%s { uint o;", fn[f].head);
            if (direct)  // (whole chunks: an aligned access in one never leaves it; the storage buffer, not the address)
                for (int k = 0; k < GMEM_DIRECT_MAX; k++) {
                    char act[192], *d = act;
                    for (const char *a = fn[f].act; *a && d < act + sizeof act - 8; a++) {
                        if (a[0] == 'p' && a[1] == '.') d += snprintf(d, 8, "gdir%d_", k);
                        else *d++ = *a;
                    }
                    *d = 0;
                    sp(&s, " o = i - gd[%d].x; if (o < gd[%d].y) { %s }", k, k, act);
                }
            char lim[16];
            if (fn[f].last) snprintf(lim, sizeof lim, "o + %du", fn[f].last); else snprintf(lim, sizeof lim, "o");
            sp(&s, " uint c = GMC(i); o = GMO(i); uvec4 e = gchunk[c]; if (%s < e.z) { GW p = GW((uint64_t(e.y) << 32) | uint64_t(e.x)); %s } "
                       "gmiss(c, i); %s }\n", lim, fn[f].act, fn[f].miss);
        }
        sp(&s, "#define GMR(i) GMR1(i)\n");
        if (c->uses_gmem_store) sp(&s, "#define GMW(i, v) GMW1(i, v)\n#define GMAND(i, m) GMAND1(i, m)\n#define GMOR(i, m) GMOR1(i, m)\n");
    }
    if (info->uses_gmem_map) {  // (compute) the dispatch's storage buffers, device-local: gmap[i] = heap offset, size, word
        sp(&s, "layout(std140, set = %d, binding = %d) uniform GMap { uvec4 gmap[%d]; };\n", set, GMAP_BUFFER_INDEX, GMAP_MAX + 1);
        sp(&s, "layout(std430, set = %d, binding = %d) %sbuffer GFast { uint w[]; } gfast_;\n", set, GFAST_BUFFER_INDEX,
           c->uses_gmem_store ? "" : "readonly ");
        sp(&s, "uint gfind(uint g) { uint n = gmap[%d].x; for (uint i = 0u; i < n; i++) { uint o = g - gmap[i].x; "
               "if (g >= gmap[i].x && o < gmap[i].y) return gmap[i].z + (o >> 2); } return 0xFFFFFFFFu; }\n", GMAP_MAX);
        if (c->uses_gmem_store)  // (the stores' lookup: a store inside entry i's bound bytes - gmap[i].w, exec.c gmap_build -
                                 //  writes the dispatch's stamp to GFast word GFAST_FLAG_WORD + i; the padding doesn't count)
            sp(&s, "uint gfindw(uint g) { uint n = gmap[%d].x; for (uint i = 0u; i < n; i++) { uint o = g - gmap[i].x; "
                   "if (g >= gmap[i].x && o < gmap[i].y) { if (o >= (gmap[i].w & 0xFFu) && (o >> 2) < (gmap[i].w >> 8)) "
                   "gfast_.w[%uu + i] = gmap[%d].y; return gmap[i].z + (o >> 2); } } "
                   "return 0xFFFFFFFFu; }\n", GMAP_MAX, GFAST_FLAG_WORD, GMAP_MAX);
    }
    if (c->uses_rscale && stage != STAGE_VERTEX) {
        sp(&s, "layout(std140, set = %d, binding = 18) uniform RScale { vec4 rscale[%d]; };\n", set,
           key && key->flip_x ? 66 : 65);  // ([65]: mirror mode's render target width)
        info->uses_rscale = true;
    }
    bool tint = stage == STAGE_FRAGMENT && !dbg && (c->omap_target & 0x7) == 0x7 && (!key || key->rt_kind[0] == 0);
    if (tint) {
        sp(&s, "layout(std140, set = %d, binding = 19) uniform Tint { vec4 ltint[4]; };\n", set);
        info->uses_tint = true;
    }
    for (int i = 0; i < info->ntex; i++) {
        ShaderTexture *t = &info->tex[i];
        if (t->is_image) {  // (SULD / SUST: no format qualifier; access qualifiers from the uses)
            static const char *const it[] = {"image1D", "image2D", "image3D", "imageCube", "image2DArray", "imageCubeArray", "image1DArray"};
            sp(&s, "layout(set = %d, binding = %d) uniform %s%s%s t%d;\n", set, 32 + i, !c->img_read[i] ? "writeonly " : "",
               !c->img_write[i] ? "readonly " : "", it[t->type <= TEXTYPE_1D_ARRAY ? t->type : TEXTYPE_2D], i);
            switch (t->type) {  // (TXQ on an image: one level)
            case TEXTYPE_1D: sp(&s, "ivec3 tsize%d(int l) { return ivec3(imageSize(t%d), 1, 1); }\n", i, i); break;
            case TEXTYPE_1D_ARRAY: sp(&s, "ivec3 tsize%d(int l) { ivec2 s = imageSize(t%d); return ivec3(s.x, 1, s.y); }\n", i, i); break;
            case TEXTYPE_3D: case TEXTYPE_2D_ARRAY: sp(&s, "ivec3 tsize%d(int l) { return imageSize(t%d); }\n", i, i); break;
            default: sp(&s, "ivec3 tsize%d(int l) { return ivec3(imageSize(t%d), 1); }\n", i, i);
            }
            sp(&s, "int tlevels%d() { return 1; }\n", i);
            continue;
        }
        if (t->type == TEXTYPE_BUFFER) {  // (ShaderKey.tex_buffer: a uniform texel buffer)
            sp(&s, "layout(set = %d, binding = %d) uniform %ssamplerBuffer t%d;\n", set, 32 + i,
               t->buffer_kind == 2 ? "u" : t->buffer_kind == 3 ? "i" : "", i);
            sp(&s, "ivec3 tsize%d(int l) { return ivec3(textureSize(t%d), 1, 1); }\nint tlevels%d() { return 1; }\n", i, i, i);
            continue;
        }
        sp(&s, "layout(set = %d, binding = %d) uniform %s t%d;\n", set, 32 + i, glsl_sampler_type(t), i);
        // size of level l as (width, height, depth or layers)
        switch (t->type) {
        case TEXTYPE_1D: sp(&s, "ivec3 tsize%d(int l) { return ivec3(textureSize(t%d, l), 1, 1); }\n", i, i); break;
        case TEXTYPE_1D_ARRAY: sp(&s, "ivec3 tsize%d(int l) { ivec2 s = textureSize(t%d, l); return ivec3(s.x, 1, s.y); }\n", i, i); break;
        case TEXTYPE_3D: case TEXTYPE_2D_ARRAY: case TEXTYPE_CUBE_ARRAY:
            sp(&s, "ivec3 tsize%d(int l) { return textureSize(t%d, l); }\n", i, i); break;
        default: sp(&s, "ivec3 tsize%d(int l) { return ivec3(textureSize(t%d, l), 1); }\n", i, i);
        }
        sp(&s, "int tlevels%d() { return textureQueryLevels(t%d); }\n", i, i);
    }
    if (stage == STAGE_VERTEX) {
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            int kind = key ? key->attr_kind[i] : 0;
            sp(&s, "layout(location = %d) in %s a%d;\n", i, kind == 1 ? "ivec4" : kind == 2 ? "uvec4" : "vec4", i);
        }
        // depth pre-pass + EQUAL test needs identical positions from different shaders: invariant, and precise so no
        // compiler fuses or reorders the math feeding it (MSL gets this from preserveInvariance; without it foliage
        // drawn twice flickered black)
        sp(&s, "invariant gl_Position;\nprecise gl_Position;\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "layout(location = %d) out vec4 v%d;\n", i, i);
    } else if (stage == STAGE_FRAGMENT) {
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            const char *interp = (c->flat_mask & (1u << i)) ? "flat " : (c->noperspective_mask & (1u << i)) ? "noperspective " : "";
            sp(&s, "layout(location = %d) %sin vec4 v%d;\n", i, interp, i);
        }
        for (int rt = 0; rt < 8; rt++) {
            if (!((c->omap_target >> (rt * 4)) & 0xF)) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            sp(&s, "layout(location = %d) out %s c%d;\n", rt, kind == 1 ? "ivec4" : kind == 2 ? "uvec4" : "vec4", rt);
        }
        if (c->omap_depth) info->writes_depth = true;
        if (glsl_early_tests(c)) sp(&s, "layout(early_fragment_tests) in;\n");
    } else {
        sp(&s, "layout(local_size_x_id = 0, local_size_y_id = 1, local_size_z_id = 2) in;\n");
        // The pipeline specializes this to the NVN control section's allocation. Reserving 48 KB for every
        // shader, including those with a few counters, unnecessarily limits resident workgroups on AMD APUs.
        // Unknown/old records retain the original 48 KB default. The same constant bounds LDS/STS/ATOMS.
        if (c->uses_shared)
            sp(&s, "layout(constant_id = 3) const uint smem_words_ = %du;\nshared uint smem[smem_words_];\n", SMEM_WORDS);
    }
    sp(&s, "void main() {\n");
    if (stage == STAGE_VERTEX) {
        sp(&s, "  gl_Position = vec4(0, 0, 0, 1);\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  v%d = vec4(0, 0, 0, 1);\n", i);
        if (info->writes_point_size) sp(&s, "  gl_PointSize = 0.0f;\n");
    }
    for (int i = 0; i < 256; i++)
        if (c->reg_used[i]) sp(&s, "  uint r%d = 0u;\n", i);
    for (int i = 0; i < 7; i++)
        if (c->pred_used[i]) sp(&s, "  bool p%d = false;\n", i);
    if (c->uses_cc) sp(&s, "  uint cc = 0u; bool ccz = false, ccs = false;\n");
    if (c->uses_local) sp(&s, "  uint lmem[%d]; for (int li_ = 0; li_ < %d; li_++) lmem[li_] = 0u;\n", LMEM_WORDS, LMEM_WORDS);
    if (dbg) sp(&s, maxwell_dbg_pc == 0xffffffffu ? "  float dbgv = 1.0f;\n" : "  float dbgv = -1.0f;\n");
    if (c->structured) {  // (structure_flow; pc / exit_now: unused)
        sp(&s, "  uint pc = 8u; bool exit_now = false; uint jmp_ = 0u; uint guard_ = 0u;\n");
        sp(&s, "  do {\n%s  } while (false);\n", body.buf ? body.buf : "");
    } else if (any_branch) {
        if (c->ssy_max < 0) {
            sp(&s, "  uint pc = 8u; uint ssy[16]; uint ssy_n = 0u; uint pbk[16]; uint pbk_n = 0u; uint pcnt[16]; uint pcnt_n = 0u;\n");
        } else {  // (static_stacks: a slot per level)
            sp(&s, "  uint pc = 8u; uint pcnt[16]; uint pcnt_n = 0u;\n");
            for (int i = 0; i < c->ssy_max; i++) sp(&s, "  uint ssy%d = 0u;\n", i);
            for (int i = 0; i < c->pbk_max; i++) sp(&s, "  uint pbk%d = 0u;\n", i);
        }
        sp(&s, "  uint cst[16]; uint cst_n = 0u;\n");
        sp(&s, "  bool exit_now = false;\n");
        sp(&s, "  for (uint guard = 0u; guard < 100000u; guard++) {\n");
        if (stage == STAGE_COMPUTE && maxwell_glsl_minpc)  // (shader.h; a wait isn't one of the lane's own steps)
            sp(&s, "  if (pc != subgroupMin(pc)) { guard--; continue; }\n");
        sp(&s, "  switch (pc) {\n");
        sp(&s, "%s", body.buf ? body.buf : "");
        sp(&s, "    exit_now = true; break;\n  default: exit_now = true; break;\n  }\n  if (exit_now) break;\n  }\n");
    } else {
        sp(&s, "  do {\n%s  } while (false);\n", body.buf ? body.buf : "");
    }
    free(body.buf);
    if (stage == STAGE_VERTEX) {
        if (key && key->flip_y) sp(&s, "  gl_Position.y = -gl_Position.y;\n");
        if (key && key->flip_x) sp(&s, "  gl_Position.x = -gl_Position.x;\n");
        if (key && key->depth_minus_one) sp(&s, "  gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5f;\n");
    } else if (stage == STAGE_FRAGMENT) {
        int r = 0;
        for (int rt = 0; rt < 8; rt++) {
            uint32_t m = (c->omap_target >> (rt * 4)) & 0xF;
            if (!m) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            const char *cv = kind == 1 ? "int" : kind == 2 ? "uint" : "";
            for (int comp = 0; comp < 4; comp++) {
                if (!(m & (1u << comp))) continue;
                if (dbg) {
                    if (comp == 3) sp(&s, "  c%d[%d] = %s(1.0f);\n", rt, comp, cv);
                    else sp(&s, "  c%d[%d] = %s(dbgv);\n", rt, comp, cv);
                } else if (r < 256 && c->reg_used[r]) {
                    if (kind == 0) sp(&s, "  c%d[%d] = as_f(r%d);\n", rt, comp, r);
                    else sp(&s, "  c%d[%d] = %s(r%d);\n", rt, comp, cv, r);
                } else {
                    sp(&s, "  c%d[%d] = 0;\n", rt, comp);
                }
                r++;
            }
        }
        if (tint)
            sp(&s, "  if (ltint[0].w > 0.0f) { vec3 g = c0.rgb; float l = dot(g, vec3(0.299f, 0.587f, 0.114f));\n"
                   "    c0.rgb = mix(g, ltint[0].rgb * pow(l, ltint[1].w) + ltint[1].rgb * l, ltint[0].w); }\n");
        if (tint) emit_recolour(&s, "c0", "vec3", "vec4");
        if (c->omap_depth) {
            if (c->reg_used[r + 1]) sp(&s, "  gl_FragDepth = as_f(r%d);\n", r + 1);
            else sp(&s, "  gl_FragDepth = gl_FragCoord.z;\n");
        }
    }
    sp(&s, "}\n");
    info->msl = s.buf;
    info->attr_mask = c->in_attr_mask;
    info->out_mask = stage == STAGE_FRAGMENT ? c->omap_target : c->out_attr_mask;
    free(c->body.buf);
    free(c);
    return info;
}

// The table index's bound: the compiler clamps it (IMNMX Rx, Rx, K, maybe shifted in place) before the LDC at ldc.
// Returns the number of entries, K + 1, or 256 when no clamp is found. (static_stacks: the words after a table that
// still look like entries would add targets reached at other stack depths.)
static int brx_table_entries(const uint64_t *code, size_t ldc) {
    int rx = (int)bits(code[ldc], 8, 8);
    if (rx == RZ) return 256;
    for (size_t k = ldc, seen = 0; k-- > 0 && seen < 8;) {
        if (k % 4 == 0) continue;
        seen++;
        uint64_t w = code[k];
        if ((w & 0xfff0000000000000ull) >= 0xe200000000000000ull && (w & 0xfff0000000000000ull) <= 0xe350000000000000ull)
            return 256;
        if ((int)bits(w, 0, 8) != rx) continue;
        if ((w & 0xfef8000000000000ull) == 0x3848000000000000ull && (int)bits(w, 8, 8) == rx) continue;  // SHL rx, rx, imm
        if ((w & 0xfef8000000000000ull) == 0x3820000000000000ull && (int)bits(w, 8, 8) == rx &&  // IMNMX rx, rx, imm: min
            bits(w, 39, 3) == PT && !bits(w, 42, 1) && !bits(w, 56, 1)) {
            uint32_t v = (uint32_t)bits(w, 20, 19);
            return v < 256 ? (int)v + 1 : 256;
        }
        return 256;  // (something else writes rx)
    }
    return 256;
}

// BRX (indirect branch, Vulkan / GLSL only): label just the targets its jump table can name. A BRX used to make every
// instruction a case of the dispatcher switch (any one may be a target): hundreds of cases falling into each other
// inside the dispatcher loop. Suspected of what AMD's Windows driver never finishes building (issue #237: the dialogue
// body text's vertex shader stayed "building", its draws skipped - Tarin's name showed, the message stayed blank).
// The compiler puts the table in the program's own constants (cb1, after the code: blob + const_offset), loaded into
// the BRX register by an LDC c[0x1][Rx + table] shortly before: BRX Ra, imm goes to pc + 8 + table[k] + imm. Entries
// are read until one isn't an instruction of this shader (the end of the table; a word past it that still looks like
// one only adds a harmless case). False: not that shape (the register set some other way, no constants, nothing
// valid) - the caller labels every instruction as before. LARECOMP_BRX_ALL_LABELS=1: always every instruction.
// clamped (static_stacks): only the entries the index can reach (brx_table_entries), all of them targets.
static bool brx_table_labels(const uint8_t *code, size_t i, size_t end, const uint8_t *blob, size_t const_offset,
                             uint8_t *label, bool clamped) {
    static int all = -1;
    if (all < 0) { const char *e = getenv("LARECOMP_BRX_ALL_LABELS"); all = e && atoi(e) != 0; }
    if (all || const_offset == 0x10000 || const_offset < 0x80) return false;  // (0x10000: no constants known; a program
                                                                              //  can be bigger, the effect compute ones are)
    uint64_t brx = ((const uint64_t *)code)[i];
    int ra = (int)bits(brx, 8, 8);
    int64_t imm = sbits(brx, 20, 24);
    if (ra == RZ) return false;
    // the instruction that set ra: an LDC from cb1 within the few before, nothing else writing ra (or a branch) between
    int64_t table = -1;
    int entries = 256;
    for (size_t k = i, seen = 0; k-- > 0 && seen < 32;) {
        if (k % 4 == 0) continue;  // (scheduling words)
        seen++;
        uint64_t w = ((const uint64_t *)code)[k];
        if ((w & 0xfff0000000000000ull) >= 0xe200000000000000ull && (w & 0xfff0000000000000ull) <= 0xe350000000000000ull)
            return false;  // flow control between: not straight-line
        if ((w & 0xfff8000000000000ull) == 0xef90000000000000ull && (int)bits(w, 0, 8) == ra) {  // LDC ra, c[idx][Rx + off]
            if (bits(w, 36, 5) != 1 || bits(w, 48, 3) != 4) return false;  // (cb1, 32-bit)
            table = (int64_t)bits(w, 20, 16);
            if (clamped) entries = brx_table_entries((const uint64_t *)code, k);
            break;
        }
        if ((int)bits(w, 0, 8) == ra) return false;  // (something else may write ra: unknown)
    }
    if (table < 0) return false;
    const uint8_t *tab = blob + const_offset + table;
    uint8_t *mark = calloc(end + 4, 1);
    int n = 0;
    for (int k = 0; k < entries; k++) {
        int32_t v;
        memcpy(&v, tab + 4 * k, 4);
        int64_t t = (int64_t)i * 8 + 8 + v + imm;
        if (t < 8 || (t & 7) || (size_t)(t / 8) >= end) break;
        t = norm_target(t);  // (as the BRX itself does at run time)
        if ((size_t)(t / 8) >= end) break;
        mark[t / 8] = 1;
        n++;
    }
    if (clamped && entries < 256 && n != entries) n = 0;  // (a clamped table whose entries aren't all targets: unknown)
    if (n) for (size_t k = 0; k < end; k++) label[k] |= mark[k];
    free(mark);
    return n > 0;
}

// (GLSL) SSY / PBK stack depths known at translation time. The dispatcher's ssy[16] / pbk[16] arrays are indexed by a
// run-time counter: drivers keep such arrays in registers as 16-way selects for every push and pop (RADV: the big
// effect compute programs hit the 256-VGPR limit with thousands of spilled SGPRs, ~300 KB of code, minutes to build).
// Maxwell's compiler nests SSY/SYNC and PBK/BRK statically, so each instruction sees one depth on every path: then a
// push or pop names its slot (ssy0, ssy1, ...) directly. A BRX goes to its jump table's targets (brx_table_labels).
// Any path with another depth, a BRX without a known table, JMX, a subroutine that isn't inlined, PCNT / CONT or a
// stack deeper than 16 keeps the arrays. LARECOMP_STATIC_STACKS=0: off (the arrays always).
static void static_stacks(Ctx *c, const uint64_t *code, size_t end, const uint8_t *blob, size_t max_size) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_STATIC_STACKS"); on = !e || !*e || atoi(e) != 0; }
    if (!on || end < 2) return;
    // A stack state is a node: the state before its push (np), the pushed target (nt) and the depth (nd); node 0 is
    // the empty stack. Each instruction keeps the node its paths bring (sn: SSY, bn: PBK; -1: not reached).
    enum { NODES_MAX = 4096 };
    int32_t *sn = malloc(end * sizeof *sn), *bn = malloc(end * sizeof *bn), *np = malloc(NODES_MAX * sizeof *np);
    uint32_t *nt = malloc(NODES_MAX * sizeof *nt), *work = malloc(end * sizeof *work);
    int8_t *nd = malloc(NODES_MAX), *sd = malloc(end), *bd = malloc(end);
    uint8_t *brx = NULL;  // (a BRX's targets)
    int nn = 1;
    bool mixed = false;  // (one depth on every path, but different targets: the slots work, structure_flow doesn't)
    if (!sn || !bn || !np || !nt || !work || !nd || !sd || !bd) goto off;
    for (size_t k = 0; k < end; k++) sn[k] = bn[k] = -1;
    np[0] = -1; nt[0] = 0; nd[0] = 0;
    size_t nw = 0;
    int smax = 0, bmax = 0;
#define SS_VISIT(j, s_, b_) do { size_t j_ = (j); int32_t s1_ = (s_), b1_ = (b_); \
        if (j_ % 4 == 0) j_++; \
        if (j_ >= end) break; \
        if (sn[j_] < 0) { sn[j_] = s1_; bn[j_] = b1_; work[nw++] = (uint32_t)j_; } \
        else if (sn[j_] != s1_ || bn[j_] != b1_) { \
            if (nd[sn[j_]] != nd[s1_] || nd[bn[j_]] != nd[b1_]) goto off; \
            mixed = true; } } while (0)
#define SS_PUSH(n_, t_, out_) do { int32_t m_ = -1; \
        for (int q_ = 1; q_ < nn; q_++) if (np[q_] == (n_) && nt[q_] == (t_)) { m_ = q_; break; } \
        if (m_ < 0) { \
            if (nn >= NODES_MAX || nd[n_] >= 16) goto off; \
            m_ = nn++; np[m_] = (n_); nt[m_] = (t_); nd[m_] = (int8_t)(nd[n_] + 1); } \
        (out_) = m_; } while (0)
    SS_VISIT(1, 0, 0);
    while (nw) {
        size_t i = work[--nw];
        uint64_t w = code[i], top12 = w & 0xfff0000000000000ull;
        int32_t s = sn[i], b = bn[i], s2, b2;
        if (nd[s] > smax) smax = nd[s];
        if (nd[b] > bmax) bmax = nd[b];
        int pr = (int)bits(w, 16, 3);
        bool prn = bits(w, 19, 1);
        bool flow_push = top12 == 0xe290000000000000ull || top12 == 0xe2a0000000000000ull || top12 == 0xe2b0000000000000ull ||
                         top12 == 0xe270000000000000ull;
        if (flow_push) pr = PT, prn = false;
        bool never = pr == PT && prn, always = pr == PT && !prn;
        size_t next = i + 1;
        if (never) { SS_VISIT(next, s, b); continue; }
        int64_t t = norm_target((int64_t)i * 8 + 8 + sbits(w, 20, 24));
        size_t ti = t >= 0 && (size_t)(t / 8) < end ? (size_t)(t / 8) : end;
        if (top12 == 0xe300000000000000ull || top12 == 0xe320000000000000ull) {  // EXIT / RET (no CAL: the end)
            if (!always) SS_VISIT(next, s, b);
        } else if (top12 == 0xe240000000000000ull) {  // BRA
            if (w & 0x20) goto off;
            if (ti < end) SS_VISIT(ti, s, b);
            if (!always) SS_VISIT(next, s, b);
        } else if (top12 == 0xe290000000000000ull) {  // SSY
            if (w & 0x20) goto off;
            if (ti < end) SS_VISIT(ti, s, b);  // (where its SYNC goes: the stack as before the push)
            SS_PUSH(s, (uint32_t)t, s2);
            SS_VISIT(next, s2, b);
        } else if (top12 == 0xe2a0000000000000ull) {  // PBK
            if (w & 0x20) goto off;
            if (ti < end) SS_VISIT(ti, s, b);
            SS_PUSH(b, (uint32_t)t, b2);
            SS_VISIT(next, s, b2);
        } else if ((w & 0xfff8000000000000ull) == 0xf0f8000000000000ull) {  // SYNC: on to the SSY's target
            if (s == 0) goto off;
            if (!always) SS_VISIT(next, s, b);
        } else if (top12 == 0xe340000000000000ull) {  // BRK
            if (b == 0) goto off;
            if (!always) SS_VISIT(next, s, b);
        } else if (top12 == 0xe250000000000000ull) {  // BRX: the jump table's targets (others: the dispatcher's exit)
            if (w & 0x20) goto off;
            if (!brx && !(brx = malloc(end + 4))) goto off;
            memset(brx, 0, end + 4);
            if (!brx_marks(c, i, brx)) goto off;
            for (size_t k = 0; k < end; k++)
                if (brx[k]) SS_VISIT(k, s, b);
            if (!always) SS_VISIT(next, s, b);
        } else if (top12 == 0xe260000000000000ull) {  // CAL: only subroutines inlined (no flow of their own)
            if ((w & 0x20) || !cal_inline_ret(c, (uint32_t)t)) goto off;
            SS_VISIT(next, s, b);
        } else if ((w >> 56) == 0xe2 || top12 == 0xe310000000000000ull || top12 == 0xe350000000000000ull) {
            goto off;  // BRX, JMX / JMP / JCAL, PCNT / CONT, PRET, PLONGJMP / LONGJMP: not followed
        } else {
            SS_VISIT(next, s, b);
        }
    }
#undef SS_VISIT
#undef SS_PUSH
    for (size_t k = 0; k < end; k++) {
        sd[k] = sn[k] < 0 ? -1 : nd[sn[k]];
        bd[k] = bn[k] < 0 ? -1 : nd[bn[k]];
    }
    if (!mixed && (c->pop_tgt = malloc(end * sizeof *c->pop_tgt))) {
        for (size_t k = 0; k < end; k++) {  // (SYNC / BRK: where they go)
            uint64_t w = code[k];
            c->pop_tgt[k] = UINT32_MAX;
            if (k % 4 == 0 || sn[k] < 0) continue;
            if ((w & 0xfff8000000000000ull) == 0xf0f8000000000000ull) c->pop_tgt[k] = nt[sn[k]];
            else if ((w & 0xfff0000000000000ull) == 0xe340000000000000ull) c->pop_tgt[k] = nt[bn[k]];
        }
    }
    free(sn); free(bn); free(np); free(nt); free(work); free(nd); free(brx);
    c->ssy_depth = sd; c->pbk_depth = bd;
    c->ssy_max = smax; c->pbk_max = bmax;
    return;
off:
    free(sn); free(bn); free(np); free(nt); free(work); free(nd); free(sd); free(bd); free(brx);
}

// (structure_flow) Subroutines with flow of their own (CALs cal_inline_ret doesn't inline) keep a return stack and
// with it the pc dispatcher (cs 72b45dd400: 8 subroutines, 2.3 MB of GLSL, a Steam Deck lockup). expand_calls lays the
// program out again with a copy of each such subroutine in place of every call: the CAL becomes a BRA past the copy
// with the CAL's predicate inverted (emit_insn has always guarded a CAL with its predicate bits), each RET in the copy
// a BRA past it. Instructions are packed three to a group of four (index % 4 == 0: an empty scheduling word) and every
// relative target is rewritten for the new layout; a BRX keeps its original pc and table (brx_marks, emit_insn map its
// targets through vmap). NULL: nothing to expand, or a subroutine that can't be copied (a branch out of it, calls
// nested too deep, too big a result).
struct VCtx { size_t t, r; int32_t *map; };  // a copy of the instructions t..r (context 0: the whole program)

static size_t vmap(const Ctx *c, int ctx, size_t k) {
    const VCtx *x = &c->vctxs[ctx];
    if (k < x->t || k > x->r || x->map[k - x->t] < 0) return SIZE_MAX;
    return (size_t)x->map[k - x->t];
}

static size_t vidx(size_t e) { return e / 3 * 4 + e % 3 + 1; }  // entry -> index (three instructions per group)

// the subroutine at t: up to the unconditional RET past every branch target inside it; 0: not one that can be copied
static size_t sub_end(const uint64_t *code, size_t end, size_t t) {
    size_t reach = t;
    for (size_t k = t; k < end; k++) {
        if (k % 4 == 0) continue;
        uint64_t x = code[k], top12 = x & 0xfff0000000000000ull;
        if (top12 == 0xe240000000000000ull || top12 == 0xe290000000000000ull || top12 == 0xe2a0000000000000ull) {
            size_t bt = norm_target((int64_t)k * 8 + 8 + sbits(x, 20, 24)) / 8;
            if (bt < t) return 0;  // (out of the subroutine, backwards)
            if (bt > reach) reach = bt;
        } else if ((x >> 56) == 0xe2 && top12 != 0xe250000000000000ull && top12 != 0xe260000000000000ull) {
            return 0;  // (JMX / JMP / JCAL, PRET, PLONGJMP, PCNT)
        } else if (top12 == 0xe310000000000000ull || top12 == 0xe350000000000000ull) {
            return 0;  // (LONGJMP, CONT)
        }
        if (top12 == 0xe320000000000000ull && bits(x, 16, 4) == PT && k >= reach) return k;
    }
    return 0;
}

typedef struct { uint64_t w; uint32_t orig; uint16_t ctx; uint8_t kind; uint32_t fix; } VEnt;  // kind 1: BRA to entry fix
typedef struct { const uint64_t *code; size_t end; VEnt *e; size_t n, cap; VCtx *x; int nx, capx; bool bad; } VLay;

static void vlay_put(VLay *l, VEnt v) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4096;
        if (l->cap > 196608) { l->bad = true; return; }
        VEnt *e = realloc(l->e, l->cap * sizeof *e);
        if (!e) { l->bad = true; return; }
        l->e = e;
    }
    l->e[l->n++] = v;
}

static void vlay_region(Ctx *c, VLay *l, size_t t, size_t r, int depth) {
    if (l->nx == l->capx) {
        l->capx = l->capx ? l->capx * 2 : 64;
        VCtx *x = realloc(l->x, l->capx * sizeof *x);
        if (!x) { l->bad = true; return; }
        l->x = x;
    }
    int ctx = l->nx++;
    l->x[ctx] = (VCtx){.t = t, .r = r, .map = malloc((r - t + 1) * sizeof(int32_t))};
    if (!l->x[ctx].map || ctx > 65535) { l->bad = true; return; }
    for (size_t k = t; k <= r; k++) l->x[ctx].map[k - t] = -1;
    size_t first_ret_fix = l->n;  // (this region's RETs, fixed up below: past the copy)
    for (size_t k = t; k <= r && !l->bad; k++) {
        if (k % 4 == 0) continue;
        uint64_t w = l->code[k];
        l->x[ctx].map[k - t] = (int32_t)l->n;  // (the entry number for now; an index below)
        if ((w & 0xfff0000000000020ull) == 0xe260000000000000ull) {  // CAL
            size_t st = norm_target((int64_t)k * 8 + 8 + sbits(w, 20, 24)) / 8;
            if (st >= l->end) { l->bad = true; return; }
            if (!cal_inline_ret(c, (uint32_t)(st * 8))) {
                size_t sr = sub_end(l->code, l->end, st);
                if (!sr || depth >= 8) { l->bad = true; return; }
                size_t at = l->n;
                vlay_put(l, (VEnt){.w = w ^ 0x80000ull, .orig = (uint32_t)k, .ctx = (uint16_t)ctx, .kind = 1});  // @!P BRA past
                vlay_region(c, l, st, sr, depth + 1);
                if (l->bad) return;
                l->e[at].fix = (uint32_t)l->n;
                continue;
            }
        }
        if (ctx && (w & 0xfff0000000000000ull) == 0xe320000000000000ull)  // RET in a copy: BRA past it (fixed up below)
            vlay_put(l, (VEnt){.w = w, .orig = (uint32_t)k, .ctx = (uint16_t)ctx, .kind = 2});
        else
            vlay_put(l, (VEnt){.w = w, .orig = (uint32_t)k, .ctx = (uint16_t)ctx});
    }
    if (ctx)
        for (size_t e = first_ret_fix; e < l->n; e++)
            if (l->e[e].kind == 2 && l->e[e].ctx == ctx) { l->e[e].kind = 1; l->e[e].fix = (uint32_t)l->n; }
}

static uint64_t with_target(uint64_t w, size_t at, size_t target) {
    int64_t off = (int64_t)target * 8 - ((int64_t)at * 8 + 8);
    return (w & ~(0xffffffull << 20)) | (((uint64_t)off & 0xffffffull) << 20);
}

static uint64_t *expand_calls(Ctx *c, const uint64_t *code, size_t end, size_t *vend) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_STRUCTURED"); on = !e || !*e || atoi(e) != 0; }
    if (!on) return NULL;
    bool any = false;
    for (size_t k = 1; k < end && !any; k++)
        if (k % 4 && (code[k] & 0xfff0000000000020ull) == 0xe260000000000000ull &&
            !cal_inline_ret(c, norm_target((int64_t)k * 8 + 8 + sbits(code[k], 20, 24))))
            any = true;
    if (!any) return NULL;
    VLay l = {.code = code, .end = end};
    vlay_region(c, &l, 0, end - 1, 0);
    uint64_t *v = NULL;
    size_t n = l.n ? vidx(l.n - 1) + 1 : 0;
    if (l.bad || !n || !(v = calloc(n, sizeof *v)) || !(c->vorig = calloc(n, sizeof *c->vorig)) ||
        !(c->vctx = calloc(n, sizeof *c->vctx)))
        goto fail;
    for (int x = 0; x < l.nx; x++)  // (entry numbers -> indices)
        for (size_t k = l.x[x].t; k <= l.x[x].r; k++)
            if (l.x[x].map[k - l.x[x].t] >= 0) l.x[x].map[k - l.x[x].t] = (int32_t)vidx((size_t)l.x[x].map[k - l.x[x].t]);
    c->vctxs = l.x; c->nvctx = l.nx;
    for (size_t e = 0; e < l.n; e++) {
        VEnt *x = &l.e[e];
        size_t i = vidx(e);
        uint64_t w = x->w, top12 = w & 0xfff0000000000000ull;
        c->vorig[i] = x->orig; c->vctx[i] = x->ctx;
        if (x->kind == 1) {  // (BRA past a copy, predicate bits kept)
            w = with_target(0xe24000000000000full | (w & 0xf0000ull), i, x->fix < l.n ? vidx(x->fix) : n);
        } else if (((top12 == 0xe240000000000000ull || top12 == 0xe290000000000000ull || top12 == 0xe2a0000000000000ull) &&
                    !(w & 0x20)) || (top12 == 0xe260000000000000ull && !(w & 0x20))) {  // BRA / SSY / PBK / CAL (inlined)
            size_t ot = norm_target((int64_t)x->orig * 8 + 8 + sbits(w, 20, 24)) / 8;
            size_t nt = top12 == 0xe260000000000000ull ? vmap(c, 0, ot) : vmap(c, x->ctx, ot);
            if (nt == SIZE_MAX) {
                if (x->ctx || ot < end) goto fail;  // (out of a copy; into a gap)
                nt = n;  // (past the end of the program: its end, as before)
            }
            w = with_target(w, i, nt);
        }
        v[i] = w;
    }
    free(l.e);
    c->ocode = (const uint8_t *)code; c->oend = end;
    *vend = n;
    return v;
fail:
    for (int x = 0; x < l.nx; x++) free(l.x[x].map);
    free(l.x); free(l.e); free(v); free(c->vorig); free(c->vctx);
    c->vorig = NULL; c->vctx = NULL; c->vctxs = NULL; c->nvctx = 0;
    return NULL;
}

static void expand_free(Ctx *c) {
    for (int x = 0; x < c->nvctx; x++) free(c->vctxs[x].map);
    free(c->vctxs); free(c->vorig); free(c->vctx); free(c->vcode);
    c->vctxs = NULL; c->vorig = NULL; c->vctx = NULL; c->vcode = NULL; c->nvctx = 0;
}

// A BRX's targets as indices of the program being translated (expand_calls: through its original pc and vmap).
static bool brx_marks(Ctx *c, size_t i, uint8_t *mark) {
    if (!c->vorig) return brx_table_labels(c->code, i, c->ninsts, c->blob, c->blob_size, mark, true);
    uint8_t *om = calloc(c->oend + 4, 1);
    bool ok = om && brx_table_labels(c->ocode, c->vorig[i], c->oend, c->blob, c->blob_size, om, true);
    for (size_t k = 0; ok && k < c->oend; k++)
        if (om[k]) {
            size_t nk = vmap(c, c->vctx[i], k);
            if (nk == SIZE_MAX) ok = false;
            else mark[nk] = 1;
        }
    free(om);
    return ok;
}

// (GLSL compute) Structured control flow. The pc dispatcher (switch in a loop) makes every register a value carried
// around the loop into every case: the driver keeps all of them live everywhere (the big effect compute programs:
// 256 VGPRs, thousands of spills, still slow after static_stacks). With every SYNC / BRK target known (static_stacks:
// one stack content on every path), each jump has a fixed target, and the program is straight-line code with gotos.
// A forward goto to L becomes a break out of a do { } while (false) block that ends at L, a backward one a continue
// of a for (;;) loop that starts at L. Blocks may start earlier and loops end later until all of them nest; a jump
// into the middle of a loop doesn't, and keeps the dispatcher. A jump out of more than one construct sets jmp_ and
// breaks; each construct's end passes it on (structure_close). LARECOMP_STRUCTURED=0: off.
struct Construct { uint32_t start, stop; uint8_t loop, clear, cont, prop; };  // [start, stop) of the instructions

static size_t next_insn(size_t i) { i++; return i % 4 == 0 ? i + 1 : i; }

static void structure_flow(Ctx *c, const uint64_t *code, size_t end) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_STRUCTURED"); on = !e || !*e || atoi(e) != 0; }
    if (!on || !c->pop_tgt || !c->ssy_depth) return;  // (every stage: structure_exit ends a vertex / fragment one)
    uint32_t stop_end = (uint32_t)end + 1;  // (constructs that last to the end of the program)
    uint32_t *fwd = malloc((end + 2) * sizeof *fwd), *back = malloc((end + 2) * sizeof *back);
    uint8_t *mark = malloc(end + 4);
    Construct *cs = NULL;
    int n = 0;
    if (!fwd || !back || !mark) goto off;
    for (size_t k = 0; k < end + 2; k++) fwd[k] = UINT32_MAX, back[k] = 0;
    // the jumps: forward ones by target (the first source), backward ones by target (the last source + 1)
#define SF_JUMP(src_, l_) do { size_t s_ = (src_), l1_ = (l_); \
        if (l1_ % 4 == 0) l1_++; \
        if (l1_ >= end) break; \
        if (l1_ > s_) { if (s_ < fwd[l1_]) fwd[l1_] = (uint32_t)s_; } \
        else { size_t e_ = next_insn(s_); if (e_ > back[l1_]) back[l1_] = (uint32_t)e_; } } while (0)
    for (size_t i = 1; i < end; i++) {
        if (i % 4 == 0 || c->ssy_depth[i] < 0) continue;
        uint64_t w = code[i], top12 = w & 0xfff0000000000000ull;
        int pr = (int)bits(w, 16, 3);
        if (pr == PT && bits(w, 19, 1) && top12 != 0xe290000000000000ull && top12 != 0xe2a0000000000000ull) continue;
        int64_t t = norm_target((int64_t)i * 8 + 8 + sbits(w, 20, 24));
        if (top12 == 0xe240000000000000ull) {  // BRA
            if (t >= 0) SF_JUMP(i, (size_t)(t / 8));
        } else if ((w & 0xfff8000000000000ull) == 0xf0f8000000000000ull || top12 == 0xe340000000000000ull) {  // SYNC, BRK
            if (c->pop_tgt[i] == UINT32_MAX) goto off;
            SF_JUMP(i, c->pop_tgt[i] / 8);
        } else if (top12 == 0xe250000000000000ull) {  // BRX
            memset(mark, 0, end + 4);
            if (!brx_marks(c, i, mark)) goto off;
            for (size_t k = 0; k < end; k++)
                if (mark[k]) SF_JUMP(i, k);
        }
    }
#undef SF_JUMP
    int cap = 0;
    for (size_t l = 1; l < end; l++) cap += (fwd[l] != UINT32_MAX) + (back[l] != 0);
    if (cap > 8192 || !(cs = calloc(cap ? cap : 1, sizeof *cs))) goto off;
    for (size_t l = 1; l < end; l++) {
        if (fwd[l] != UINT32_MAX) cs[n++] = (Construct){.start = fwd[l], .stop = (uint32_t)l};
        if (back[l]) cs[n++] = (Construct){.start = (uint32_t)l, .stop = back[l] >= end ? stop_end : back[l], .loop = 1};
    }
    // nest: a block crossing another starts earlier (around it); a loop crossing a later one ends later
    for (int pass = 0, changed = 1; changed; pass++) {
        if (pass > 2000) goto off;
        changed = 0;
        for (int a = 0; a < n; a++)
            for (int b = 0; b < n; b++) {
                Construct *x = &cs[a], *y = &cs[b];
                if (!(x->start < y->start && y->start < x->stop && x->stop < y->stop)) continue;  // (x opens first)
                if (!y->loop) y->start = x->start;
                else if (x->loop) x->stop = y->stop;
                else goto off;  // (a block from before a loop to inside it: a jump into the loop)
                changed = 1;
            }
    }
    // opening order: by start, the outer (later stop) first, a loop around a block of the same span
    for (int a = 1; a < n; a++) {
        Construct t = cs[a];
        int b = a - 1;
        while (b >= 0 && (cs[b].start > t.start || (cs[b].start == t.start &&
                          (cs[b].stop < t.stop || (cs[b].stop == t.stop && !cs[b].loop && t.loop))))) {
            cs[b + 1] = cs[b];
            b--;
        }
        cs[b + 1] = t;
    }
    // the nesting depth (and a check that equal spans or shared ends still nest)
    int depth = 0, maxd = 0, *stk = malloc((n + 1) * sizeof *stk);
    if (!stk) goto off;
    for (int a = 0; a < n; a++) {
        while (depth && cs[stk[depth - 1]].stop <= cs[a].start) depth--;
        if (depth && cs[a].stop > cs[stk[depth - 1]].stop) { free(stk); goto off; }
        stk[depth++] = a;
        if (depth > maxd) maxd = depth;
    }
    free(stk);
    if (maxd > 200) goto off;
    c->cons_at = calloc(2 * (end + 2), sizeof *c->cons_at);  // [2 l]: the block ending at l, [2 l + 1]: the loop at l
    c->stack = malloc((n + 1) * sizeof *c->stack);
    if (!c->cons_at || !c->stack) { free(c->cons_at); free(c->stack); c->cons_at = NULL; c->stack = NULL; goto off; }
    for (int a = 0; a < n; a++) {
        if (cs[a].loop) c->cons_at[2 * cs[a].start + 1] = a + 1;
        else c->cons_at[2 * cs[a].stop] = a + 1;
    }
    c->cons = cs; c->ncons = n; c->nstack = 0;
    c->structured = true;
    free(fwd); free(back); free(mark);
    return;
off:
    free(fwd); free(back); free(mark); free(cs);
}

// (structure_flow) A jump from the current position to instruction l (>= end: the program's end).
static void structure_goto(Ctx *c, size_t from, size_t l) {
    if (l % 4 == 0) l++;
    if (l >= c->ninsts) { structure_exit(c); return; }
    int id = c->cons_at[2 * l + (l <= from)];
    int k = c->nstack;
    while (k-- > 0 && c->stack[k] != id - 1) {}
    if (!id || k < 0) {  // (not around this jump: can't be, structure_flow nests them)
        fprintf(stderr, "[shader] structured flow: no construct for a jump %zx -> %zx\n", from * 8, l * 8);
        structure_exit(c);
        return;
    }
    Construct *t = &c->cons[id - 1];
    if (k == c->nstack - 1) { sp(&c->body, t->loop ? "    continue;\n" : "    break;\n"); return; }
    for (int j = k + 1; j < c->nstack; j++) {
        Construct *x = &c->cons[c->stack[j]];
        if (j == k + 1 && t->loop) x->cont = 1;
        else x->prop = 1;
    }
    if (!t->loop) t->clear = 1;
    sp(&c->body, "    jmp_ = %du; break;\n", id);
}

// The program's end: a compute shader returns; a vertex / fragment one leaves every construct and the body's
// do { } while (false), on to its outputs (glsl_finish): jmp_ set to a value no construct has, passed on by each.
static void structure_exit(Ctx *c) {
    if (c->stage == STAGE_COMPUTE) { sp(&c->body, "    return;\n"); return; }
    if (!c->nstack) { sp(&c->body, "    break;\n"); return; }
    for (int j = 0; j < c->nstack; j++) c->cons[c->stack[j]].prop = 1;
    sp(&c->body, "    jmp_ = 0xffffffffu; break;\n");
}

static void structure_open(Ctx *c, int a) {
    c->stack[c->nstack++] = a;
    if (c->cons[a].loop) sp(&c->body, "  for (;;) {\n  if (++guard_ > 100000u) return;\n");
    else sp(&c->body, "  do {\n");
}

static void structure_close(Ctx *c) {
    Construct *x = &c->cons[c->stack[--c->nstack]];
    sp(&c->body, x->loop ? "  break;\n  }\n" : "  } while (false);\n");
    if (x->clear) sp(&c->body, "  if (jmp_ == %du) jmp_ = 0u;\n", (int)(x - c->cons) + 1);
    if (x->cont) sp(&c->body, "  if (jmp_ == %du) { jmp_ = 0u; continue; }\n", c->stack[c->nstack - 1] + 1);
    if (x->prop) sp(&c->body, "  if (jmp_ != 0u) break;\n");
}

ShaderInfo *maxwell_translate(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key) {
    ShaderInfo *info = calloc(1, sizeof *info);
    info->stage = stage;
    Ctx *c = calloc(1, sizeof *c);
    c->info = info;
    c->key = key;
    c->stage = stage;
    c->ipa_mode = -1;
    c->glsl = key && key->glsl;
    const uint8_t *sph = blob + 0x30;
    const uint8_t *code = blob + 0x80;
    // compute programs have neither the 0x30-byte NVN header nor an SPH: code starts at the blob
    if (stage == STAGE_COMPUTE) code = blob;
    if (stage == STAGE_FRAGMENT) {
        c->omap_target = *(const uint32_t *)(sph + 0x48);
        c->omap_depth = (*(const uint32_t *)(sph + 0x4C) >> 1) & 1;
    }
    // find the end: the last EXIT followed by self-branch padding, or max_size
    size_t n = (max_size - (code - blob)) / 8;
    if (n > 65536) n = 65536;
    size_t end = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t w = ((const uint64_t *)code)[i];
        if (i % 4 == 0) continue;
        if ((w & 0xfff0000000000000ull) == 0xe240000000000000ull && sbits(w, 20, 24) == -8 && end) break;  // bra self
        if (w == 0 && i > 4) break;
        end = i + 1;
    }
    c->code = code;
    c->ninsts = end;
    // collect branch targets (labels)
    uint8_t *label = calloc(end + 4, 1);
    label[1] = 1;
    bool any_branch = false;
    for (size_t i = 0; i < end; i++) {
        if (i % 4 == 0) continue;
        uint64_t w = ((const uint64_t *)code)[i];
        uint32_t pc = (uint32_t)(i * 8);
        bool is_flow = (w & 0xfff0000000000020ull) == 0xe240000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe290000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe2a0000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe2b0000000000000ull;
        if (is_flow) {
            int64_t t = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
            if (t >= 0 && (size_t)(t / 8) < end) label[t / 8] = 1;
            any_branch = true;
        }
        if ((w & 0xfff0000000000020ull) == 0xe260000000000000ull &&  // CAL: the subroutine and the return address
            !cal_inline_ret(c, norm_target((int64_t)pc + 8 + sbits(w, 20, 24)))) {
            int64_t t = norm_target((int64_t)pc + 8 + sbits(w, 20, 24)), r = norm_target((int64_t)pc + 8);
            if (t >= 0 && (size_t)(t / 8) < end) label[t / 8] = 1;
            if ((size_t)(r / 8) < end) label[r / 8] = 1;
            any_branch = true;
        }
        if ((w & 0xfff0000000000000ull) == 0xe250000000000000ull) {  // BRX: its jump table's targets, else any instruction
            any_branch = true;
            if (!c->glsl || !brx_table_labels(code, i, end, blob, max_size, label, false)) memset(label, 1, end);
        }
        if ((w & 0xfff8000000000000ull) == 0xf0f8000000000000ull || (w & 0xfff0000000000000ull) == 0xe340000000000000ull ||
            (w & 0xfff0000000000000ull) == 0xe350000000000000ull)
            any_branch = true;
        (void)is_branch_target_op;
    }
    // translate
    bool dbg = maxwell_dbg_code && blob == maxwell_dbg_code && stage == STAGE_FRAGMENT;
    static int annotate = -1;
    if (annotate < 0) annotate = getenv("LARECOMP_SAVE_MSL") != NULL;
    c->branchy = any_branch;
    c->blob = blob; c->blob_size = max_size;
    const uint64_t *sc = (const uint64_t *)code;  // (structure_flow: the program, its subroutines copied in: expand_calls)
    size_t send = end;
    if (c->glsl && any_branch && stage == STAGE_COMPUTE && (c->vcode = expand_calls(c, sc, end, &send))) {
        sc = c->vcode;
        c->code = (const uint8_t *)sc; c->ninsts = send;
        static_stacks(c, sc, send, blob, max_size);
        structure_flow(c, sc, send);
        if (!c->structured) {  // (back to the program as it is, for the dispatcher)
            free(c->ssy_depth); free(c->pbk_depth); free(c->pop_tgt);
            c->ssy_depth = c->pbk_depth = NULL; c->pop_tgt = NULL;
            expand_free(c);
            sc = (const uint64_t *)code; send = end;
            c->code = code; c->ninsts = end;
        }
    }
    if (c->glsl && any_branch && !c->structured) static_stacks(c, (const uint64_t *)code, end, blob, max_size);
    if (c->glsl && any_branch && !c->structured) structure_flow(c, (const uint64_t *)code, end);
    if (c->structured) {  // (structure_flow: the instructions in order, the constructs around them; unreached ones left out)
        for (size_t i = 1, oi = 0; i < send && (!c->fail || survey_on()); i++) {
            if (i % 4 == 0) continue;
            while (c->nstack && c->cons[c->stack[c->nstack - 1]].stop == i) structure_close(c);
            while (oi < (size_t)c->ncons && c->cons[oi].start == i) structure_open(c, (int)oi++);
            uint32_t pc = (uint32_t)(i * 8);
            if (c->ssy_depth[i] >= 0) {
                if (annotate) sp(&c->body, "    // pc %x: %016llx\n", pc, (unsigned long long)sc[i]);
                emit_insn(c, sc[i], pc);
                light_tiles_fix(c, sc, send, pc);
            }
            if (i < end) info->num_insts++;
        }
        while (c->nstack) structure_close(c);
    }
    for (size_t i = 0; i < end && !c->structured && (!c->fail || survey_on()); i++) {
        if (i % 4 == 0) continue;
        uint32_t pc = (uint32_t)(i * 8);
        if (any_branch && label[i]) sp(&c->body, "  case %uu:\n", pc);
        if (annotate) sp(&c->body, "    // pc %x: %016llx\n", pc, (unsigned long long)((const uint64_t *)code)[i]);
        emit_insn(c, ((const uint64_t *)code)[i], pc);
        light_tiles_fix(c, (const uint64_t *)code, end, pc);
        if (dbg && pc == maxwell_dbg_pc) {
            int rr = maxwell_dbg_reg % 1000, half = maxwell_dbg_reg / 1000;  // +1000: low f16, +2000: high f16
            c->reg_used[rr] = 1;
            if (half) sp(&c->body, c->glsl ? "    dbgv = unpackHalf2x16(r%d)[%d];\n" : "    dbgv = float(as_type<half2>(r%d)[%d]);\n", rr, half - 1);
            else sp(&c->body, "    dbgv = as_type<float>(r%d);\n", rr);
        }
        info->num_insts++;
    }
    free(label);
    bool static_slots = c->ssy_depth != NULL;  // (glsl_finish declares the slots: ssy_max / pbk_max)
    free(c->ssy_depth); free(c->pbk_depth); free(c->pop_tgt); free(c->cons); free(c->cons_at); free(c->stack);
    c->ssy_depth = c->pbk_depth = NULL; c->pop_tgt = NULL; c->cons = NULL; c->cons_at = c->stack = NULL;
    expand_free(c);
    c->code = code; c->ninsts = end;
    // A compute program left as a big pc dispatcher can take the driver minutes to build and the GPU long enough to hang
    // the device (cs 72b45dd400 before expand_calls: 2.3 MB, a Steam Deck lockup). Such a program fails here: its
    // dispatches are skipped. LARECOMP_MAX_DISPATCHER_KB (default 1600; 0: no limit).
    if (c->glsl && stage == STAGE_COMPUTE && any_branch && !c->structured && !c->fail) {
        static long max_kb = -1;
        if (max_kb < 0) { const char *e = getenv("LARECOMP_MAX_DISPATCHER_KB"); max_kb = e && *e ? atol(e) : 1600; }
        if (max_kb > 0 && c->body.len > (size_t)max_kb * 1024)
            fail(c, "compute dispatcher too large (%zu KB, LARECOMP_MAX_DISPATCHER_KB=%ld)", c->body.len / 1024, max_kb);
    }
    c->ssy_max = static_slots ? c->ssy_max + 1 : -1;  // (slots to declare; -1: the arrays)
    c->pbk_max = static_slots ? c->pbk_max + 1 : -1;
    // (MSL: more textures than sampler slots take a slot table, at a buffer index the vertex stage's streams use)
    if (!c->glsl && stage == STAGE_VERTEX && info->ntex > MSL_MAX_SAMPLERS)
        fail(c, "%d textures in a vertex shader (Metal: %d samplers)", info->ntex, MSL_MAX_SAMPLERS);
    if (c->fail) { free(c->body.buf); free(c); return info; }
    if (c->glsl) return glsl_finish(c, any_branch, dbg);

    // ---------------------------------------------------------- MSL source
    Str s = {0};
    sp(&s, "#include <metal_stdlib>\nusing namespace metal;\n");
    sp(&s, "static inline uint cbr(constant uint4 *cb, uint off) { return cb[off >> 4][(off >> 2) & 3]; }\n");
    sp(&s, "static inline uint bfe_u(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return 0u; return extract_bits(v, pos, min(len, 32u - pos)); }\n");
    sp(&s, "static inline uint bfe_s(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return (int(v) < 0) ? 0xFFFFFFFFu : 0u; return uint(extract_bits(int(v), pos, min(len, 32u - pos))); }\n");
    sp(&s, "static inline uint bfi(uint ins, uint ctl, uint base) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u || pos >= 32u) return base; return insert_bits(base, ins, pos, min(len, 32u - pos)); }\n");
    sp(&s, "static inline float fswzadd(float a, float b, uint swz, uint lane) { uint m = (swz >> ((lane & 3u) * 2u)) & 3u; float ma = m == 1u ? -1.0f : (m == 3u ? 0.0f : 1.0f); float mb = m == 2u ? -1.0f : 1.0f; return a * ma + b * mb; }\n");
    sp(&s, "static inline uint2 rsc(uint2 c, float4 s) { return (s.x == 1.0f && s.y == 1.0f) ? c : uint2(floor((float2(c) + 0.5f) * s.xy)); }\n");
    sp(&s, "static inline float2 uvs(float2 uv, float4 s) { return (s.z == 1.0f && s.w == 1.0f) ? uv : min(uv, 1.0f) * s.zw; }\n");
    sp(&s, "static inline float2 hx(uint v, uint swz) { if (swz == 1u) return float2(as_type<float>(v)); float2 h = float2(as_type<half2>(v)); return swz == 2u ? h.xx : swz == 3u ? h.yy : h; }\n");
    // merges on half2 lanes rather than with integer masks: on an M1 (macOS 15.0.1) the Metal compiler miscompiled
    // (old & 0xFFFF0000u) | lo in Link's body shader - the merged low half read back as a constant 512.0 and Link
    // rendered white. Same bits either way.
    sp(&s, "static inline uint hpack(float2 r, uint merge, uint old) { if (merge == 1u) return as_type<uint>(r.x); half2 o = as_type<half2>(old), n = half2(r); if (merge == 2u) o.x = n.x; else if (merge == 3u) o.y = n.y; else o = n; return as_type<uint>(o); }\n");
    sp(&s, "static inline uint prmt(uint a, uint b, uint sel) { uint r = 0; for (uint i = 0; i < 4; i++) { uint s = (sel >> (i * 4)) & 7u; uint byte = s < 4 ? (a >> (s * 8)) & 0xFFu : (b >> ((s - 4) * 8)) & 0xFFu; r |= byte << (i * 8); } return r; }\n");

    // inputs/outputs
    if (stage == STAGE_VERTEX) {
        sp(&s, "struct VIn {\n");
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            int kind = key ? key->attr_kind[i] : 0;
            sp(&s, "  %s4 a%d [[attribute(%d)]];\n", kind == 1 ? "int" : kind == 2 ? "uint" : "float", i, i);
        }
        sp(&s, "};\n");
        sp(&s, "struct VOut {\n  float4 pos [[position, invariant]];\n");  // depth pre-pass + EQUAL test needs identical positions
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  float4 v%d [[user(locn%d)]];\n", i, i);
        if (info->writes_point_size) sp(&s, "  float psize [[point_size]];\n");
        sp(&s, "};\n");
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "struct FIn {\n  float4 pos [[position]];\n");
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            const char *interp = (c->flat_mask & (1u << i)) ? " [[flat]]" : (c->noperspective_mask & (1u << i)) ? " [[center_no_perspective]]" : "";
            sp(&s, "  float4 v%d [[user(locn%d)]]%s;\n", i, i, interp);
        }
        sp(&s, "};\n");
        sp(&s, "struct FOut {\n");
        for (int rt = 0; rt < 8; rt++) {
            if (!((c->omap_target >> (rt * 4)) & 0xF)) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            sp(&s, "  %s4 c%d [[color(%d)]];\n", kind == 1 ? "int" : kind == 2 ? "uint" : "float", rt, rt);
        }
        if (c->omap_depth) { sp(&s, "  float depth [[depth(any)]];\n"); info->writes_depth = true; }
        sp(&s, "};\n");
    }

    // entry point
    Str args = {0};
    for (int i = 0; i < 18; i++)
        if (info->cbuf_mask & (1u << i)) sp(&args, ", constant uint4 *cb%d [[buffer(%d)]]", i, i);
    if (info->uses_gmem) {
        if (info->cbuf_mask & (1u << GMEM_BUFFER_INDEX)) {
            free(args.buf);
            args = (Str){0};  // (was a use after free + double free of args.buf)
            fail(c, "cbuf %d clashes with gmem", GMEM_BUFFER_INDEX);
        }
        sp(&args, ", device uchar *gmem [[buffer(%d)]]", GMEM_BUFFER_INDEX);
    }
    if (c->uses_rscale && stage != STAGE_VERTEX) {
        sp(&args, ", constant float4 *rscale [[buffer(18)]]");
        info->uses_rscale = true;
    }
    // Runtime highlight (e.g. golden unchecked pots): fragment shaders writing a float color target 0 get a
    // per-draw tint, float4 [0] = colour.rgb + strength, [1] = added glow.rgb (strength 0 = untouched; < 0: the
    // Cosmetics recolour, emit_recolour).
    bool tint = stage == STAGE_FRAGMENT && !dbg && (c->omap_target & 0x7) == 0x7 && (!key || key->rt_kind[0] == 0);
    if (tint) {
        sp(&args, ", constant float4 *ltint [[buffer(19)]]");
        info->uses_tint = true;
    }
    // Metal has 16 sampler slots a stage (textures: 31). Up to 16 textures, texture i samples with sampler i. With more
    // (issue #215: a cave floor's fragment shader with 17), the samplers come as one array of the 16 slots and a table
    // (buffer SMAP_BUFFER_INDEX) names each texture's slot: the renderer puts each distinct sampler state the draw uses
    // in a slot of its own, so every texture keeps its own filtering and wrapping (renderer.m sampler_slots).
    bool smap = info->ntex > MSL_MAX_SAMPLERS;  // (not in vertex shaders: refused above)
    if (smap) sp(&args, ", constant uint *smap [[buffer(%d)]], array<sampler, %d> ss [[sampler(0)]]", SMAP_BUFFER_INDEX,
                 MSL_MAX_SAMPLERS);
    for (int i = 0; i < info->ntex; i++) {
        ShaderTexture *t = &info->tex[i];
        const char *tn = tex_type_name(t->type);
        sp(&args, ", %s%s<float> t%d [[texture(%d)]]", t->shadow ? "depth" : "texture", tn, i, i);
        if (!smap) sp(&args, ", sampler s%d [[sampler(%d)]]", i, i);
    }
    if (stage == STAGE_VERTEX) {
        sp(&s, "vertex VOut main0(VIn in [[stage_in]], uint vid [[vertex_id]], uint iid [[instance_id]]%s%s) {\n",
           (maxwell_dbg_vs && blob == maxwell_dbg_vs) ? ", device float4 *dbgout [[buffer(29)]]" : "",
           args.buf ? args.buf : "");
        sp(&s, "  VOut out = {};\n  out.pos = float4(0, 0, 0, 1);\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  out.v%d = float4(0, 0, 0, 1);\n", i);
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "fragment FOut main0(FIn in [[stage_in]], bool front_facing [[front_facing]]%s%s) {\n",
           c->uses_lane ? ", uint lane [[thread_index_in_simdgroup]]" : "", args.buf ? args.buf : "");
    } else {
        sp(&s, "kernel void main0(uint3 tid [[thread_position_in_threadgroup]], uint3 ctaid [[threadgroup_position_in_grid]]%s) {\n",
           args.buf ? args.buf : "");
    }
    free(args.buf);
    if (smap)
        for (int i = 0; i < info->ntex; i++) sp(&s, "  sampler s%d = ss[smap[%d]];\n", i, i);
    for (int i = 0; i < 256; i++)
        if (c->reg_used[i]) sp(&s, "  uint r%d = 0;\n", i);
    for (int i = 0; i < 7; i++)
        if (c->pred_used[i]) sp(&s, "  bool p%d = false;\n", i);
    if (c->uses_cc) sp(&s, "  uint cc = 0; bool ccz = false, ccs = false;\n");
    if (c->uses_local) sp(&s, "  uint lmem[%d] = {};\n", LMEM_WORDS);
    if (dbg) sp(&s, maxwell_dbg_pc == 0xffffffffu ? "  float dbgv = 1.0f;\n" : "  float dbgv = -1.0f;\n");
    if (any_branch) {
        sp(&s, "  uint pc = 8u; uint ssy[16]; uint ssy_n = 0; uint pbk[16]; uint pbk_n = 0; uint pcnt[16]; uint pcnt_n = 0;\n");
        sp(&s, "  uint cst[16]; uint cst_n = 0;\n");
        sp(&s, "  bool exit_now = false;\n");
        sp(&s, "  for (uint guard = 0; guard < 100000u; guard++) {\n  switch (pc) {\n");
        sp(&s, "%s", c->body.buf ? c->body.buf : "");
        sp(&s, "    exit_now = true; break;\n  default: exit_now = true; break;\n  }\n  if (exit_now) break;\n  }\n");
    } else {
        sp(&s, "  do {\n%s  } while (false);\n", c->body.buf ? c->body.buf : "");
    }
    if (stage == STAGE_VERTEX) {
        if (key && key->flip_y) sp(&s, "  out.pos.y = -out.pos.y;\n");
        if (key && key->flip_x) sp(&s, "  out.pos.x = -out.pos.x;\n");
        if (key && key->depth_minus_one) sp(&s, "  out.pos.z = (out.pos.z + out.pos.w) * 0.5f;\n");
        if (maxwell_dbg_vs && blob == maxwell_dbg_vs) sp(&s, "  if (iid < 64u && vid < 8u) { dbgout[iid * 8u + vid] = out.pos; dbgout[512u + iid * 8u + vid] = %s; }\n", (c->out_attr_mask & 2u) ? "out.v1" : "float4(0)");
        sp(&s, "  return out;\n}\n");
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "  FOut o;\n");
        int r = 0;
        for (int rt = 0; rt < 8; rt++) {
            uint32_t m = (c->omap_target >> (rt * 4)) & 0xF;
            if (!m) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            for (int comp = 0; comp < 4; comp++) {
                if (!(m & (1u << comp))) continue;
                if (dbg) {
                    sp(&s, comp == 3 ? "  o.c%d[%d] = 1.0f;\n" : "  o.c%d[%d] = dbgv;\n", rt, comp);
                } else if (r < 256 && c->reg_used[r]) {
                    if (kind == 0) sp(&s, "  o.c%d[%d] = as_type<float>(r%d);\n", rt, comp, r);
                    else sp(&s, "  o.c%d[%d] = r%d;\n", rt, comp, r);
                } else {
                    sp(&s, "  o.c%d[%d] = 0;\n", rt, comp);
                }
                r++;
            }
        }
        if (tint)
            sp(&s, "  if (ltint[0].w > 0.0f) { float3 g = o.c0.rgb; float l = dot(g, float3(0.299f, 0.587f, 0.114f));\n"
                   "    o.c0.rgb = mix(g, ltint[0].rgb * pow(l, ltint[1].w) + ltint[1].rgb * l, ltint[0].w); }\n");
        if (tint) emit_recolour(&s, "o.c0", "float3", "float4");
        if (c->omap_depth) {
            if (c->reg_used[r + 1]) sp(&s, "  o.depth = as_type<float>(r%d);\n", r + 1);
            else sp(&s, "  o.depth = in.pos.z;\n");
        }
        sp(&s, "  return o;\n}\n");
    } else {
        sp(&s, "  return;\n}\n");
    }
    info->msl = s.buf;
    info->attr_mask = c->in_attr_mask;
    info->out_mask = stage == STAGE_FRAGMENT ? c->omap_target : c->out_attr_mask;
    free(c->body.buf);
    free(c);
    return info;
}

void maxwell_free(ShaderInfo *s) {
    if (!s) return;
    free(s->msl);
    free(s);
}

void maxwell_disassemble(const uint8_t *code, size_t max_size, int stage, void *file) {
    (void)code; (void)max_size; (void)stage; (void)file;
}
