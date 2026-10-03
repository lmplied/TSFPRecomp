/*
 * d3d8_xbox_map.h -- Xbox D3D8 enumerations to the host layer's PC ones.
 *
 * Used by shadow mode (hle_d3d8.c). The frame replay tool shared these while
 * captures held Xbox values; captures now hold host values (d3d8_capture.h,
 * version 2), so replay no longer needs them. Header-only and static inline
 * all the same, so a second user can include them without linking xbox_hle.
 *
 * Windows only: the host D3D8 types come from d3d8_xbox.h, which is part of
 * the D3D11 backend. Everything here is pure value mapping -- no device, no
 * state, no allocation.
 */
#ifndef XBOXRECOMP_D3D8_XBOX_MAP_H
#define XBOXRECOMP_D3D8_XBOX_MAP_H

#include <stdint.h>
#include "d3d8_xbox.h"

/* Xbox D3DPRIMITIVETYPE (Cxbx-Reloaded, XbD3D8Types.h). */
enum {
    XPT_POINTLIST = 1, XPT_LINELIST, XPT_LINELOOP, XPT_LINESTRIP,
    XPT_TRIANGLELIST, XPT_TRIANGLESTRIP, XPT_TRIANGLEFAN,
    XPT_QUADLIST, XPT_QUADSTRIP, XPT_POLYGON
};

/* Xbox D3DCLEAR_* bits: TARGET is 0xF0, one bit per channel (0x10 R, 0x20 G,
 * 0x40 B, 0x80 A); ZBUFFER is 0x01 and STENCIL 0x02. The host layer takes the
 * PC values. Two things are not honoured: a per-channel target mask (a target
 * bit clears all four channels, as Cxbx-Reloaded also does) and clear
 * rectangles, which the host dev_Clear ignores, so a rectangle-limited clear
 * clears the whole target.
 *
 * Except that an alpha-only clear (0x80) clears nothing: TimeSplitters: Future
 * Perfect resets destination alpha to 0 over its finished picture (its menus
 * after the 3D background, its in-game hint screens after the scene), and
 * passing that on as a whole-target clear left both black but for the text.
 * Keeping the colour and leaving alpha uncleared is the lesser error. */
static __inline DWORD xbox_clear_flags_to_host(uint32_t xbox_flags)
{
    DWORD host = 0;

    if (xbox_flags & 0x70)
        host |= D3DCLEAR_TARGET;
    if (xbox_flags & 0x01)
        host |= D3DCLEAR_ZBUFFER;
    if (xbox_flags & 0x02)
        host |= D3DCLEAR_STENCIL;
    return host;
}

/* An Xbox primitive type and a vertex count, against the host's PC numbering
 * and a primitive count. Line loops, quad strips and polygons have no PC
 * equivalent: a quad strip draws as the triangle strip over the same
 * vertices, a polygon as a fan, and a line loop as a strip that repeats its
 * first vertex. */
static __inline int xbox_primitive_to_host(uint32_t xpt, uint32_t vertices,
                                           D3DPRIMITIVETYPE *pt, UINT *prims)
{
    switch (xpt) {
    case XPT_POINTLIST:     *pt = D3DPT_POINTLIST;     *prims = vertices;       break;
    case XPT_LINELIST:      *pt = D3DPT_LINELIST;      *prims = vertices / 2;   break;
    case XPT_LINELOOP:      *pt = D3DPT_LINESTRIP;     *prims = vertices;       break;
    case XPT_LINESTRIP:     *pt = D3DPT_LINESTRIP;     *prims = vertices - 1;   break;
    case XPT_TRIANGLELIST:  *pt = D3DPT_TRIANGLELIST;  *prims = vertices / 3;   break;
    case XPT_TRIANGLESTRIP: *pt = D3DPT_TRIANGLESTRIP; *prims = vertices - 2;   break;
    case XPT_QUADSTRIP:     /* whole quads only; an odd last vertex is unused */
                            *pt = D3DPT_TRIANGLESTRIP; *prims = (vertices & ~1u) - 2; break;
    case XPT_TRIANGLEFAN:
    case XPT_POLYGON:       *pt = D3DPT_TRIANGLEFAN;   *prims = vertices - 2;   break;
    case XPT_QUADLIST:      *pt = D3DPT_QUADLIST;      *prims = vertices / 4;   break;
    default:
        return 0;
    }
    /* A single point is a valid draw (Cxbx-Reloaded, IsValidXboxVertexCount);
     * too few vertices for anything else leaves prims at 0 or wrapped below. */
    return (int)*prims > 0 && *prims <= vertices;
}

/* The Xbox packs the transform states together -- VIEW 0, PROJECTION 1,
 * TEXTURE0-3 2-5, WORLD-WORLD3 6-9 (Cxbx-Reloaded, XbD3D8Types.h) -- where
 * the host has the PC's 2, 3, 16-19, 256-259. Returns 0 for a state outside
 * that range, which the caller drops. */
static __inline int xbox_transform_state_to_host(uint32_t state, DWORD *host)
{
    static const DWORD table[10] = { 2, 3, 16, 17, 18, 19, 256, 257, 258, 259 };

    if (state >= 10u)
        return 0;
    *host = table[state];
    return 1;
}

#endif /* XBOXRECOMP_D3D8_XBOX_MAP_H */
