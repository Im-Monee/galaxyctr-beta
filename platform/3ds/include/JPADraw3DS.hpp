#pragma once

// JPA particles on the 3DS (platform/3ds/gx/particle.cpp): what the Wii's JPA draws send to GX
// (vertex arrays and display lists, position and texture matrices, texture coordinate generation,
// TEV colours and modes, alpha test, point size and line width), kept here and drawn through
// Citro3D with the vertices and texture coordinates GX would make of them.

#include <revolution/gx.h>
#include <revolution/mtx.h>

namespace JPA3DS {
    // The position and texture coordinate arrays the display lists index (JPAResource::setPTev /
    // setCTev: jpa_pos and jpa_crd at the shape's offsets) and the texture map of the particle's
    // texture (GX_TEXMAP0 base particles, GX_TEXMAP1 child ones).
    void setArrays(const u8* pPositions, const u8* pTexCoords, u32 textureMap);
    // The extra texture shape's second texture (TEV stage 1 times GX_TEXMAP3) and indirect texture.
    void setExTex(bool second, bool indirect);
    // The extra texture's indirect matrix (2 rows of 3) and exponent.
    void setIndirect(const f32* pMatrix, s8 exponent);
    // Texture coordinate 0's generation (JPAGenTexCrdMtx*): the vertex's (s, t), the texture
    // matrix times (s, t, 1, 1), or the texture matrix times the position (projected).
    enum { TEXGEN_IDENTITY, TEXGEN_MATRIX, TEXGEN_POSITION };
    void setTexGen(u32 mode);
    // GXLoadTexMtxImm of GX_TEXMTX0 (rows 0-1 for TEXGEN_MATRIX, 0-2 for TEXGEN_POSITION).
    void loadTexMtx(const Mtx matrix);
    // GXLoadPosMtxImm of GX_PNMTX0: model to camera space.
    void loadPosMtx(const Mtx matrix);
    // GXSetTevColor of GX_TEVREG0 (prm) and GX_TEVREG1 (env).
    void setTevColor(GXTevRegID id, GXColor color);
    // JPABaseShape::setGX: the colour and alpha modes, and the alpha test (two comparisons).
    void setModes(u32 colorMode, u32 alphaMode);
    void setAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1);
    // GXSetPointSize / GXSetLineWidth, in sixths of a pixel.
    void setPointSize(f32 sixths);
    void setLineWidth(f32 sixths);
    // GXCallDisplayList of jpa_dl / jpa_dl_x: GX_QUADS of indexed positions and coordinates.
    void callDisplayList(const u8* pList);
    // GXBegin / GXPosition3f32 + GXTexCoord2f32 / GXEnd of direct vertices (GX_POINTS, GX_LINES,
    // GX_TRIANGLESTRIP).
    void begin(GXPrimitive primitive);
    void vertex(f32 x, f32 y, f32 z, f32 s, f32 t);
    void end();
    // Draws the triangles kept so far: before the direct state they depend on changes.
    void flush();
    // JPAEmitterManager::draw, after its direct setup: the shapes' state below is set anew.
    void beginDraw();
    // JPABaseShape::setGX: blending and depth (TDDraw::setDirectBlendState, setDirectDepthMode).
    // The kept triangles are drawn first only when they change, so emitters that share them join
    // one batch (each emitter was a draw of its own).
    void setShapeState(u8 equation, u8 source, u8 destination, bool zEnable, u8 zTest, bool zUpdate);
    // For the periodic log: batches drawn, quads kept and quads dropped off screen since the last call.
    void takeStats(u32& batches, u32& quads, u32& culled);
    // For the periodic log: why the batches since the last call were drawn.
    void logFlushReasons();
}  // namespace JPA3DS
