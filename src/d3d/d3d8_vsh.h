/**
 * NV2A Vertex Shader Microcode to HLSL Translator
 *
 * The Xbox NV2A GPU has a programmable vertex shader unit compatible with
 * (and extending) the original GeForce3/4 vertex shader architecture.
 * Games upload sequences of 128-bit microcode instructions via
 * D3DDevice_CreateVertexShader(). At draw time, the NV2A executes
 * these instructions in its vertex shader pipeline.
 *
 * This module translates NV2A vertex shader microcode into HLSL source
 * code, compiles it with D3DCompile, and caches the resulting
 * host vertex shader (rhi.h) for use by the D3D8 compatibility layer.
 *
 * Decoding the microcode and running it on the CPU is not here. That half
 * needs no graphics API, so it lives in src/kernel/nv2a_vsh.{c,h} and builds
 * on every platform; the instruction set, register model and the types
 * NV2AVshProgram and NV2AVshState are documented there. This header adds only
 * what is specific to the Direct3D 11 path.
 *
 * References:
 *   - envytools NV20 vertex shader documentation
 *   - xemu NV2A vertex shader implementation
 *   - Xbox SDK D3D vertex shader programming guide
 *   - US Patent 7,002,588 (Microsoft/Nvidia vertex shader architecture)
 *   - docs/technical/nv2a-vertex-program-encoding.md
 */

#ifndef XBOXRECOMP_D3D8_VSH_H
#define XBOXRECOMP_D3D8_VSH_H

#include <stdint.h>
#include <windows.h>

#include "rhi.h"

#include "../kernel/nv2a_vsh.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of shader programs that can be stored.
 *
 * 128 was enough for the first four titles and is not enough for Panzer
 * Dragoon Orta, which creates at least 183 vertex shaders during start-up and
 * printed "No free shader slots" 55 times. A slot is about 2.5 KB (136
 * instructions of microcode plus a declaration), so this table is roughly
 * 640 KB of static data at 256 and the headroom is cheap. Running out is not:
 * the shader is dropped, and the draws that use it are skipped silently
 * apart from that one line. */
#define NV2A_VS_MAX_SLOTS           256

/** Shader cache size (hashed microcode -> compiled shader). */
#define NV2A_VS_CACHE_SIZE          64

/** The fog value a vertex program that never writes oFog hands the pixel
 * stage, and the test that recognises it there: far below any fog value a
 * program computes, so the pixel shaders can tell "never written" from a
 * written 0 or 1. The combiners resolve it by D3DRS_FOGENABLE (1 with fog
 * on, 0 with it off, d3d8_combiners.c); the fixed-function pixel shader
 * reads it as 1, the default it always had (d3d8_shaders.c). */
#define D3D8_VSH_FOG_UNWRITTEN       "(-1.0e30)"
#define D3D8_VSH_FOG_IS_UNWRITTEN(x) "(" x " < -1.0e29)"

/* ================================================================
 * Shader Slot (stored microcode)
 * ================================================================ */

/**
 * A stored vertex shader program slot.
 *
 * Created by CreateVertexShader(), indexed by handle.
 * The microcode is stored as raw DWORDs; parsing and compilation
 * are deferred until the shader is first used in a draw call.
 */
/**
 * One vertex register a program reads, laid out as the title's vertex
 * declaration says: the format of the bytes stored in the vertex (an
 * RhiFormat, which is the DXGI number captures store) and
 * their offset from the start of a stream 0 vertex.
 */
typedef struct D3D8VshInput {
    int         reg;        /* v0..v15 */
    RhiFormat   format;
    UINT        offset;
} D3D8VshInput;

typedef struct NV2AVshSlot {
    DWORD   microcode[NV2A_VS_MAX_INSTRUCTIONS * 4]; /* Raw 128-bit instructions */
    int     length;         /* Number of instructions */
    int     in_use;         /* 1 if this slot is allocated */
    uint32_t hash;          /* of the microcode, the compiled-shader cache key */
    /* The vertex declaration, when one was given (d3d8_vsh_set_declaration).
     * Without it the input layout is guessed from the registers read. */
    D3D8VshInput decl[NV2A_VS_MAX_INPUTS];
    int          decl_count;
    uint32_t     decl_hash;
} NV2AVshSlot;

/* ================================================================
 * VS Constant Buffer Layout (HLSL)
 *
 * Uploaded to register(b1) so it doesn't conflict with the
 * fixed-function transform CB at b0.
 *
 * Must be 16-byte aligned and match the HLSL cbuffer declaration.
 * ================================================================ */

typedef struct NV2AVSConstants {
    float c[NV2A_VS_MAX_CONSTANTS][4];  /* 192 float4 constants */
} NV2AVSConstants;

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * Initialize the vertex shader translator.
 * Allocates the constant buffer and shader cache.
 * Must be called after D3D11 device creation.
 */
HRESULT d3d8_vsh_init(void);

/**
 * Shut down the vertex shader translator.
 * Releases all cached shaders, input layouts, and buffers.
 */
void d3d8_vsh_shutdown(void);

/**
 * Store a vertex shader program (CreateVertexShader).
 *
 * Copies the microcode into an internal slot. The shader is not
 * compiled until first use.
 *
 * @param microcode   Pointer to the 128-bit instruction array (4 DWORDs each)
 * @param num_insns   Number of instructions
 * @param out_handle  Receives the shader handle (>= 0x10000 to distinguish from FVF)
 * @return S_OK on success
 */
HRESULT d3d8_vsh_create_shader(const DWORD *microcode, int num_insns,
                                DWORD *out_handle);

/**
 * Delete a previously created vertex shader.
 *
 * @param handle  The shader handle from d3d8_vsh_create_shader
 * @return S_OK on success
 */
HRESULT d3d8_vsh_delete_shader(DWORD handle);

/**
 * Set a vertex shader constant register.
 *
 * @param start_reg  First register index (0-191)
 * @param data       Pointer to float4 data (4 floats per register)
 * @param count      Number of float4 registers to set
 */
void d3d8_vsh_set_constant(int start_reg, const float *data, int count);
/* Which constant register the experimental Hor+ scale applies to. */
int  d3d8_vsh_hor_plus_reg(void);
/* The registers frame interpolation blends (xbox_D3D8SetInterpRegisters):
 * the projection's first register or -1, and a run of 4-register affine
 * matrices (count a multiple of 4; first -1 and count 0 for none). */
void d3d8_vsh_interp_registers(int *projection, int *affine_first, int *affine_count);

/**
 * The whole constant bank, as NV2A_VS_MAX_CONSTANTS float4 registers laid out
 * back to back (NV2AVSConstants).
 *
 * Constants arrive a few registers at a time and are never read back by the
 * renderer, so nothing needed this until frame capture: a capture has to be
 * self-contained, and the frame it records draws with constants set before it
 * began (src/hle/hle_d3d8_record.c). Read-only; the pointer stays valid for
 * the life of the process.
 */
const float *d3d8_vsh_constants(void);

/**
 * Check if a shader handle refers to a programmable vertex shader
 * (as opposed to an FVF code).
 *
 * On Xbox, handles > 0xFFFF are shader handles.
 */
BOOL d3d8_vsh_is_programmable(DWORD handle);

/**
 * Give a program the vertex layout its declaration describes. Registers the
 * program reads are bound at these formats and offsets instead of being
 * packed one after another at default formats.
 *
 * @param handle  The shader handle from d3d8_vsh_create_shader
 * @param inputs  One entry per declared register (stream 0)
 * @param count   Number of entries, at most NV2A_VS_MAX_INPUTS; 0 clears it
 */
HRESULT d3d8_vsh_set_declaration(DWORD handle, const D3D8VshInput *inputs, int count);

/**
 * Xbox vertex programs write oPos in render-target pixels. With a scale and
 * offset set, every program undoes that before the rasteriser:
 *   oPos = (oPos - offset) / scale;  if (w == 0) w = 1;  xyz *= w
 * (Cxbx-Reloaded, CxbxVertexShaderTemplate.hlsl). Until this is called the
 * programs leave oPos as they wrote it.
 *
 * @param scale   (width / 2, -height / 2, depth-buffer Z scale, 1)
 * @param offset  (width / 2, height / 2, 0, 0)
 */
void d3d8_vsh_set_screenspace(const float scale[4], const float offset[4]);

/**
 * What d3d8_vsh_set_screenspace last set, and whether it is on. Frame capture
 * (src/hle/hle_d3d8_record.c) reads it into a capture's opening snapshot.
 */
void d3d8_vsh_get_screenspace(float scale[4], float offset[4], int *enabled);

/**
 * Turn the screen-space undo off again, as it is before the first
 * d3d8_vsh_set_screenspace. Nothing in a live run needs this; the replay tool
 * does, to put a capture that begins with it off back into that state on
 * every loop.
 */
void d3d8_vsh_clear_screenspace(void);

/**
 * The current value of input register reg (0-15): what a program reads from
 * a register its declaration does not feed, as the NV2A does with the value
 * the SetVertexData* calls last set (src/hle forwards SetVertexDataColor and
 * SetVertexData2f; the other variants are not yet). Starts at (1,1,1,1) for v3, the
 * diffuse colour, and (0,0,0,1) for the rest. The getter is for frame
 * capture's snapshot.
 */
void d3d8_vsh_set_vertex_data(int reg, const float value[4]);
void d3d8_vsh_get_vertex_data(int reg, float value[4]);

/**
 * One stored program, by slot index (0 .. NV2A_VS_MAX_SLOTS-1): its handle,
 * microcode (length instructions of 4 DWORDs) and declaration. FALSE for a
 * free slot. The pointers stay valid until the slot is deleted. For frame
 * capture, which has to carry every program a frame may select, including
 * those created long before it.
 */
BOOL d3d8_vsh_get_slot(int slot, DWORD *handle, const DWORD **microcode,
                       int *length, const D3D8VshInput **decl, int *decl_count);

/**
 * Prepare for a draw call using a programmable vertex shader.
 *
 * - Parses microcode if not yet parsed
 * - Generates HLSL and compiles if not cached
 * - Updates the constant buffer
 * - Binds the vertex shader, input layout, and constant buffer
 *
 * @param handle  The active vertex shader handle
 * @return TRUE if a programmable VS was bound, FALSE on fallback
 */
BOOL d3d8_vsh_prepare_draw(DWORD handle);

/* Whether the program bound by the last prepare_draw transforms through
 * the projection's first column. False means it draws in screen
 * coordinates of its own -- the HUD, a menu, a full-screen quad. */
int  d3d8_vsh_bound_uses_projection(void);
/* ... and whether that projection, as uploaded right now, is orthographic:
 * a constant w row. A HUD drawn through the 3D programs looks like this. */
int  d3d8_vsh_bound_projection_is_ortho(void);

/* Of the 4-register groups from c[first] (count registers), which the
 * bound program reads: bit g for c[first+4g..first+4g+3]. A program that
 * indexes through a0 is taken to read them all. */
uint32_t d3d8_vsh_bound_reads(int first, int count);

/* The input register the bound program copies oPos from, or -1; and where
 * a register sits in the vertex, per that program's own declaration.
 * Together these let the draw path measure how wide a screen-space draw
 * is without knowing anything about the title. */
int  d3d8_vsh_bound_pos_input(void);
int  d3d8_vsh_bound_input_offset(int reg, UINT *offset);

/**
 * Generate HLSL vertex shader source from parsed program.
 *
 * @param program   Parsed program (from nv2a_vsh_parse)
 * @param buf       Output buffer for HLSL source
 * @param bufsize   Size of output buffer
 * @return Number of characters written, or -1 on error
 */
int d3d8_vsh_generate_hlsl(const NV2AVshProgram *program,
                            char *buf, int bufsize);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D8_VSH_H */
