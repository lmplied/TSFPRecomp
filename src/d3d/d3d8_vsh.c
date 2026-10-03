/**
 * NV2A Vertex Shader Microcode to HLSL Translator - Implementation
 *
 * Translates NV2A 128-bit vertex shader microcode instructions into
 * HLSL vertex shader source, compiles them, and caches the results.
 *
 * The translation pipeline is:
 *   1. Parse: nv2a_vsh_parse(), in src/kernel/nv2a_vsh.c
 *   2. Analyze: determine which input registers (v0-v15) are read
 *   3. Generate HLSL: emit HLSL code mapping NV2A ops to HLSL intrinsics
 *   4. Compile: rhi_shader_create -> host vertex shader
 *   5. Cache: hash microcode -> reuse compiled shader on subsequent draws
 *
 * The generated HLSL uses:
 *   - cbuffer at b1: 192 float4 constants (c0-c191)
 *   - cbuffer at b2: the screen-space undo; at b3: input current values
 *   - Input semantics: ATTR0-ATTR15 mapped to v0-v15
 *   - Output semantics: SV_POSITION, COLOR0/1, TEXCOORD0-3, FOG, PSIZE
 */

#include "d3d8_internal.h"
#include <float.h>
#include <math.h>
#include "d3d8_vsh.h"
#include "recomp_config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>


/* ================================================================
 * Module State
 * ================================================================ */

/* Stored shader programs */
static NV2AVshSlot g_vsh_slots[NV2A_VS_MAX_SLOTS];
static int g_vsh_slot_count = 0;

/* Screen-space undo for programs (d3d8_vsh_set_screenspace), at register(b2).
 * enable.x == 0 leaves oPos untouched. 48 bytes: a multiple of 16. */
typedef struct {
    float scale[4];
    float offset[4];
    float enable[4];
} VshScreenspace;

static VshScreenspace g_vsh_screen;
static RhiBuffer     *g_vsh_screen_cb;
static BOOL           g_vsh_screen_dirty;

/* The NV2A's current value of each input register, at register(b3). A
 * program that reads a register its vertex does not carry reads this
 * instead: the value SetVertexData4f / SetVertexDataColor last set, as on
 * the hardware. fed[r / 4][r % 4] is 1 for a register the bound layout reads
 * from the vertex. 320 bytes: a multiple of 16. */
typedef struct {
    float value[NV2A_VS_MAX_INPUTS][4];
    float fed[4][4];
} VshVertexData;

static VshVertexData  g_vsh_vdata;
static RhiBuffer     *g_vsh_vdata_cb;
static BOOL           g_vsh_vdata_dirty;
static uint16_t       g_vsh_vdata_fed;     /* the mask g_vsh_vdata.fed holds */

/* Constant registers (192 float4) */
static NV2AVSConstants g_vsh_constants;
static BOOL g_vsh_constants_dirty = TRUE;

/* Constant buffer for VS constants */
static RhiBuffer *g_vsh_cb = NULL;

/* Shader cache: maps microcode hash to compiled shader + input layout */
typedef struct {
    uint32_t            hash;
    int                 in_use;
    RhiShader          *vs;           /* keeps what input layouts need */
    RhiVertexLayout    *layouts[16];  /* Cached layouts per input mask subset */
    uint32_t            layout_masks[16];  /* inputs_read | (texcoord-field<<16) */
    int                 layout_count;
    RhiVertexLayout    *decl_layouts[16];  /* Cached layouts per declaration */
    uint32_t            decl_keys[16];     /* NV2AVshSlot.decl_hash */
    int                 decl_layout_count;
    uint16_t            inputs_read;  /* Which v registers are read */
    int                 uses_proj;    /* reads the projection's first column */
    int                 pos_input;    /* v# oPos is copied from, or -1 */
    uint64_t            const_read[3];/* constant registers read by index, a bit each */
    int                 const_indexed;/* reads through a0: any register, as far as known */
} VshCacheEntry;

static VshCacheEntry g_vsh_cache[NV2A_VS_CACHE_SIZE];

/* ================================================================
 * Hash Function (FNV-1a)
 * ================================================================ */

/* Set while a program's HLSL is generated; see the constant emitter. */
static int g_emit_uses_proj;

/* The same fact about the program currently bound, for the draw path. */
static int g_bound_uses_proj;

int d3d8_vsh_bound_uses_projection(void) { return g_bound_uses_proj; }

/* Whether the projection the bound program reads is orthographic right now.
 *
 * Reading the projection register says a program transforms through a
 * matrix; it does not say which. OutRun 2 draws its HUD through the same
 * programs as its road, with an orthographic matrix in the same registers
 * (c[160..163]), so the register alone put the HUD among the 3D draws and
 * left it stretched in widescreen. The matrix tells them apart: a
 * perspective projection derives w from z, an orthographic one has a
 * constant w row, (0, 0, 0, 1) in the register that produces oPos.w
 * (column 3, i.e. reg + 3 of the transposed matrix the program dp4s by).
 * Checked against the constants as currently uploaded, per draw. */
int d3d8_vsh_bound_projection_is_ortho(void)
{
    const float *w;
    int reg = d3d8_vsh_hor_plus_reg() + 3;

    if (!g_bound_uses_proj || reg >= NV2A_VS_MAX_CONSTANTS)
        return 0;
    w = g_vsh_constants.c[reg];

    /* RECOMP_D3D8_PROJ_PROBE=1: each distinct projection the 3D programs draw
     * with, once -- the four rows as uploaded -- so a title's HUD matrix can
     * be told from its camera's without guessing. At most 24 lines. */
    {
        static int probe = -1, shown;
        static int seen[40][12];
        if (probe < 0)
            probe = getenv("RECOMP_D3D8_PROJ_PROBE") != NULL;
        if (probe && shown < 40) {
            /* Keyed on the rotation and scale of the four rows, rounded,
             * so a camera that moves every frame prints once and a HUD's
             * fixed matrix is not crowded out. */
            const float *r0 = g_vsh_constants.c[reg - 3], *r1 = g_vsh_constants.c[reg - 2];
            const float *r2 = g_vsh_constants.c[reg - 1];
            int key[12], i, k;
            for (k = 0; k < 3; k++) {
                key[k]     = (int)(r0[k] * 100.0f);
                key[3 + k] = (int)(r1[k] * 100.0f);
                key[6 + k] = (int)(r2[k] * 100.0f);
                key[9 + k] = (int)(w[k] * 100.0f);
            }
            for (i = 0; i < shown; i++)
                if (memcmp(seen[i], key, sizeof key) == 0)
                    break;
            if (i == shown) {
                memcpy(seen[shown++], key, sizeof key);
                fprintf(stderr, "[VSH-PROJ] c[%d..%d]: x(%g %g %g %g) y(%g %g %g %g) "
                        "z(%g %g %g %g) w(%g %g %g %g)\n", reg - 3, reg,
                        r0[0], r0[1], r0[2], r0[3], r1[0], r1[1], r1[2], r1[3],
                        r2[0], r2[1], r2[2], r2[3], w[0], w[1], w[2], w[3]);
            }
        }
    }
    return fabsf(w[0]) < 1e-6f && fabsf(w[1]) < 1e-6f && fabsf(w[2]) < 1e-6f
        && fabsf(w[3] - 1.0f) < 1e-4f;
}

/* Which constants a program reads, for frame interpolation: a matrix the
 * program never reads may hold anything, and is not the draw's. */
static uint64_t g_emit_const_read[3], g_bound_const_read[3];
static int      g_emit_const_indexed, g_bound_const_indexed;

uint32_t d3d8_vsh_bound_reads(int first, int count)
{
    uint32_t mask = 0;
    int g, r;

    for (g = 0; g * 4 < count && g < 32; g++) {
        int hit = g_bound_const_indexed;
        for (r = first + g * 4; !hit && r < first + g * 4 + 4; r++)
            if (r >= 0 && r < NV2A_VS_MAX_CONSTANTS &&
                (g_bound_const_read[r >> 6] >> (r & 63)) & 1)
                hit = 1;
        if (hit)
            mask |= 1u << g;
    }
    return mask;
}

/* For a program that writes oPos straight from an input register -- the
 * shape every screen-space program has -- the register that input came
 * from, so the draw path can read the positions and measure how wide the
 * draw actually is. -1 when the position was computed rather than
 * copied, which is every program that went through a projection and so
 * is never asked about. */
static int g_emit_pos_input = -1;
static int g_bound_pos_input = -1;
static const D3D8VshInput *g_bound_decl;
static int                 g_bound_decl_count;

/* Where register `reg` sits in the vertex, for the bound program's own
 * declaration. FALSE when it did not declare one, or not that register. */
int d3d8_vsh_bound_input_offset(int reg, UINT *offset)
{
    int i;

    for (i = 0; i < g_bound_decl_count; i++)
        if (g_bound_decl[i].reg == reg) {
            if (offset) *offset = g_bound_decl[i].offset;
            return 1;
        }
    return 0;
}

int d3d8_vsh_bound_pos_input(void) { return g_bound_pos_input; }

static uint32_t fnv1a_hash(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 0x811c9dc5;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193;
    }
    return h;
}

/* ================================================================
 * HLSL Code Generator
 * ================================================================ */

/* String buffer helper */
typedef struct {
    char *buf;
    int   pos;
    int   size;
} StrBuf;

static void sb_init(StrBuf *sb, char *buf, int size)
{
    sb->buf  = buf;
    sb->pos  = 0;
    sb->size = size;
    if (size > 0) buf[0] = '\0';
}

static void sb_append(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    int remaining;
    if (sb->pos >= sb->size - 1) return;
    remaining = sb->size - sb->pos;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->pos, remaining, fmt, ap);
    va_end(ap);
    if (n > 0 && n < remaining)
        sb->pos += n;
    else if (n >= remaining)
        sb->pos = sb->size - 1;
}

/* Component name table */
static const char g_comp_names[] = "xyzw";

/**
 * Emit a swizzle suffix.
 * If the swizzle is identity (.xyzw), emit nothing (saves readability).
 */
static void emit_swizzle(StrBuf *sb, const NV2AVshSwizzle *swz)
{
    /* Check for identity swizzle */
    if (swz->x == 0 && swz->y == 1 && swz->z == 2 && swz->w == 3)
        return;

    sb_append(sb, ".%c%c%c%c",
              g_comp_names[swz->x & 3],
              g_comp_names[swz->y & 3],
              g_comp_names[swz->z & 3],
              g_comp_names[swz->w & 3]);
}

/**
 * Emit a scalar swizzle for ILU ops that replicate a single component.
 * Uses .x/.y/.z/.w for the selected component.
 */
static void emit_scalar_swizzle(StrBuf *sb, const NV2AVshSwizzle *swz)
{
    /* ILU operations use only one component; the swizzle X field selects it */
    sb_append(sb, ".%c", g_comp_names[swz->x & 3]);
}

/**
 * Emit a source operand reference.
 *
 * Handles register bank selection, swizzle, negate, and relative addressing.
 */
static void emit_source(StrBuf *sb, const NV2AVshSrcOperand *src, int scalar)
{
    if (src->negate)
        sb_append(sb, "(-");

    switch (src->reg_type) {
    case NV2A_VSH_REG_TEMP:
        /* R12 is the oPos alias. R13-R15 do not exist: a read of one is
         * zero, as in the CPU interpreter, instead of an undeclared name that
         * fails the whole shader's compile. */
        if (src->reg_index >= 0 && src->reg_index <= 12)
            sb_append(sb, "R%d", src->reg_index);
        else
            sb_append(sb, "float4(0,0,0,0)");
        break;
    case NV2A_VSH_REG_INPUT:
        sb_append(sb, "v%d", src->reg_index);
        break;
    case NV2A_VSH_REG_CONST:
        if (src->rel_addr) {
            sb_append(sb, "c[a0 + %d]", src->reg_index);
            g_emit_const_indexed = 1;
        } else {
            sb_append(sb, "c[%d]", src->reg_index);
            if (src->reg_index >= 0 && src->reg_index < NV2A_VS_MAX_CONSTANTS)
                g_emit_const_read[src->reg_index >> 6] |= 1ull << (src->reg_index & 63);
            /* A program that reads the register holding the first column of
             * the title's projection is transforming geometry through it,
             * which is what makes it 3D. One that never does is drawing in
             * screen coordinates it worked out itself -- the HUD, menus,
             * a full-screen quad -- and is not widened by Hor+, so it needs
             * the compensating squeeze instead. Relative addressing is not
             * counted: an indexed read is a bone or a light, never this. */
            if (src->reg_index == d3d8_vsh_hor_plus_reg())
                g_emit_uses_proj = 1;
        }
        break;
    default:
        sb_append(sb, "float4(0,0,0,0)");
        break;
    }

    if (scalar)
        emit_scalar_swizzle(sb, &src->swizzle);
    else
        emit_swizzle(sb, &src->swizzle);

    if (src->negate)
        sb_append(sb, ")");
}

/**
 * Emit a write mask suffix (.xyzw subset).
 * The mask is encoded as: bit3=x, bit2=y, bit1=z, bit0=w.
 */
static void emit_write_mask(StrBuf *sb, uint8_t mask)
{
    if (mask == 0xF) return; /* Full write, no mask needed */

    sb_append(sb, ".");
    if (mask & 0x8) sb_append(sb, "x");
    if (mask & 0x4) sb_append(sb, "y");
    if (mask & 0x2) sb_append(sb, "z");
    if (mask & 0x1) sb_append(sb, "w");
}

/**
 * Map NV2A output register enum to an HLSL variable name.
 */
static const char *output_reg_name(NV2AVshOutputReg reg)
{
    switch (reg) {
    case NV2A_VSH_OUT_POS:  return "oPos";
    case NV2A_VSH_OUT_D0:   return "oD0";
    case NV2A_VSH_OUT_D1:   return "oD1";
    case NV2A_VSH_OUT_FOG:  return "oFog";
    case NV2A_VSH_OUT_PTS:  return "oPts";
    case NV2A_VSH_OUT_B0:   return "oB0";
    case NV2A_VSH_OUT_B1:   return "oB1";
    case NV2A_VSH_OUT_T0:   return "oT0";
    case NV2A_VSH_OUT_T1:   return "oT1";
    case NV2A_VSH_OUT_T2:   return "oT2";
    case NV2A_VSH_OUT_T3:   return "oT3";
    default:                return NULL;
    }
}

/**
 * Emit a destination assignment (temp and/or output register write).
 *
 * The NV2A can write to both a temp register and an output register
 * simultaneously from the same operation. We emit two assignments
 * when both are active.
 *
 * @param prefix  The destination: temp register name
 * @param dst     The destination operand
 * @param rhs     The HLSL expression to assign (right-hand side)
 */
static void emit_dest_assign(StrBuf *sb, const NV2AVshDstOperand *dst,
                              const char *rhs)
{
    uint8_t omask = nv2a_vsh_output_write_mask(dst);

    /* rhs is a local computed once for the slot, so the order of these two
     * writes does not change the value. It did while rhs was the expression
     * itself: the temp write came first and the output then re-evaluated the
     * expression against the new temp. */
    if (dst->output_reg != NV2A_VSH_OUT_NONE && omask != 0) {
        const char *name = output_reg_name(dst->output_reg);
        if (name) {
            sb_append(sb, "        %s", name);
            emit_write_mask(sb, omask);
            sb_append(sb, " = (%s)", rhs);
            emit_write_mask(sb, omask);
            sb_append(sb, ";\n");
        }
    }

    /* R13-R15 do not exist; the interpreter ignores writes to them too. */
    if (dst->temp_reg >= 0 && dst->temp_reg <= 12 && dst->write_mask != 0) {
        sb_append(sb, "        R%d", dst->temp_reg);
        emit_write_mask(sb, dst->write_mask);
        sb_append(sb, " = (%s)", rhs);
        emit_write_mask(sb, dst->write_mask);
        sb_append(sb, ";\n");
    }
}

/**
 * Emit the HLSL for one MAC operation.
 */
static void emit_mac_op(StrBuf *sb, const NV2AVshInstruction *inst)
{
    StrBuf expr;
    char expr_buf[512];
    sb_init(&expr, expr_buf, sizeof(expr_buf));

    switch (inst->mac_op) {
    case NV2A_VSH_MAC_NOP:
        return;

    case NV2A_VSH_MAC_MOV:
        /* dst = A */
        emit_source(&expr, &inst->mac_src[0], 0);
        break;

    case NV2A_VSH_MAC_MUL:
        /* dst = A * B */
        sb_append(&expr, "(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, " * ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_ADD:
        /* dst = A + C */
        sb_append(&expr, "(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, " + ");
        emit_source(&expr, &inst->mac_src[2], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_MAD:
        /* dst = A * B + C */
        sb_append(&expr, "(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, " * ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, " + ");
        emit_source(&expr, &inst->mac_src[2], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_DP3:
        /* dst.xyzw = dot(A.xyz, B.xyz) replicated */
        sb_append(&expr, "dot(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ".xyz, ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ".xyz).xxxx");
        break;

    case NV2A_VSH_MAC_DPH:
        /* dst = dot(float4(A.xyz, 1.0), B) */
        sb_append(&expr, "dot(float4(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ".xyz, 1.0), ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_MAC_DP4:
        /* dst.xyzw = dot(A, B) replicated */
        sb_append(&expr, "dot(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_MAC_DST:
        /* dst = float4(1.0, A.y * B.y, A.z, B.w) */
        sb_append(&expr, "float4(1.0, ");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ".y * ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ".y, ");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ".z, ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ".w)");
        break;

    case NV2A_VSH_MAC_MIN:
        sb_append(&expr, "min(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_MAX:
        sb_append(&expr, "max(");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_SLT:
        /* dst = (A < B) ? 1.0 : 0.0
         * SLT is the complement of SGE: slt(a,b) = 1 - step(b, a)
         * Equivalent to: step(a, b) where a < b yields 1
         * Using explicit form for clarity: */
        sb_append(&expr, "(1.0 - step(");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, "))");
        break;

    case NV2A_VSH_MAC_SGE:
        /* dst = (A >= B) ? 1.0 : 0.0
         * step(edge, x) returns 1 if x >= edge, 0 otherwise */
        sb_append(&expr, "step(");
        emit_source(&expr, &inst->mac_src[1], 0);
        sb_append(&expr, ", ");
        emit_source(&expr, &inst->mac_src[0], 0);
        sb_append(&expr, ")");
        break;

    case NV2A_VSH_MAC_ARL:
        /* a0 = floor(A.x). Held in a local and stored after the ILU has read
         * its source, like every other write in the slot. */
        sb_append(sb, "        int _a0 = (int)floor(");
        emit_source(sb, &inst->mac_src[0], 0);
        sb_append(sb, ".x);\n");
        return;

    default:
        return;
    }

    sb_append(sb, "        float4 _mac = %s;\n", expr_buf);
}

/**
 * Emit the HLSL for one ILU operation.
 */
static void emit_ilu_op(StrBuf *sb, const NV2AVshInstruction *inst)
{
    StrBuf expr;
    char expr_buf[512];
    sb_init(&expr, expr_buf, sizeof(expr_buf));

    switch (inst->ilu_op) {
    case NV2A_VSH_ILU_NOP:
        return;

    case NV2A_VSH_ILU_MOV:
        /* dst = C */
        emit_source(&expr, &inst->ilu_src, 0);
        break;

    case NV2A_VSH_ILU_RCP:
        /* dst = (1.0 / C.x).xxxx */
        sb_append(&expr, "(1.0 / ");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ").xxxx");
        break;

    case NV2A_VSH_ILU_RCC:
        /* dst = sign(1/C.x) * clamp(|1/C.x|, 5.42101e-20, 1.8446744e+19),
         * replicated -- the same asymmetric clamp nv2a_vsh_execute() applies.
         * The sign is taken as ">= 0", not sign(): at C.x = +/-inf, 1/x is a
         * signed zero, which sign() makes 0 and the interpreter treats as
         * positive. 1/0 is +/-inf and clamps to the upper bound. */
        sb_append(&expr, "(((1.0 / ");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ") >= 0.0 ? 1.0 : -1.0) * clamp(abs(1.0 / ");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, "), 5.42101e-20, 1.8446744e+19)).xxxx");
        break;

    case NV2A_VSH_ILU_RSQ:
        /* dst = (1.0 / sqrt(abs(C.x))).xxxx */
        sb_append(&expr, "rsqrt(abs(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ")).xxxx");
        break;

    case NV2A_VSH_ILU_EXP:
        /* The NV2A's EXP is D3D's expp, four results at once:
         *   dst = (2^floor(C.x), C.x - floor(C.x), 2^C.x, 1)
         * (xemu vsh-prog.c _EXP; Cxbx-Reloaded x_expp). A program that reads
         * .y gets the fraction: TimeSplitters: Future Perfect's particle
         * program wraps its rotation angle into [0,1) that way before a
         * sine/cosine series, and with 2^x there the series ran at angles up
         * to ~15 radians and its sprites came out as screen-sized sheets. */
        sb_append(&expr, "float4(exp2(floor(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, ")), ");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, " - floor(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, "), exp2(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, "), 1.0)");
        break;

    case NV2A_VSH_ILU_LOG: {
        /* D3D's logp, four results at once (xemu vsh-prog.c _LOG):
         *   dst = (floor(log2|C.x|), |C.x| / 2^floor(log2|C.x|), log2|C.x|, 1)
         * log2(0) is -inf on the NV2A; |C.x| is held at the smallest normal
         * float instead, as before. */
        sb_append(&expr, "float4(floor(log2(max(abs(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, "), 1.175494e-38))), max(abs(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, "), 1.175494e-38) / exp2(floor(log2(max(abs(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, "), 1.175494e-38)))), log2(max(abs(");
        emit_source(&expr, &inst->ilu_src, 1);
        sb_append(&expr, "), 1.175494e-38)), 1.0)");
        break;
    }

    case NV2A_VSH_ILU_LIT: {
        /* NV2A LIT instruction:
         *   dst.x = 1.0
         *   dst.y = max(src.x, 0.0)
         *   dst.z = (src.x > 0) ? pow(max(src.y, 0), clamp(src.w, -128, 128)) : 0
         *   dst.w = 1.0
         *
         * We emit a helper call. The lit() HLSL intrinsic has similar but
         * not identical semantics, so we use an inline expansion. */
        sb_append(&expr, "float4(1.0, max(");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".x, 0.0), (");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".x > 0.0) ? exp2(clamp(");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".w, -128.0, 128.0) * log2(max(");
        emit_source(&expr, &inst->ilu_src, 0);
        sb_append(&expr, ".y, 0.0) + 1e-30)) : 0.0, 1.0)");
        break;
    }

    default:
        return;
    }

    sb_append(sb, "        float4 _ilu = %s;\n", expr_buf);
}

/**
 * Map NV2A input register index to a D3D11 input semantic.
 *
 * Xbox NV2A vertex shader input registers map to vertex attributes:
 *   v0  = Position
 *   v1  = Blend weight
 *   v2  = Normal
 *   v3  = Diffuse color
 *   v4  = Specular color
 *   v5  = Fog coordinate
 *   v6  = Point size / back diffuse
 *   v7  = Back specular
 *   v8  = Texture coord 0
 *   v9  = Texture coord 1
 *   v10 = Texture coord 2
 *   v11 = Texture coord 3
 *   v12-v15 = Additional attributes
 *
 * We use generic ATTR semantics so the input layout can match any
 * vertex buffer format at bind time.
 */
static const char *input_semantic_name(int reg_index)
{
    /* All inputs use the generic TEXCOORD semantic with unique indices
     * to avoid mismatches. The input layout will map them correctly. */
    (void)reg_index;
    return "ATTR";
}

int d3d8_vsh_generate_hlsl(const NV2AVshProgram *program,
                            char *buf, int bufsize)
{
    StrBuf sb;
    int i;
    int writes_fog = 0;     /* does any instruction write oFog? */
    uint16_t inputs = program->inputs_read;

    sb_init(&sb, buf, bufsize);

    /* Constant buffer: 192 float4 constants */
    sb_append(&sb,
        "/* Auto-generated NV2A vertex shader */\n"
        "\n"
        "cbuffer VSH_Constants : register(b1) {\n"
        "    float4 c[%d];\n"
        "};\n"
        "\n"
        "cbuffer VSH_Screenspace : register(b2) {\n"
        "    float4 xboxScreenspaceScale;\n"
        "    float4 xboxScreenspaceOffset;\n"
        "    float4 xboxScreenspaceEnable;\n"
        "};\n"
        "\n"
        "cbuffer VSH_VertexData : register(b3) {\n"
        "    float4 xboxVertexData[%d];\n"
        "    float4 xboxVertexFed[4];\n"
        "};\n"
        "\n", NV2A_VS_MAX_CONSTANTS, NV2A_VS_MAX_INPUTS);

    /* Input structure - only declare used inputs */
    sb_append(&sb, "struct VS_IN {\n");
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (inputs & (1u << i)) {
            sb_append(&sb, "    float4 v%d : ATTR%d;\n", i, i);
        }
    }
    sb_append(&sb, "};\n\n");

    /* Output structure. The names are the ones the pixel shaders ask for
     * (d3d8_shaders.c and d3d8_combiners.c, PS_IN): fog at TEXCOORD4 and a
     * view-space position for table fog at TEXCOORD5. An earlier version
     * wrote fog to FOG, which no pixel shader here reads, so fog never
     * reached them.
     *
     * The names are not the whole contract: the stages are linked by
     * register and component as each compiler packed them, so the types and
     * the order have to match the pixel shaders' too. Every stage declares
     * four float4 texture coordinates, then fog, then the view position.
     * The pixel shaders used to declare float3 coordinates, which packed
     * their `fog` into TEXCOORD3's spare w, so under a program every one of
     * them read oT3.w as its fog factor. */
    sb_append(&sb,
        "struct VS_OUT {\n"
        "    float4 oPos : SV_POSITION;\n"
        "    float4 oD0  : COLOR0;\n"
        "    float4 oD1  : COLOR1;\n"
        "    float4 oT0  : TEXCOORD0;\n"
        "    float4 oT1  : TEXCOORD1;\n"
        "    float4 oT2  : TEXCOORD2;\n"
        "    float4 oT3  : TEXCOORD3;\n"
        "    float  oFog : TEXCOORD4;\n"
        "    float4 oViewPos : TEXCOORD5;\n"
        "    float  oPts : PSIZE;\n"
        "    float4 oB0  : TEXCOORD6;\n"
        "    float4 oB1  : TEXCOORD7;\n"
        "};\n\n");

    /* Main function */
    sb_append(&sb, "VS_OUT main(VS_IN input) {\n");

    /* Declare temporary registers R0-R12 */
    sb_append(&sb, "    /* Temporary registers */\n");
    for (i = 0; i <= 12; i++) {
        sb_append(&sb, "    float4 R%d = float4(0,0,0,0);\n", i);
    }

    /* Address register */
    sb_append(&sb, "    int a0 = 0;\n\n");

    /* Input registers: from the vertex when the layout feeds them, otherwise
     * the register's current value (VSH_VertexData). */
    sb_append(&sb, "    /* Input registers */\n");
    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (inputs & (1u << i)) {
            sb_append(&sb, "    float4 v%d = xboxVertexFed[%d][%d] != 0 ? input.v%d : "
                      "xboxVertexData[%d];\n", i, i / 4, i % 4, i, i);
        }
    }
    sb_append(&sb, "\n");

    /* Output register variables: w of 1, and fog of 1 -- no fog -- for a
     * program that never writes it (Cxbx-Reloaded, CxbxVertexShaderTemplate.hlsl;
     * test case there: Lego Star Wars II) */
    sb_append(&sb,
        "    /* Output registers */\n"
        "    float4 oPos = float4(0,0,0,1);\n"
        "    float4 oD0  = float4(0,0,0,1);\n"
        "    float4 oD1  = float4(0,0,0,1);\n"
        "    float4 oFog = float4(1,1,1,1);\n"
        "    float4 oPts = float4(0,0,0,0);\n"
        "    float4 oB0  = float4(0,0,0,1);\n"
        "    float4 oB1  = float4(0,0,0,1);\n"
        "    float4 oT0  = float4(0,0,0,1);\n"
        "    float4 oT1  = float4(0,0,0,1);\n"
        "    float4 oT2  = float4(0,0,0,1);\n"
        "    float4 oT3  = float4(0,0,0,1);\n"
        "\n");

    /* R12 is aliased to oPos on NV2A */
    sb_append(&sb, "    /* R12 is aliased to oPos */\n");
    sb_append(&sb, "    #define R12 oPos\n\n");

    /* Emit instructions */
    sb_append(&sb, "    /* --- Program body (%d instructions) --- */\n",
              program->length);

    for (i = 0; i < program->length; i++) {
        const NV2AVshInstruction *inst = &program->insns[i];

        int mac_runs = inst->mac_op != NV2A_VSH_MAC_NOP
                    && (unsigned)inst->mac_op < NV2A_VSH_MAC_COUNT;
        int ilu_runs = inst->ilu_op != NV2A_VSH_ILU_NOP;

        sb_append(&sb, "\n    /* Instruction %d */\n    {\n", i);

        /* The two units run in parallel: both read the register file as it
         * was at the start of the slot. So every value is computed into a
         * local first -- _mac, _a0, _ilu -- and only then written. Emitting
         * the MAC write before evaluating the ILU let an ILU source read a
         * temp the MAC had just changed, which the CPU interpreter never did.
         * MAC opcodes 14 and 15 do not exist and emit nothing. */
        if (mac_runs)
            emit_mac_op(&sb, inst);
        if (ilu_runs)
            emit_ilu_op(&sb, inst);

        /* oPos written straight from an input: note where position came
         * from. See g_emit_pos_input. */
        if (mac_runs && inst->mac_op == NV2A_VSH_MAC_MOV &&
            inst->mac_dst.output_reg == NV2A_VSH_OUT_POS &&
            inst->mac_src[0].reg_type == NV2A_VSH_REG_INPUT)
            g_emit_pos_input = inst->mac_src[0].reg_index;

        if (mac_runs && inst->mac_op == NV2A_VSH_MAC_ARL)
            sb_append(&sb, "        a0 = _a0;\n");
        else if (mac_runs)
            emit_dest_assign(&sb, &inst->mac_dst, "_mac");
        if (ilu_runs)
            emit_dest_assign(&sb, &inst->ilu_dst, "_ilu");

        /* Whether oFog is ever written: the same test emit_dest_assign
         * applies before it emits an output write. */
        if (mac_runs && inst->mac_op != NV2A_VSH_MAC_ARL &&
            inst->mac_dst.output_reg == NV2A_VSH_OUT_FOG &&
            nv2a_vsh_output_write_mask(&inst->mac_dst) != 0)
            writes_fog = 1;
        if (ilu_runs && inst->ilu_dst.output_reg == NV2A_VSH_OUT_FOG &&
            nv2a_vsh_output_write_mask(&inst->ilu_dst) != 0)
            writes_fog = 1;

        sb_append(&sb, "    }\n");
    }

    /* Undo the R12 alias */
    sb_append(&sb, "\n    #undef R12\n\n");

    /* Xbox programs end in the XDK's screen-space transform, oPos in
     * render-target pixels with Z scaled by the depth buffer. Undo it to clip
     * space (Cxbx-Reloaded, CxbxVertexShaderTemplate.hlsl), when the caller
     * has said what the screen is. */
    sb_append(&sb,
        "    /* Undo the Xbox screen-space transform */\n"
        "    if (xboxScreenspaceEnable.x != 0) {\n"
        "        oPos -= xboxScreenspaceOffset;\n"
        "        oPos /= xboxScreenspaceScale;\n"
        "        if (oPos.w == 0) oPos.w = 1;\n"
        "        oPos.xyz *= oPos.w;\n"
        "    }\n\n");

    /* Populate output structure */
    sb_append(&sb,
        "    /* Write outputs */\n"
        "    VS_OUT o;\n"
        "    o.oPos = oPos;\n"
        "    o.oD0  = saturate(oD0);\n"  /* Colors clamped to [0,1] */
        "    o.oD1  = saturate(oD1);\n"
        "    o.oT0  = oT0;\n"
        "    o.oT1  = oT1;\n"
        "    o.oT2  = oT2;\n"
        "    o.oT3  = oT3;\n");
    /* A program that never writes oFog hands on a marker, not the default 1:
     * what fog alpha the combiners then see depends on whether fog is on
     * (D3D8_VSH_FOG_UNWRITTEN). TimeSplitters: Future Perfect's particle
     * program writes none, draws its muzzle flash with fog off, and blends
     * by a final combiner that mixes toward the fog colour by fog alpha: a
     * default of 1 drew every flash quad as a square of fog colour. */
    if (writes_fog)
        sb_append(&sb, "    o.oFog = oFog.x;\n");
    else
        sb_append(&sb, "    o.oFog = " D3D8_VSH_FOG_UNWRITTEN ";\n");
    sb_append(&sb,
        /* Table fog reads -viewpos.z (or its length, for range fog) as the
         * distance; on the Xbox that distance is the program's oFog. */
        "    o.oViewPos = float4(0, 0, -oFog.x, 1);\n"
        "    o.oPts = oPts.x;\n"
        "    o.oB0  = saturate(oB0);\n"
        "    o.oB1  = saturate(oB1);\n"
        "    return o;\n"
        "}\n");

    return sb.pos;
}

/* ================================================================
 * Input Layout Management
 *
 * When using a programmable VS, we need an input layout that matches
 * the shader's declared inputs. The layout maps vertex buffer elements
 * to the ATTR# semantics declared in the generated HLSL.
 *
 * The mapping from NV2A v# registers to vertex data depends on the
 * game's vertex stream setup. We use a simple mapping:
 *
 *   v0  -> ATTR0  -> POSITION (float4, offset 0)
 *   v1  -> ATTR1  -> BLENDWEIGHT (float4)
 *   v2  -> ATTR2  -> NORMAL (float4)
 *   v3  -> ATTR3  -> DIFFUSE (float4 / D3DCOLOR)
 *   v4  -> ATTR4  -> SPECULAR (float4 / D3DCOLOR)
 *   v5  -> ATTR5  -> FOG (float4)
 *   v6  -> ATTR6  -> POINTSIZE / BACKDIFFUSE (float4)
 *   v7  -> ATTR7  -> BACKSPECULAR (float4)
 *   v8  -> ATTR8  -> TEXCOORD0 (float4)
 *   v9  -> ATTR9  -> TEXCOORD1 (float4)
 *   v10 -> ATTR10 -> TEXCOORD2 (float4)
 *   v11 -> ATTR11 -> TEXCOORD3 (float4)
 *   v12-v15 -> ATTR12-15 -> additional
 *
 * The actual format (float2/3/4, D3DCOLOR, etc.) is determined at
 * draw time from the active stream source FVF/stride. For now, we
 * create a layout assuming the standard Xbox vertex attribute mapping.
 * ================================================================ */

/**
 * Default format for each input register.
 * This is the common Xbox convention; games may vary.
 * Texcoord registers 8-11 use the FVF-declared size (float1/2/3/4).
 */
static UINT vsh_fvf_texcoord_size(DWORD fvf, UINT t)
{
    /* D3DFVF_TEXTUREFORMAT1..4 are 3, 0, 1, 2 (see fvf_texcoord_size in
     * d3d8_shaders.c): the field is not the count. */
    static const UINT floats[4] = { 2, 3, 4, 1 };
    return floats[(fvf >> (16 + t * 2)) & 0x3];
}

static RhiFormat default_input_format(int vreg, DWORD fvf)
{
    switch (vreg) {
    case 0:  return RHI_FORMAT_R32G32B32_FLOAT;    /* Position (xyz) */
    case 1:  return RHI_FORMAT_R32G32B32A32_FLOAT;  /* Blend weights */
    case 2:  return RHI_FORMAT_R32G32B32_FLOAT;     /* Normal */
    case 3:  return RHI_FORMAT_R8G8B8A8_UNORM;      /* Diffuse (D3DCOLOR) */
    case 4:  return RHI_FORMAT_R8G8B8A8_UNORM;      /* Specular (D3DCOLOR) */
    case 5:  return RHI_FORMAT_R32_FLOAT;            /* Fog */
    case 6:  return RHI_FORMAT_R32_FLOAT;            /* Point size */
    case 7:  return RHI_FORMAT_R8G8B8A8_UNORM;      /* Back specular */
    case 8:  return vsh_fvf_texcoord_size(fvf, 0) == 1 ? RHI_FORMAT_R32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 0) == 3 ? RHI_FORMAT_R32G32B32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 0) == 4 ? RHI_FORMAT_R32G32B32A32_FLOAT
                 : RHI_FORMAT_R32G32_FLOAT;          /* Texcoord 0 */
    case 9:  return vsh_fvf_texcoord_size(fvf, 1) == 1 ? RHI_FORMAT_R32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 1) == 3 ? RHI_FORMAT_R32G32B32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 1) == 4 ? RHI_FORMAT_R32G32B32A32_FLOAT
                 : RHI_FORMAT_R32G32_FLOAT;          /* Texcoord 1 */
    case 10: return vsh_fvf_texcoord_size(fvf, 2) == 1 ? RHI_FORMAT_R32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 2) == 3 ? RHI_FORMAT_R32G32B32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 2) == 4 ? RHI_FORMAT_R32G32B32A32_FLOAT
                 : RHI_FORMAT_R32G32_FLOAT;          /* Texcoord 2 */
    case 11: return vsh_fvf_texcoord_size(fvf, 3) == 1 ? RHI_FORMAT_R32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 3) == 3 ? RHI_FORMAT_R32G32B32_FLOAT
                 : vsh_fvf_texcoord_size(fvf, 3) == 4 ? RHI_FORMAT_R32G32B32A32_FLOAT
                 : RHI_FORMAT_R32G32_FLOAT;          /* Texcoord 3 */
    default: return RHI_FORMAT_R32G32B32A32_FLOAT;   /* Generic */
    }
}

static UINT input_format_size(RhiFormat fmt)
{
    switch (fmt) {
    case RHI_FORMAT_R32_FLOAT:            return 4;
    case RHI_FORMAT_R32G32_FLOAT:         return 8;
    case RHI_FORMAT_R32G32B32_FLOAT:      return 12;
    case RHI_FORMAT_R32G32B32A32_FLOAT:   return 16;
    case RHI_FORMAT_R8G8B8A8_UNORM:       return 4;
    default:                                return 16;
    }
}

static RhiVertexLayout *create_vsh_input_layout(
    uint16_t inputs_read, DWORD fvf, const RhiShader *vs)
{
    RhiVertexElement elems[NV2A_VS_MAX_INPUTS];
    UINT elem_count = 0;
    UINT offset = 0;
    RhiVertexLayout *layout = NULL;
    int i;

    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        if (!(inputs_read & (1u << i)))
            continue;

        RhiFormat fmt = default_input_format(i, fvf);

        elems[elem_count].semantic       = "ATTR";
        elems[elem_count].semantic_index = (UINT)i;
        elems[elem_count].format         = fmt;
        elems[elem_count].slot           = 0;
        elems[elem_count].offset         = offset;
        elem_count++;

        offset += input_format_size(fmt);
    }

    if (elem_count == 0) return NULL;

    layout = rhi_vertex_layout_create(elems, elem_count, vs);
    if (!layout) {
        fprintf(stderr, "D3D8 VSH: CreateInputLayout failed\n");
        return NULL;
    }

    return layout;
}

/* ================================================================
 * Shader Compilation and Caching
 * ================================================================ */

static VshCacheEntry *cache_lookup(uint32_t hash)
{
    int idx = (int)(hash % NV2A_VS_CACHE_SIZE);
    int i;
    for (i = 0; i < NV2A_VS_CACHE_SIZE; i++) {
        int probe = (idx + i) % NV2A_VS_CACHE_SIZE;
        if (!g_vsh_cache[probe].in_use)
            return NULL;
        if (g_vsh_cache[probe].hash == hash)
            return &g_vsh_cache[probe];
    }
    return NULL;
}

static VshCacheEntry *cache_insert(uint32_t hash)
{
    int idx = (int)(hash % NV2A_VS_CACHE_SIZE);
    int i;

    /* Find an empty slot or reuse the probed slot */
    for (i = 0; i < NV2A_VS_CACHE_SIZE; i++) {
        int probe = (idx + i) % NV2A_VS_CACHE_SIZE;
        if (!g_vsh_cache[probe].in_use) {
            g_vsh_cache[probe].hash   = hash;
            g_vsh_cache[probe].in_use = 1;
            return &g_vsh_cache[probe];
        }
    }

    /* Cache full: evict the first probed entry */
    {
        VshCacheEntry *evict = &g_vsh_cache[idx];
        int j;

        rhi_shader_destroy(evict->vs);
        for (j = 0; j < evict->layout_count; j++)
            rhi_vertex_layout_destroy(evict->layouts[j]);
        /* The declaration-keyed layouts are a second set, and leak with the
         * entry if they are not released here too. */
        for (j = 0; j < evict->decl_layout_count; j++)
            rhi_vertex_layout_destroy(evict->decl_layouts[j]);
        memset(evict, 0, sizeof(*evict));
        evict->hash   = hash;
        evict->in_use = 1;
        return evict;
    }
}

/**
 * Compile a vertex shader from microcode.
 *
 * Parses, generates HLSL, compiles, and caches the result.
 * Returns the cache entry (with compiled VS and blob).
 */
static VshCacheEntry *compile_shader(const DWORD *microcode, int num_insns,
                                      uint32_t hash)
{
    NV2AVshProgram program;
    /* A 136-instruction program runs to about 30 KB of source. Heap, not
     * stack: this runs on whichever guest thread draws. */
    enum { HLSL_BUF = 65536 };
    char *hlsl_buf = (char *)malloc(HLSL_BUF);
    int hlsl_len;
    RhiShaderSource ss;
    RhiShader *vs;
    char err[4096];
    VshCacheEntry *entry;

    /* Parse microcode */
    nv2a_vsh_parse((const uint32_t *)microcode, num_insns, &program);

    /* Generate HLSL. A source that fills the buffer was cut off. */
    if (!hlsl_buf)
        return NULL;
    g_emit_uses_proj = 0;
    g_emit_pos_input = -1;
    memset(g_emit_const_read, 0, sizeof g_emit_const_read);
    g_emit_const_indexed = 0;
    hlsl_len = d3d8_vsh_generate_hlsl(&program, hlsl_buf, HLSL_BUF);
    if (hlsl_len <= 0 || hlsl_len >= HLSL_BUF - 1) {
        fprintf(stderr, "D3D8 VSH: HLSL generation failed (%d bytes)\n", hlsl_len);
        free(hlsl_buf);
        return NULL;
    }

    /* Debug switch, RECOMP_D3D8_VS_DUMP=1: print every program's generated
     * source, tagged with its microcode hash. Otherwise the source is only
     * printed when the compile fails. */
    {
        static int dump = -1;

        if (dump < 0)
            dump = getenv("RECOMP_D3D8_VS_DUMP") != NULL;
        if (dump)
            fprintf(stderr, "--- VS HLSL hash 0x%08X, %d instructions ---\n%s\n"
                    "--- End VS HLSL ---\n", hash, num_insns, hlsl_buf);
    }

    /* Compile HLSL to a host vertex shader */
    memset(&ss, 0, sizeof ss);
    ss.hlsl = hlsl_buf;
    ss.len = (size_t)hlsl_len;
    ss.name = "nv2a_vsh";
    ss.entry = "main";
    ss.target = "vs_5_0";
    ss.optimize = 1;
    {
        long long started = d3d8_compile_clock();
        vs = rhi_shader_create(RHI_STAGE_VERTEX, &ss, err, sizeof err);
        d3d8_compile_note(2, started);
    }
    if (!vs) {
        fprintf(stderr, "D3D8 VSH: Compile failed: %s\n", err[0] ? err : "unknown");
        fprintf(stderr, "--- Generated HLSL ---\n%s\n--- End ---\n", hlsl_buf);
        free(hlsl_buf);
        return NULL;
    }
    free(hlsl_buf);

    /* Insert into cache */
    entry = cache_insert(hash);
    if (!entry) {
        rhi_shader_destroy(vs);
        return NULL;
    }

    entry->vs          = vs;
    entry->inputs_read = program.inputs_read;
    entry->uses_proj   = g_emit_uses_proj;
    entry->pos_input   = g_emit_pos_input;
    memcpy(entry->const_read, g_emit_const_read, sizeof entry->const_read);
    entry->const_indexed = g_emit_const_indexed;
    entry->layout_count = 0;

    fprintf(stderr, "D3D8 VSH: Compiled shader (hash 0x%08X, %d insns, inputs 0x%04X)\n",
            hash, program.length, program.inputs_read);

    return entry;
}

/* The layout a program's declaration describes: each register it reads at the
 * declared format and offset in the stream 0 vertex. A register the program
 * reads that the declaration does not feed takes its current value
 * (VSH_VertexData) in the shader; D3D11 still requires every shader input
 * bound, so the layout gives it the vertex's first byte, which is unused. */
static RhiVertexLayout *create_vsh_input_layout_decl(
    uint16_t inputs_read, const NV2AVshSlot *vsh, const RhiShader *vs)
{
    RhiVertexElement elems[NV2A_VS_MAX_INPUTS];
    RhiVertexLayout *layout = NULL;
    UINT elem_count = 0;
    int i, j;

    for (i = 0; i < NV2A_VS_MAX_INPUTS; i++) {
        const D3D8VshInput *in = NULL;

        if (!(inputs_read & (1u << i)))
            continue;
        for (j = 0; j < vsh->decl_count; j++) {
            if (vsh->decl[j].reg == i) {
                in = &vsh->decl[j];
                break;
            }
        }
        if (!in)
            fprintf(stderr, "D3D8 VSH: a program reads v%d, which its declaration "
                    "does not feed; it reads the register's current value\n", i);
        elems[elem_count].semantic       = "ATTR";
        elems[elem_count].semantic_index = (UINT)i;
        elems[elem_count].format         = in ? in->format : RHI_FORMAT_R8_UNORM;
        elems[elem_count].slot           = 0;
        elems[elem_count].offset         = in ? in->offset : 0;
        elem_count++;
    }
    if (elem_count == 0)
        return NULL;

    layout = rhi_vertex_layout_create(elems, elem_count, vs);
    if (!layout) {
        fprintf(stderr, "D3D8 VSH: CreateInputLayout from declaration failed\n");
        return NULL;
    }
    return layout;
}

/**
 * Get the input layout for a cache entry.
 * Creates and caches the layout on first request per (input mask, texcoord sizes).
 */
static RhiVertexLayout *get_cached_layout(VshCacheEntry *entry, DWORD fvf)
{
    uint16_t mask = entry->inputs_read;
    uint32_t key = (uint32_t)mask | (((uint32_t)(fvf >> 16) & 0xFFFF) << 16);
    int i;

    /* Check if we already created a layout for this mask+texcoord layout */
    for (i = 0; i < entry->layout_count; i++) {
        if (entry->layout_masks[i] == key)
            return entry->layouts[i];
    }

    /* Create new layout */
    if (entry->layout_count >= 16)
        return entry->layouts[0]; /* Fallback to first */

    RhiVertexLayout *layout = create_vsh_input_layout(mask, fvf, entry->vs);
    entry->layouts[entry->layout_count]      = layout;
    entry->layout_masks[entry->layout_count] = key;
    entry->layout_count++;

    return layout;
}

/* The same per declaration. Kept apart from the mask-keyed layouts so a
 * declaration hash can never collide with an FVF key. */
static RhiVertexLayout *get_cached_layout_decl(VshCacheEntry *entry, const NV2AVshSlot *vsh)
{
    int i;

    for (i = 0; i < entry->decl_layout_count; i++) {
        if (entry->decl_keys[i] == vsh->decl_hash)
            return entry->decl_layouts[i];
    }
    if (entry->decl_layout_count >= 16)
        return entry->decl_layouts[0];

    entry->decl_layouts[entry->decl_layout_count] =
        create_vsh_input_layout_decl(entry->inputs_read, vsh, entry->vs);
    entry->decl_keys[entry->decl_layout_count] = vsh->decl_hash;
    return entry->decl_layouts[entry->decl_layout_count++];
}

/* ================================================================
 * Public API Implementation
 * ================================================================ */

HRESULT d3d8_vsh_init(void)
{
    RhiBufferDesc cbd;

    memset(g_vsh_slots, 0, sizeof(g_vsh_slots));
    memset(g_vsh_cache, 0, sizeof(g_vsh_cache));
    memset(&g_vsh_constants, 0, sizeof(g_vsh_constants));
    g_vsh_slot_count = 0;
    g_vsh_constants_dirty = TRUE;

    /* Create the constant buffer for VS constants (192 * float4 = 3072 bytes) */
    memset(&cbd, 0, sizeof(cbd));
    cbd.size       = sizeof(NV2AVSConstants);
    cbd.usage      = RHI_USAGE_DYNAMIC;
    cbd.bind       = RHI_BIND_UNIFORM;
    cbd.cpu_access = RHI_CPU_WRITE;

    g_vsh_cb = rhi_buffer_create(&cbd, NULL);
    if (!g_vsh_cb) {
        fprintf(stderr, "D3D8 VSH: Failed to create constant buffer\n");
        return E_FAIL;
    }

    /* The screen-space undo starts disabled: every enable component is 0. */
    memset(&g_vsh_screen, 0, sizeof(g_vsh_screen));
    cbd.size = sizeof(VshScreenspace);
    g_vsh_screen_cb = rhi_buffer_create(&cbd, &g_vsh_screen);
    if (!g_vsh_screen_cb)
        fprintf(stderr, "D3D8 VSH: Failed to create screen-space buffer\n");
    g_vsh_screen_dirty = FALSE;

    /* Current values start as the D3D defaults: diffuse white, everything
     * else (0, 0, 0, 1). */
    memset(&g_vsh_vdata, 0, sizeof(g_vsh_vdata));
    for (int r = 0; r < NV2A_VS_MAX_INPUTS; r++)
        g_vsh_vdata.value[r][3] = 1.0f;
    for (int k = 0; k < 3; k++)
        g_vsh_vdata.value[3][k] = 1.0f;
    g_vsh_vdata_fed = 0;
    cbd.size = sizeof(VshVertexData);
    g_vsh_vdata_cb = rhi_buffer_create(&cbd, NULL);
    if (!g_vsh_vdata_cb) {
        /* Without it every program would read every input as zero, so
         * prepare_draw refuses programs instead. */
        fprintf(stderr, "D3D8 VSH: Failed to create vertex data buffer\n");
        return E_FAIL;
    }
    g_vsh_vdata_dirty = TRUE;

    fprintf(stderr, "D3D8 VSH: Vertex shader translator initialized\n");
    return S_OK;
}

void d3d8_vsh_shutdown(void)
{
    int i, j;

    /* Release all cached shaders and layouts */
    for (i = 0; i < NV2A_VS_CACHE_SIZE; i++) {
        VshCacheEntry *e = &g_vsh_cache[i];
        if (!e->in_use) continue;
        rhi_shader_destroy(e->vs);
        for (j = 0; j < e->layout_count; j++)
            rhi_vertex_layout_destroy(e->layouts[j]);
        for (j = 0; j < e->decl_layout_count; j++)
            rhi_vertex_layout_destroy(e->decl_layouts[j]);
    }
    memset(g_vsh_cache, 0, sizeof(g_vsh_cache));

    rhi_buffer_destroy(g_vsh_cb);        g_vsh_cb = NULL;
    rhi_buffer_destroy(g_vsh_screen_cb); g_vsh_screen_cb = NULL;
    rhi_buffer_destroy(g_vsh_vdata_cb);  g_vsh_vdata_cb = NULL;

    memset(g_vsh_slots, 0, sizeof(g_vsh_slots));
    g_vsh_slot_count = 0;
}

HRESULT d3d8_vsh_create_shader(const DWORD *microcode, int num_insns,
                                DWORD *out_handle)
{
    int slot;

    if (!microcode || num_insns <= 0 || !out_handle)
        return E_INVALIDARG;

    if (num_insns > NV2A_VS_MAX_INSTRUCTIONS)
        num_insns = NV2A_VS_MAX_INSTRUCTIONS;

    /* Find a free slot */
    slot = -1;
    for (int i = 0; i < NV2A_VS_MAX_SLOTS; i++) {
        if (!g_vsh_slots[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        fprintf(stderr, "D3D8 VSH: No free shader slots\n");
        return E_OUTOFMEMORY;
    }

    /* Store microcode (deferred compilation); its hash is the cache key
     * every draw with this program looks up, so it is taken once here. */
    memcpy(g_vsh_slots[slot].microcode, microcode,
           (size_t)num_insns * 4 * sizeof(DWORD));
    g_vsh_slots[slot].length = num_insns;
    g_vsh_slots[slot].hash = fnv1a_hash(microcode, (size_t)num_insns * 4 * sizeof(DWORD));
    g_vsh_slots[slot].in_use = 1;
    g_vsh_slots[slot].decl_count = 0;
    g_vsh_slots[slot].decl_hash = 0;
    g_vsh_slot_count++;

    /* Generate handle: slot index + 0x10000 to distinguish from FVF codes.
     * Xbox D3D8 uses handles with the high bit set (> 0xFFFF). */
    *out_handle = (DWORD)(slot + 0x10000);

    /* Titles that stream programs create thousands of these; stderr is
     * unbuffered and this line was a tenth of a frame. */
    {
        static int logged;
        if (logged < 64) {
            fprintf(stderr, "D3D8 VSH: Created shader handle 0x%lX (%d instructions)%s\n",
                    *out_handle, num_insns,
                    ++logged == 64 ? " (further creates not logged)" : "");
        }
    }

    return S_OK;
}

HRESULT d3d8_vsh_delete_shader(DWORD handle)
{
    int slot;

    if (!d3d8_vsh_is_programmable(handle))
        return E_INVALIDARG;

    slot = (int)(handle - 0x10000);
    if (slot < 0 || slot >= NV2A_VS_MAX_SLOTS)
        return E_INVALIDARG;

    if (g_vsh_slots[slot].in_use) {
        g_vsh_slots[slot].in_use = 0;
        g_vsh_slots[slot].decl_count = 0;
        g_vsh_slot_count--;
    }

    return S_OK;
}

const float *d3d8_vsh_constants(void)
{
    return &g_vsh_constants.c[0][0];
}

/* Hor+ widescreen, experimental: RECOMP_HOR_PLUS=<factor> scales one
 * constant register as it is uploaded, and RECOMP_HOR_PLUS_REG=<n> says
 * which (60 by default, which is what TimeSplitters 2 uses).
 *
 * A title whose vertex program transforms by a matrix in constant
 * registers keeps that matrix transposed for dp4, so the register that
 * produces oPos.x holds column 0 of the matrix. Scaling that column is
 * exactly equivalent to scaling the projection's [0][0], because the
 * projection is the rightmost factor of whatever composite the title
 * built: M * diag(k,1,1,1) scales the product's first column whether M
 * is a bare projection or a world-view-projection. 0.75, which is
 * (4/3)/(16/9), widens the horizontal field of view and leaves the
 * vertical alone -- more of the scene rather than the same scene
 * stretched.
 *
 * This is the 3D half only. A title's pre-transformed 2D layer never
 * goes through this register and is not widened by it.
 *
 * Nor is anything the title works out on the CPU from its own projection:
 * frustum culling, portals, what is on screen. Those stay at 4:3, and the
 * wider view shows the world missing past the old edges. A title project
 * that widens the camera where the game builds it takes the factor with
 * xbox_D3D8ClaimHorPlus, and the register is then left as uploaded. */
static BOOL g_hor_plus_claimed;

static float hor_plus_factor(void)
{
    static float factor = -1.0f;

    if (factor < 0.0f) {
        const char *v = recomp_config_lookup("RECOMP_HOR_PLUS", "hor_plus");

        factor = (v && *v) ? (float)atof(v) : 1.0f;
        if (factor == 0.0f)
            factor = 1.0f;              /* the config's "leave it alone" */
        if (factor <= 0.0f || factor > 4.0f)
            factor = 1.0f;
        if (factor != 1.0f)
            fprintf(stderr, "D3D8 VSH: Hor+ scaling c[%d] by %.4f (experimental)\n",
                    d3d8_vsh_hor_plus_reg(), (double)factor);
    }
    return factor;
}

float xbox_D3D8ClaimHorPlus(void)
{
    float factor = hor_plus_factor();

    if (!g_hor_plus_claimed && factor != 1.0f)
        fprintf(stderr, "D3D8 VSH: Hor+ %.4f taken over by the caller; "
                "c[%d] is left as uploaded\n", (double)factor, d3d8_vsh_hor_plus_reg());
    g_hor_plus_claimed = TRUE;
    return factor;
}

/* RECOMP_CAMERA_ZOOM (settings file: camera_zoom): narrow the field of
 * view of what reaches the screen by scaling the projection register's x
 * and y rows together, so nothing changes shape.
 *
 * For a title whose own widescreen mode widens the view a long way: OutRun
 * 2's 16:9 camera keeps the 4:3 vertical angle and opens the horizontal one
 * to about 96 degrees, and at that angle the car in the foreground stretches
 * front to back. 1.15 brings it to about 88. Only draws to the screen are
 * zoomed -- the same register projects the title's shadow map, environment
 * cube and glare passes, which have to keep matching what they were made
 * for -- and the title culls to its own, wider view, so nothing is missing
 * at the edges. 1 (the default) leaves the register alone. */
static float camera_zoom(void)
{
    static float zoom = -1.0f;

    if (zoom < 0.0f) {
        const char *v = recomp_config_lookup("RECOMP_CAMERA_ZOOM", "camera_zoom");

        zoom = (v && *v) ? (float)atof(v) : 1.0f;
        if (zoom < 0.5f || zoom > 3.0f)
            zoom = 1.0f;
        if (zoom != 1.0f)
            fprintf(stderr, "D3D8 VSH: camera zoom %.3f on c[%d..%d] for draws to the "
                    "screen\n", (double)zoom, d3d8_vsh_hor_plus_reg(),
                    d3d8_vsh_hor_plus_reg() + 1);
    }
    return zoom;
}

int d3d8_vsh_hor_plus_reg(void)
{
    static int reg = -1;

    if (reg < 0) {
        const char *v = recomp_config_lookup("RECOMP_HOR_PLUS_REG", "hor_plus_register");

        reg = (v && *v) ? atoi(v) : 60;
        if (reg < 0 || reg >= NV2A_VS_MAX_CONSTANTS)
            reg = 60;
    }
    return reg;
}

/* Frame interpolation's registers: what the title said
 * (xbox_D3D8SetInterpRegisters), under what the settings say. */
static int g_interp_title_said;
static int g_interp_title_proj = -1, g_interp_title_first = -1, g_interp_title_count;

void xbox_D3D8SetInterpRegisters(int projection, int affine_first, int affine_count)
{
    g_interp_title_proj = projection;
    g_interp_title_first = affine_first;
    g_interp_title_count = affine_count;
    g_interp_title_said = 1;
}

void d3d8_vsh_interp_registers(int *projection, int *affine_first, int *affine_count)
{
    static int read;
    static const char *proj_v, *affine_v;
    int proj, first, count;

    if (!read) {
        read = 1;
        proj_v = recomp_config_lookup("RECOMP_INTERP_PROJECTION", "interp_projection");
        affine_v = recomp_config_lookup("RECOMP_INTERP_AFFINE", "interp_affine");
    }
    proj = g_interp_title_said ? g_interp_title_proj : d3d8_vsh_hor_plus_reg();
    first = g_interp_title_said ? g_interp_title_first : -1;
    count = g_interp_title_said ? g_interp_title_count : 0;
    if (proj_v && *proj_v)
        proj = (strcmp(proj_v, "none") == 0) ? -1 : atoi(proj_v);
    if (affine_v && *affine_v) {
        /* "64-75", or "none" */
        int lo = -1, hi = -1;
        if (strcmp(affine_v, "none") != 0 && sscanf(affine_v, "%d-%d", &lo, &hi) == 2 &&
            hi >= lo) {
            first = lo;
            count = hi - lo + 1;
        } else {
            first = -1;
            count = 0;
        }
    }
    if (proj < 0 || proj + 4 > NV2A_VS_MAX_CONSTANTS)
        proj = -1;
    if (first < 0 || count < 4 || first + count > NV2A_VS_MAX_CONSTANTS) {
        first = -1;
        count = 0;
    }
    *projection = proj;
    *affine_first = first;
    *affine_count = count & ~3;
}

void d3d8_vsh_set_constant(int start_reg, const float *data, int count)
{
    int end_reg;
    float hp;

    if (!data || start_reg < 0)
        return;

    end_reg = start_reg + count;
    if (end_reg > NV2A_VS_MAX_CONSTANTS)
        end_reg = NV2A_VS_MAX_CONSTANTS;

    for (int i = start_reg; i < end_reg; i++) {
        int src_offset = (i - start_reg) * 4;
        g_vsh_constants.c[i][0] = data[src_offset + 0];
        g_vsh_constants.c[i][1] = data[src_offset + 1];
        g_vsh_constants.c[i][2] = data[src_offset + 2];
        g_vsh_constants.c[i][3] = data[src_offset + 3];
    }

    hp = hor_plus_factor();
    if (hp != 1.0f && !g_hor_plus_claimed) {
        int reg = d3d8_vsh_hor_plus_reg();

        if (reg >= start_reg && reg < end_reg) {
            g_vsh_constants.c[reg][0] *= hp;
            g_vsh_constants.c[reg][1] *= hp;
            g_vsh_constants.c[reg][2] *= hp;
            g_vsh_constants.c[reg][3] *= hp;
        }
    }

    g_vsh_constants_dirty = TRUE;
}

HRESULT d3d8_vsh_set_declaration(DWORD handle, const D3D8VshInput *inputs, int count)
{
    int slot;
    NV2AVshSlot *vsh;

    if (!d3d8_vsh_is_programmable(handle))
        return E_INVALIDARG;
    slot = (int)(handle - 0x10000);
    if (slot < 0 || slot >= NV2A_VS_MAX_SLOTS || !g_vsh_slots[slot].in_use)
        return E_INVALIDARG;
    if (count < 0 || count > NV2A_VS_MAX_INPUTS || (count && !inputs))
        return E_INVALIDARG;

    vsh = &g_vsh_slots[slot];
    vsh->decl_count = count;
    if (count)
        memcpy(vsh->decl, inputs, (size_t)count * sizeof(*inputs));
    vsh->decl_hash = count
        ? fnv1a_hash(vsh->decl, (size_t)count * sizeof(vsh->decl[0]))
        : 0;
    return S_OK;
}

void d3d8_vsh_set_screenspace(const float scale[4], const float offset[4])
{
    if (!scale || !offset)
        return;
    memcpy(g_vsh_screen.scale, scale, sizeof(g_vsh_screen.scale));
    memcpy(g_vsh_screen.offset, offset, sizeof(g_vsh_screen.offset));
    g_vsh_screen.enable[0] = 1.0f;
    g_vsh_screen_dirty = TRUE;
}

void d3d8_vsh_get_screenspace(float scale[4], float offset[4], int *enabled)
{
    if (scale)
        memcpy(scale, g_vsh_screen.scale, sizeof(g_vsh_screen.scale));
    if (offset)
        memcpy(offset, g_vsh_screen.offset, sizeof(g_vsh_screen.offset));
    if (enabled)
        *enabled = g_vsh_screen.enable[0] != 0.0f;
}

void d3d8_vsh_set_vertex_data(int reg, const float value[4])
{
    if (!value || reg < 0 || reg >= NV2A_VS_MAX_INPUTS)
        return;
    if (memcmp(g_vsh_vdata.value[reg], value, sizeof(g_vsh_vdata.value[reg])) == 0)
        return;
    memcpy(g_vsh_vdata.value[reg], value, sizeof(g_vsh_vdata.value[reg]));
    g_vsh_vdata_dirty = TRUE;
}

void d3d8_vsh_get_vertex_data(int reg, float value[4])
{
    if (value && reg >= 0 && reg < NV2A_VS_MAX_INPUTS)
        memcpy(value, g_vsh_vdata.value[reg], sizeof(g_vsh_vdata.value[reg]));
}

void d3d8_vsh_clear_screenspace(void)
{
    memset(&g_vsh_screen, 0, sizeof(g_vsh_screen));
    g_vsh_screen_dirty = TRUE;
}

BOOL d3d8_vsh_get_slot(int slot, DWORD *handle, const DWORD **microcode,
                       int *length, const D3D8VshInput **decl, int *decl_count)
{
    const NV2AVshSlot *vsh;

    if (slot < 0 || slot >= NV2A_VS_MAX_SLOTS || !g_vsh_slots[slot].in_use)
        return FALSE;
    vsh = &g_vsh_slots[slot];
    if (handle)     *handle = (DWORD)(slot + 0x10000);
    if (microcode)  *microcode = vsh->microcode;
    if (length)     *length = vsh->length;
    if (decl)       *decl = vsh->decl;
    if (decl_count) *decl_count = vsh->decl_count;
    return TRUE;
}

BOOL d3d8_vsh_is_programmable(DWORD handle)
{
    return (handle >= 0x10000) ? TRUE : FALSE;
}

BOOL d3d8_vsh_prepare_draw(DWORD handle)
{
    int slot;
    NV2AVshSlot *vsh;
    uint32_t hash;
    VshCacheEntry *entry;
    RhiVertexLayout *layout;
    void *mapped;
    uint16_t fed;

    if (!d3d8_vsh_is_programmable(handle) || !g_vsh_cb || !g_vsh_vdata_cb)
        return FALSE;

    if (!rhi_device_ready()) return FALSE;

    /* Resolve handle to shader slot */
    slot = (int)(handle - 0x10000);
    if (slot < 0 || slot >= NV2A_VS_MAX_SLOTS)
        return FALSE;

    vsh = &g_vsh_slots[slot];
    if (!vsh->in_use)
        return FALSE;

    /* The microcode's hash, taken when the program was loaded */
    hash = vsh->hash;

    /* Look up in cache */
    entry = cache_lookup(hash);
    if (!entry) {
        /* Cache miss: parse, generate HLSL, compile */
        entry = compile_shader(vsh->microcode, vsh->length, hash);
        if (!entry)
            return FALSE;
    }

    /* Bind the vertex shader */
    rhi_set_shader(RHI_STAGE_VERTEX, entry->vs);
    g_bound_uses_proj = entry->uses_proj;
    memcpy(g_bound_const_read, entry->const_read, sizeof g_bound_const_read);
    g_bound_const_indexed = entry->const_indexed;
    g_bound_pos_input = entry->pos_input;
    g_bound_decl = vsh->decl_count ? vsh->decl : NULL;
    g_bound_decl_count = vsh->decl_count;

    /* Bind the input layout: the program's declaration when it has one,
     * otherwise sized from the bound stream FVF */
    if (vsh->decl_count) {
        /* Bound even when it could not be created: a stale layout would read
         * this vertex at another program's offsets, where no layout draws
         * nothing. */
        layout = get_cached_layout_decl(entry, vsh);
        rhi_set_vertex_layout(layout);
        fed = 0;
        for (int j = 0; j < vsh->decl_count; j++)
            if (vsh->decl[j].reg >= 0 && vsh->decl[j].reg < NV2A_VS_MAX_INPUTS)
                fed |= (uint16_t)(1u << vsh->decl[j].reg);
    } else {
        /* The FVF layout gives every register it reads a place in the
         * vertex, so all count as fed, as before current values existed. */
        layout = get_cached_layout(entry, d3d8_GetCurrentFVF());
        if (layout)
            rhi_set_vertex_layout(layout);
        fed = 0xFFFF;
    }
    if (fed != g_vsh_vdata_fed) {
        for (int r = 0; r < NV2A_VS_MAX_INPUTS; r++)
            g_vsh_vdata.fed[r / 4][r % 4] = (fed & (1u << r)) ? 1.0f : 0.0f;
        g_vsh_vdata_fed = fed;
        g_vsh_vdata_dirty = TRUE;
    }

    /* Update constant buffer if dirty, or if the camera zoom starts or stops
     * applying (camera_zoom: screen draws only). */
    {
        static BOOL zoomed;
        float zoom = camera_zoom();
        BOOL want = zoom != 1.0f && d3d8_target_is_screen();

        if (want != zoomed)
            g_vsh_constants_dirty = TRUE;
        if (g_vsh_constants_dirty) {
            mapped = rhi_buffer_map(g_vsh_cb, RHI_MAP_WRITE_DISCARD);
            if (mapped) {
                memcpy(mapped, &g_vsh_constants, sizeof(g_vsh_constants));
                if (want) {
                    int reg = d3d8_vsh_hor_plus_reg(), k;
                    float (*c)[4] = ((NV2AVSConstants *)mapped)->c;

                    if (reg + 1 < NV2A_VS_MAX_CONSTANTS)
                        for (k = 0; k < 4; k++) {
                            c[reg][k] *= zoom;
                            c[reg + 1][k] *= zoom;
                        }
                }
                rhi_buffer_unmap(g_vsh_cb);
            }
            g_vsh_constants_dirty = FALSE;
            zoomed = want;
        }
    }

    /* Bind constant buffer to slot b1 */
    rhi_set_uniform_buffers(RHI_STAGE_VERTEX, 1, 1, &g_vsh_cb);

    /* And the screen-space undo to b2 */
    if (g_vsh_screen_cb) {
        if (g_vsh_screen_dirty) {
            mapped = rhi_buffer_map(g_vsh_screen_cb, RHI_MAP_WRITE_DISCARD);
            if (mapped) {
                memcpy(mapped, &g_vsh_screen, sizeof(g_vsh_screen));
                rhi_buffer_unmap(g_vsh_screen_cb);
                g_vsh_screen_dirty = FALSE;
            }
        }
        rhi_set_uniform_buffers(RHI_STAGE_VERTEX, 2, 1, &g_vsh_screen_cb);
    }

    /* And the current input values to b3 */
    if (g_vsh_vdata_cb) {
        if (g_vsh_vdata_dirty) {
            mapped = rhi_buffer_map(g_vsh_vdata_cb, RHI_MAP_WRITE_DISCARD);
            if (mapped) {
                memcpy(mapped, &g_vsh_vdata, sizeof(g_vsh_vdata));
                rhi_buffer_unmap(g_vsh_vdata_cb);
                g_vsh_vdata_dirty = FALSE;
            }
        }
        rhi_set_uniform_buffers(RHI_STAGE_VERTEX, 3, 1, &g_vsh_vdata_cb);
    }

    return TRUE;
}
