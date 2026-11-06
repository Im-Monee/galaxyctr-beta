#pragma once

#include <JSystem/JGeometry/TMatrix.hpp>
#include <revolution/gx.h>
// 3DS: host.h for the Citro3D draws below.
#ifdef __3DS__
#include "host.h"
class J3DModelData;
#endif

class JUTTexture;

namespace TDDraw {
    void setViewMtx(MtxPtr);
    void loadViewMtx(MtxPtr);
    void setModelMtx(MtxPtr);
    void resetViewMtx();
    void close();
    void setup(u32, u32, u8);
#ifdef __3DS__
    void setupDirect(u32, u32, u8);
    void closeDirect();
    void setDirectDepthMode(bool, u8, bool);
    void setDirectViewMtx(MtxPtr);
    void setDirectProjectionMtx(const Mtx44&);
    void setDirectBlendState(u8, u8, u8);
    void setDirectAlphaTest(u8, u8);
    void setDirectCullMode(u8);
    void setDirectPixelWriteMask(u8);
    void setDirectScissor(u16, u16, u16, u16);
    void setDirectDestinationAlpha(bool, u8);
    // Stencil test (GPU_TESTFUNC, GPU_STENCILOP): enabled, function, reference, mask, write mask,
    // fail, zfail, zpass. Reset by setupDirect and closeDirect.
    void setDirectStencil(bool, u8, u8, u8, u8, u8, u8, u8);
    void setModelMtxDirect(MtxPtr);
    void resetViewMtxDirect();
    void drawDirectTriangles(const TVec3f*, const u32*, u32);
    void drawDirectStrip(const TVec3f*, const u32*, u32);
    void drawTexturedTriangles(const TVec3f*, const TVec2f*, JUTTexture*, u32);
    void drawColoredTexturedTriangles(const TVec3f*, const TVec2f*, const u32*, JUTTexture*, u32);
    void drawNativeColoredTriangles(const TVec3f*, const u32*, u32);
    void drawStateTriangles(const TVec3f*, const TVec2f (*)[3], const u32*, const HostGfxState&, u32);
    // The combiner of a native textured draw: texture times vertex colour; the vertex colour with
    // the texture's alpha (nw4r's intensity fonts); or the texture mapped from mapMin to mapMax
    // (0xRRGGBBAA), times the vertex colour (nw4r's colour mapping).
    enum { NATIVE_TEX_MODULATE, NATIVE_TEX_INTENSITY, NATIVE_TEX_COLOR_MAPPING };
    void drawNativeTexturedTriangles(const TVec3f*, const TVec2f*, const u32*, const void*, u16, u16, u32, const void*, u32, u16, u8, u8,
                                     u8, u32, u32 mode = NATIVE_TEX_MODULATE, u32 mapMin = 0, u32 mapMax = 0xFFFFFFFF);
    void drawTexturedTintedTriangles(const TVec3f*, const TVec2f*, JUTTexture*, u32, u32);
    // Triangles with the given combiner stages (their own constants), one vertex colour
    // (0xRRGGBBAA) or one per vertex, texture 0 (or none) and optionally texture 1 with its own
    // coordinates: an effect's Wii TEV translated stage by stage.
    void drawStageTriangles(const TVec3f*, const TVec2f*, u32, JUTTexture*, u32, const HostGfxStage*, u32, const u32* = nullptr,
                            JUTTexture* = nullptr, const TVec2f* = nullptr);
    // drawStageTriangles for an indexed triangle list (up to kStageIndexedVertices vertices): shared
    // vertices are transformed once and the whole list is one draw.
    enum { kStageIndexedVertices = 512 };
    void drawStageIndexed(const TVec3f*, const TVec2f*, u32, const u16*, u32, JUTTexture*, u32, const HostGfxStage*, u32);
    // A 0xRRGGBBAA colour as a stage constant (0xAABBGGRR).
    u32 stageConstant(u32);
    void drawTexturedColorLerpTriangles(const TVec3f*, const TVec2f*, const u32*, JUTTexture*, u32, u32, u32);
    void drawMaskedTexturedTriangles(const TVec3f*, const TVec2f*, const TVec2f*, JUTTexture*, JUTTexture*, u32);
    void drawMaskedTexturedIndexed(const TVec3f*, const TVec2f*, const TVec2f*, u32, const u16*, u32, JUTTexture*, JUTTexture*);
    void drawJ2DPictureTriangles(const TVec3f*, const TVec2f*, const u32*, JUTTexture* const*, const f32*, u32, u32);
    // The last u8: the stages whose alpha the combiner buffer takes (bit n: stage n).
    void drawParticleTriangles(HostGfxVertex*, const TVec2f*, const TVec2f*, u32, const u16*, u32, JUTTexture*, JUTTexture*, u32, const HostGfxStage*,
                               u32, u8 = 0);
    // A sample displaced by a third texture: unit 0 a step back on coordinate 0, unit 1 a step
    // forward, unit 2 the distortion on coordinate 1; the first two stages keep their constants.
    void drawParticleDistortedTriangles(HostGfxVertex*, const TVec2f*, const TVec2f*, u32, const u16*, u32, JUTTexture*, JUTTexture*, const f32*, const f32*, u32,
                                        const HostGfxStage*, u32);
    // A particle vertex for those draws: a camera-space position (JPAEmitterManager::draw's view is
    // the identity) in clip space with its colour, its texture coordinates empty.
    void particleVertexToClip(const TVec3f&, HostGfxVertex&, u32);
    f32 directViewUnitsPerPixel(const TVec3f&);
    // Immediate geometry (GXImmediate3DS.hpp): the vertices GX sends between GXBegin and GXEnd,
    // transformed by the DirectDraw view and projection and drawn with the DirectDraw state, the
    // texture set with setDirectTexture and the combine mode set with setDirectCombine.
    enum {
        DIRECT_COMBINE_VERTEX,                        // colour and alpha: vertex
        DIRECT_COMBINE_TEXTURE_X_VERTEX,              // colour and alpha: texture x vertex
        DIRECT_COMBINE_TEXTURE_X_VERTEX_TEX_ALPHA,    // colour: texture x vertex; alpha: texture
        DIRECT_COMBINE_TEXTURE_X_VERTEX_KONST_ALPHA,  // colour: texture x vertex; alpha: the constant's
        DIRECT_COMBINE_TEXTURE,                       // colour and alpha: texture
        DIRECT_COMBINE_STAGES,                        // the stages set with setDirectStages
        DIRECT_COMBINE_STATE,                         // the stages and textures of setDirectStagesFromState
    };
    // Texture coordinate generation of the immediate vertices (GXSetTexCoordGen2 with a matrix),
    // made on the CPU: the input is the vertex's texture coordinate (s, t, 1, 1), normal or
    // position (x, y, z, 1); a projective (3x4) matrix divides by its third row.
    enum { DIRECT_TEXGEN_TEX0, DIRECT_TEXGEN_NRM, DIRECT_TEXGEN_POS };
    void setDirectTexture(JUTTexture*);
    void setDirectCombine(u32, u32);
    void loadDirectPosMtx(const f32 (*)[4], u32);
    void beginDirectPrimitive(u32, u32);
    void directPosition(f32, f32, f32);
    void directColor(u32);
    void directTexCoord(f32, f32);
    void endDirectPrimitive();
    void flushDirectPrimitive();
    void directNormal(f32, f32, f32);
    void directTexComponent(f32);
    void setDirectTexGen(u32, u32, const f32 (*)[4], bool);
    void setDirectStages(const HostGfxStage*, u32, JUTTexture*, JUTTexture*, JUTTexture*);
    void setDirectStageConstant(u32, u32);
    void setDirectStagesFromState(const HostGfxState&);
    void setDirectVertexColor(u32);
    void drawDirectSimpleModel(J3DModelData*);
    void drawDirectSimpleModelSlots(J3DModelData*, const f32 (*const*)[4]);
    // The GX material of an immediate draw, with its GX values: camera view and projection, the
    // texture and combine mode, GXSetBlendMode, GXSetAlphaCompare (one comparison), GXSetZMode,
    // GXSetCullMode.
    void setupDirectImmediate(JUTTexture*, u32, u32, u32, u32, u32, u32, u8, bool, u32, bool, u32);
    void drawTexture(const TVec2f&, JUTTexture*, const TVec2f&, u8);
    // A host texture (hostGfx handle, e.g. the movie decoder's) over the rectangle, (0, 0) to (u, v)
    // of it, replacing the colour.
    void drawHostTexture(const TVec2f&, const TVec2f&, void*, f32, f32);
    void drawTexture3D(const TVec3f&, const TVec3f&, const TVec3f&, f32, f32, JUTTexture*, bool, bool, u8);
    void drawTextureTaps(const TVec2f&, JUTTexture*, const TVec2f&, const f32 (*)[2], u32, u8);
    void drawTextureTapsAtDepth(const TVec2f&, JUTTexture*, const TVec2f&, const f32 (*)[2], u32, u8, f32);
    void drawTextureBrightPass(const TVec2f&, JUTTexture*, const TVec2f&, u8, const u8*);
    void drawTextureStages(const TVec2f&, JUTTexture*, const TVec2f&, const HostGfxStage*, u32, bool);
#endif
    void sendPoint(const TVec3f&, u32) NO_INLINE;
    void sendPoint(const TVec3f&) NO_INLINE;
    void drawLine(const TVec3f&, const TVec3f&, u32);
    // 3DS: a line with a width (the PICA draws no lines: a thin quad).
#ifdef __3DS__
    void drawLineWidth(const TVec3f&, const TVec3f&, u32, u8);
#endif
    void drawCircle(const TVec3f&, const TVec3f&, f32, u32, u32);
    void drawCircle(const TVec3f&, const TVec3f&, const TVec3f&, f32, u32, u32);
    void drawFillCircle(const TVec3f&, f32, u32, u32, u32);
    void drawFillCircle(const TVec3f&, const TVec3f&, f32, u32, u32);
    void drawFillFan(const TVec3f&, const TVec3f&, const TVec3f&, u32, f32, f32, u32);
    void drawCylinder(const TVec3f&, const TVec3f&, f32, u32, u32, u32);
    void drawSpherePart(const TPos3f&, f32, f32, f32, f32, f32, u32, u32, u32);
    void drawSphere(const TVec3f&, f32, u32, u32);
    void drawSphere3D(TVec3f, f32, u32, u32);
    void drawTexture(const TVec2f&, JUTTexture*, const TVec2f&);
    void drawTexture3D(const TVec3f&, const TVec3f&, const TVec3f&, f32, f32, JUTTexture*, bool, bool);
    void drawFillBox(const TVec3f&, const TVec3f&, u32);
    void drawFillBox(const TVec2f&, const TVec2f&, u32);
    void drawFillBox3D(const TVec3f&, const TVec3f&, const TVec3f&, const TVec3f&, u32);
    void cameraInit3D();
    void cameraInit2D();
    void mixFogColor(TVec3f, f32, u32);
    void tileConversion8(u8*, u32, u32);
    void tileConversion16(u16*, u32, u32);
    u32 getTexel32(const JUTTexture*, u32, u32);
    u32 getTexel32(const u8*, u32, u32, u32);
    void setTexel32(JUTTexture*, u32, u32, u32);
    void setTexel32(u8*, u32, u32, u32, u32);
    void invProject(TVec3f*, const TVec3f&, MtxPtr, const f32*, const f32*, bool);
    void project2D(TVec3f*, const TVec3f&);
    void project2D(TVec2f*, const TVec3f&);
    void fix2Dpos(TVec3f*);

    inline void setGXColor(u32 color, GXColor* pColor) NO_INLINE {
        pColor->r = (color >> 24) & 0xFF;
        pColor->g = (color >> 16) & 0xFF;
        pColor->b = (color >> 8) & 0xFF;
        pColor->a = color & 0xFF;
    }
};  // namespace TDDraw
