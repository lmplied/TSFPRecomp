/**
 * NV2A vertex program instruction set: decode and CPU execution.
 *
 * Moved out of src/d3d/d3d8_vsh.c, which keeps the half that needs Direct3D 11
 * (HLSL generation, D3DCompile, input layouts, the shader cache). This half is
 * pure C over 32-bit words, so it builds on every platform and the push-buffer
 * executor in nv2a_pb_exec.c can use it without pulling a graphics API into
 * xbox_kernel. See nv2a_vsh.h.
 */

#include "nv2a_vsh.h"

#include <float.h>
#include <math.h>
#include <string.h>

/* Bits are numbered 0..127, LSB-first within each 32-bit word; word N starts
 * at bit 32*N. */
static inline uint32_t vsh_extract(const uint32_t *insn, int start, int count)
{
    int word_idx = start / 32;
    int bit_ofs  = start % 32;
    uint32_t mask = (count == 32) ? 0xFFFFFFFF : ((1u << count) - 1);

    if (bit_ofs + count <= 32) {
        return (insn[word_idx] >> bit_ofs) & mask;
    }
    /* Field spans two words */
    uint32_t lo = insn[word_idx] >> bit_ofs;
    uint32_t hi = insn[word_idx + 1] << (32 - bit_ofs);
    return (lo | hi) & mask;
}

/* ================================================================
 * Microcode field table
 *
 * Absolute bit offsets; dword N starts at bit 32*N, LSB-first within each
 * dword. dword0 carries no fields -- it is zero in every instruction of
 * every program observed, which is what exposed the previous table: it
 * placed the opcodes there and so read zero for all of them.
 *
 * Derived from Burnout 2's own microcode and then confirmed field by field
 * against abaire/nv2a_vsh_asm, whose vsh_instruction.py defines the same
 * encoding as ctypes bitfields. docs/technical/nv2a-vertex-program-encoding.md
 * records the derivation and carries the twelve-instruction disassembly that
 * tools/vsh_audit/test_vsh_encoding.py asserts against.
 *
 * Note what is NOT here, because the hardware does not have it: a separate
 * destination per unit. The two units share one temp register index
 * (OUT_TEMP) with a write mask each, and there is a single output register
 * write (OUT_ADDRESS under OUT_O_MASK) fed by whichever unit OUT_MUX selects.
 * A slot where both units run is "paired", and pairing changes the temp
 * writes: the ILU writes R1 whatever OUT_TEMP says, and xemu drops a paired
 * MAC write aimed at R1. nv2a_vsh_parse() resolves both, so consumers never
 * see the raw fields.
 * ================================================================ */

/* dword1 -- source A swizzle, the shared input and const indices, opcodes */
#define VSH_FIELD_SRC_A_SWZ_W_START 32
#define VSH_FIELD_SRC_A_SWZ_Z_START 34
#define VSH_FIELD_SRC_A_SWZ_Y_START 36
#define VSH_FIELD_SRC_A_SWZ_X_START 38
#define VSH_FIELD_SRC_A_NEG_START   40
#define VSH_FIELD_INPUT_IDX_START   41
#define VSH_FIELD_INPUT_IDX_SIZE     4
#define VSH_FIELD_CONST_IDX_START   45
#define VSH_FIELD_CONST_IDX_SIZE     8
#define VSH_FIELD_MAC_OP_START      53
#define VSH_FIELD_MAC_OP_SIZE        4
/* Three bits, not four: the ILU enum has eight entries, and reading a fourth
 * bit makes opcodes that do not exist reachable. */
#define VSH_FIELD_ILU_OP_START      57
#define VSH_FIELD_ILU_OP_SIZE        3

/* dword2 -- sources C, B and A's bank/temp */
#define VSH_FIELD_SRC_C_TEMP_HI_START 64   /* high 2 bits; low 2 at bit 126 */
#define VSH_FIELD_SRC_C_SWZ_W_START   66
#define VSH_FIELD_SRC_C_SWZ_Z_START   68
#define VSH_FIELD_SRC_C_SWZ_Y_START   70
#define VSH_FIELD_SRC_C_SWZ_X_START   72
#define VSH_FIELD_SRC_C_NEG_START     74
#define VSH_FIELD_SRC_B_MUX_START     75
#define VSH_FIELD_SRC_B_TEMP_START    77
#define VSH_FIELD_SRC_B_SWZ_W_START   81
#define VSH_FIELD_SRC_B_SWZ_Z_START   83
#define VSH_FIELD_SRC_B_SWZ_Y_START   85
#define VSH_FIELD_SRC_B_SWZ_X_START   87
#define VSH_FIELD_SRC_B_NEG_START     89
#define VSH_FIELD_SRC_A_MUX_START     90
#define VSH_FIELD_SRC_A_TEMP_START    92

/* dword3 -- flags, the single output write, the shared temp, source C's bank */
#define VSH_FIELD_FINAL_START         96
#define VSH_FIELD_A0X_START           97   /* a0.x relative addressing */
#define VSH_FIELD_OUT_MUX_START       98   /* 0 = MAC drives the output, 1 = ILU */
#define VSH_FIELD_OUT_ADDRESS_START   99
#define VSH_FIELD_OUT_ADDRESS_SIZE     8
#define VSH_FIELD_OUT_ORB_START      107
#define VSH_FIELD_OUT_O_MASK_START   108
#define VSH_FIELD_OUT_ILU_MASK_START 112
#define VSH_FIELD_OUT_TEMP_START     116
#define VSH_FIELD_OUT_MAC_MASK_START 120
#define VSH_FIELD_SRC_C_MUX_START    124
#define VSH_FIELD_SRC_C_TEMP_LO_START 126

/* Source banks. Zero is not a bank -- the previous table mapped the raw
 * value onto an enum starting at zero, so every operand came out one bank
 * wrong. */
#define VSH_SRC_BANK_TEMP   1
#define VSH_SRC_BANK_INPUT  2
#define VSH_SRC_BANK_CONST  3

/* Which of sources A, B and C (bits 0, 1, 2) each MAC opcode reads, from
 * xemu's mac_opcode_params. The ILU reads C only. */
static const unsigned char g_mac_reads[NV2A_VSH_MAC_COUNT] = {
    0, /* NOP */  1, /* MOV: A */  3, /* MUL: A B */  5, /* ADD: A C */
    7, /* MAD */  3, /* DP3 */     3, /* DPH */       3, /* DP4 */
    3, /* DST */  3, /* MIN */     3, /* MAX */       3, /* SLT */
    3, /* SGE */  1, /* ARL: A */
};

/* ================================================================
 * Microcode Parser
 * ================================================================ */

static void parse_source(const uint32_t *insn,
                          int neg_start, int mux_start, int temp_index,
                          int swz_x_start, int swz_y_start,
                          int swz_z_start, int swz_w_start,
                          int input_index, int const_index,
                          NV2AVshSrcOperand *src)
{
    uint32_t bank = vsh_extract(insn, mux_start, 2);

    src->negate    = vsh_extract(insn, neg_start, 1);
    src->swizzle.x = (uint8_t)vsh_extract(insn, swz_x_start, 2);
    src->swizzle.y = (uint8_t)vsh_extract(insn, swz_y_start, 2);
    src->swizzle.z = (uint8_t)vsh_extract(insn, swz_z_start, 2);
    src->swizzle.w = (uint8_t)vsh_extract(insn, swz_w_start, 2);
    src->rel_addr  = 0;

    switch (bank) {
    case VSH_SRC_BANK_TEMP:
        src->reg_type  = NV2A_VSH_REG_TEMP;
        src->reg_index = temp_index;
        break;
    case VSH_SRC_BANK_INPUT:
        src->reg_type  = NV2A_VSH_REG_INPUT;
        src->reg_index = input_index;
        break;
    case VSH_SRC_BANK_CONST:
        src->reg_type  = NV2A_VSH_REG_CONST;
        src->reg_index = const_index;
        break;
    default:
        /* Bank 0 does not exist on this hardware; xemu asserts on it. Seeing
         * one means the instruction is not laid out the way this table says.
         * It reads as R0, which may hold data -- the least surprising choice,
         * not a correct one. */
        src->reg_type  = NV2A_VSH_REG_TEMP;
        src->reg_index = 0;
        break;
    }
}

static NV2AVshOutputReg decode_output_addr(uint32_t out_addr)
{
    /* OUT_ADDRESS names the output register in its low four bits, as xemu
     * reads it. Whether there is an output write at all is OUT_O_MASK's
     * decision: the 0xFF a pass-through shader leaves here only ever appears
     * with a zero mask. 1, 2, 13 and 14 name nothing, and 15 is a0.x, which
     * is not modelled as an output write. */
    uint32_t out_idx = out_addr & 0xF;

    switch (out_idx) {
    case 0:  return NV2A_VSH_OUT_POS;
    case 3:  return NV2A_VSH_OUT_D0;
    case 4:  return NV2A_VSH_OUT_D1;
    case 5:  return NV2A_VSH_OUT_FOG;
    case 6:  return NV2A_VSH_OUT_PTS;
    case 7:  return NV2A_VSH_OUT_B0;
    case 8:  return NV2A_VSH_OUT_B1;
    case 9:  return NV2A_VSH_OUT_T0;
    case 10: return NV2A_VSH_OUT_T1;
    case 11: return NV2A_VSH_OUT_T2;
    case 12: return NV2A_VSH_OUT_T3;
    default: return NV2A_VSH_OUT_NONE;
    }
}

void nv2a_vsh_parse(const uint32_t *microcode, int num_insns,
                     NV2AVshProgram *program)
{
    int i;
    memset(program, 0, sizeof(*program));
    program->inputs_read = 0;

    if (num_insns > NV2A_VS_MAX_INSTRUCTIONS)
        num_insns = NV2A_VS_MAX_INSTRUCTIONS;

    for (i = 0; i < num_insns; i++) {
        const uint32_t *insn = &microcode[i * 4];
        NV2AVshInstruction *inst = &program->insns[i];

        /* Extract opcodes */
        inst->mac_op = (NV2AVshMacOp)vsh_extract(insn, VSH_FIELD_MAC_OP_START,
                                                   VSH_FIELD_MAC_OP_SIZE);
        inst->ilu_op = (NV2AVshIluOp)vsh_extract(insn, VSH_FIELD_ILU_OP_START,
                                                   VSH_FIELD_ILU_OP_SIZE);

        /* Shared constant and input register indices */
        inst->const_index = (int)vsh_extract(insn, VSH_FIELD_CONST_IDX_START,
                                              VSH_FIELD_CONST_IDX_SIZE);
        inst->input_index = (int)vsh_extract(insn, VSH_FIELD_INPUT_IDX_START,
                                              VSH_FIELD_INPUT_IDX_SIZE);

        /* Clamp indices to valid ranges */
        if (inst->const_index >= NV2A_VS_MAX_CONSTANTS)
            inst->const_index = 0;
        if (inst->input_index >= NV2A_VS_MAX_INPUTS)
            inst->input_index = 0;

        /* Source C's temp index is split across two dwords and cannot be
         * read as one field. */
        {
            int c_temp = (int)((vsh_extract(insn, VSH_FIELD_SRC_C_TEMP_HI_START, 2) << 2)
                             |  vsh_extract(insn, VSH_FIELD_SRC_C_TEMP_LO_START, 2));
            int a_temp = (int)vsh_extract(insn, VSH_FIELD_SRC_A_TEMP_START, 4);
            int b_temp = (int)vsh_extract(insn, VSH_FIELD_SRC_B_TEMP_START, 4);

            parse_source(insn,
                         VSH_FIELD_SRC_A_NEG_START, VSH_FIELD_SRC_A_MUX_START,
                         a_temp,
                         VSH_FIELD_SRC_A_SWZ_X_START, VSH_FIELD_SRC_A_SWZ_Y_START,
                         VSH_FIELD_SRC_A_SWZ_Z_START, VSH_FIELD_SRC_A_SWZ_W_START,
                         inst->input_index, inst->const_index,
                         &inst->mac_src[0]);

            parse_source(insn,
                         VSH_FIELD_SRC_B_NEG_START, VSH_FIELD_SRC_B_MUX_START,
                         b_temp,
                         VSH_FIELD_SRC_B_SWZ_X_START, VSH_FIELD_SRC_B_SWZ_Y_START,
                         VSH_FIELD_SRC_B_SWZ_Z_START, VSH_FIELD_SRC_B_SWZ_W_START,
                         inst->input_index, inst->const_index,
                         &inst->mac_src[1]);

            parse_source(insn,
                         VSH_FIELD_SRC_C_NEG_START, VSH_FIELD_SRC_C_MUX_START,
                         c_temp,
                         VSH_FIELD_SRC_C_SWZ_X_START, VSH_FIELD_SRC_C_SWZ_Y_START,
                         VSH_FIELD_SRC_C_SWZ_Z_START, VSH_FIELD_SRC_C_SWZ_W_START,
                         inst->input_index, inst->const_index,
                         &inst->mac_src[2]);
        }

        /* ILU source = source C */
        inst->ilu_src = inst->mac_src[2];

        /* Relative addressing through a0.x */
        {
            uint32_t rel = vsh_extract(insn, VSH_FIELD_A0X_START, 1);
            if (rel) {
                int t;
                for (t = 0; t < 3; t++) {
                    if (inst->mac_src[t].reg_type == NV2A_VSH_REG_CONST)
                        inst->mac_src[t].rel_addr = 1;
                }
                if (inst->ilu_src.reg_type == NV2A_VSH_REG_CONST)
                    inst->ilu_src.rel_addr = 1;
            }
        }

        /* Destinations, as xemu's decode_opcode() and abaire/nv2a_vsh_asm
         * resolve them.
         *
         * Temps: one index (OUT_TEMP) shared by both units, a write mask each.
         * When both units run ("paired"), the ILU writes R1 whatever OUT_TEMP
         * says -- both references agree -- and xemu drops a paired MAC write
         * aimed at R1. abaire keeps that MAC write; xemu is the one run against
         * real titles, so it is followed here.
         *
         * Output: at most one write per slot, only when OUT_O_MASK is non-zero,
         * made by the unit OUT_MUX names, and only if that unit runs.
         *
         * Timing differs from xemu on purpose. xemu's GLSL writes a paired
         * MAC's output before the ILU is evaluated (only the MAC's temp write
         * is deferred), so an ILU reading R12 sees a new oPos. Here both units
         * read the register file as it was at the start of the slot, which is
         * what "parallel" means; which one hardware does is unverified. OUT_ORB
         * says whether the target is an output register or a constant
         * register; a constant write is recorded and, as in xemu, not
         * emulated. */
        {
            int out_temp      = (int)vsh_extract(insn, VSH_FIELD_OUT_TEMP_START, 4);
            uint32_t mac_mask = vsh_extract(insn, VSH_FIELD_OUT_MAC_MASK_START, 4);
            uint32_t ilu_mask = vsh_extract(insn, VSH_FIELD_OUT_ILU_MASK_START, 4);
            uint32_t o_mask   = vsh_extract(insn, VSH_FIELD_OUT_O_MASK_START, 4);
            uint32_t out_addr = vsh_extract(insn, VSH_FIELD_OUT_ADDRESS_START,
                                            VSH_FIELD_OUT_ADDRESS_SIZE);
            int ilu_drives_output = (int)vsh_extract(insn, VSH_FIELD_OUT_MUX_START, 1);
            int to_output_reg     = (int)vsh_extract(insn, VSH_FIELD_OUT_ORB_START, 1);
            int paired = inst->mac_op != NV2A_VSH_MAC_NOP
                      && inst->ilu_op != NV2A_VSH_ILU_NOP;
            NV2AVshDstOperand *out_dst = NULL;

            inst->mac_dst.temp_reg    = -1;
            inst->mac_dst.write_mask  = 0;
            inst->mac_dst.output_reg  = NV2A_VSH_OUT_NONE;
            inst->mac_dst.output_mask = 0;
            inst->ilu_dst.temp_reg    = -1;
            inst->ilu_dst.write_mask  = 0;
            inst->ilu_dst.output_reg  = NV2A_VSH_OUT_NONE;
            inst->ilu_dst.output_mask = 0;
            inst->out_const_index     = -1;

            if (inst->mac_op != NV2A_VSH_MAC_NOP && mac_mask
                && !(paired && out_temp == 1)) {
                inst->mac_dst.temp_reg   = out_temp;
                inst->mac_dst.write_mask = (uint8_t)mac_mask;
            }
            if (inst->ilu_op != NV2A_VSH_ILU_NOP && ilu_mask) {
                inst->ilu_dst.temp_reg   = paired ? 1 : out_temp;
                inst->ilu_dst.write_mask = (uint8_t)ilu_mask;
            }

            if (o_mask) {
                if (ilu_drives_output && inst->ilu_op != NV2A_VSH_ILU_NOP)
                    out_dst = &inst->ilu_dst;
                else if (!ilu_drives_output && inst->mac_op != NV2A_VSH_MAC_NOP)
                    out_dst = &inst->mac_dst;
            }
            if (out_dst) {
                if (to_output_reg) {
                    NV2AVshOutputReg out_reg = decode_output_addr(out_addr);
                    if (out_reg != NV2A_VSH_OUT_NONE) {
                        out_dst->output_reg  = out_reg;
                        out_dst->output_mask = (uint8_t)o_mask;
                    }
                } else {
                    inst->out_const_index = (int)out_addr;
                }
            }
        }

        inst->is_final = vsh_extract(insn, VSH_FIELD_FINAL_START, 1) ? 1 : 0;

        /* Track input register usage -- only for sources the opcodes actually
         * read. A MOV leaves B and C encoded with whatever bank they happen to
         * hold. All sources share one v# index, so an unread input source can
         * only add a v# the slot already reads -- except when the source the
         * opcode does read is a temp or constant, and then counting the unread
         * one adds an input the program never touches, shifting every later
         * element of the D3D11 input layout. Burnout 2's frontend shader has
         * no such slot. */
        {
            int s;
            unsigned reads = (unsigned)inst->mac_op < NV2A_VSH_MAC_COUNT
                           ? g_mac_reads[inst->mac_op] : 0u;
            for (s = 0; s < 3; s++) {
                if ((reads & (1u << s))
                    && inst->mac_src[s].reg_type == NV2A_VSH_REG_INPUT)
                    program->inputs_read |= (uint16_t)(1u << inst->mac_src[s].reg_index);
            }
            if (inst->ilu_op != NV2A_VSH_ILU_NOP
                && inst->ilu_src.reg_type == NV2A_VSH_REG_INPUT)
                program->inputs_read |= (uint16_t)(1u << inst->ilu_src.reg_index);
        }

        program->length = i + 1;

        /* Stop at final instruction */
        if (inst->is_final)
            break;
    }
}

/* ================================================================
 * CPU execution of a parsed vertex program
 * ================================================================
 *
 * The parser above turns microcode into NV2AVshProgram; src/d3d/d3d8_vsh.c turns
 * that into a shader for the D3D11 path. This runs it directly, because the
 * software rasteriser in nv2a_pb_exec.c has no shader stage and every batch
 * Burnout 2 draws needs one: with a program bound, attribute 0 is object
 * space, so without executing the program there is nothing sensible to
 * rasterise. Measured on Burnout 2's frontend: 322 batches a frame, all of
 * them in program mode, none pre-transformed.
 *
 * Two things about the encoding matter for correctness and are easy to get
 * backwards:
 *
 *   MAC ADD reads A and C, not A and B. So does MAD's addend.
 *   The two units are parallel. Both read the register file as it was at the
 *   start of the slot, so sources are gathered before either writes.
 *
 * R12 is not a temp that happens to hold the position -- it *is* oPos, the
 * same register under two names. Storing it once and aliasing the output
 * avoids a program that writes R12 and reads oPos (or the reverse) seeing two
 * different values.
 */

#define VSH_TEMP_OPOS 12

static float vsh_src_component(const NV2AVshSrcOperand *src,
                               const NV2AVshState *st,
                               const float inputs[][4],
                               const float (*consts)[4], int const_count,
                               int sel)
{
    const float *v;
    float f;
    int idx = src->reg_index;

    switch (src->reg_type) {
    case NV2A_VSH_REG_INPUT:
        v = inputs[idx & 15];
        break;
    case NV2A_VSH_REG_CONST:
        if (src->rel_addr) {
            /* Bound before converting: a0 is float, and (int) of a NaN or of
             * 1e30 is undefined rather than merely wrong. */
            float a0 = st->addr;
            if (!(a0 > -1024.0f && a0 < 1024.0f))
                return 0.0f;
            idx += (int)a0;
        }
        if (idx < 0 || idx >= const_count)
            return 0.0f;
        v = consts[idx];
        break;
    case NV2A_VSH_REG_TEMP:
    default:
        /* R13-R15 do not exist. Reading used to alias them onto oPos while
         * writing discarded them, so a garbage index could pick up the
         * position; both now agree that they are zero. */
        if (idx < 0 || idx > VSH_TEMP_OPOS)
            return 0.0f;
        v = st->temp[idx];
        break;
    }

    f = v[sel & 3];
    return src->negate ? -f : f;
}

static void vsh_read_src(const NV2AVshSrcOperand *src,
                         const NV2AVshState *st,
                         const float inputs[][4],
                         const float (*consts)[4], int const_count,
                         float out[4])
{
    out[0] = vsh_src_component(src, st, inputs, consts, const_count,
                               src->swizzle.x);
    out[1] = vsh_src_component(src, st, inputs, consts, const_count,
                               src->swizzle.y);
    out[2] = vsh_src_component(src, st, inputs, consts, const_count,
                               src->swizzle.z);
    out[3] = vsh_src_component(src, st, inputs, consts, const_count,
                               src->swizzle.w);
}

/* xemu's fog_mask_str: a write to oFog fills the first k components for a
 * k-bit mask, taking the same-named components of the result, instead of the
 * components the mask names. Every other output uses its mask as encoded. */
uint8_t nv2a_vsh_output_write_mask(const NV2AVshDstOperand *dst)
{
    static const uint8_t fog[16] = {
        0x0, 0x8, 0x8, 0xC, 0x8, 0xC, 0xC, 0xE,
        0x8, 0xC, 0xC, 0xE, 0xC, 0xE, 0xE, 0xF,
    };

    if (dst->output_reg == NV2A_VSH_OUT_FOG)
        return fog[dst->output_mask & 0xF];
    return dst->output_mask;
}

/* write_mask is bit3=x .. bit0=w, per the header. */
static void vsh_write_dst(const NV2AVshDstOperand *dst, NV2AVshState *st,
                          const float val[4])
{
    int c;
    uint8_t omask = nv2a_vsh_output_write_mask(dst);

    if (!dst->write_mask && !omask)
        return;

    for (c = 0; c < 4; c++) {
        if ((dst->write_mask & (8 >> c))
            && dst->temp_reg >= 0 && dst->temp_reg <= VSH_TEMP_OPOS)
            st->temp[dst->temp_reg][c] = val[c];
        if ((omask & (8 >> c))
            && dst->output_reg != NV2A_VSH_OUT_NONE
            && dst->output_reg < NV2A_VSH_OUT_COUNT) {
            if (dst->output_reg == NV2A_VSH_OUT_POS)
                st->temp[VSH_TEMP_OPOS][c] = val[c];   /* oPos *is* R12 */
            else
                st->out[dst->output_reg][c] = val[c];
        }
    }
    if (omask && dst->output_reg != NV2A_VSH_OUT_NONE
        && dst->output_reg < NV2A_VSH_OUT_COUNT)
        st->out_written |= (uint16_t)(1u << dst->output_reg);
    if (dst->temp_reg == VSH_TEMP_OPOS)
        st->out_written |= (uint16_t)(1u << NV2A_VSH_OUT_POS);
}

static void vsh_splat(float out[4], float f)
{
    out[0] = out[1] = out[2] = out[3] = f;
}

/* Returns 0 when the opcode computes nothing, so the caller can leave the
 * destination alone. Zeroing it instead turns an opcode this does not know
 * into a positive assertion that the register is zero -- and the MAC field
 * is four bits, so 14 and 15 reach here. */
static int vsh_do_mac(NV2AVshMacOp op, const float a[4], const float b[4],
                      const float c[4], float r[4])
{
    switch (op) {
    case NV2A_VSH_MAC_MOV:
        r[0] = a[0]; r[1] = a[1]; r[2] = a[2]; r[3] = a[3];
        break;
    case NV2A_VSH_MAC_MUL:
        r[0] = a[0]*b[0]; r[1] = a[1]*b[1];
        r[2] = a[2]*b[2]; r[3] = a[3]*b[3];
        break;
    case NV2A_VSH_MAC_ADD:                 /* A + C, not A + B */
        r[0] = a[0]+c[0]; r[1] = a[1]+c[1];
        r[2] = a[2]+c[2]; r[3] = a[3]+c[3];
        break;
    case NV2A_VSH_MAC_MAD:
        r[0] = a[0]*b[0]+c[0]; r[1] = a[1]*b[1]+c[1];
        r[2] = a[2]*b[2]+c[2]; r[3] = a[3]*b[3]+c[3];
        break;
    case NV2A_VSH_MAC_DP3:
        vsh_splat(r, a[0]*b[0] + a[1]*b[1] + a[2]*b[2]);
        break;
    case NV2A_VSH_MAC_DPH:
        vsh_splat(r, a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + b[3]);
        break;
    case NV2A_VSH_MAC_DP4:
        vsh_splat(r, a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3]);
        break;
    case NV2A_VSH_MAC_DST:
        r[0] = 1.0f; r[1] = a[1]*b[1]; r[2] = a[2]; r[3] = b[3];
        break;
    case NV2A_VSH_MAC_MIN:
        r[0] = a[0]<b[0]?a[0]:b[0]; r[1] = a[1]<b[1]?a[1]:b[1];
        r[2] = a[2]<b[2]?a[2]:b[2]; r[3] = a[3]<b[3]?a[3]:b[3];
        break;
    case NV2A_VSH_MAC_MAX:
        r[0] = a[0]>b[0]?a[0]:b[0]; r[1] = a[1]>b[1]?a[1]:b[1];
        r[2] = a[2]>b[2]?a[2]:b[2]; r[3] = a[3]>b[3]?a[3]:b[3];
        break;
    case NV2A_VSH_MAC_SLT:
        r[0] = a[0]<b[0]?1.0f:0.0f; r[1] = a[1]<b[1]?1.0f:0.0f;
        r[2] = a[2]<b[2]?1.0f:0.0f; r[3] = a[3]<b[3]?1.0f:0.0f;
        break;
    case NV2A_VSH_MAC_SGE:
        r[0] = a[0]>=b[0]?1.0f:0.0f; r[1] = a[1]>=b[1]?1.0f:0.0f;
        r[2] = a[2]>=b[2]?1.0f:0.0f; r[3] = a[3]>=b[3]?1.0f:0.0f;
        break;
    case NV2A_VSH_MAC_ARL:
    case NV2A_VSH_MAC_NOP:
    default:
        return 0;                          /* nothing computed, nothing stored */
    }
    return 1;
}

static int vsh_do_ilu(NV2AVshIluOp op, const float c[4], float r[4])
{
    float x = c[0];

    switch (op) {
    case NV2A_VSH_ILU_MOV:
        r[0] = c[0]; r[1] = c[1]; r[2] = c[2]; r[3] = c[3];
        break;
    case NV2A_VSH_ILU_RCP:
        /* Signed, because -0.0 == 0.0 compares true and the sign is the whole
         * difference between +inf and -inf. FLT_MAX stands in for infinity
         * here, as it does for RSQ and LOG. */
        vsh_splat(r, x == 0.0f
                     ? (signbit(x) ? -FLT_MAX : FLT_MAX)
                     : 1.0f / x);
        break;
    case NV2A_VSH_ILU_RCC: {
        float v = x == 0.0f ? (signbit(x) ? -FLT_MAX : FLT_MAX) : 1.0f / x;
        /* The odd clamp is the hardware's, and it is asymmetric. */
        if (v >= 0.0f)
            v = v < 5.42101e-20f ? 5.42101e-20f
              : (v > 1.8446744e+19f ? 1.8446744e+19f : v);
        else
            v = v > -5.42101e-20f ? -5.42101e-20f
              : (v < -1.8446744e+19f ? -1.8446744e+19f : v);
        vsh_splat(r, v);
        break;
    }
    case NV2A_VSH_ILU_RSQ: {
        float m = x < 0.0f ? -x : x;
        vsh_splat(r, m == 0.0f ? FLT_MAX : 1.0f / sqrtf(m));
        break;
    }
    case NV2A_VSH_ILU_EXP: {
        /* D3D's expp: (2^floor(x), x - floor(x), 2^x, 1), as d3d8_vsh.c
         * emits it (xemu vsh-prog.c _EXP). */
        float f = floorf(x);
        r[0] = powf(2.0f, f);
        r[1] = x - f;
        r[2] = powf(2.0f, x);
        r[3] = 1.0f;
        break;
    }
    case NV2A_VSH_ILU_LOG: {
        /* D3D's logp: (exponent, mantissa, log2|x|, 1) (xemu vsh-prog.c
         * _LOG); log2(0) stays -FLT_MAX, with the mantissa 1 as xemu has it. */
        float m = x < 0.0f ? -x : x;
        if (m == 0.0f) {
            r[0] = -FLT_MAX;
            r[1] = 1.0f;
            r[2] = -FLT_MAX;
        } else {
            float e = floorf(log2f(m));
            r[0] = e;
            r[1] = m / powf(2.0f, e);
            r[2] = log2f(m);
        }
        r[3] = 1.0f;
        break;
    }
    case NV2A_VSH_ILU_LIT: {
        float diffuse = c[0] > 0.0f ? c[0] : 0.0f;
        float spec_in = c[1] > 0.0f ? c[1] : 0.0f;
        float power   = c[3] < -128.0f ? -128.0f : (c[3] > 128.0f ? 128.0f : c[3]);
        r[0] = 1.0f;
        r[1] = diffuse;
        r[2] = c[0] > 0.0f ? powf(spec_in, power) : 0.0f;
        r[3] = 1.0f;
        break;
    }
    case NV2A_VSH_ILU_NOP:
    default:
        return 0;
    }
    return 1;
}

void nv2a_vsh_execute(const NV2AVshProgram *program,
                      const float inputs[][4],
                      const float (*consts)[4], int const_count,
                      NV2AVshState *st)
{
    int i;

    memset(st, 0, sizeof *st);

    /* oPos starts as (0,0,0,1), which is what this file's own HLSL generator
     * emits and what the hardware presents. Leaving w at zero means a program
     * that writes only oPos.xyz -- or one whose destination decode is wrong --
     * produces w == 0 for every vertex, and the caller drops the whole batch
     * on a perspective divide by zero. An invisible frame with no error is the
     * worst failure available here. */
    st->temp[VSH_TEMP_OPOS][3] = 1.0f;

    for (i = 0; i < program->length && i < NV2A_VS_MAX_INSTRUCTIONS; i++) {
        const NV2AVshInstruction *in = &program->insns[i];
        float a[4], b[4], c[4], ilu_c[4], mac_r[4], ilu_r[4];
        int did_mac = in->mac_op != NV2A_VSH_MAC_NOP;
        int did_ilu = in->ilu_op != NV2A_VSH_ILU_NOP;

        /* Both units see the register file as it was at the start of the
         * slot, so every source is read before either result is stored. */
        if (did_mac) {
            vsh_read_src(&in->mac_src[0], st, inputs, consts, const_count, a);
            vsh_read_src(&in->mac_src[1], st, inputs, consts, const_count, b);
            vsh_read_src(&in->mac_src[2], st, inputs, consts, const_count, c);
        }
        if (did_ilu)
            vsh_read_src(&in->ilu_src, st, inputs, consts, const_count, ilu_c);

        if (did_mac) {
            if (in->mac_op == NV2A_VSH_MAC_ARL) {
                st->addr = floorf(a[0]);
            } else if (vsh_do_mac(in->mac_op, a, b, c, mac_r)) {
                vsh_write_dst(&in->mac_dst, st, mac_r);
            }
        }
        if (did_ilu && vsh_do_ilu(in->ilu_op, ilu_c, ilu_r))
            vsh_write_dst(&in->ilu_dst, st, ilu_r);

        if (in->is_final)
            break;
    }

    /* oPos is R12; hand it back under the name the caller asked for. */
    st->out[NV2A_VSH_OUT_POS][0] = st->temp[VSH_TEMP_OPOS][0];
    st->out[NV2A_VSH_OUT_POS][1] = st->temp[VSH_TEMP_OPOS][1];
    st->out[NV2A_VSH_OUT_POS][2] = st->temp[VSH_TEMP_OPOS][2];
    st->out[NV2A_VSH_OUT_POS][3] = st->temp[VSH_TEMP_OPOS][3];
}
