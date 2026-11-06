#include "Game/Util/DrawUtil.hpp"
#include "Game/Scene/GameSceneFunction.hpp"
#include "Game/Screen/ScreenAlphaCapture.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/Color.hpp"
// 3DS: DirectDraw, TDDraw's Citro3D draws; GXToCitro3D.hpp, GX values as 3DS GPU values.
#ifdef __3DS__
#include "Game/Util/DirectDraw.hpp"
#include "GXToCitro3D.hpp"
#endif
#include "Game/Util/MtxUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3DGraphBase/J3DSys.hpp>
#include <JSystem/JGeometry/TMatrix.hpp>
#include <JSystem/JUtility/JUTVideo.hpp>
// 3DS: GXDelta3DS.hpp, GX state changes as data; host.h, the port's host interface.
#ifdef __3DS__
#include "GXDelta3DS.hpp"
#include "host.h"
#endif
#include <revolution/gx/GXBump.h>
#include <revolution/gx/GXCull.h>
#include <revolution/gx/GXEnum.h>
#include <revolution/gx/GXFrameBuf.h>
#include <revolution/gx/GXGeometry.h>
#include <revolution/gx/GXPixel.h>
#include <revolution/gx/GXRegs.h>
#include <revolution/gx/GXStruct.h>
#include <revolution/gx/GXTransform.h>
#include <revolution/gx/GXVert.h>

static u8 sTexImgObj[] ATTRIBUTE_ALIGN(32) = {0,    0xFF, 0,    0xFF, 0,    0xFF, 0,    0xFF, 0,    0xFF, 0,    0xFF, 0,    0xFF, 0,    0xFF,
                                              0,    0xFF, 0,    0xFF, 0,    0xFF, 0,    0xFF, 0,    0xFF, 0,    0xFF, 0,    0xFF, 0,    0xFF,
                                              0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                              0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static bool sIsReinitTextureCache = true;
static GXTexCacheSize sReinitTextureCacheSize = GX_TEXCACHE_128K;

void DrawUtil_FORCE_MATCH_SDATA2() {
    (void)-10000.0f;
    (void)10000.0f;
    (void)608.0f;
    (void)-1000.0f;
    (void)1000.0f;
    (void)-0.5f;
    (void)-30000.0f;
    (void)30000.0f;
}

namespace {
#ifdef __3DS__
    GXColor sDirectFillScreenColor;
    bool sDirectFillScreenAlphaBlend;
    // fillScreenArea only where the stencil is not 0, which it resets (fillSilhouetteColor).
    bool sDirectFillScreenStencil;
#endif
    GXTexObj clear_z_tobj;

    const f32 cNearZ = -10000.0f;
    const f32 cFarZ = 10000.0f;

    inline s32 getScreenHeightInline() {
        return MR::getScreenHeight();
    }
};  // namespace

const JUTTexture* mShadowTex;
TVec3f mShadowVec;

namespace MR {
    void drawInit() {
        // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
        // (DETAILS.md, section 5).
#ifdef __3DS__
        return;
#else
        j3dSys.offFlag(2);
        j3dSys.drawInit();
        j3dSys.setTexCacheRegion(GX_TEXCACHE_128K);
#endif
    }

    void reinitGX() {
        // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
        // (DETAILS.md, section 5).
#ifdef __3DS__
        return;
#else
        j3dSys.reinitGX();
        GXSetAlphaUpdate(GX_FALSE);
        J3DShape::resetVcdVatCache();

        if (sIsReinitTextureCache) {
            j3dSys.setTexCacheRegion(sReinitTextureCacheSize);
        }
#endif
    }

    void resetTextureCacheSize() {
        // 3DS: no GX texture cache to set up.
#ifdef __3DS__
        return;
#else
        if (sIsReinitTextureCache) {
            j3dSys.setTexCacheRegion(sReinitTextureCacheSize);
        }
#endif
    }

    void loadViewMtxFor2DModel() {
        Mtx viewMtx;
        PSMTXIdentity(viewMtx);
        PSMTXCopy(viewMtx, j3dSys.mViewMtx);
    }

    void drawInitFor2DModel() {
#ifdef __3DS__
        // The Wii's orthographic projection for the native draws, until the frame ends
        // (rvlNativeEndFrame); no GX state to set. Lens flares and logos are placed in it.
        Mtx44 projMtx;
        C_MTXOrtho(projMtx, 0.0f, -MR::getScreenHeight(), 0.0f, MR::getScreenWidth(), ::cNearZ, ::cFarZ);
        rvlNativeSetViewport(projMtx, nullptr, nullptr);

        Mtx viewMtx;
        PSMTXIdentity(viewMtx);
        PSMTXCopy(viewMtx, j3dSys.mViewMtx);
#else
        MR::reinitGX();
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);

        Mtx44 projMtx;
        C_MTXOrtho(projMtx, 0.0f, -MR::getScreenHeight(), 0.0f, MR::getScreenWidth(), ::cNearZ, ::cFarZ);
        GXSetProjection(projMtx, GX_ORTHOGRAPHIC);

        Mtx viewMtx;
        PSMTXIdentity(viewMtx);
        PSMTXCopy(viewMtx, j3dSys.mViewMtx);
#endif
    }

    void setDefaultViewportAndScissor() {
        // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
        // (DETAILS.md, section 5).
#ifdef __3DS__
        return;
#else
        s32 width = MR::getFrameBufferWidth();
        s32 height = ::getScreenHeightInline();
        GXSetViewport(0.0f, 0.0f, width, height, 0.0f, 1.0f);
        GXSetScissor(0, 0, width, height);
#endif
    }

    void loadProjectionMtxFor2D() {
        J2DOrthoGraphSimple orthoGraph;
        orthoGraph.setPort();
    }

    void setupDrawForNW4RLayout(f32 scale, bool) {
#ifdef __3DS__
        // The layout's orthographic projection, as on the Wii. Its panes load their own view
        // matrices (lyt Pane::LoadMtx) and its materials their blending (lyt Material::SetupGX).
        const f32 height = MR::getScreenHeight() * 0.5f * scale;
        const f32 halfWidth = 608.0f / 2.0f * scale;
        Mtx44 projMtx;
        C_MTXOrtho(projMtx, height, -height, -halfWidth, halfWidth, -1000.0f, 1000.0f);
        TDDraw::setupDirect(0, 1, 3);
        TDDraw::setDirectProjectionMtx(projMtx);
        TDDraw::setDirectCullMode(GXToCitro3D::CULL_NONE);
        TDDraw::setDirectDepthMode(false, GXToCitro3D::TEST_ALWAYS, false);
#else
        f32 height = MR::getScreenHeight();
        f32 width = 608.0f;
        f32 halfWidth = width / 2.0f;
        height *= 0.5f;
        halfWidth *= scale;
        height *= scale;
        Mtx44 projMtx;
        C_MTXOrtho(projMtx, height, -height, -halfWidth, halfWidth, -1000.0f, 1000.0f);
        GXSetProjection(projMtx, GX_ORTHOGRAPHIC);
        GXSetCullMode(GX_CULL_NONE);
        GXSetZMode(GX_FALSE, GX_NEVER, GX_FALSE);
#endif
    }

    void clearZBuffer() {
#ifdef __3DS__
        // The GX path samples a constant maximum-depth Z texture. A depth-only
        // full-screen Citro3D pass reproduces that clear without touching color.
        // EFB depth reads see the scene's depth, as on the Wii, not this clear.
        hostGfxKeepDepthForReads();
        const f32 corners[6][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f},
                                   {-1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
        HostGfxVertex vertices[6] = {};
        for (u32 i = 0; i < 6; i++) {
            vertices[i].x = corners[i][0];
            vertices[i].y = corners[i][1];
            vertices[i].z = 0.0f;  // the far plane (3DS depth 0); clip space keeps z in [-w, 0]
            vertices[i].w = 1.0f;
            vertices[i].rgba[0] = vertices[i].rgba[1] = vertices[i].rgba[2] = vertices[i].rgba[3] = 255;
        }
        HostGfxState state = {};
        state.alphaTestFunc = 1;
        state.depthTest = 1;
        state.depthFunc = 1;  // GPU_ALWAYS
        state.writeMask = 0x10;  // depth only
        state.cull = 0;
        state.blendSrc = 1;
        state.scissor[2] = 640;
        state.scissor[3] = 456;
        hostGfxDraw(vertices, 6, nullptr, 6, HOST_GFX_TRIANGLES, &state);
#else
        JUTVideo* pJUTVideo = JUTVideo::getManager();
        u16 width = pJUTVideo->getFbWidth();
        u16 height = pJUTVideo->getEfbHeight();
        Mtx44 projMtx;
        C_MTXOrtho(projMtx, 0.0f, height, 0.0f, width, 0.0f, 1.0f);
        GXSetProjection(projMtx, GX_ORTHOGRAPHIC);
        MR::setDefaultViewportAndScissor();
        TMtx34f mtxImm;
        mtxImm.identity();
        GXLoadPosMtxImm(mtxImm, GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_U16, 0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_POS_XYZ, GX_U8, 0);
        GXSetNumChans(0);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetNumTexGens(1);
        GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
        GXInitTexObj(&::clear_z_tobj, sTexImgObj, 4, 4, GX_TF_Z24X8, GX_REPEAT, GX_REPEAT, GX_FALSE);
        GXInitTexObjLOD(&::clear_z_tobj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
        GXLoadTexObj(&::clear_z_tobj, GX_TEXMAP0);
        GXSetNumTevStages(1);
        GXColor tev0;
        tev0.r = 0;
        tev0.g = 0;
        tev0.b = 0;
        tev0.a = 0;
        GXSetTevColor(GX_TEVREG0, tev0);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
        GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
        GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
        GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GXSetZTexture(GX_ZT_REPLACE, GX_TF_Z24X8, 0);
        GXSetZCompLoc(GX_FALSE);
        GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_NOOP);
        GXSetColorUpdate(GX_FALSE);
        GXSetAlphaUpdate(GX_FALSE);
        GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
        GXSetCullMode(GX_CULL_BACK);

        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        {
            GXPosition2u16(0, 0);
            GXTexCoord2u8(0, 0);

            GXPosition2u16(width, 0);
            GXTexCoord2u8(1, 0);

            GXPosition2u16(width, height);
            GXTexCoord2u8(1, 1);

            GXPosition2u16(0, height);
            GXTexCoord2u8(0, 1);
        }
        GXEnd();

        GXSetZTexture(GX_ZT_DISABLE, GX_TF_Z24X8, 0);
        GXSetZCompLoc(GX_TRUE);
        GXSetColorUpdate(GX_TRUE);
#endif
    }

    void clearAlphaBuffer(u8 alpha) {
        JUTVideo* pVideo = JUTVideo::getManager();
        f32 height = pVideo->getEfbHeight();
        f32 width = pVideo->getFbWidth();
        clearAlphaBuffer(alpha, TVec2f(0.0f, 0.0f), TVec2f(width, height));
    }

    void clearAlphaBuffer(u8 alpha, const TVec2f& rPosition, const TVec2f& rSize) {
#ifdef __3DS__
        const f32 width = getFrameBufferWidth();
        const f32 height = ::getScreenHeightInline();
        Mtx44 projection;
        C_MTXOrtho(projection, 0.0f, height, 0.0f, width, -1.0f, 1.0f);
        const f32 x0 = rPosition.x, y0 = rPosition.y;
        const f32 x1 = x0 + rSize.x, y1 = y0 + rSize.y;
        const f32 points[6][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y0}, {x1, y1}, {x0, y1}};
        HostGfxVertex vertices[6] = {};
        for (u32 i = 0; i < 6; i++) {
            const f32 x = points[i][0], y = points[i][1], z = 0.0f;
            vertices[i].x = x * projection[0][0] + y * projection[0][1] + z * projection[0][2] + projection[0][3];
            vertices[i].y = x * projection[1][0] + y * projection[1][1] + z * projection[1][2] + projection[1][3];
            vertices[i].z = x * projection[2][0] + y * projection[2][1] + z * projection[2][2] + projection[2][3];
            vertices[i].w = x * projection[3][0] + y * projection[3][1] + z * projection[3][2] + projection[3][3];
            vertices[i].rgba[0] = vertices[i].rgba[1] = vertices[i].rgba[2] = 255;
            vertices[i].rgba[3] = alpha;
        }
        HostGfxState state = {};
        state.dstAlphaEnable = 1;
        state.dstAlpha = alpha;
        state.alphaTestFunc = 1;
        state.blendSrc = 1;
        state.depthFunc = 1;
        state.writeMask = 0x08;  // GXSetAlphaUpdate(true), with colour and depth writes disabled.
        state.scissor[2] = 640;
        state.scissor[3] = 456;
        hostGfxDraw(vertices, 6, nullptr, 6, HOST_GFX_TRIANGLES, &state);
#else
        u16 width = rSize.x;
        u16 height = rSize.y;
        Mtx44 projection;
        C_MTXOrtho(projection, 0.0f, ::getScreenHeightInline(), 0.0f, getFrameBufferWidth(), -1.0f, 1.0f);
        GXSetProjection(projection, GX_ORTHOGRAPHIC);
        GXSetCurrentMtx(GX_PNMTX0);
        Mtx matrix;
        PSMTXIdentity(matrix);
        GXLoadPosMtxImm(matrix, GX_PNMTX0);
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GXSetNumTexGens(0);
        GXSetNumTevStages(1);
        GXSetTevDirect(GX_TEVSTAGE0);
        GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        GXSetNumChans(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetCoPlanar(GX_FALSE);
        GXSetClipMode(GX_CLIP_DISABLE);
        GXSetCullMode(GX_CULL_NONE);
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_COPY);
        GXSetColorUpdate(GX_FALSE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetDstAlpha(GX_TRUE, alpha);

        GXBegin(GX_TRIANGLES, GX_VTXFMT0, 6);
        GXPosition3f32(rPosition.x, rPosition.y, 0.0f);
        GXPosition3f32(width + rPosition.x, rPosition.y, 0.0f);
        GXPosition3f32(width + rPosition.x, height + rPosition.y, 0.0f);
        GXPosition3f32(rPosition.x, rPosition.y, 0.0f);
        GXPosition3f32(width + rPosition.x, height + rPosition.y, 0.0f);
        GXPosition3f32(rPosition.x, height + rPosition.y, 0.0f);
        GXEnd();

        GXSetDstAlpha(GX_FALSE, 0);
        GXSetClipMode(GX_CLIP_ENABLE);
#endif
    }

    void fillScreenSetup(const GXColor& rColor) {
        // 3DS: the fill colour, for the DirectDraw fill.
#ifdef __3DS__
        sDirectFillScreenColor = rColor;
#else
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_U16, 0);
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        TMtx34f mtxImm;
        mtxImm.identity();
        GXLoadPosMtxImm(mtxImm, GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);
        Mtx44 projMtx;
        C_MTXOrtho(projMtx, 0.0f, ::getScreenHeightInline(), 0.0f, MR::getFrameBufferWidth(), -1.0f, 1.0f);
        GXSetProjection(projMtx, GX_ORTHOGRAPHIC);
        GXSetNumChans(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GXSetZMode(GX_FALSE, GX_LEQUAL, GX_FALSE);
        GXSetZCompLoc(GX_FALSE);
        GXSetCullMode(GX_CULL_NONE);
        GXSetDither(GX_FALSE);
        GXSetNumTexGens(0);
        GXSetNumTevStages(1);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
        GXSetTevColor(GX_TEVREG0, rColor);
        GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_C0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
        GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_A0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
        GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
#endif
    }

    void fillScreenArea(const TVec2s& rMin, const TVec2s& rMax) {
#ifdef __3DS__
        Mtx44 projection;
        C_MTXOrtho(projection, 0.0f, ::getScreenHeightInline(), 0.0f, MR::getFrameBufferWidth(), -1.0f, 1.0f);
        const f32 x0 = rMin.x, y0 = rMin.y, x1 = rMax.x, y1 = rMax.y;
        const f32 points[6][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y0}, {x1, y1}, {x0, y1}};
        HostGfxVertex vertices[6] = {};
        for (u32 i = 0; i < 6; i++) {
            const f32 x = points[i][0], y = points[i][1], z = 0.0f;
            vertices[i].x = x * projection[0][0] + y * projection[0][1] + projection[0][3];
            vertices[i].y = x * projection[1][0] + y * projection[1][1] + projection[1][3];
            vertices[i].z = x * projection[2][0] + y * projection[2][1] + projection[2][3];
            vertices[i].w = x * projection[3][0] + y * projection[3][1] + projection[3][3];
            vertices[i].rgba[0] = sDirectFillScreenColor.r;
            vertices[i].rgba[1] = sDirectFillScreenColor.g;
            vertices[i].rgba[2] = sDirectFillScreenColor.b;
            vertices[i].rgba[3] = sDirectFillScreenColor.a;
        }
        HostGfxState state = {};
        state.stageCount = 1;
        state.stages[0].colorSrc[0] = state.stages[0].alphaSrc[0] = 0x00;  // primary vertex colour
        state.stages[0].colorFunc = state.stages[0].alphaFunc = 0;  // replace
        state.alphaTestFunc = 1;
        state.depthFunc = 5;
        state.writeMask = 0x0F;
        state.blendEquation = 0;
        state.blendSrc = sDirectFillScreenAlphaBlend ? 6 : 1;
        state.blendDst = sDirectFillScreenAlphaBlend ? 7 : 0;
        state.cull = 0;
        state.scissor[2] = 640;
        state.scissor[3] = 456;
        if (sDirectFillScreenStencil) {
            state.stencilEnable = 1;
            state.stencilFunc = GXToCitro3D::TEST_NOTEQUAL;
            state.stencilRef = 0;
            state.stencilMask = 0xFF;
            state.stencilWriteMask = 0xFF;
            state.stencilFail = state.stencilZFail = GXToCitro3D::STENCIL_KEEP;
            state.stencilZPass = GXToCitro3D::STENCIL_ZERO;
        }
        hostGfxDraw(vertices, 6, nullptr, 6, HOST_GFX_TRIANGLES, &state);
#else
        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        {
            u16 maxX = rMax.x;
            u16 minX = rMin.x;
            u16 minY = rMin.y;
            u16 maxY = rMax.y;

            GXPosition2u16(minX, minY);
            GXPosition2u16(maxX, minY);
            GXPosition2u16(maxX, maxY);
            GXPosition2u16(minX, maxY);
        }
        GXEnd();
#endif
    }

    void fillScreen(const GXColor& color) {
        fillScreenSetup(color);
        u16 width = getFrameBufferWidth();
        u16 height = getScreenHeight();
        fillScreenArea(TVec2s(0, 0), TVec2s(width, height));
    }

    // 3DS: a fill that blends by its alpha (LogoFader, WipeFade), through DirectDraw.
#ifdef __3DS__
    void fillScreenAlpha(const GXColor& color) {
#ifdef __3DS__
        sDirectFillScreenAlphaBlend = true;
        fillScreen(color);
        sDirectFillScreenAlphaBlend = false;
#else
        GXSetColorUpdate(GX_TRUE);
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
        fillScreen(color);
#endif
    }

#endif
    void setupShadowVolumeDraw() {
        // 3DS: the same drawing through DirectDraw (Citro3D) instead of GX.
#ifdef __3DS__
        TDDraw::setupDirect(0, 0, 0);
        TDDraw::setDirectDepthMode(true, GXToCitro3D::depthTest(GX_GEQUAL), false);
        TDDraw::setDirectAlphaTest(1, 0);
        TDDraw::setDirectCullMode(0);
#else
        GXSetTevDirect(GX_TEVSTAGE0);
        GXSetNumIndStages(0);
        GXSetNumTexGens(0);
        GXSetNumChans(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetChanMatColor(GX_COLOR0A0, Color8(0));
        GXSetNumTevStages(1);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_C0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
        GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_A0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
        GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GXSetZCompLoc(GX_FALSE);
        GXSetZMode(GX_TRUE, GX_GEQUAL, GX_FALSE);
        GXSetClipMode(GX_CLIP_ENABLE);
#endif
    }

    void drawSimpleModel(J3DModelData* pModelData) {
#ifdef __3DS__
        // The model's shapes with the loaded matrix and the TEV colour the caller set
        // (TDDraw::setDirectVertexColor), as the Wii's simpleDraw of every shape.
        TDDraw::drawDirectSimpleModel(pModelData);
#else
        J3DMaterial* pMaterial = pModelData->getJointNodePointer(0)->mMesh;
        j3dSys.setVtxPos(pModelData->mVertexData.mVtxPosArray);
        j3dSys.setVtxNrm(pModelData->mVertexData.mVtxNrmArray);
        j3dSys.setVtxCol(pModelData->mVertexData.mVtxColorArray[0]);

        while (pMaterial != nullptr) {
            pMaterial->mShape->simpleDraw();
            pMaterial = pMaterial->mNext;
        }
#endif
    }

    void fillSilhouetteColor() {
#ifdef __3DS__
        // The Wii captures the alpha and fills the screen with TEVREG0 (black, alpha 0x80) blended
        // SRCALPHA/INVSRCALPHA where the captured alpha is above 1 (the alpha compare), writing
        // alpha 0 there (destination alpha). The shadow volumes and Mario's silhouette count in
        // the stencil here: the same fill where it is not 0, resetting it.
        GXColor shade = {0, 0, 0, 0x80};
        sDirectFillScreenStencil = true;
        fillScreenAlpha(shade);
        sDirectFillScreenStencil = false;
#else
        MR::captureScreenAlpha(0);
        MR::loadScreenAlphaTexture(0, GX_TEXMAP0);
        Mtx44 projMtx;
        C_MTXOrtho(projMtx, 0.0f, MR::getScreenHeight(), 0.0f, MR::getScreenWidth(), -1.0f, 1.0f);
        GXSetProjection(projMtx, GX_ORTHOGRAPHIC);
        GXSetTevDirect(GX_TEVSTAGE0);
        GXSetNumIndStages(0);
        TMtx34f mtxImm;
        mtxImm.identity();
        GXLoadPosMtxImm(mtxImm, GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_U16, 0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_POS_XYZ, GX_F32, 0);
        GXSetNumChans(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetNumTexGens(1);
        GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
        GXSetNumTevStages(1);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
        GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_C0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
        GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevKColor(GX_KCOLOR0, Color8(0, 0, 0, 1));
        GXSetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
        GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_TEXA, GX_CA_KONST, GX_CA_A0, GX_CA_ZERO);
        GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_COMP_RGB8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GXSetZMode(GX_TRUE, GX_ALWAYS, GX_FALSE);
        GXSetZCompLoc(GX_FALSE);
        GXSetCullMode(GX_CULL_NONE);
        GXSetDither(GX_FALSE);
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetDstAlpha(GX_TRUE, 0);
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
        GXSetTevColor(GX_TEVREG0, Color8(0x00000080));

        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);
        {
            u16 width = MR::getScreenWidth();
            u16 height = MR::getScreenHeight();

            GXPosition2u16(0, 0);
            GXTexCoord2f32(0.0f, 0.0f);

            GXPosition2u16(width, 0);
            GXTexCoord2f32(1.0f, 0.0f);

            GXPosition2u16(0, height);
            GXTexCoord2f32(0.0f, 1.0f);

            GXPosition2u16(width, height);
            GXTexCoord2f32(1.0f, 1.0f);
        }
        GXEnd();

        GXSetDstAlpha(GX_FALSE, 0);
        GXSetAlphaUpdate(GX_FALSE);
#endif
    }

    const JUTTexture* getMarioShadowTex() {
        return mShadowTex;
    }

    const JUTTexture* getMarioShadowTexForLoad() {
        return mShadowTex;
    }

    const TVec3f& getMarioShadowVec() {
        return mShadowVec;
    }

    void setMarioShadowTex(const JUTTexture* pShadowTex) {
        mShadowTex = pShadowTex;
    }

    void setMarioShadowVec(const TVec3f& rVec) {
        mShadowVec = rVec;
    }

    void loadTexProjectionMtx(u32 id) {
        // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
        // (DETAILS.md, section 5).
#ifdef __3DS__
        (void)id;
        return;
#else
        TProj3f cameraProjection = MR::getCameraProjectionMtx();
        cameraProjection.mMtx[2][0] = 0.0f;
        cameraProjection.mMtx[2][1] = 0.0f;
        cameraProjection.mMtx[2][2] = -1.0f;
        cameraProjection.mMtx[2][3] = 0.0f;
        cameraProjection.mMtx[3][0] = 0.0f;
        cameraProjection.mMtx[3][1] = 0.0f;
        cameraProjection.mMtx[3][2] = 0.0f;
        cameraProjection.mMtx[3][3] = 1.0f;
        TPos3f matrix;
        MR::multMtx(matrix, MR::getCameraViewMtx(), cameraProjection);
        TPos3f mat;
        mat.identity();
        mat[0][0] = 0.5f;
        mat[0][2] = 0.5f;
        mat[1][1] = -0.5f;
        mat[1][2] = 0.5f;
        MR::multMtx(matrix, matrix, mat);
        GXLoadTexMtxImm(matrix, id, GX_MTX3x4);
#endif
    }

    void nonFilteredCapture(JUTTexture* pTexture, s16 width, s16 height) {
        // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
        // (DETAILS.md, section 5).
#ifdef __3DS__
        (void)pTexture;
        (void)width;
        (void)height;
        return;
#else
        GXRenderModeObj* pRenderModeObj = JUTVideo::getManager()->getRenderMode();
        GXSetCopyFilter(GX_FALSE, pRenderModeObj->sample_pattern, GX_FALSE, pRenderModeObj->vfilter);
        pTexture->capture(width, height, (GXTexFmt)pTexture->getFormat(), false, 0);
        GXSetCopyFilter(GX_FALSE, pRenderModeObj->sample_pattern, GX_TRUE, pRenderModeObj->vfilter);
#endif
    }

    void simpleDraw(J3DModel* pModel, J3DMaterial* pMaterial) {
        // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
        // (DETAILS.md, section 5).
#ifdef __3DS__
        (void)pModel;
        (void)pMaterial;
        return;
#else
        if (pMaterial != nullptr && pMaterial->mShape != nullptr && !pMaterial->mShape->checkFlag(1)) {
            J3DShape::resetVcdVatCache();
            J3DMatPacket* pMatPacket = pModel->getMatPacket(pMaterial->mIndex);
            J3DShapePacket* pShapePacket = pModel->getShapePacket(pMaterial->mShape->mIndex);
            j3dSys.setMatPacket(pMatPacket);
            pMatPacket->callDL();
            pShapePacket->getShape()->loadPreDrawSetting();

            if (pShapePacket->isValidDisplayListObj()) {
                pShapePacket->callDL();
            }

            pShapePacket->drawFast();
        }
#endif
    }

    void activateGameSceneDraw3D() {
        GameSceneFunction::activateDraw3D();
    }

    void deactivateGameSceneDraw3D() {
        GameSceneFunction::deactivateDraw3D();
    }
};  // namespace MR

J2DOrthoGraphSimple::J2DOrthoGraphSimple() : J2DOrthoGraph(0.0f, 0.0f, MR::getFrameBufferWidth(), ::getScreenHeightInline(), -30000.0f, 30000.0f) {
    TBox2f bounds(0.0f, 0.0f, MR::getScreenWidth(), MR::getScreenHeight());
    setOrtho(0.0f, 0.0f, bounds.getWidth(), bounds.getHeight(), -30000.0f, 30000.0f);
}

void J2DOrthoGraphSimple::setPort() {
    J2DOrthoGraph::setPort();
}
