/**
 * NV2A vertex program instruction set: decode and CPU execution.
 *
 * The NV2A vertex shader unit runs programs of up to 136 instruction slots.
 * Each slot is 128 bits (four 32-bit words) and carries two operations that
 * execute in parallel: one MAC (multiply-accumulate) and one ILU (inverse,
 * log and lighting) operation.
 *
 * This header is deliberately free of any graphics API and of <windows.h>.
 * Two consumers need it:
 *
 *   src/d3d/d3d8_vsh.c         turns a parsed program into an HLSL shader
 *   src/kernel/nv2a_pb_exec.c  runs a program per vertex on the CPU
 *
 * The second is part of xbox_kernel, which builds on every platform. It used
 * to include d3d8_vsh.h for these two functions, and that header pulled
 * <d3d11.h> into the kernel library everywhere -- which is what kept the POSIX
 * build broken. The encoding itself is documented, with its evidence, in
 * docs/technical/nv2a-vertex-program-encoding.md.
 *
 * Registers:
 *   v0  - v15   input vertex attributes (read-only)
 *   R0  - R11   temporaries
 *   R12         oPos -- the same register under two names
 *   c0  - c191  constants (optionally indexed through a0.x)
 *   oD0, oD1, oFog, oPts, oB0, oB1, oT0 - oT3   outputs
 */

#ifndef XBOXRECOMP_NV2A_VSH_H
#define XBOXRECOMP_NV2A_VSH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum program length in 128-bit instruction slots. */
#define NV2A_VS_MAX_INSTRUCTIONS    136

/** Number of constant registers (c0 - c191). */
#define NV2A_VS_MAX_CONSTANTS       192

/** Number of input attribute registers (v0 - v15). */
#define NV2A_VS_MAX_INPUTS          16

/** Number of temporary registers (R0 - R11, plus R12 = oPos). */
#define NV2A_VS_MAX_TEMPS           13

/** MAC unit opcodes. Four bits in the instruction; 14 and 15 are unused. */
typedef enum NV2AVshMacOp {
    NV2A_VSH_MAC_NOP = 0,   /* No operation */
    NV2A_VSH_MAC_MOV = 1,   /* dst = A */
    NV2A_VSH_MAC_MUL = 2,   /* dst = A * B */
    NV2A_VSH_MAC_ADD = 3,   /* dst = A + C */
    NV2A_VSH_MAC_MAD = 4,   /* dst = A * B + C */
    NV2A_VSH_MAC_DP3 = 5,   /* dst = dot3(A.xyz, B.xyz) */
    NV2A_VSH_MAC_DPH = 6,   /* dst = dot3(A.xyz, B.xyz) + B.w */
    NV2A_VSH_MAC_DP4 = 7,   /* dst = dot4(A, B) */
    NV2A_VSH_MAC_DST = 8,   /* dst = distance vector */
    NV2A_VSH_MAC_MIN = 9,   /* dst = min(A, B) */
    NV2A_VSH_MAC_MAX = 10,  /* dst = max(A, B) */
    NV2A_VSH_MAC_SLT = 11,  /* dst = (A < B) ? 1.0 : 0.0 */
    NV2A_VSH_MAC_SGE = 12,  /* dst = (A >= B) ? 1.0 : 0.0 */
    NV2A_VSH_MAC_ARL = 13,  /* a0.x = floor(A.x) */
    NV2A_VSH_MAC_COUNT = 14,
} NV2AVshMacOp;

/** ILU unit opcodes. Three bits in the instruction, so all eight are real. */
typedef enum NV2AVshIluOp {
    NV2A_VSH_ILU_NOP = 0,   /* No operation */
    NV2A_VSH_ILU_MOV = 1,   /* dst = C */
    NV2A_VSH_ILU_RCP = 2,   /* dst = 1.0 / C.x (scalar, replicated) */
    NV2A_VSH_ILU_RCC = 3,   /* dst = clamp(1.0/C.x, 5.42101e-20, 1.8446744e+19) */
    NV2A_VSH_ILU_RSQ = 4,   /* dst = 1.0 / sqrt(abs(C.x)) */
    NV2A_VSH_ILU_EXP = 5,   /* dst = (2^floor(C.x), frac(C.x), 2^C.x, 1): expp */
    NV2A_VSH_ILU_LOG = 6,   /* dst = (exponent, mantissa, log2|C.x|, 1): logp */
    NV2A_VSH_ILU_LIT = 7,   /* dst = lighting helper */
    NV2A_VSH_ILU_COUNT = 8,
} NV2AVshIluOp;

/**
 * Which register bank a source operand reads.
 *
 * These are this module's own values. The instruction encodes banks as
 * 1 = temp, 2 = input, 3 = const, and nv2a_vsh_parse() maps them onto this.
 */
typedef enum NV2AVshRegType {
    NV2A_VSH_REG_TEMP   = 0,  /* R0-R11 (R12 = oPos alias) */
    NV2A_VSH_REG_INPUT  = 1,  /* v0-v15 */
    NV2A_VSH_REG_CONST  = 2,  /* c0-c191 (may be indexed via a0) */
    NV2A_VSH_REG_COUNT  = 3,
} NV2AVshRegType;

/** Output register selectors, in the order the instruction encodes them. */
typedef enum NV2AVshOutputReg {
    NV2A_VSH_OUT_POS  = 0,   /* oPos (clip-space position) */
    NV2A_VSH_OUT_D0   = 3,   /* oD0 (diffuse color) */
    NV2A_VSH_OUT_D1   = 4,   /* oD1 (specular color) */
    NV2A_VSH_OUT_FOG  = 5,   /* oFog (fog factor) */
    NV2A_VSH_OUT_PTS  = 6,   /* oPts (point size) */
    NV2A_VSH_OUT_B0   = 7,   /* oB0 (back diffuse) */
    NV2A_VSH_OUT_B1   = 8,   /* oB1 (back specular) */
    NV2A_VSH_OUT_T0   = 9,   /* oT0 (texcoord 0) */
    NV2A_VSH_OUT_T1   = 10,  /* oT1 (texcoord 1) */
    NV2A_VSH_OUT_T2   = 11,  /* oT2 (texcoord 2) */
    NV2A_VSH_OUT_T3   = 12,  /* oT3 (texcoord 3) */
    NV2A_VSH_OUT_NONE = 0xFF, /* No output register write */
} NV2AVshOutputReg;

/** One past the highest real output selector, for sizing arrays. */
#define NV2A_VSH_OUT_COUNT 13

/** Component selectors for one source, each 0=x, 1=y, 2=z, 3=w. */
typedef struct NV2AVshSwizzle {
    uint8_t x;
    uint8_t y;
    uint8_t z;
    uint8_t w;
} NV2AVshSwizzle;

/** A fully decoded source operand. */
typedef struct NV2AVshSrcOperand {
    NV2AVshRegType  reg_type;    /* TEMP, INPUT, or CONST */
    int             reg_index;   /* Register number within the bank */
    int             negate;      /* 1 = negate the value */
    NV2AVshSwizzle  swizzle;     /* Per-component swizzle */
    int             rel_addr;    /* 1 = use a0.x relative addressing (CONST only) */
} NV2AVshSrcOperand;

/** A fully decoded destination operand. */
typedef struct NV2AVshDstOperand {
    int               temp_reg;    /* Temp register index (0-12), or -1 if none */
    NV2AVshOutputReg  output_reg;  /* Output register, or NV2A_VSH_OUT_NONE */
    uint8_t           write_mask;  /* Temp write mask: bit3=x, bit2=y, bit1=z, bit0=w */
    /* The output write has its own mask (OUT_O_MASK) and it is not the temp
     * mask: an instruction can write a temp and an output with different
     * masks in the same slot. Using one mask for both wrote channels the
     * program never asked for. */
    uint8_t           output_mask;
} NV2AVshDstOperand;

/**
 * One decoded instruction slot: a MAC and an ILU operation that execute in
 * parallel. Either or both may be NOP.
 */
typedef struct NV2AVshInstruction {
    /* MAC unit */
    NV2AVshMacOp      mac_op;
    NV2AVshSrcOperand mac_src[3];  /* A, B, C */
    NV2AVshDstOperand mac_dst;

    /* ILU unit */
    NV2AVshIluOp      ilu_op;
    NV2AVshSrcOperand ilu_src;     /* C (ILU only reads source C) */
    NV2AVshDstOperand ilu_dst;

    /* Constant register index (shared) */
    int               const_index;

    /* Input register index v# (shared) */
    int               input_index;

    /* Final instruction flag */
    int               is_final;

    /* >= 0: the slot's output write targets constant register N (OUT_ORB
     * = 0) instead of an output register. Recorded so a consumer can
     * report it; not emulated, as in xemu. -1 otherwise. */
    int               out_const_index;
} NV2AVshInstruction;

/** A complete parsed vertex program. */
typedef struct NV2AVshProgram {
    NV2AVshInstruction  insns[NV2A_VS_MAX_INSTRUCTIONS];
    int                 length;     /* Number of instructions */

    /* Bitmask of input registers read (v0-v15). Bit N = vN is used.
     * Used to determine the required input layout. */
    uint16_t            inputs_read;
} NV2AVshProgram;

/**
 * The register file after a program has run.
 *
 * temp[12] is oPos: R12 and oPos are the same register on this hardware, so
 * it is stored once and mirrored into out[] on the way out. out_written says
 * which outputs the program actually wrote, which is how a caller tells
 * "black" from "never assigned".
 */
typedef struct NV2AVshState {
    float    temp[NV2A_VS_MAX_TEMPS][4];
    float    out[NV2A_VSH_OUT_COUNT][4];
    float    addr;              /* a0.x, from ARL */
    uint16_t out_written;       /* bit N = out[N] was written */
} NV2AVshState;

/**
 * Decode microcode into a program.
 *
 * @param microcode  Four 32-bit words per instruction
 * @param num_insns  Number of instructions available; decoding also stops at
 *                   the first instruction with the final flag set
 * @param program    Receives the decoded program
 */
void nv2a_vsh_parse(const uint32_t *microcode, int num_insns,
                    NV2AVshProgram *program);

/**
 * Run a parsed program over one vertex.
 *
 * inputs is v0-v15, consts is c0-c(const_count-1).
 */
void nv2a_vsh_execute(const NV2AVshProgram *program,
                      const float inputs[][4],
                      const float (*consts)[4], int const_count,
                      NV2AVshState *st);

/**
 * The mask an output write actually applies: output_mask, except for oFog,
 * whose writes fill the first k components for a k-bit mask.
 */
uint8_t nv2a_vsh_output_write_mask(const NV2AVshDstOperand *dst);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_NV2A_VSH_H */
