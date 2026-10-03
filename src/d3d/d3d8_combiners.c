/**
 * NV2A Register Combiner to HLSL Pixel Shader Translator - Implementation
 *
 * Translates Xbox NV2A register combiner configurations into HLSL pixel
 * shaders compiled for D3D11. See d3d8_combiners.h for the full model
 * description.
 *
 * Implementation overview:
 *
 * 1. STATE TRACKING
 *    The game sets combiner configuration through either:
 *    (a) SetPixelShader(DWORD token) - a packed DWORD encoding combiner
 *        count and texture modes, with actual stage config in render states
 *    (b) Direct render state writes (D3DRS_PSALPHAINPUTS0..7, etc.)
 *    We parse either path into an NV2ACombinerState structure.
 *
 * 2. HLSL GENERATION
 *    From the combiner state, we emit a complete HLSL pixel shader that:
 *    - Samples textures based on tex_mode per stage
 *    - Walks each active general combiner stage performing AB*CD math
 *    - Executes the final combiner (lerp + add)
 *    - Handles alpha test (fog is the final combiner's own)
 *
 * 3. SHADER CACHE
 *    We hash the full NV2ACombinerState and maintain a fixed-size cache
 *    (128 entries) of compiled pixel shaders (rhi.h).  Most Xbox games
 *    use fewer than 20 unique combiner configurations, so this is ample.
 *
 * 4. DRAW INTEGRATION
 *    d3d8_combiners_prepare_draw() is called before each draw. It checks
 *    if state is dirty, rebuilds/looks up the shader, uploads constants,
 *    and binds everything to the D3D11 pipeline.
 */

#include "d3d8_internal.h"
#include "d3d8_combiners.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>


/* ================================================================
 * Internal State
 * ================================================================ */

/** Current pixel shader token (0 = no combiner shader / fixed-function). */
static DWORD g_ps_token = 0;

/** Current parsed combiner state. */
static NV2ACombinerState g_combiner_state;

/** Dirty flag - set when any PS render state changes. */
static BOOL g_dirty = TRUE;

/** PS constant buffer (uploaded to GPU each draw). */
static RhiBuffer *g_combiner_cb = NULL;

/* ================================================================
 * Shader Cache
 *
 * Simple open-addressing hash table with linear probing.
 * 128 entries is generous - most games use <20 unique PS configs.
 * On a full table, the oldest entry is evicted (LRU approximation
 * via frame counter).
 * ================================================================ */

#define COMBINER_CACHE_SIZE 128

typedef struct CombinerCacheEntry {
    BOOL                in_use;
    uint32_t            hash;
    NV2ACombinerState   state;
    RhiShader          *shader;
    uint32_t            last_used_frame;
} CombinerCacheEntry;

static CombinerCacheEntry g_cache[COMBINER_CACHE_SIZE];
static uint32_t g_frame_counter = 0;

/* ================================================================
 * Hashing
 *
 * FNV-1a over the combiner state structure. This is fast enough
 * for our purposes and produces good distribution.
 * ================================================================ */

static uint32_t fnv1a_hash(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 0x811C9DC5u;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

/* Only the structural part of the state is the key: the constant colours
 * that follow it are uploaded per draw and are not in the HLSL, and keying
 * on them made every distinct colour a new entry and a new D3DCompile in a
 * 128-entry table that then evicted and recompiled the same shaders in
 * steady state. */
static uint32_t combiner_state_hash(const NV2ACombinerState *state)
{
    return fnv1a_hash(state, NV2A_COMBINER_KEY_BYTES);
}

static BOOL combiner_state_equal(const NV2ACombinerState *a,
                                 const NV2ACombinerState *b)
{
    return memcmp(a, b, NV2A_COMBINER_KEY_BYTES) == 0;
}

/* How many generated shaders RECOMP_D3D8_PS_DUMP asks to see, and whether it
 * was asked for at all. The switch used to be a flag that meant "two"; a
 * count is what it always wanted to be, and a bare RECOMP_D3D8_PS_DUMP=1
 * still gives a useful dump, just of one shader rather than two. */
static int ps_dump_limit(void)
{
    const char *v = getenv("RECOMP_D3D8_PS_DUMP");
    int n;

    if (!v || !*v)
        return 0;
    n = atoi(v);
    return n > 0 ? n : 2;
}

/* The shader for g_combiner_state as last parsed. The lookup hashes ~1.5 KB
 * per call and was the largest host-side symbol in TimeSplitters 2's
 * profile; the state only changes when a PS render state does, so the
 * result is kept until the next parse. Only this path calls the lookup in
 * the runtime, so the entry cannot be evicted while it is held. */
static RhiShader *g_last_shader;

/* ================================================================
 * Color Helpers
 *
 * Convert D3DCOLOR (ARGB packed DWORD) to float4 (RGBA).
 * D3DCOLOR byte layout in memory: BGRA (little-endian ARGB).
 * ================================================================ */

static void d3dcolor_to_float4(DWORD color, float out[4])
{
    out[0] = ((color >> 16) & 0xFF) / 255.0f; /* R */
    out[1] = ((color >>  8) & 0xFF) / 255.0f; /* G */
    out[2] = ((color >>  0) & 0xFF) / 255.0f; /* B */
    out[3] = ((color >> 24) & 0xFF) / 255.0f; /* A */
}

/* ================================================================
 * Combiner Input Parsing
 *
 * Each combiner input is packed as 8 bits in the render state DWORDs:
 *   [3:0] register select (NV2ACombinerRegister value)
 *   [4]   alpha channel replicate
 *   [7:5] input mapping mode (NV2AInputMapping value)
 * ================================================================ */

/* The NV2A texture-shader modes, PS_TEXTUREMODES_* (xemu
 * hw/xbox/nv2a/pgraph/glsl/psh.c, which this file follows for each one). */
enum {
    XTM_NONE = 0x00, XTM_PROJECT2D = 0x01, XTM_PROJECT3D = 0x02,
    XTM_CUBEMAP = 0x03, XTM_PASSTHRU = 0x04, XTM_CLIPPLANE = 0x05,
    XTM_BUMPENVMAP = 0x06, XTM_BUMPENVMAP_LUM = 0x07, XTM_BRDF = 0x08,
    XTM_DOT_ST = 0x09, XTM_DOT_ZW = 0x0A, XTM_DOT_RFLCT_DIFF = 0x0B,
    XTM_DOT_RFLCT_SPEC = 0x0C, XTM_DOT_STR_3D = 0x0D, XTM_DOT_STR_CUBE = 0x0E,
    XTM_DPNDNT_AR = 0x0F, XTM_DPNDNT_GB = 0x10, XTM_DOTPRODUCT = 0x11,
    XTM_DOT_RFLCT_SPEC_CONST = 0x12, XTM_LAST = 0x12
};

/* What a stage in mode m samples: nothing for the modes whose result is a
 * dot product, the coordinates themselves or zero. */
static NV2ATextureMode xmode_sampler(int m)
{
    switch (m) {
    case XTM_PROJECT2D: case XTM_BUMPENVMAP: case XTM_BUMPENVMAP_LUM:
    case XTM_DOT_ST: case XTM_DPNDNT_AR: case XTM_DPNDNT_GB:
        return NV2A_TEXMODE_2D;
    case XTM_PROJECT3D: case XTM_DOT_STR_3D:
        return NV2A_TEXMODE_3D;
    case XTM_CUBEMAP: case XTM_DOT_RFLCT_DIFF: case XTM_DOT_RFLCT_SPEC:
    case XTM_DOT_STR_CUBE:
        return NV2A_TEXMODE_CUBEMAP;
    default:
        return NV2A_TEXMODE_NONE;
    }
}

static void parse_combiner_input(DWORD packed, NV2ACombinerInput *input)
{
    input->reg       = (NV2ACombinerRegister)(packed & 0xF);
    input->alpha_rep = (packed >> 4) & 1;
    input->mapping   = (NV2AInputMapping)((packed >> 5) & 0x7);
}

/**
 * Parse a 32-bit input register DWORD containing 4 packed inputs.
 *
 * The Xbox packs A first, in the most significant byte:
 *   PS_COMBINERINPUTS(a,b,c,d) = ((a)<<24)|((b)<<16)|((c)<<8)|(d)
 * (Cxbx-Reloaded, XbPixelShader.h). Layout: [31:24]=A [23:16]=B [15:8]=C
 * [7:0]=D.
 *
 * This read them the other way round, exchanging A with D and B with C in
 * every stage and in the final combiner. A stage whose other pair is zero
 * survives that, because AB + CD is symmetric, which is why textured
 * geometry still looked roughly right while the final combiner did not.
 */
static void parse_four_inputs(DWORD dword, NV2ACombinerInput inputs[4])
{
    parse_combiner_input((dword >> 24) & 0xFF, &inputs[0]); /* A */
    parse_combiner_input((dword >> 16) & 0xFF, &inputs[1]); /* B */
    parse_combiner_input((dword >>  8) & 0xFF, &inputs[2]); /* C */
    parse_combiner_input((dword >>  0) & 0xFF, &inputs[3]); /* D */
}

/**
 * Parse a 32-bit output configuration DWORD for one channel.
 *
 * Output DWORD layout (the PS_COMBINEROUTPUT_* flags sit from bit 12):
 *   [3:0]   CD destination register
 *   [7:4]   AB destination register
 *   [11:8]  SUM destination register
 *   [12]    CD dot product flag         (CD_DOT_PRODUCT 0x01)
 *   [13]    AB dot product flag         (AB_DOT_PRODUCT 0x02)
 *   [14]    mux instead of sum          (AB_CD_MUX 0x04)
 *   [17:15] output mapping (scale/bias) (OUTPUTMAPPING_* 0x08-0x38)
 *   [18]    CD blue to alpha            (CD_BLUE_TO_ALPHA 0x40)
 *   [19]    AB blue to alpha            (AB_BLUE_TO_ALPHA 0x80)
 */
static void parse_output(DWORD dword, NV2ACombinerOutput *output)
{
    /* PS_COMBINEROUTPUTS(ab,cd,mux_sum,flags) =
     *     ((flags)<<12)|((mux_sum)<<8)|((ab)<<4)|(cd)
     * (Cxbx-Reloaded, XbPixelShader.h), so CD is the low nibble and AB the
     * next one. These two were the other way round here. */
    output->cd_dst     = (NV2ACombinerRegister)((dword >>  0) & 0xF);
    output->ab_dst     = (NV2ACombinerRegister)((dword >>  4) & 0xF);
    output->sum_dst    = (NV2ACombinerRegister)((dword >>  8) & 0xF);
    output->cd_dot     = (dword >> 12) & 1;
    output->ab_dot     = (dword >> 13) & 1;
    output->mux_sum    = (dword >> 14) & 1;
    output->output_map = (NV2AOutputMapping)((dword >> 15) & 0x7);
    output->cd_blue_to_alpha = (dword >> 18) & 1;
    output->ab_blue_to_alpha = (dword >> 19) & 1;
}

/* ================================================================
 * Token & Render State Parsing
 * ================================================================ */

void d3d8_combiners_parse_token(DWORD token, const DWORD *rs,
                                NV2ACombinerState *state)
{
    int i;
    memset(state, 0, sizeof(*state));

    /* Bits [3:0]: number of active combiner stages (1-8) */
    state->num_stages = token & 0xF;
    if (state->num_stages < 1) state->num_stages = 1;
    if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
        state->num_stages = NV2A_MAX_COMBINER_STAGES;

    /* Bits [8:23]: texture mode per stage (4 bits each) */
    state->tex_mode[0] = (NV2ATextureMode)((token >>  8) & 0xF);
    state->tex_mode[1] = (NV2ATextureMode)((token >> 12) & 0xF);
    state->tex_mode[2] = (NV2ATextureMode)((token >> 16) & 0xF);
    state->tex_mode[3] = (NV2ATextureMode)((token >> 20) & 0xF);

    /* Bits [24:31]: dot mapping and other flags */
    state->flags = (token >> 24) & 0xFF;

    /* Parse per-stage inputs and outputs from render states */
    d3d8_combiners_from_render_states(rs, state);

    /* Preserve the token-derived fields (from_render_states may overwrite).
     *
     * The texture modes come back only when the token actually carries them.
     * A token with all its mode bits clear means "the modes are in
     * D3DRS_PSTEXTUREMODES", which from_render_states has just read; putting
     * the zeros back defeated that path entirely and left every stage
     * sampling a 2D texture, including the stages with nothing bound. */
    state->num_stages = token & 0xF;
    if (state->num_stages < 1) state->num_stages = 1;
    if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
        state->num_stages = NV2A_MAX_COMBINER_STAGES;
    if (((token >> 8) & 0xFFFF) != 0) {
        state->tex_mode[0] = (NV2ATextureMode)((token >>  8) & 0xF);
        state->tex_mode[1] = (NV2ATextureMode)((token >> 12) & 0xF);
        state->tex_mode[2] = (NV2ATextureMode)((token >> 16) & 0xF);
        state->tex_mode[3] = (NV2ATextureMode)((token >> 20) & 0xF);
    }
    state->flags = (token >> 24) & 0xFF;
}

void d3d8_combiners_from_render_states(const DWORD *rs,
                                       NV2ACombinerState *state)
{
    int i;

    /*
     * If called standalone (not from parse_token), read combiner count
     * from D3DRS_PSCOMBINERCOUNT render state.
     *
     * D3DRS_PSCOMBINERCOUNT layout:
     *   [3:0] number of stages
     *   [8]   unique C0 per stage (1) vs shared (0)
     *   [9]   unique C1 per stage (1) vs shared (0)
     *   [16]  mux_MSB for final combiner (not commonly used)
     */
    if (state->num_stages == 0) {
        state->num_stages = rs[D3DRS_PSCOMBINERCOUNT] & 0xF;
        if (state->num_stages < 1) state->num_stages = 1;
        if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
            state->num_stages = NV2A_MAX_COMBINER_STAGES;
    }

    /* Parse stage inputs */
    for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
        parse_four_inputs(rs[D3DRS_PSRGBINPUTS0 + i],
                          state->stages[i].rgb_input);
        parse_four_inputs(rs[D3DRS_PSALPHAINPUTS0 + i],
                          state->stages[i].alpha_input);
    }

    /* Parse stage outputs */
    for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
        parse_output(rs[D3DRS_PSRGBOUTPUTS0 + i],
                     &state->stages[i].rgb_output);
        parse_output(rs[D3DRS_PSALPHAOUTPUTS0 + i],
                     &state->stages[i].alpha_output);
    }

    /*
     * Parse final combiner inputs.
     *
     * D3DRS_PSFINALCOMBINERINPUTSABCD packs inputs A,B,C,D as 8 bits each:
     *   [7:0]=A  [15:8]=B  [23:16]=C  [31:24]=D
     *
     * D3DRS_PSFINALCOMBINERINPUTSEFG packs E,F,G:
     *   [7:0]=E  [15:8]=F  [23:16]=G  [31:24]=reserved
     */
    {
        DWORD abcd = rs[D3DRS_PSFINALCOMBINERINPUTSABCD];
        DWORD efg  = rs[D3DRS_PSFINALCOMBINERINPUTSEFG];

        /* Same packing as every other input DWORD: the first input is in the
         * most significant byte. Read low-first, Burnout 2's fog lerp
         * (A = fog alpha, B = r0, C = fog colour, D = zero) came out as
         * "r0 + fog alpha", which saturates to white, and G -- the final
         * alpha -- was read as zero.
         *
         * The low byte of EFG is PS_FINALCOMBINERSETTING (CLAMP_SUM 0x80,
         * COMPLEMENT_V1 0x40, COMPLEMENT_R0 0x20), which is not applied yet: a
         * title relying on those flags gets the uncomplemented, unclamped
         * value. */
        /* A shader that never programs the final combiner leaves both words
         * zero, and every one of TimeSplitters 2's front-end shaders does
         * (count 0x11101, one stage writing r0). Read literally that is
         * A = B = C = D = ZERO and G = ZERO: out = 0 + 0*0 + 1*0 = black with
         * alpha 0, which is what the shadow renderer drew -- a black frame
         * over a progress bar the fixed-function path showed plainly. The
         * console's D3D gives an unprogrammed final combiner the pass-through
         * the shader assembler documents as its default, `xfc r0.a, zero,
         * zero, zero, zero, zero, r0`: colour D = r0, alpha G = r0.a. Do the
         * same. A title that wants black writes ZERO into D explicitly, which
         * is a different word from "nothing written". */
        if (abcd == 0 && efg == 0) {
            abcd = 0x0000000Cu;              /* D = R0 */
            efg  = 0x00001C00u;              /* G = R0 alpha */
        }
        parse_combiner_input((abcd >> 24) & 0xFF, &state->final_input[0]); /* A */
        parse_combiner_input((abcd >> 16) & 0xFF, &state->final_input[1]); /* B */
        parse_combiner_input((abcd >>  8) & 0xFF, &state->final_input[2]); /* C */
        parse_combiner_input((abcd >>  0) & 0xFF, &state->final_input[3]); /* D */
        state->final_flags = efg & 0xE0u;
        parse_combiner_input((efg  >> 24) & 0xFF, &state->final_input[4]); /* E */
        parse_combiner_input((efg  >> 16) & 0xFF, &state->final_input[5]); /* F */
        parse_combiner_input((efg  >>  8) & 0xFF, &state->final_input[6]); /* G */
    }

    {
        DWORD cc = rs[D3DRS_PSCOMBINERCOUNT];
        state->count_flags = cc ? (NV2A_COUNT_KNOWN | ((cc >> 8) & 0x111u)) : 0;
    }

    /* Per-stage constant colors */
    for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
        state->c0[i] = rs[D3DRS_PSCONSTANT0_0 + i];
        state->c1[i] = rs[D3DRS_PSCONSTANT1_0 + i];
    }

    /* The final combiner has constants of its own -- the NV2A's
     * SPECULAR_FOG_FACTOR registers, D3D's PSFinalCombinerConstant0/1 -- and
     * its C0 and C1 inputs read those, not the last stage's. Taking the last
     * stage's made every Halo (XDK 3925) world and object surface black: its
     * eight-stage shaders end in a final combiner that mixes r0 with C0 and
     * keeps the last stage's constants at zero. */
    state->final_c0 = rs[D3DRS_PSFINALCOMBINERCONSTANT0];
    state->final_c1 = rs[D3DRS_PSFINALCOMBINERCONSTANT1];

    /* Read texture modes from render state if not already set by token */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        state->xmode[i] = NV2A_XMODE_UNKNOWN;
        state->dot_map[i] = 0;
        state->input_tex[i] = 0;
    }
    if (state->tex_mode[0] == 0 && state->tex_mode[1] == 0 &&
        state->tex_mode[2] == 0 && state->tex_mode[3] == 0) {
        DWORD tm = rs[D3DRS_PSTEXTUREMODES];
        if (tm & D3D8_PSTEXTUREMODES_XBOX) {
            /* The title's own modes, 5 bits a stage, with the dot-product
             * stages' input mapping (PSDotMapping: stage 1 in bits 0-3,
             * 2 in 4-7, 3 in 8-11) and source stage (PSInputTexture: stage 2
             * in bits 16-19, 3 in 20-23; stage 1 always reads stage 0). */
            DWORD dm = rs[D3DRS_PSDOTMAPPING], it = rs[D3DRS_PSINPUTTEXTURE];
            for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
                DWORD m = (tm >> (5 * i)) & 0x1F;
                state->xmode[i] = (uint8_t)(m <= XTM_LAST ? m : XTM_NONE);
                state->tex_mode[i] = xmode_sampler(state->xmode[i]);
            }
            state->dot_map[1] = (uint8_t)(dm & 0xF);
            state->dot_map[2] = (uint8_t)((dm >> 4) & 0xF);
            state->dot_map[3] = (uint8_t)((dm >> 8) & 0xF);
            state->input_tex[2] = (uint8_t)((it >> 16) & 0xF);
            state->input_tex[3] = (uint8_t)((it >> 20) & 0xF);
            for (i = 2; i < NV2A_MAX_TEXTURES; i++)
                if (state->input_tex[i] >= i)
                    state->input_tex[i] = 0;
        } else {
            state->tex_mode[0] = (NV2ATextureMode)((tm >>  0) & 0xF);
            state->tex_mode[1] = (NV2ATextureMode)((tm >>  4) & 0xF);
            state->tex_mode[2] = (NV2ATextureMode)((tm >>  8) & 0xF);
            state->tex_mode[3] = (NV2ATextureMode)((tm >> 12) & 0xF);
        }
    }
}

/* ================================================================
 * HLSL Code Generation
 *
 * Strategy: build the shader string via snprintf into a large buffer.
 * Each section appends to a running offset. This is not the prettiest
 * approach but it's straightforward, debuggable, and has zero
 * external dependencies.
 *
 * Generated shader structure:
 *   1. Texture sampler declarations
 *   2. Constant buffer (matches NV2APSConstants layout)
 *   3. Input struct (SV_POSITION, COLOR0, COLOR1, TEXCOORD0-3)
 *   4. Input mapping helper function
 *   5. Output mapping helper function
 *   6. main():
 *      a. Initialize register file from inputs
 *      b. Execute each general combiner stage
 *      c. Execute final combiner (fog is its A/B/C inputs; the NV2A has no
 *         fog stage after it)
 *      d. Apply alpha test
 *      e. Return result
 * ================================================================ */

/**
 * Emit HLSL to append a string to the output buffer.
 * Returns new offset, or -1 if buffer overflow.
 */
#define EMIT(fmt, ...) do { \
    int _n = snprintf(buf + off, bufsize - off, fmt, ##__VA_ARGS__); \
    if (_n < 0 || off + _n >= bufsize) return -1; \
    off += _n; \
} while (0)

/**
 * Get the HLSL variable name for a register in the NV2A register file.
 *
 * The register file is represented as local float4 variables in the
 * generated shader. This returns the name used in the HLSL code.
 */
static const char *reg_name(NV2ACombinerRegister reg)
{
    switch (reg) {
    case NV2A_REG_ZERO:     return "r_zero";
    case NV2A_REG_C0:       return "r_c0";
    case NV2A_REG_C1:       return "r_c1";
    case NV2A_REG_FOG:      return "r_fog";
    case NV2A_REG_V0:       return "r_v0";
    case NV2A_REG_V1:       return "r_v1";
    case NV2A_REG_T0:       return "r_t0";
    case NV2A_REG_T1:       return "r_t1";
    case NV2A_REG_T2:       return "r_t2";
    case NV2A_REG_T3:       return "r_t3";
    case NV2A_REG_R0:       return "r_r0";
    case NV2A_REG_R1:       return "r_r1";
    case NV2A_REG_EF_PROD:  return "r_ef";
    case NV2A_REG_V1R0_SUM: return "r_v1r0sum";
    default:                return "r_zero";
    }
}

/**
 * Emit HLSL expression for reading a combiner input.
 *
 * An input consists of:
 *   1. Register selection (which variable to read)
 *   2. Channel selection (full RGBA or alpha-replicated)
 *   3. Mapping function (how to transform the value)
 *
 * For alpha-replicate: .aaaa swizzle
 * For normal RGB read in RGB path: .rgb (or .rgba for alpha path)
 *
 * The mapping function is applied inline as an arithmetic expression.
 *
 * @param suffix  ".rgb" for RGB path, ".a" for alpha path (determines swizzle)
 */
static void emit_mapped_input(char *buf, int bufsize, int *off,
                               const NV2ACombinerInput *input,
                               const char *suffix, int c0_idx, int c1_idx)
{
    const char *rn;
    char swizzle[8];
    char base_expr[128];
    int n;

    rn = reg_name(input->reg);

    /* C0/C1 of the stage that holds them (its own, or stage 0's when the
     * stages share them); the final combiner (index < 0) has its own pair,
     * fc0 and fc1. */
    if (input->reg == NV2A_REG_C0) {
        if (c0_idx < 0)
            snprintf(base_expr, sizeof(base_expr), "fc0");
        else
            snprintf(base_expr, sizeof(base_expr), "c0[%d]", c0_idx);
    } else if (input->reg == NV2A_REG_C1) {
        if (c1_idx < 0)
            snprintf(base_expr, sizeof(base_expr), "fc1");
        else
            snprintf(base_expr, sizeof(base_expr), "c1[%d]", c1_idx);
    } else {
        snprintf(base_expr, sizeof(base_expr), "%s", rn);
    }

    /* Determine swizzle based on alpha replicate and target channel.
     *
     * The bit selects the channel, and what "not alpha" means depends on the
     * portion: in the RGB portion it is rgb, in the ALPHA portion it is the
     * register's BLUE component, not its alpha (NV_register_combiners, and
     * Cxbx-Reloaded XbPixelShader.cpp, which emits .b there). Reading alpha
     * for those inputs collapsed Burnout 2's alpha chains to zero, so its
     * car, HUD and fade -- every blended draw -- disappeared with the
     * combiners on. */
    if (input->alpha_rep) {
        /* Alpha replicate: use .aaa for RGB, .a for alpha */
        if (strcmp(suffix, ".a") == 0)
            snprintf(swizzle, sizeof(swizzle), ".a");
        else
            snprintf(swizzle, sizeof(swizzle), ".aaa");
    } else if (strcmp(suffix, ".a") == 0) {
        snprintf(swizzle, sizeof(swizzle), ".b");
    } else {
        snprintf(swizzle, sizeof(swizzle), "%s", suffix);
    }

    /* Build the full variable reference */
    char var_ref[160];
    snprintf(var_ref, sizeof(var_ref), "%s%s", base_expr, swizzle);

    /* Apply input mapping */
    switch (input->mapping) {
    case NV2A_MAP_UNSIGNED_IDENTITY:
        /* x - passthrough */
        n = snprintf(buf + *off, bufsize - *off, "max(%s, 0.0)", var_ref);
        break;
    case NV2A_MAP_UNSIGNED_INVERT:
        /* 1 - x, x clamped to [0,1] first */
        n = snprintf(buf + *off, bufsize - *off,
                     "(1.0 - saturate(%s))", var_ref);
        break;
    case NV2A_MAP_EXPAND_NORMAL:
        /* 2x - 1 */
        n = snprintf(buf + *off, bufsize - *off,
                     "(2.0 * max(%s, 0.0) - 1.0)", var_ref);
        break;
    case NV2A_MAP_EXPAND_NEGATE:
        /* 1 - 2x */
        n = snprintf(buf + *off, bufsize - *off,
                     "(1.0 - 2.0 * max(%s, 0.0))", var_ref);
        break;
    case NV2A_MAP_HALFBIAS_NORMAL:
        /* x - 0.5 */
        n = snprintf(buf + *off, bufsize - *off,
                     "(max(%s, 0.0) - 0.5)", var_ref);
        break;
    case NV2A_MAP_HALFBIAS_NEGATE:
        /* 0.5 - x */
        n = snprintf(buf + *off, bufsize - *off,
                     "(0.5 - max(%s, 0.0))", var_ref);
        break;
    case NV2A_MAP_SIGNED_IDENTITY:
        /* x (allow negative values) */
        n = snprintf(buf + *off, bufsize - *off, "%s", var_ref);
        break;
    case NV2A_MAP_SIGNED_NEGATE:
        /* -x */
        n = snprintf(buf + *off, bufsize - *off, "(-%s)", var_ref);
        break;
    default:
        n = snprintf(buf + *off, bufsize - *off, "%s", var_ref);
        break;
    }
    if (n > 0) *off += n;
}

/**
 * Emit HLSL expression for the output mapping (scale/bias).
 */
static const char *output_map_prefix(NV2AOutputMapping map)
{
    switch (map) {
    case NV2A_OUT_IDENTITY:         return "";
    case NV2A_OUT_BIAS:             return "(";
    case NV2A_OUT_SHIFTLEFT_1:      return "(";
    case NV2A_OUT_SHIFTLEFT_1_BIAS: return "((";
    case NV2A_OUT_SHIFTLEFT_2:      return "(";
    case NV2A_OUT_SHIFTLEFT_2_BIAS: return "((";
    case NV2A_OUT_SHIFTRIGHT_1:     return "(";
    case NV2A_OUT_SHIFTRIGHT_1_BIAS: return "((";
    default:                        return "";
    }
}

static const char *output_map_suffix(NV2AOutputMapping map)
{
    switch (map) {
    case NV2A_OUT_IDENTITY:         return "";
    case NV2A_OUT_BIAS:             return " - 0.5)";
    case NV2A_OUT_SHIFTLEFT_1:      return " * 2.0)";
    case NV2A_OUT_SHIFTLEFT_1_BIAS: return " - 0.5) * 2.0)";
    case NV2A_OUT_SHIFTLEFT_2:      return " * 4.0)";
    case NV2A_OUT_SHIFTLEFT_2_BIAS: return " - 0.5) * 4.0)";
    case NV2A_OUT_SHIFTRIGHT_1:     return " * 0.5)";
    case NV2A_OUT_SHIFTRIGHT_1_BIAS: return " - 0.5) * 0.5)";
    default:                        return "";
    }
}

/* A shadow-map stage (its texture is a depth format;
 * d3d8_combiners_prepare_draw sets state->shadow): r_t<i> is the compare,
 * whatever the stage's mode.
 * Projected, then four neighbouring texels compared and the results
 * filtered bilinearly: the hardware's soft edge. Load, so the comparison
 * sees stored depth, not a blend of it.
 *
 * Both texture paths call this -- the title's own modes
 * (emit_xbox_textures) as well as the plain sampler kinds -- so the
 * compare does not depend on how the stage's mode reached the host. The
 * caller declares r_t<i>. */
static int emit_shadow_compare(char *buf, int bufsize, int *poff, int i)
{
    int off = *poff;

    EMIT("    {\n");
    EMIT("        float4 q = input.tc%d;\n", i);
    EMIT("        float qw = abs(q.w) > 1e-20 ? q.w : 1.0;\n");
    EMIT("        float2 uv = q.xy / qw * tex_scale[%d].xy;\n", i);
    EMIT("        float ref = q.z / qw;\n");
    EMIT("        float m = shadow_max[%d];\n", i);
    EMIT("        float2 sz; tex%d.GetDimensions(sz.x, sz.y);\n", i);
    EMIT("        float2 p = uv * sz - 0.5;\n");
    EMIT("        float2 f = frac(p);\n");
    EMIT("        int2 c0 = (int2)floor(p), hi = (int2)sz - 1;\n");
    EMIT("        float s00 = shadow_test(tex%d.Load(int3(clamp(c0, 0, hi), 0)).r * m, ref);\n", i);
    EMIT("        float s10 = shadow_test(tex%d.Load(int3(clamp(c0 + int2(1, 0), 0, hi), 0)).r * m, ref);\n", i);
    EMIT("        float s01 = shadow_test(tex%d.Load(int3(clamp(c0 + int2(0, 1), 0, hi), 0)).r * m, ref);\n", i);
    EMIT("        float s11 = shadow_test(tex%d.Load(int3(clamp(c0 + int2(1, 1), 0, hi), 0)).r * m, ref);\n", i);
    EMIT("        float s = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);\n");
    /* RECOMP_D3D8_SHADOW_VIEW: the draw shows one of these instead
     * of its colour -- 1 the stored depth, 2 the reference r/q, 3 and
     * 4 the texel position across and down, 5 the filtered test, 6
     * the reference unscaled and 7 its fraction (its range, when 2
     * reads black). */
    EMIT("        if (shadow_func.z == 1) shadow_dbg = tex%d.Load(int3(clamp(c0, 0, hi), 0)).r;\n", i);
    EMIT("        if (shadow_func.z == 2) shadow_dbg = saturate(ref / m);\n");
    EMIT("        if (shadow_func.z == 3) shadow_dbg = saturate(uv.x);\n");
    EMIT("        if (shadow_func.z == 4) shadow_dbg = saturate(uv.y);\n");
    EMIT("        if (shadow_func.z == 5) shadow_dbg = s;\n");
    EMIT("        if (shadow_func.z == 6) shadow_dbg = saturate(ref);\n");
    EMIT("        if (shadow_func.z == 7) shadow_dbg = frac(ref);\n");
    EMIT("        r_t%d = float4(s, s, s, s);\n", i);
    EMIT("    }\n");
    *poff = off;
    return off;
}

/* The four texture registers from the title's own texture modes, in stage
 * order (a dependent stage reads an earlier one). Each mode as xemu's
 * psh.c has it, with these differences: BUMPENVMAP and BUMPENVMAP_LUM look
 * up their own coordinates without the bump offset (the bump-environment
 * matrix is a texture stage state not forwarded yet), CLIPPLANE kills
 * nothing (PSCOMPAREMODE is not forwarded), DOT_ZW does not replace depth.
 * A mode at a stage the hardware does not allow it at reads zero. */
static int emit_xbox_textures(const NV2ACombinerState *state, char *buf,
                              int bufsize, int *poff)
{
    int off = *poff, i;

    EMIT("    float dot1 = 0.0, dot2 = 0.0, dot3 = 0.0;\n");
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        int m = state->xmode[i];
        int in = state->input_tex[i];
        int dm = state->dot_map[i] <= 3 ? state->dot_map[i] : 0;
        int dm3 = state->dot_map[3] <= 3 ? state->dot_map[3] : 0;

        EMIT("    float4 r_t%d = float4(0, 0, 0, 0);\n", i);
        if (state->shadow[i]) {
            *poff = off;
            if (emit_shadow_compare(buf, bufsize, poff, i) < 0)
                return -1;
            off = *poff;
            continue;   /* no alpha_only fix-up: the result is the test */
        }
        switch (m) {
        case XTM_NONE:
            EMIT("    r_t%d = float4(0, 0, 0, 1);\n", i);
            break;
        case XTM_PROJECT2D:
        case XTM_BUMPENVMAP:
        case XTM_BUMPENVMAP_LUM:
            EMIT("    r_t%d = tex%d.Sample(samp%d, input.tc%d.xy / qdiv(input.tc%d.w)"
                 " * tex_scale[%d].xy);\n", i, i, i, i, i, i);
            break;
        case XTM_PROJECT3D:
            EMIT("    r_t%d = tex%d.Sample(samp%d, input.tc%d.xyz / qdiv(input.tc%d.w));\n",
                 i, i, i, i, i);
            break;
        case XTM_CUBEMAP:
            EMIT("    r_t%d = tex%d.Sample(samp%d, input.tc%d.xyz);\n", i, i, i, i);
            break;
        case XTM_PASSTHRU:
            EMIT("    r_t%d = saturate(input.tc%d);\n", i, i);
            break;
        case XTM_DOT_ST:
            if (i < 2)
                break;
            EMIT("    dot%d = dot(input.tc%d.xyz, dotmap%d(r_t%d));\n", i, i, dm, in);
            EMIT("    r_t%d = tex%d.Sample(samp%d, float2(dot%d, dot%d) * tex_scale[%d].xy);\n",
                 i, i, i, i - 1, i, i);
            break;
        case XTM_DOTPRODUCT:
        case XTM_DOT_ZW:
            if (i < 1 || (m == XTM_DOTPRODUCT && i > 2) || (m == XTM_DOT_ZW && i < 2))
                break;
            EMIT("    dot%d = dot(input.tc%d.xyz, dotmap%d(r_t%d));\n", i, i, dm, in);
            break;
        case XTM_DOT_RFLCT_DIFF:
            /* Stage 2 only: the normal is the three dot products, the third
             * computed here with stage 3's mapping and source. */
            if (i != 2)
                break;
            EMIT("    dot2 = dot(input.tc2.xyz, dotmap%d(r_t%d));\n", dm, in);
            EMIT("    r_t2 = tex2.Sample(samp2, float3(dot1, dot2,"
                 " dot(input.tc3.xyz, dotmap%d(r_t%d))));\n", dm3, state->input_tex[3]);
            break;
        case XTM_DOT_RFLCT_SPEC:
            /* Stage 3 only: the eye vector is the three stages' q, and the
             * lookup is along it reflected about the normal. */
            if (i != 3)
                break;
            EMIT("    dot3 = dot(input.tc3.xyz, dotmap%d(r_t%d));\n", dm, in);
            EMIT("    {\n"
                 "        float3 n = float3(dot1, dot2, dot3);\n"
                 "        float3 e = float3(input.tc1.w, input.tc2.w, input.tc3.w);\n"
                 "        float nn = dot(n, n);\n"
                 "        float3 rv = 2.0 * n * dot(n, e) / (nn != 0.0 ? nn : 1.0) - e;\n"
                 "        r_t3 = tex3.Sample(samp3, rv);\n"
                 "    }\n");
            break;
        case XTM_DOT_STR_3D:
        case XTM_DOT_STR_CUBE:
            if (i != 3)
                break;
            EMIT("    dot3 = dot(input.tc3.xyz, dotmap%d(r_t%d));\n", dm, in);
            EMIT("    r_t3 = tex3.Sample(samp3, float3(dot1, dot2, dot3));\n");
            break;
        case XTM_DPNDNT_AR:
        case XTM_DPNDNT_GB:
            if (i < 1)
                break;
            EMIT("    r_t%d = tex%d.Sample(samp%d, r_t%d.%s * tex_scale[%d].xy);\n",
                 i, i, i, in, m == XTM_DPNDNT_AR ? "ar" : "gb", i);
            break;
        default:            /* CLIPPLANE, BRDF, DOT_RFLCT_SPEC_CONST: zero */
            break;
        }
        if (state->tex_mode[i] != NV2A_TEXMODE_NONE)
            EMIT("    if (alpha_only[%d]) r_t%d.rgb = 1.0;\n", i, i);
    }
    *poff = off;
    return off;
}

int d3d8_combiners_generate_hlsl(const NV2ACombinerState *state,
                                 char *buf, int bufsize)
{
    int off = 0;
    int i;

    /* ---- Texture samplers ---- */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (state->tex_mode[i] != NV2A_TEXMODE_NONE) {
            if (state->shadow[i]) {
                /* A depth texture: read as 2D whatever the stage mode, and
                 * compared rather than filtered (below). */
                EMIT("Texture2D    tex%d : register(t%d);\n", i, i);
            } else if (state->tex_mode[i] == NV2A_TEXMODE_CUBEMAP) {
                EMIT("TextureCube  tex%d : register(t%d);\n", i, i);
            } else if (state->tex_mode[i] == NV2A_TEXMODE_3D) {
                EMIT("Texture3D    tex%d : register(t%d);\n", i, i);
            } else {
                EMIT("Texture2D    tex%d : register(t%d);\n", i, i);
            }
            EMIT("SamplerState samp%d : register(s%d);\n", i, i);
        }
    }
    EMIT("\n");

    /* ---- Constant buffer ---- */
    EMIT("cbuffer CombinerCB : register(b0) {\n");
    EMIT("    float4 c0[8];\n");    /* Per-stage C0 */
    EMIT("    float4 c1[8];\n");    /* Per-stage C1 */
    EMIT("    float4 fc0;\n");      /* Final combiner C0 */
    EMIT("    float4 fc1;\n");      /* Final combiner C1 */
    EMIT("    float4 fog_color;\n");
    EMIT("    float  alpha_ref;\n");
    EMIT("    uint   alpha_func;\n");
    EMIT("    uint   alpha_test_enable;\n");
    EMIT("    uint   fog_enable;\n");
    EMIT("    uint4  alpha_only;\n");
    EMIT("    float4 tex_scale[4];\n");   /* texel -> normalised, linear textures */
    EMIT("    float4 shadow_max;\n");     /* per stage: the depth format's top */
    EMIT("    uint4  shadow_func;\n");    /* x: D3DCMPFUNC, y: compare reversed */
    EMIT("};\n\n");

    /* ---- Input structure ----
     *
     * The vertex and pixel stages are linked by register, not by name
     * alone: each compiler packs its own signature, and a pixel shader reads
     * whichever register its packing gave an input. So every vertex stage
     * (d3d8_shaders.c's fixed function, d3d8_vsh.c's programs) and every
     * pixel stage (this one and d3d8_shaders.c's) declares the same
     * interface: four float4 texture coordinates, the fog factor at
     * TEXCOORD4, then the view position at TEXCOORD5 (which this one does
     * not read). The coordinates here used to be float3 while the programs
     * wrote float4: the compiler packed `fog` into TEXCOORD3's spare w, and
     * under a vertex program every combiner read oT3.w -- 1.0 unless the
     * program wrote it -- as its fog factor. */
    EMIT("struct PS_IN {\n");
    EMIT("    float4 pos     : SV_POSITION;\n");
    EMIT("    float4 color0  : COLOR0;\n");
    EMIT("    float4 color1  : COLOR1;\n");
    /* All four components: q divides a projected 2D lookup and a shadow-map
     * stage's compare, and the reflection modes take the eye vector from the
     * three stages' w. */
    EMIT("    float4 tc0     : TEXCOORD0;\n");
    EMIT("    float4 tc1     : TEXCOORD1;\n");
    EMIT("    float4 tc2     : TEXCOORD2;\n");
    EMIT("    float4 tc3     : TEXCOORD3;\n");
    /* The fog register's alpha is the fog factor the vertex stage
     * interpolated, which both vertex paths write to TEXCOORD4
     * (d3d8_shaders.c for fixed function, d3d8_vsh.c for a program). */
    EMIT("    float  fog     : TEXCOORD4;\n");
    EMIT("};\n\n");

    /* ---- Shadow compare ---- */
    for (i = 0; i < NV2A_MAX_TEXTURES && !state->shadow[i]; i++)
        ;
    if (i < NV2A_MAX_TEXTURES) {
        /* The NV2A's shadow-map read: the texel, in the depth format's own
         * units, against r/q under D3DRS_SHADOWFUNC; 1 where the test
         * passes. 0 (unset) passes everywhere rather than shadowing all. */
        EMIT("float shadow_test(float texel, float ref) {\n");
        EMIT("    float a = shadow_func.y ? ref : texel;\n");
        EMIT("    float b = shadow_func.y ? texel : ref;\n");
        EMIT("    switch (shadow_func.x) {\n");
        EMIT("    case 1: return 0.0;\n");
        EMIT("    case 2: return a <  b ? 1.0 : 0.0;\n");
        EMIT("    case 3: return a == b ? 1.0 : 0.0;\n");
        EMIT("    case 4: return a <= b ? 1.0 : 0.0;\n");
        EMIT("    case 5: return a >  b ? 1.0 : 0.0;\n");
        EMIT("    case 6: return a != b ? 1.0 : 0.0;\n");
        EMIT("    case 7: return a >= b ? 1.0 : 0.0;\n");
        EMIT("    default: return 1.0;\n");
        EMIT("    }\n");
        EMIT("}\n\n");
    }

    /* The dot-product stages' input mappings, PS_DOTMAPPING_* (xemu psh.c
     * dotmap_*): 0..1 as is, and three ways of reading an unsigned byte as
     * -1..1. The HILO mappings (4-7) are read as 0..1. */
    if (state->xmode[0] != NV2A_XMODE_UNKNOWN) {
        EMIT("float qdiv(float q) { return q != 0.0 ? q : 1.0; }\n");
        EMIT("float3 dotmap0(float4 c) { return c.rgb; }\n");
        EMIT("float3 dotmap1(float4 c) { return (c.rgb * 255.0 - 128.0) / 127.0; }\n");
        EMIT("float3 dotmap2(float4 c) { float3 x = c.rgb * 255.0;\n"
             "    return x >= 128.0 ? (x - 255.5) / 127.5 : (x + 0.5) / 127.5; }\n");
        EMIT("float3 dotmap3(float4 c) { float3 x = c.rgb * 255.0;\n"
             "    return x >= 128.0 ? (x - 256.0) / 127.0 : x / 127.0; }\n\n");
    }

    /* ---- Main function ---- */
    EMIT("float4 main(PS_IN input) : SV_TARGET {\n");
    EMIT("    float shadow_dbg = 0.0;\n");

    /* Initialize register file */
    EMIT("    /* Register file initialization */\n");
    EMIT("    float4 r_zero = float4(0, 0, 0, 0);\n");
    EMIT("    float4 r_c0   = c0[0];\n");
    EMIT("    float4 r_c1   = c1[0];\n");
    /* The NV2A fog register is the fog colour in rgb and the interpolated fog
     * factor in alpha. Taking alpha from the fog colour instead made the
     * final combiner's usual lerp -- A = fog alpha, B = shaded colour,
     * C = fog colour -- constant across the frame, which washed the whole
     * image in fog colour or saturated it to white.
     *
     * The factor is the vertex stage's fog output whether or not fog is
     * enabled. It used to read 1 (no fog) with fog disabled, because taking
     * the interpolated value regardless blanked Burnout 2's menus to the fog
     * colour -- but under a vertex program the value interpolated then was
     * not the fog output at all (PS_IN above). With the stages linked, a
     * program that writes no fog delivers 1 (its default), and the
     * fixed-function stage writes 1 unless fog is on. TimeSplitters: Future
     * Perfect needs the real value: it turns fog off and blends by its own
     * factor, computed in its vertex programs (A = fog alpha, B = fog
     * colour, C = r0), so with 1 forced here its guns, hands and cave came
     * out the flat fog colour.
     *
     * A program that never writes oFog has no value to hand on, and what it
     * stands for depends on fog: 1 with fog on, 0 with it off. Future
     * Perfect's particle program writes none and draws its muzzle flash,
     * dust and sparks with fog off through that same final combiner; read
     * as 1, every flash quad added a square of fog colour (play-tests 2 and
     * 3), where 0 leaves the textured flare. Its sky is a program without
     * oFog drawn with fog on, and stays the fog colour it was. A written
     * value is used as before, fog on or off. */
    EMIT("    float4 r_fog  = float4(fog_color.rgb, "
         D3D8_VSH_FOG_IS_UNWRITTEN("input.fog") " ? (fog_enable ? 1.0 : 0.0)"
         " : saturate(input.fog));\n");

    /* Vertex colors: Xbox D3DCOLOR is BGRA in memory, the vertex shader
     * should have already swizzled to RGBA. */
    EMIT("    float4 r_v0   = input.color0;\n");
    EMIT("    float4 r_v1   = input.color1;\n");

    /* Texture samples */
    if (state->xmode[0] != NV2A_XMODE_UNKNOWN) {
        if (emit_xbox_textures(state, buf, bufsize, &off) < 0)
            return -1;
    } else for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (state->tex_mode[i] == NV2A_TEXMODE_NONE) {
            EMIT("    float4 r_t%d = float4(0, 0, 0, 0);\n", i);
        } else if (state->shadow[i]) {
            EMIT("    float4 r_t%d;\n", i);
            if (emit_shadow_compare(buf, bufsize, &off, i) < 0)
                return -1;
            continue;   /* no alpha_only fix-up: the result is the test */
        } else if (state->tex_mode[i] == NV2A_TEXMODE_CUBEMAP) {
            /* Cube map: use the full 3-component reflection/TCI vector */
            EMIT("    float4 r_t%d = tex%d.Sample(samp%d, input.tc%d.xyz);\n",
                 i, i, i, i);
        } else if (state->tex_mode[i] == NV2A_TEXMODE_3D) {
            /* 3D texture: use the full 3-component coordinate */
            EMIT("    float4 r_t%d = tex%d.Sample(samp%d, input.tc%d.xyz);\n",
                 i, i, i, i);
        } else {
            EMIT("    float4 r_t%d = tex%d.Sample(samp%d, input.tc%d.xy * tex_scale[%d].xy);\n",
                 i, i, i, i, i);
        }
        /* Preserve disabled stages and sampled alpha. */
        if (state->tex_mode[i] != NV2A_TEXMODE_NONE)
            EMIT("    if (alpha_only[%d]) r_t%d.rgb = 1.0;\n", i, i);
    }

    /* Temporary registers: r0 starts at zero with texture 0's alpha, or 1
     * when stage 0 samples nothing (xemu psh.c; upstream nv2a_combiner.c);
     * r1 at zero. */
    EMIT("    float4 r_r0 = float4(0, 0, 0, %s);\n",
         (state->xmode[0] != NV2A_XMODE_UNKNOWN ? state->xmode[0] != XTM_NONE
                                                : state->tex_mode[0] != NV2A_TEXMODE_NONE)
         ? "r_t0.a" : "1.0");
    EMIT("    float4 r_r1 = float4(0, 0, 0, 0);\n\n");

    /* ---- General combiner stages ----
     *
     * A stage as the NV2A computes it (xemu psh.c, add_stage_code and
     * get_combiner_output; upstream xboxrecomp src/kernel/nv2a_combiner.c):
     *  - both portions read all their inputs before either writes, so an
     *    alpha input sees the registers as the stage found them, not what
     *    its own RGB portion just wrote;
     *  - the products, and the sum or mux of the raw products, go through
     *    the output mapping and are clamped to -1..1, as main already did at
     *    the write (Outrun 2's road came out white without the clamp);
     *  - the mux returns CD when its bit of r0.a is set and AB otherwise
     *    (this had it the other way round), testing r0.a >= 0.5 with
     *    PS_COMBINERCOUNT_MUX_MSB and the byte's low bit without it;
     *  - a stage reads its own C0/C1 with PS_COMBINERCOUNT_UNIQUE_C0/C1 and
     *    stage 0's without;
     *  - AB_BLUE_TO_ALPHA / CD_BLUE_TO_ALPHA also write the product's blue
     *    to the destination's alpha (RGB portion only).
     * When PSCOMBINERCOUNT was never given (count_flags 0) the constants and
     * the mux bit stay as they were: per stage, and r0.a >= 0.5. */
    for (i = 0; i < state->num_stages; i++) {
        const NV2ACombinerInput *rgb_in  = state->stages[i].rgb_input;
        const NV2ACombinerInput *alpha_in = state->stages[i].alpha_input;
        const NV2ACombinerOutput *rgb_out = &state->stages[i].rgb_output;
        const NV2ACombinerOutput *alpha_out = &state->stages[i].alpha_output;
        int known = (state->count_flags & NV2A_COUNT_KNOWN) != 0;
        int c0i = (!known || (state->count_flags & 0x010u)) ? i : 0;
        int c1i = (!known || (state->count_flags & 0x100u)) ? i : 0;
        const char *omp, *oms;
        int k;
        static const char *const rn_rgb[4] = { "a_rgb", "b_rgb", "c_rgb", "d_rgb" };
        static const char *const rn_a[4] = { "a_a", "b_a", "c_a", "d_a" };

        EMIT("    /* ---- Stage %d ---- */\n", i);
        EMIT("    r_c0 = c0[%d];\n", c0i);
        EMIT("    r_c1 = c1[%d];\n", c1i);
        EMIT("    {\n");
        EMIT("        bool mux_cd = %s;\n", (!known || (state->count_flags & 0x001u))
             ? "r_r0.a >= 0.5"
             : "(((uint)(saturate(r_r0.a) * 255.0 + 0.5)) & 1u) != 0u");
        for (k = 0; k < 4; k++) {
            EMIT("        float3 %s = ", rn_rgb[k]);
            emit_mapped_input(buf, bufsize, &off, &rgb_in[k], ".rgb", c0i, c1i);
            EMIT(";\n");
        }
        for (k = 0; k < 4; k++) {
            EMIT("        float %s = ", rn_a[k]);
            emit_mapped_input(buf, bufsize, &off, &alpha_in[k], ".a", c0i, c1i);
            EMIT(";\n");
        }
        EMIT("        float3 ab_rgb = %s;\n",
             rgb_out->ab_dot ? "dot(a_rgb, b_rgb).xxx" : "a_rgb * b_rgb");
        EMIT("        float3 cd_rgb = %s;\n",
             rgb_out->cd_dot ? "dot(c_rgb, d_rgb).xxx" : "c_rgb * d_rgb");
        EMIT("        float3 sum_rgb = %s;\n",
             rgb_out->mux_sum ? "mux_cd ? cd_rgb : ab_rgb" : "ab_rgb + cd_rgb");
        EMIT("        float ab_a = a_a * b_a;\n");
        EMIT("        float cd_a = c_a * d_a;\n");
        EMIT("        float sum_a = %s;\n",
             alpha_out->mux_sum ? "mux_cd ? cd_a : ab_a" : "ab_a + cd_a");

        omp = output_map_prefix(rgb_out->output_map);
        oms = output_map_suffix(rgb_out->output_map);
        EMIT("        ab_rgb = clamp(%sab_rgb%s, -1.0, 1.0);\n", omp, oms);
        EMIT("        cd_rgb = clamp(%scd_rgb%s, -1.0, 1.0);\n", omp, oms);
        EMIT("        sum_rgb = clamp(%ssum_rgb%s, -1.0, 1.0);\n", omp, oms);
        omp = output_map_prefix(alpha_out->output_map);
        oms = output_map_suffix(alpha_out->output_map);
        EMIT("        ab_a = clamp(%sab_a%s, -1.0, 1.0);\n", omp, oms);
        EMIT("        cd_a = clamp(%scd_a%s, -1.0, 1.0);\n", omp, oms);
        EMIT("        sum_a = clamp(%ssum_a%s, -1.0, 1.0);\n", omp, oms);

        if (rgb_out->ab_dst != NV2A_REG_ZERO) {
            EMIT("        %s.rgb = ab_rgb;\n", reg_name(rgb_out->ab_dst));
            if (rgb_out->ab_blue_to_alpha)
                EMIT("        %s.a = ab_rgb.b;\n", reg_name(rgb_out->ab_dst));
        }
        if (rgb_out->cd_dst != NV2A_REG_ZERO) {
            EMIT("        %s.rgb = cd_rgb;\n", reg_name(rgb_out->cd_dst));
            if (rgb_out->cd_blue_to_alpha)
                EMIT("        %s.a = cd_rgb.b;\n", reg_name(rgb_out->cd_dst));
        }
        if (rgb_out->sum_dst != NV2A_REG_ZERO)
            EMIT("        %s.rgb = sum_rgb;\n", reg_name(rgb_out->sum_dst));
        if (alpha_out->ab_dst != NV2A_REG_ZERO)
            EMIT("        %s.a = ab_a;\n", reg_name(alpha_out->ab_dst));
        if (alpha_out->cd_dst != NV2A_REG_ZERO)
            EMIT("        %s.a = cd_a;\n", reg_name(alpha_out->cd_dst));
        if (alpha_out->sum_dst != NV2A_REG_ZERO)
            EMIT("        %s.a = sum_a;\n", reg_name(alpha_out->sum_dst));
        EMIT("    }\n\n");
    }

    /* ---- Final combiner ----
     *
     * The NV2A final combiner computes:
     *   result.rgb = D + lerp(C, B, A)
     *              = D + A*B + (1-A)*C
     *   result.a   = G.a
     *
     * Additionally, E*F is computed and made available as the EF_PROD
     * register, and V1+R0 is available as V1R0_SUM. These are computed
     * BEFORE the final combiner reads its inputs.
     */
    EMIT("    /* ---- Final Combiner ---- */\n");

    /* Compute specials: EF product and V1R0 sum */
    EMIT("    float4 r_ef = float4(0, 0, 0, 0);\n");
    EMIT("    float4 r_v1r0sum = float4(0, 0, 0, 0);\n");

    /* V1 + R0, colour only, with the final combiner settings: either
     * term complemented (COMPLEMENT_V1 0x40, COMPLEMENT_R0 0x20) and the sum
     * clamped only with CLAMP_SUM (0x80). This clamped it always and never
     * complemented. Then E * F, colour only, as upstream's executor has
     * both. */
    EMIT("    r_v1r0sum.rgb = %s(%s + %s);\n",
         (state->final_flags & 0x80u) ? "saturate" : "",
         (state->final_flags & 0x40u) ? "(1.0 - r_v1.rgb)" : "r_v1.rgb",
         (state->final_flags & 0x20u) ? "(1.0 - r_r0.rgb)" : "r_r0.rgb");
    EMIT("    r_ef.rgb = ");
    emit_mapped_input(buf, bufsize, &off, &state->final_input[4], ".rgb", -1, -1);
    EMIT(" * ");
    emit_mapped_input(buf, bufsize, &off, &state->final_input[5], ".rgb", -1, -1);
    EMIT(";\n\n");

    /* Final combiner: result.rgb = D + A*B + (1-A)*C */
    /* C0/C1 in the final combiner are its own constants (fc0, fc1) */
    {
        int fc_stage = -1;

        EMIT("    float4 result;\n");
        EMIT("    {\n");

        /* Read final combiner inputs A, B, C, D */
        EMIT("        float3 fc_a = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[0], ".rgb", fc_stage, fc_stage);
        EMIT(";\n");
        EMIT("        float3 fc_b = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[1], ".rgb", fc_stage, fc_stage);
        EMIT(";\n");
        EMIT("        float3 fc_c = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[2], ".rgb", fc_stage, fc_stage);
        EMIT(";\n");
        EMIT("        float3 fc_d = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[3], ".rgb", fc_stage, fc_stage);
        EMIT(";\n");

        /* result.rgb = D + lerp(C, B, A) = D + A*B + (1-A)*C */
        EMIT("        result.rgb = saturate(fc_d + fc_a * fc_b + (1.0 - fc_a) * fc_c);\n");

        /* result.a = G.a */
        EMIT("        result.a = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[6], ".a", fc_stage, fc_stage);
        EMIT(";\n");

        EMIT("    }\n\n");
    }

    /* ---- Fog ---- */
    /* No fog after the final combiner. The NV2A has no fog stage of its own:
     * the XDK puts fog into the final combiner (A = fog alpha, B = r0,
     * C = fog colour), and that has already run. Blending again here fogged
     * every pixel twice. */

    /* ---- Alpha test ---- */
    EMIT("    /* Alpha test */\n");
    EMIT("    if (alpha_test_enable) {\n");
    EMIT("        bool alpha_pass = true;\n");
    EMIT("        if      (alpha_func == 1u) alpha_pass = false;\n");
    EMIT("        else if (alpha_func == 2u) alpha_pass = (result.a <  alpha_ref);\n");
    EMIT("        else if (alpha_func == 3u) alpha_pass = (result.a == alpha_ref);\n");
    EMIT("        else if (alpha_func == 4u) alpha_pass = (result.a <= alpha_ref);\n");
    EMIT("        else if (alpha_func == 5u) alpha_pass = (result.a >  alpha_ref);\n");
    EMIT("        else if (alpha_func == 6u) alpha_pass = (result.a != alpha_ref);\n");
    EMIT("        else if (alpha_func == 7u) alpha_pass = (result.a >= alpha_ref);\n");
    EMIT("        if (!alpha_pass) discard;\n");
    EMIT("    }\n\n");

    {
        /* Debug switch, RECOMP_D3D8_PS_SHOW=v0|v1|t0|t1|r0|r1: replaces
         * the result with one register, so a frame shows what the combiners
         * were actually handed rather than what they made of it. Also foga
         * (the fog factor), fogon (white where fog is enabled), tc0 (stage 0
         * coordinates, wrapped) and tc0raw (their size, /16). */
        static const char *show = (const char *)-1;
        static const char *names[] = { "v0", "v1", "t0", "t1", "t2", "t3", "r0", "r1" };
        size_t n;

        if (show == (const char *)-1)
            show = getenv("RECOMP_D3D8_PS_SHOW");
        for (n = 0; show && n < sizeof names / sizeof names[0]; n++) {
            if (strcmp(show, names[n]) == 0) {
                EMIT("    result.rgb = r_%s.rgb;\n", names[n]);
                EMIT("    result.a = 1.0;\n");
                break;
            }
        }
        if (show && n < sizeof names / sizeof names[0]) {
            /* nothing: one of the register views above matched */
        } else if (show && strlen(show) == 3 && show[2] == 'a') {
            /* r0a, v0a, t0a ...: that register's alpha as grey, which is
             * what a blended draw is multiplied by. */
            char reg[3] = { show[0], show[1], 0 };

            for (n = 0; n < sizeof names / sizeof names[0]; n++)
                if (strcmp(reg, names[n]) == 0) {
                    EMIT("    result.rgb = r_%s.aaa;\n", names[n]);
                    EMIT("    result.a = 1.0;\n");
                    break;
                }
        }
        if (show && strcmp(show, "foga") == 0) {     /* the fog factor */
            EMIT("    result.rgb = r_fog.aaa;\n");
            EMIT("    result.a = 1.0;\n");
        } else if (show && strcmp(show, "tc0") == 0) {    /* stage 0 coordinates */
            EMIT("    result.rgb = float3(frac(input.tc0.xy), saturate(input.tc0.z));\n");
            EMIT("    result.a = 1.0;\n");
        } else if (show && strcmp(show, "tc0raw") == 0) { /* coordinate size */
            EMIT("    result.rgb = saturate(abs(input.tc0.xyz) / 16.0);\n");
            EMIT("    result.a = 1.0;\n");
        } else if (show && strcmp(show, "fogon") == 0) {  /* fog enabled: white */
            EMIT("    result.rgb = fog_enable ? 1.0 : 0.0;\n");
            EMIT("    result.a = 1.0;\n");
        }
    }

    for (i = 0; i < NV2A_MAX_TEXTURES && !state->shadow[i]; i++)
        ;
    if (i < NV2A_MAX_TEXTURES)
        EMIT("    if (shadow_func.z) return float4(shadow_dbg, shadow_dbg, shadow_dbg, 1.0);\n");
    EMIT("    return result;\n");
    EMIT("}\n");

    return off;
}

#undef EMIT

/* ================================================================
 * Shader Compilation & Cache
 * ================================================================ */

static RhiShader *compile_combiner_shader(const NV2ACombinerState *state)
{
    /* 16KB should be more than enough for any combiner shader */
    char hlsl[16384];
    char err[4096];
    RhiShaderSource ss;
    RhiShader *ps = NULL;
    int len;

    len = d3d8_combiners_generate_hlsl(state, hlsl, sizeof(hlsl));
    if (len < 0) {
        fprintf(stderr, "NV2A combiners: HLSL generation failed (buffer overflow)\n");
        return NULL;
    }

    {
        /* Debug switch, RECOMP_D3D8_PS_DUMP=<n>: prints the source of the
         * first n shaders built (n defaults to 2). The source is otherwise
         * only printed when the compile fails, which says nothing about a
         * shader that compiles and draws the wrong colour.
         *
         * This is called on a cache miss, so the order here is the order
         * shaders are *built*, which is not the order they are drawn with and
         * not the title's own pixel shader numbering. Reading a dump as
         * belonging to a particular draw is therefore a guess -- one that
         * cost a wrong diagnosis of the TimeSplitters 2 brightness bug. The
         * hash below is the same value d3d8_combiners_apply() prints when it
         * binds a shader, so draw and source can be matched instead. */
        static int dumped, limit = -1;

        if (limit < 0)
            limit = ps_dump_limit();
        if (dumped < limit) {
            dumped++;
            fprintf(stderr, "NV2A combiners: shader %08lX: state stages %d, "
                    "tex_mode %d %d %d %d, "
                    "c0[0] 0x%08lX c1[0] 0x%08lX, final_c0 0x%08lX final_c1 0x%08lX\n",
                    (unsigned long)combiner_state_hash(state),
                    state->num_stages, (int)state->tex_mode[0], (int)state->tex_mode[1],
                    (int)state->tex_mode[2], (int)state->tex_mode[3],
                    (unsigned long)state->c0[0], (unsigned long)state->c1[0],
                    (unsigned long)state->final_c0, (unsigned long)state->final_c1);
            fprintf(stderr, "--- Generated HLSL ---\n%s\n--- End HLSL ---\n", hlsl);
            fflush(stderr);
        }
    }

    memset(&ss, 0, sizeof ss);
    ss.hlsl = hlsl;
    ss.len = (size_t)len;
    ss.name = "ps_combiner";
    ss.entry = "main";
    ss.target = "ps_5_0";
    ss.optimize = 1;
    {
        long long started = d3d8_compile_clock();
        ps = rhi_shader_create(RHI_STAGE_PIXEL, &ss, err, sizeof err);
        d3d8_compile_note(0, started);
    }
    if (!ps) {
        fprintf(stderr, "NV2A combiners: HLSL compile failed: %s\n",
                err[0] ? err : "unknown error");
        /* Dump the generated source for debugging */
        fprintf(stderr, "--- Generated HLSL ---\n%s\n--- End HLSL ---\n", hlsl);
        return NULL;
    }

    return ps;
}

RhiShader *d3d8_combiners_get_shader(const NV2ACombinerState *state)
{
    uint32_t hash = combiner_state_hash(state);
    uint32_t idx = hash & (COMBINER_CACHE_SIZE - 1);
    int probe;

    /* Linear probe lookup */
    for (probe = 0; probe < COMBINER_CACHE_SIZE; probe++) {
        uint32_t slot = (idx + probe) & (COMBINER_CACHE_SIZE - 1);
        CombinerCacheEntry *entry = &g_cache[slot];

        if (!entry->in_use) {
            /* Cache miss - compile and insert */
            RhiShader *ps = compile_combiner_shader(state);
            if (!ps) return NULL;

            entry->in_use = TRUE;
            entry->hash = hash;
            memcpy(&entry->state, state, sizeof(NV2ACombinerState));
            entry->shader = ps;
            entry->last_used_frame = g_frame_counter;
            return ps;
        }

        if (entry->hash == hash && combiner_state_equal(&entry->state, state)) {
            /* Cache hit */
            entry->last_used_frame = g_frame_counter;
            return entry->shader;
        }
    }

    /*
     * Table is full - evict the least recently used entry.
     * This is rare in practice (most games use <20 configs).
     */
    {
        uint32_t lru_slot = idx;
        uint32_t lru_frame = UINT32_MAX;
        RhiShader *ps;
        CombinerCacheEntry *entry;

        for (probe = 0; probe < COMBINER_CACHE_SIZE; probe++) {
            if (g_cache[probe].last_used_frame < lru_frame) {
                lru_frame = g_cache[probe].last_used_frame;
                lru_slot = probe;
            }
        }

        entry = &g_cache[lru_slot];
        if (entry->shader) {
            if (g_last_shader == entry->shader)
                g_last_shader = NULL;
            rhi_shader_destroy(entry->shader);
            entry->shader = NULL;
        }

        ps = compile_combiner_shader(state);
        if (!ps) return NULL;

        entry->hash = hash;
        memcpy(&entry->state, state, sizeof(NV2ACombinerState));
        entry->shader = ps;
        entry->last_used_frame = g_frame_counter;
        return ps;
    }
}

/* ================================================================
 * Initialization / Shutdown
 * ================================================================ */

HRESULT d3d8_combiners_init(void)
{
    RhiBufferDesc cbd;

    memset(g_cache, 0, sizeof(g_cache));
    memset(&g_combiner_state, 0, sizeof(g_combiner_state));
    g_ps_token = 0;
    g_dirty = TRUE;
    g_last_shader = NULL;
    g_frame_counter = 0;

    /* Create the PS constant buffer for combiner shaders.
     * Size must match NV2APSConstants, rounded up to 16-byte alignment. */
    memset(&cbd, 0, sizeof(cbd));
    cbd.size = (sizeof(NV2APSConstants) + 15) & ~15;
    cbd.usage = RHI_USAGE_DYNAMIC;
    cbd.bind = RHI_BIND_UNIFORM;
    cbd.cpu_access = RHI_CPU_WRITE;

    g_combiner_cb = rhi_buffer_create(&cbd, NULL);
    if (!g_combiner_cb) {
        fprintf(stderr, "NV2A combiners: Failed to create constant buffer\n");
        return E_FAIL;
    }

    fprintf(stderr, "NV2A combiners: Initialized (cache size=%d)\n",
            COMBINER_CACHE_SIZE);
    return S_OK;
}

void d3d8_combiners_shutdown(void)
{
    int i;

    /* Release all cached shaders */
    for (i = 0; i < COMBINER_CACHE_SIZE; i++) {
        if (g_cache[i].in_use && g_cache[i].shader) {
            rhi_shader_destroy(g_cache[i].shader);
        }
    }
    memset(g_cache, 0, sizeof(g_cache));
    g_last_shader = NULL;

    rhi_buffer_destroy(g_combiner_cb);
    g_combiner_cb = NULL;

    fprintf(stderr, "NV2A combiners: Shut down\n");
}

/* ================================================================
 * Draw Integration
 * ================================================================ */

void d3d8_combiners_set_pixel_shader(DWORD token)
{
    if (token != g_ps_token) {
        g_ps_token = token;
        g_dirty = TRUE;
    }
}

DWORD d3d8_combiners_get_pixel_shader(void)
{
    return g_ps_token;
}

BOOL d3d8_combiners_active(void)
{
    return g_ps_token != 0;
}

void d3d8_combiners_mark_dirty(void)
{
    g_dirty = TRUE;
}

BOOL d3d8_combiners_prepare_draw(void)
{
    RhiShader *ps;
    void *mapped;
    const DWORD *rs;
    int i;

    /* Not using combiner shaders - fall back to fixed-function */
    if (g_ps_token == 0)
        return FALSE;

    if (!rhi_device_ready() || !g_combiner_cb)
        return FALSE;

    rs = d3d8_GetRenderStates();

    /* Rebuild combiner state from token + render states if dirty */
    if (g_dirty) {
        d3d8_combiners_parse_token(g_ps_token, rs, &g_combiner_state);
        g_dirty = FALSE;
        g_last_shader = NULL;
    }

    /* Shadow-map stages come from the textures bound for this draw, not the
     * token: a depth-format texture is compared, not sampled. Part of the
     * shader key, so a change selects another shader. */
    {
        static int enabled = -1;

        if (enabled < 0) {
            const char *v = getenv("RECOMP_D3D8_SHADOW_COMPARE");
            enabled = !(v && v[0] == '0');
        }
        for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
            D3DFORMAT f = d3d8_base_format(d3d8_GetStageTexture(i));
            BYTE sh = (enabled && g_combiner_state.tex_mode[i] != NV2A_TEXMODE_NONE &&
                       d3d8_format_is_depth(f)) ? 1 : 0;

            if (sh != g_combiner_state.shadow[i]) {
                g_combiner_state.shadow[i] = sh;
                g_last_shader = NULL;
            }
        }
    }

    /* Get or compile the pixel shader for this combiner state */
    if (!g_last_shader)
        g_last_shader = d3d8_combiners_get_shader(&g_combiner_state);
    ps = g_last_shader;

    /* Under RECOMP_D3D8_PS_DUMP, say which shader each draw actually binds.
     * Printed only when it changes, so the log stays readable and still
     * interleaves with the replay's own per-draw lines: that pairing is what
     * identifies the shader behind a particular draw, which the dump order
     * on its own does not. */
    if (ps) {
        static int want = -1;
        static uint32_t last_printed;
        uint32_t hash;

        if (want < 0)
            want = ps_dump_limit() > 0;
        if (want) {
            hash = combiner_state_hash(&g_combiner_state);
            if (hash != last_printed) {
                last_printed = hash;
                fprintf(stderr, "NV2A combiners: binding shader %08lX\n",
                        (unsigned long)hash);
                fflush(stderr);
            }
        }
    }
    if (!ps) {
        fprintf(stderr, "NV2A combiners: Failed to get shader, "
                "falling back to FFP\n");
        return FALSE;
    }

    /* Bind the combiner pixel shader */
    rhi_set_shader(RHI_STAGE_PIXEL, ps);

    /* Update the PS constant buffer with current values.
     *
     * Even though the shader structure doesn't change, the constant
     * values (C0, C1, fog, alpha ref) can change every frame via
     * render state writes. So we always re-upload. */
    mapped = rhi_buffer_map(g_combiner_cb, RHI_MAP_WRITE_DISCARD);
    if (mapped) {
        NV2APSConstants *cb = (NV2APSConstants *)mapped;

        /* Per-stage constants */
        for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
            d3dcolor_to_float4(g_combiner_state.c0[i], cb->c0[i]);
            d3dcolor_to_float4(g_combiner_state.c1[i], cb->c1[i]);
        }

        /* Final combiner constants */
        d3dcolor_to_float4(g_combiner_state.final_c0, cb->final_c0);
        d3dcolor_to_float4(g_combiner_state.final_c1, cb->final_c1);

        /* Fog color from render state */
        d3dcolor_to_float4(rs[D3DRS_FOGCOLOR], cb->fog_color);

        /* Alpha test parameters */
        cb->alpha_ref = rs[D3DRS_ALPHAREF] / 255.0f;
        cb->alpha_func = rs[D3DRS_ALPHAFUNC];
        cb->alpha_test_enable = rs[D3DRS_ALPHATESTENABLE] ? 1 : 0;
        cb->fog_enable = rs[D3DRS_FOGENABLE] ? 1 : 0;
        for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
            IDirect3DBaseTexture8 *tex = d3d8_GetStageTexture(i);
            D3DFORMAT format = d3d8_base_format(tex);
            UINT w = 0, h = 0;

            cb->alpha_only[i] = format == D3DFMT_A8 || format == D3DFMT_LIN_A8;
            /* Linear textures are addressed in texels on the NV2A; see
             * NV2APSConstants.tex_scale. */
            cb->tex_scale[i][0] = cb->tex_scale[i][1] = 1.0f;
            cb->tex_scale[i][2] = cb->tex_scale[i][3] = 0.0f;
            if (tex && d3d8_format_is_linear(format) && d3d8_base_size(tex, &w, &h) && w && h) {
                cb->tex_scale[i][0] = 1.0f / (float)w;
                cb->tex_scale[i][1] = 1.0f / (float)h;
            }
            /* The host holds depth 0..1; the title's r/q is in the format's
             * own range, as the NV2A compares. */
            switch (format) {
            case D3DFMT_D24S8: case D3DFMT_LIN_D24S8:
                cb->shadow_max[i] = 16777215.0f; break;
            case D3DFMT_D16: case D3DFMT_LIN_D16:
                cb->shadow_max[i] = 65535.0f; break;
            default:
                cb->shadow_max[i] = 1.0f; break;
            }
        }
        {
            static int swap = -1;

            if (swap < 0) {
                const char *v = getenv("RECOMP_D3D8_SHADOW_SWAP");
                swap = (v && v[0] == '1') ? 1 : 0;
            }
            static int view = -1;

            if (view < 0) {
                const char *v = getenv("RECOMP_D3D8_SHADOW_VIEW");
                view = v ? atoi(v) : 0;
            }
            cb->shadow_func = rs[D3DRS_SHADOWFUNC];
            cb->shadow_swap = (UINT)swap;
            cb->shadow_pad[0] = (UINT)view;     /* shadow_func.z in the HLSL */
            /* Say once what the first shadow-map draw compares with. */
            for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
                static int said;
                if (g_combiner_state.shadow[i] && !said) {
                    said = 1;
                    fprintf(stderr, "NV2A combiners: shadow map on stage %d, mode %d, "
                            "SHADOWFUNC %lu (0 = unset)%s\n", i,
                            (int)g_combiner_state.tex_mode[i], (unsigned long)rs[D3DRS_SHADOWFUNC],
                            swap ? ", compare reversed" : "");
                    fflush(stderr);
                }
            }
        }

        rhi_buffer_unmap(g_combiner_cb);
    }

    /* Bind the constant buffer to PS slot 0 */
    rhi_set_uniform_buffers(RHI_STAGE_PIXEL, 0, 1, &g_combiner_cb);

    /* Advance frame counter for LRU tracking */
    g_frame_counter++;

    return TRUE;
}
