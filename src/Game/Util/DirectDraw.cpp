#include "Game/Util/DirectDraw.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/ModelUtil.hpp"
#include "Game/Util/MtxUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include "math_types.hpp"
#include <JSystem/JUtility/JUTTexture.hpp>
#include <revolution/gx/GXEnum.h>
#include <revolution/gx/GXGeometry.h>
#include <revolution/gx/GXVert.h>
// 3DS: headers the 3DS code of this file uses (cmath, cstring).
#ifdef __3DS__
#include <cmath>
#include <cstring>
#endif
// 3DS: GXToCitro3D.hpp, GX values as 3DS GPU values; JUTTexture3DS.hpp, GPU textures for
// JUTTextures; NativeMaterial3DS.hpp, a model's material for immediate geometry; host.h, the port's
// host interface.
#ifdef __3DS__
#include "GXToCitro3D.hpp"
#include "JUTTexture3DS.hpp"
#include "NativeMaterial3DS.hpp"
#include "host.h"
#endif

namespace {
    static Mtx mViewMtx;
#ifdef __3DS__
    static Mtx44 sDirectProjection;
    static u8 sDirectBlendEquation;
    static u8 sDirectBlendSrc = 6;
    static u8 sDirectBlendDst = 7;
    static bool sDirectDepthTest;
    static bool sDirectDepthWrite;
    static u8 sDirectDepthFunc = 5;
    static u8 sDirectAlphaTestFunc = 1;
    static u8 sDirectAlphaRef;
    static u8 sDirectCullMode;
    static u8 sDirectWriteMask = 0x07;
    static u16 sDirectScissor[4] = {0, 0, 640, 456};
    static bool sDirectDstAlphaEnable;
    static u8 sDirectDstAlpha;
    static u8 sDirectViewMode = 2;
    // Stencil test of the draws (setDirectStencil): enable, func, ref, mask, write mask, fail,
    // zfail, zpass in the GPU's values; off means the host's ambient stencil.
    static u8 sDirectStencil[8];

    void copyDirectProjection(const Mtx44& projection) {
        std::memcpy(sDirectProjection, projection, sizeof(sDirectProjection));
    }

    inline void viewToClip(const TVec3f& view, HostGfxVertex& vertex, u32 color) {
        const f32 x = view.x, y = view.y, z = view.z;
        vertex.x = x * sDirectProjection[0][0] + y * sDirectProjection[0][1] + z * sDirectProjection[0][2] + sDirectProjection[0][3];
        vertex.y = x * sDirectProjection[1][0] + y * sDirectProjection[1][1] + z * sDirectProjection[1][2] + sDirectProjection[1][3];
        vertex.z = x * sDirectProjection[2][0] + y * sDirectProjection[2][1] + z * sDirectProjection[2][2] + sDirectProjection[2][3];
        // J2D uses an affine screen-space projection. Keep the homogeneous component
        // explicit here: Wii Mtx44 producers only guarantee the three transform rows,
        // while Citro3D still requires a valid clip-space w.
        vertex.w = x * sDirectProjection[3][0] + y * sDirectProjection[3][1] + z * sDirectProjection[3][2] + sDirectProjection[3][3];
        if (vertex.w == 0.0f && sDirectProjection[3][0] == 0.0f && sDirectProjection[3][1] == 0.0f &&
            sDirectProjection[3][2] == 0.0f && sDirectProjection[3][3] == 0.0f) {
            vertex.w = 1.0f;
        }
        vertex.rgba[0] = static_cast<u8>(color >> 24);
        vertex.rgba[1] = static_cast<u8>(color >> 16);
        vertex.rgba[2] = static_cast<u8>(color >> 8);
        vertex.rgba[3] = static_cast<u8>(color);
    }

    void transformToClip(const TVec3f& position, HostGfxVertex& vertex, u32 color) {
        TVec3f view;
        PSMTXMultVec(mViewMtx, &position, &view);
        viewToClip(view, vertex, color);
    }

    // The fragment state DirectDraw's setters chose (setupDirect, setDirect*), for every draw.
    void applyDirectState(HostGfxState& state) {
        state.alphaTestFunc = sDirectAlphaTestFunc;
        state.alphaRef = sDirectAlphaRef;
        state.depthTest = sDirectDepthTest;
        state.depthFunc = sDirectDepthFunc;
        // GX updates Z only through an enabled Z test (GXSetZMode); the 3DS writes it either way.
        state.writeMask = sDirectWriteMask | (sDirectDepthTest && sDirectDepthWrite ? 0x10 : 0);
        state.cull = sDirectCullMode;
        state.dstAlphaEnable = sDirectDstAlphaEnable;
        state.dstAlpha = sDirectDstAlpha;
        state.scissor[0] = sDirectScissor[0];
        state.scissor[1] = sDirectScissor[1];
        state.scissor[2] = sDirectScissor[2];
        state.scissor[3] = sDirectScissor[3];
        state.blendEquation = sDirectBlendEquation;
        state.blendSrc = sDirectBlendSrc;
        state.blendDst = sDirectBlendDst;
        state.stencilEnable = sDirectStencil[0];
        state.stencilFunc = sDirectStencil[1];
        state.stencilRef = sDirectStencil[2];
        state.stencilMask = sDirectStencil[3];
        state.stencilWriteMask = sDirectStencil[4];
        state.stencilFail = sDirectStencil[5];
        state.stencilZFail = sDirectStencil[6];
        state.stencilZPass = sDirectStencil[7];
    }

    void drawLineDirect(const TVec3f& start, const TVec3f& end, u32 color, f32 widthScale) {
        HostGfxVertex vertices[4] = {};
        transformToClip(start, vertices[0], color);
        transformToClip(start, vertices[1], color);
        transformToClip(end, vertices[2], color);
        transformToClip(end, vertices[3], color);

        const f32 startW = vertices[0].w, endW = vertices[2].w;
        if (startW == 0.0f || endW == 0.0f) return;
        const f32 dx = (vertices[2].x / endW - vertices[0].x / startW) * 320.0f;
        const f32 dy = (vertices[2].y / endW - vertices[0].y / startW) * 228.0f;
        const f32 length = std::sqrt(dx * dx + dy * dy);
        if (length == 0.0f) return;

        // GX line width 6 is one Wii framebuffer pixel. Expand it to a quad because
        // Citro3D exposes triangle primitives, not the GX line primitive.
        const f32 ox = (-dy / length) * (widthScale / 640.0f);
        const f32 oy = (dx / length) * (widthScale / 456.0f);
        vertices[0].x += ox * startW;
        vertices[0].y += oy * startW;
        vertices[1].x -= ox * startW;
        vertices[1].y -= oy * startW;
        vertices[2].x += ox * endW;
        vertices[2].y += oy * endW;
        vertices[3].x -= ox * endW;
        vertices[3].y -= oy * endW;

        HostGfxState state = {};
        state.stageCount = 1;
        state.stages[0].colorSrc[0] = state.stages[0].alphaSrc[0] = 0x00;  // interpolated vertex colour
        state.stages[0].colorFunc = state.stages[0].alphaFunc = 0;
        applyDirectState(state);
        hostGfxDraw(vertices, 4, nullptr, 4, HOST_GFX_STRIP, &state);
    }

    void drawPrimitiveDirect(const TVec3f* positions, const u32* colors, u32 count, int primitive) {
        if (positions == nullptr || colors == nullptr || count < 3 || count > 514) return;
        HostGfxVertex vertices[514];
        std::memset(vertices, 0, count * sizeof(HostGfxVertex));  // the vertices used only (the array is 22 KB)
        for (u32 i = 0; i < count; i++) transformToClip(positions[i], vertices[i], colors[i]);

        HostGfxState state = {};
        state.stageCount = 1;
        state.stages[0].colorSrc[0] = state.stages[0].alphaSrc[0] = 0x00;
        state.stages[0].colorFunc = state.stages[0].alphaFunc = 0;
        applyDirectState(state);
        hostGfxDraw(vertices, count, nullptr, count, primitive, &state);
    }

    void drawFanDirect(const TVec3f* positions, const u32* colors, u32 count) {
        drawPrimitiveDirect(positions, colors, count, HOST_GFX_FAN);
    }

    void drawStripDirect(const TVec3f* positions, const u32* colors, u32 count) {
        drawPrimitiveDirect(positions, colors, count, HOST_GFX_STRIP);
    }

    void drawTrianglesDirect(const TVec3f* positions, const u32* colors, u32 count) {
        drawPrimitiveDirect(positions, colors, count, HOST_GFX_TRIANGLES);
    }

    void drawTexturedQuadDirect(const TVec3f* positions, const f32 (*uv)[2], JUTTexture* texture, u8 alpha) {
        if (positions == nullptr || uv == nullptr || texture == nullptr) return;
        void* handle = rvl3DSGetJUTTexture(texture);
        if (handle == nullptr) return;
        const u8 order[6] = {0, 1, 2, 0, 2, 3};
        HostGfxVertex vertices[6] = {};
        for (u32 i = 0; i < 6; i++) {
            const u8 source = order[i];
            transformToClip(positions[source], vertices[i], 0xFFFFFF00u | alpha);
            // A screen capture is stored on its side: its own coordinate mapping.
            rvl3DSMapJUTTextureCoord(texture, uv[source][0], uv[source][1], vertices[i].s0, vertices[i].t0);
        }

        HostGfxState state = {};
        state.textures[0] = handle;
        state.stageCount = 1;
        state.stages[0].colorSrc[0] = 0x03;  // texture 0 replaces the vertex colour
        state.stages[0].colorFunc = 0;
        state.stages[0].alphaSrc[0] = 0x03;
        state.stages[0].alphaSrc[1] = 0x00;  // multiply texture alpha by vertex alpha
        state.stages[0].alphaFunc = 1;
        applyDirectState(state);
        hostGfxDraw(vertices, 6, nullptr, 6, HOST_GFX_TRIANGLES, &state);
    }
#endif

};  // namespace

namespace TDDraw {
    void DirectDraw_FORCE_MATCH_SDATA2() {
        1.0f;
        0.0f;
        0.5f;
        -1.0f;
        3.1415927f;
        2.0f;
        6.2831855f;
        0.001f;
        -30.0f;
        16777215.0f;
    }

    void setViewMtx(MtxPtr pMtx) {
        PSMTXCopy(pMtx, ::mViewMtx);
    }

    void loadViewMtx(MtxPtr pMtx) {
        // 3DS: the view matrix is kept for DirectDraw's own transform.
#ifdef __3DS__
        PSMTXCopy(pMtx, ::mViewMtx);
        return;
#else
        GXLoadPosMtxImm(pMtx, 0);
#endif
    }

    void setModelMtx(MtxPtr pMtx) {
        // 3DS: the Wii-style call goes to its DirectDraw (Citro3D) version.
#ifdef __3DS__
        setModelMtxDirect(pMtx);
        return;
#else
        MR::multMtx(::mViewMtx, pMtx, MR::getCameraViewMtx());
        GXLoadPosMtxImm(::mViewMtx, 0);
#endif
    }

    void resetViewMtx() {
        // 3DS: the Wii-style call goes to its DirectDraw (Citro3D) version.
#ifdef __3DS__
        resetViewMtxDirect();
        return;
#else
        PSMTXCopy(MR::getCameraViewMtx(), ::mViewMtx);
        GXLoadPosMtxImm(::mViewMtx, 0);
#endif
    }

    void close() {
        // 3DS: the Wii-style call goes to its DirectDraw (Citro3D) version.
#ifdef __3DS__
        closeDirect();
        return;
#else
        MR::loadProjectionMtx();
        MR::loadViewMtx();
        GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
#endif
    }

    void setup(u32 drawMode, u32 blendMode, u8 viewMode) {
        // 3DS: the Wii-style call goes to its DirectDraw (Citro3D) version.
#ifdef __3DS__
        setupDirect(drawMode, blendMode, viewMode);
        return;
#else
        GXColor color = {0, 0, 0, 0};

        GXSetCullMode(GX_CULL_NONE);
        GXSetClipMode(GX_CLIP_ENABLE);
        GXSetCoPlanar(0);
        GXSetZMode(0, GX_ALWAYS, 0);
        GXSetZCompLoc(0);
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(0);
        GXSetLineWidth(6, GX_TO_ZERO);
        GXSetChanMatColor(GX_COLOR0A0, color);
        GXSetChanAmbColor(GX_COLOR0A0, color);
        GXSetChanMatColor(GX_COLOR1A1, color);
        GXSetChanAmbColor(GX_COLOR1A1, color);
        GXSetChanCtrl(GX_COLOR0A0, 0, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
        GXSetChanCtrl(GX_COLOR1A1, 0, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
        GXSetNumChans(1);

        Mtx mtx;
        PSMTXIdentity(mtx);

        switch (viewMode) {
        case 0:
            PSMTXCopy(MR::getCameraViewMtx(), ::mViewMtx);
#ifdef __3DS__
            copyDirectProjection(*MR::getCameraProjectionMtx().toCMtx44());
#endif
            GXLoadPosMtxImm(::mViewMtx, 0);
            PSMTXCopy(::mViewMtx, mtx);
            break;
        case 1:
            MR::loadProjectionMtx();
#ifdef __3DS__
            copyDirectProjection(*MR::getCameraProjectionMtx().toCMtx44());
#endif
            GXLoadPosMtxImm(::mViewMtx, 0);
            break;
        case 2:
            cameraInit2D();
            PSMTXTrans(mtx, -MR::getScreenWidth() / 2.0f, MR::getScreenHeight() / 2.0f, 0.0f);
            mtx[1][1] = -1.0f;
#ifdef __3DS__
            PSMTXCopy(mtx, ::mViewMtx);
#endif
            GXLoadPosMtxImm(mtx, 0);
            break;
        }

        GXLoadNrmMtxImm(mtx, 0);
        GXSetCurrentMtx(0);
        GXSetTevDirect(GX_TEVSTAGE0);
        GXSetNumIndStages(0);

        switch (drawMode) {
        case 0:
            GXSetNumTexGens(0);
            GXSetNumTevStages(1);
            GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
            GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
            GXClearVtxDesc();
            GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
            GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
            break;
        case 1:
            GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
            GXSetNumTexGens(1);
            GXSetNumTevStages(1);
            GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
            GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
            GXClearVtxDesc();
            GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
            GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
            break;
        case 2:
            GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
            GXSetNumTexGens(1);
            GXSetNumTevStages(1);
            GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
            GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_TEXC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
            GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_A0, GX_CA_ZERO);
            GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 1, GX_TEVPREV);
            GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 1, GX_TEVPREV);
            GXClearVtxDesc();
            GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
            GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
            break;
        }

        switch (blendMode) {
        case 2:
            GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
            GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
            break;
        case 1:
            GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
            GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
            break;
        case 0:
            GXSetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
            GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
            break;
        }

        GXSetFog(GX_FOG_NONE, 0.0, 0.0, 0.0, 0.0, color);
        GXSetFogRangeAdj(GX_FALSE, 0, nullptr);
#endif
    }

#ifdef __3DS__
    void setupDirect(u32 drawMode, u32 blendMode, u8 viewMode) {
        (void)drawMode;
        flushDirectPrimitive();
        sDirectViewMode = viewMode;
        sDirectBlendEquation = 0;
        if (blendMode == 2) {
            sDirectBlendSrc = 6;
            sDirectBlendDst = 1;
        } else if (blendMode == 1) {
            sDirectBlendSrc = 6;
            sDirectBlendDst = 7;
        } else {
            sDirectBlendSrc = 1;
            sDirectBlendDst = 0;
        }
        sDirectDepthTest = false;
        sDirectDepthWrite = false;
        sDirectDepthFunc = 5;
        sDirectAlphaTestFunc = 1;
        sDirectAlphaRef = 0;
        sDirectCullMode = 0;
        sDirectWriteMask = 0x07;
        sDirectDstAlphaEnable = false;
        sDirectDstAlpha = 0;
        std::memset(sDirectStencil, 0, sizeof(sDirectStencil));

        Mtx view;
        PSMTXIdentity(view);
        switch (viewMode) {
        case 0:
            PSMTXCopy(MR::getCameraViewMtx(), ::mViewMtx);
            copyDirectProjection(*MR::getCameraProjectionMtx().toCMtx44());
            break;
        case 1:
            copyDirectProjection(*MR::getCameraProjectionMtx().toCMtx44());
            break;
        case 2: {
            const f32 width = MR::getScreenWidth() / 2.0f;
            const f32 height = MR::getScreenHeight() / 2.0f;
            Mtx44 projection;
            C_MTXOrtho(projection, height, -height, -width, width, 0.0f, -1.0f);
            copyDirectProjection(projection);
            PSMTXTrans(view, -MR::getScreenWidth() / 2.0f, MR::getScreenHeight() / 2.0f, 0.0f);
            view[1][1] = -1.0f;
            PSMTXCopy(view, ::mViewMtx);
            break;
        }
        default:
            // 3: the projection and view the caller loaded (J2D ortho graph, layout pane).
            break;
        }
    }

    void closeDirect() {
        flushDirectPrimitive();
        if (sDirectViewMode == 0 || sDirectViewMode == 1) {
            PSMTXCopy(MR::getCameraViewMtx(), ::mViewMtx);
            copyDirectProjection(*MR::getCameraProjectionMtx().toCMtx44());
        }
        setDirectDepthMode(false, 5, false);
        sDirectAlphaTestFunc = 1;
        sDirectAlphaRef = 0;
        std::memset(sDirectStencil, 0, sizeof(sDirectStencil));
        sDirectScissor[0] = 0;
        sDirectScissor[1] = 0;
        sDirectScissor[2] = 640;
        sDirectScissor[3] = 456;
    }

    void setDirectDepthMode(bool enabled, u8 function, bool write) {
        sDirectDepthTest = enabled;
        sDirectDepthFunc = function;
        sDirectDepthWrite = write;
    }

    void setDirectViewMtx(MtxPtr pMtx) {
        PSMTXCopy(pMtx, ::mViewMtx);
    }

    void setDirectProjectionMtx(const Mtx44& projection) {
        copyDirectProjection(projection);
    }

    void setDirectBlendState(u8 equation, u8 source, u8 destination) {
        sDirectBlendEquation = equation;
        sDirectBlendSrc = source;
        sDirectBlendDst = destination;
    }

    void setDirectAlphaTest(u8 function, u8 reference) {
        sDirectAlphaTestFunc = function;
        sDirectAlphaRef = reference;
    }

    void setDirectCullMode(u8 mode) {
        sDirectCullMode = mode;
    }

    void setDirectPixelWriteMask(u8 mask) {
        sDirectWriteMask = mask & 0x0F;
    }

    void setDirectScissor(u16 x, u16 y, u16 width, u16 height) {
        // GXSetScissor's x, y, width, height as the x0, y0, x1, y1 rectangle of HostGfxState.
        sDirectScissor[0] = x;
        sDirectScissor[1] = y;
        sDirectScissor[2] = x + width;
        sDirectScissor[3] = y + height;
    }

    void setDirectDestinationAlpha(bool enabled, u8 alpha) {
        sDirectDstAlphaEnable = enabled;
        sDirectDstAlpha = alpha;
    }

    void setDirectStencil(bool enabled, u8 function, u8 reference, u8 mask, u8 writeMask, u8 fail, u8 zfail, u8 zpass) {
        flushDirectPrimitive();
        sDirectStencil[0] = enabled;
        sDirectStencil[1] = function;
        sDirectStencil[2] = reference;
        sDirectStencil[3] = mask;
        sDirectStencil[4] = writeMask;
        sDirectStencil[5] = fail;
        sDirectStencil[6] = zfail;
        sDirectStencil[7] = zpass;
        if (!enabled) {
            std::memset(sDirectStencil, 0, sizeof(sDirectStencil));
        }
    }

    void setModelMtxDirect(MtxPtr pMtx) {
        MR::multMtx(::mViewMtx, pMtx, MR::getCameraViewMtx());
    }

    void resetViewMtxDirect() {
        PSMTXCopy(MR::getCameraViewMtx(), ::mViewMtx);
    }

    void drawDirectTriangles(const TVec3f* positions, const u32* colors, u32 count) {
        drawTrianglesDirect(positions, colors, count);
    }

    void drawDirectStrip(const TVec3f* positions, const u32* colors, u32 count) {
        drawStripDirect(positions, colors, count);
    }

    void drawTexturedTriangles(const TVec3f* positions, const TVec2f* texcoords, JUTTexture* texture, u32 count) {
        if (positions == nullptr || texcoords == nullptr || texture == nullptr || count < 3 || count > 192 || count % 3 != 0) return;
        void* handle = rvl3DSGetJUTTexture(texture);
        if (handle == nullptr) return;

        HostGfxVertex vertices[192];
        std::memset(vertices, 0, count * sizeof(HostGfxVertex));  // the vertices used only (the array is 8 KB)
        for (u32 i = 0; i < count; i++) {
            transformToClip(positions[i], vertices[i], 0xFFFFFFFFu);
            rvl3DSMapJUTTextureCoord(texture, texcoords[i].x, texcoords[i].y, vertices[i].s0, vertices[i].t0);
        }

        HostGfxState state = {};
        state.textures[0] = handle;
        state.stageCount = 1;
        state.stages[0].colorSrc[0] = 0x03;
        state.stages[0].colorFunc = 0;
        state.stages[0].alphaSrc[0] = 0x03;
        state.stages[0].alphaSrc[1] = 0x00;
        state.stages[0].alphaFunc = 1;
        applyDirectState(state);
        hostGfxDraw(vertices, count, nullptr, count, HOST_GFX_TRIANGLES, &state);
    }

    void drawColoredTexturedTriangles(const TVec3f* positions, const TVec2f* texcoords, const u32* colors, JUTTexture* texture,
                                      u32 count) {
        if (positions == nullptr || texcoords == nullptr || colors == nullptr || texture == nullptr || count < 3 || count > 192 ||
            count % 3 != 0) return;
        void* handle = rvl3DSGetJUTTexture(texture);
        if (handle == nullptr) return;

        HostGfxVertex vertices[192];
        std::memset(vertices, 0, count * sizeof(HostGfxVertex));  // the vertices used only (the array is 8 KB)
        for (u32 i = 0; i < count; i++) {
            transformToClip(positions[i], vertices[i], colors[i]);
            rvl3DSMapJUTTextureCoord(texture, texcoords[i].x, texcoords[i].y, vertices[i].s0, vertices[i].t0);
        }

        HostGfxState state = {};
        state.textures[0] = handle;
        state.stageCount = 1;
        state.stages[0].colorSrc[0] = 0x03;  // thread texture
        state.stages[0].colorSrc[1] = 0x00;  // per-vertex thread shading
        state.stages[0].colorFunc = 1;       // multiply
        state.stages[0].alphaSrc[0] = 0x03;
        state.stages[0].alphaSrc[1] = 0x00;
        state.stages[0].alphaFunc = 1;
        applyDirectState(state);
        hostGfxDraw(vertices, count, nullptr, count, HOST_GFX_TRIANGLES, &state);
    }

    void drawNativeTexturedTriangles(const TVec3f* positions, const TVec2f* texcoords, const u32* colors, const void* image, u16 width,
                                     u16 height, u32 format, const void* palette, u32 paletteFormat, u16 paletteEntries, u8 wrapS, u8 wrapT,
                                     u8 magFilter, u32 count, u32 mode, u32 mapMin, u32 mapMax) {
        if (positions == nullptr || texcoords == nullptr || image == nullptr || count < 3 || count > 192 || count % 3 != 0) return;
        static u32 sTextureFailures;
        void* handle = rvl3DSGetNativeTexture(image, width, height, format, palette, paletteFormat, paletteEntries, wrapS, wrapT, magFilter);
        if (handle == nullptr) {
            if (sTextureFailures < 8) {
                hostLog("native UI texture rejected image=%p size=%ux%u format=%lu palette=%p entries=%u\n", image, width, height,
                        static_cast< unsigned long >(format), palette, paletteEntries);
                sTextureFailures++;
            }
            return;
        }
        HostGfxVertex vertices[192];
        std::memset(vertices, 0, count * sizeof(HostGfxVertex));  // the vertices used only (the array is 8 KB)
        for (u32 i = 0; i < count; i++) {
            transformToClip(positions[i], vertices[i], colors != nullptr ? colors[i] : 0xFFFFFFFFu);
            vertices[i].s0 = texcoords[i].x;
            vertices[i].t0 = texcoords[i].y;
        }
        for (u32 i = 0; i < count; i++) {
            if (!std::isfinite(vertices[i].x) || !std::isfinite(vertices[i].y) || !std::isfinite(vertices[i].z) ||
                !std::isfinite(vertices[i].w) || vertices[i].w == 0.0f) {
                static u32 sInvalidQuadReports;
                if (sInvalidQuadReports < 4) {
                    hostLog("native UI quad rejected vertex=%lu clip=%g,%g,%g,%g\n", static_cast< unsigned long >(i), vertices[i].x,
                            vertices[i].y, vertices[i].z, vertices[i].w);
                    sInvalidQuadReports++;
                }
                return;
            }
        }
        HostGfxState state = {};
        state.textures[0] = handle;
        state.stageCount = 1;
        state.stages[0].colorSrc[0] = 0x03;
        state.stages[0].colorSrc[1] = colors != nullptr ? 0x00 : 0x03;
        state.stages[0].colorFunc = colors != nullptr;
        state.stages[0].alphaSrc[0] = 0x03;
        state.stages[0].alphaSrc[1] = colors != nullptr ? 0x00 : 0x03;
        state.stages[0].alphaFunc = colors != nullptr;
        enum { PRIMARY = 0x00, TEXTURE = 0x03, CONSTANT = 0x0E, PREVIOUS = 0x0F, REPLACE = 0, MODULATE = 1, MULTIPLY_ADD = 8 };
        if (mode == NATIVE_TEX_INTENSITY) {
            // CharWriter::SetupGXForI: colour RASC, alpha TEXA * RASA.
            state.stages[0].colorSrc[0] = PRIMARY;
            state.stages[0].colorFunc = REPLACE;
            state.stages[0].alphaSrc[0] = TEXTURE;
            state.stages[0].alphaSrc[1] = PRIMARY;
            state.stages[0].alphaFunc = MODULATE;
        } else if (mode == NATIVE_TEX_COLOR_MAPPING) {
            // CharWriter::SetupGXWithColorMapping: lerp(min, max, texture) times the vertex colour,
            // as texture * max, then (1 - texture) * min + that, then times the vertex colour.
            const u32 abgrMax = (mapMax >> 24) | ((mapMax >> 8) & 0xFF00) | ((mapMax << 8) & 0xFF0000) | (mapMax << 24);
            const u32 abgrMin = (mapMin >> 24) | ((mapMin >> 8) & 0xFF00) | ((mapMin << 8) & 0xFF0000) | (mapMin << 24);
            HostGfxStage* s = state.stages;
            s[0] = HostGfxStage();
            s[0].colorSrc[0] = s[0].alphaSrc[0] = TEXTURE;
            s[0].colorSrc[1] = s[0].alphaSrc[1] = CONSTANT;
            s[0].colorFunc = s[0].alphaFunc = MODULATE;
            s[0].constant = abgrMax;
            s[1].colorSrc[0] = s[1].alphaSrc[0] = TEXTURE;
            s[1].colorOp[0] = s[1].alphaOp[0] = 1;  // one minus
            s[1].colorSrc[1] = s[1].alphaSrc[1] = CONSTANT;
            s[1].colorSrc[2] = s[1].alphaSrc[2] = PREVIOUS;
            s[1].colorFunc = s[1].alphaFunc = MULTIPLY_ADD;
            s[1].constant = abgrMin;
            s[2].colorSrc[0] = s[2].alphaSrc[0] = PREVIOUS;
            s[2].colorSrc[1] = s[2].alphaSrc[1] = colors != nullptr ? PRIMARY : PREVIOUS;
            s[2].colorFunc = s[2].alphaFunc = colors != nullptr ? MODULATE : REPLACE;
            state.stageCount = 3;
        }
        applyDirectState(state);
        hostGfxDraw(vertices, count, nullptr, count, HOST_GFX_TRIANGLES, &state);
    }

    // Triangles with a ready combiner program and textures (rState's stages and textures), a
    // texture coordinate per unit and vertex, and DirectDraw's fragment state.
    void drawStateTriangles(const TVec3f* positions, const TVec2f (*texcoords)[3], const u32* colors, const HostGfxState& rState, u32 count) {
        if (positions == nullptr || count < 3 || count > 192 || count % 3 != 0) {
            return;
        }
        HostGfxVertex vertices[192];
        std::memset(vertices, 0, count * sizeof(HostGfxVertex));  // the vertices used only (the array is 8 KB)
        for (u32 i = 0; i < count; i++) {
            transformToClip(positions[i], vertices[i], colors != nullptr ? colors[i] : 0xFFFFFFFFu);
            if (texcoords != nullptr) {
                vertices[i].s0 = texcoords[i][0].x;
                vertices[i].t0 = texcoords[i][0].y;
                vertices[i].s1 = texcoords[i][1].x;
                vertices[i].t1 = texcoords[i][1].y;
                vertices[i].s2 = texcoords[i][2].x;
                vertices[i].t2 = texcoords[i][2].y;
            }
        }
        HostGfxState state = rState;
        applyDirectState(state);
        hostGfxDraw(vertices, count, nullptr, count, HOST_GFX_TRIANGLES, &state);
    }

    void drawTexturedTintedTriangles(const TVec3f* positions, const TVec2f* texcoords, JUTTexture* texture, u32 count, u32 tint) {
        if (positions == nullptr || texcoords == nullptr || texture == nullptr || count < 3 || count > 192 || count % 3 != 0) return;
        void* handle = rvl3DSGetJUTTexture(texture);
        if (handle == nullptr) return;

        HostGfxVertex vertices[192];
        std::memset(vertices, 0, count * sizeof(HostGfxVertex));  // the vertices used only (the array is 8 KB)
        for (u32 i = 0; i < count; i++) {
            transformToClip(positions[i], vertices[i], tint);
            rvl3DSMapJUTTextureCoord(texture, texcoords[i].x, texcoords[i].y, vertices[i].s0, vertices[i].t0);
        }

        HostGfxState state = {};
        state.textures[0] = handle;
        state.stageCount = 1;
        // PICA interpolate is s0*s2 + s1*(1-s2): white moves toward the vertex tint
        // as the GX mask alpha falls. This replaces the unsupported per-pixel TEV path.
        state.stages[0].colorSrc[0] = 0x0E;  // constant white
        state.stages[0].colorSrc[1] = 0x00;  // vertex tint
        state.stages[0].colorSrc[2] = 0x03;  // texture 0 alpha
        state.stages[0].colorOp[0] = 0x00;
        state.stages[0].colorOp[1] = 0x00;
        state.stages[0].colorOp[2] = 0x02;
        state.stages[0].colorFunc = 0x04;  // interpolate
        state.stages[0].constant = 0xFFFFFFFFul;
        state.stages[0].alphaSrc[0] = 0x03;
        state.stages[0].alphaOp[0] = 0x00;
        state.stages[0].alphaFunc = 0;
        state.stages[0].constant = 0xFFFFFFFFul;
        applyDirectState(state);
        hostGfxDraw(vertices, count, nullptr, count, HOST_GFX_TRIANGLES, &state);
    }

    // Triangles with the given combiner stages (each with its own constant, 0xAABBGGRR), one
    // vertex colour (0xRRGGBBAA) and texture 0: the effects translate their Wii TEV into stages.
    void drawStageTriangles(const TVec3f* positions, const TVec2f* texcoords, u32 count, JUTTexture* texture, u32 color,
                            const HostGfxStage* stages, u32 stageCount, const u32* colors, JUTTexture* texture1,
                            const TVec2f* texcoords1) {
        if (positions == nullptr || count < 3 || count > 192 || count % 3 != 0 || stages == nullptr || stageCount == 0 ||
            stageCount > HOST_GFX_STAGES) {
            return;
        }
        HostGfxState state = {};
        state.textures[0] = texture != nullptr ? rvl3DSGetJUTTexture(texture) : nullptr;
        if (texture != nullptr && state.textures[0] == nullptr) return;
        state.textures[1] = texture1 != nullptr ? rvl3DSGetJUTTexture(texture1) : nullptr;
        if (texture1 != nullptr && state.textures[1] == nullptr) return;
        HostGfxVertex vertices[192];
        std::memset(vertices, 0, count * sizeof(HostGfxVertex));  // the vertices used only (the array is 8 KB)
        for (u32 i = 0; i < count; i++) {
            transformToClip(positions[i], vertices[i], colors != nullptr ? colors[i] : color);
            if (texture != nullptr && texcoords != nullptr) {
                rvl3DSMapJUTTextureCoord(texture, texcoords[i].x, texcoords[i].y, vertices[i].s0, vertices[i].t0);
            }
            if (texture1 != nullptr && texcoords1 != nullptr) {
                rvl3DSMapJUTTextureCoord(texture1, texcoords1[i].x, texcoords1[i].y, vertices[i].s1, vertices[i].t1);
            }
        }
        state.stageCount = static_cast< u8 >(stageCount);
        for (u32 i = 0; i < stageCount; i++) {
            state.stages[i] = stages[i];
        }
        applyDirectState(state);
        hostGfxDraw(vertices, count, nullptr, count, HOST_GFX_TRIANGLES, &state);
    }

    void drawStageIndexed(const TVec3f* positions, const TVec2f* texcoords, u32 vertexCount, const u16* indices, u32 indexCount,
                          JUTTexture* texture, u32 color, const HostGfxStage* stages, u32 stageCount) {
        if (positions == nullptr || indices == nullptr || vertexCount == 0 || vertexCount > kStageIndexedVertices || indexCount < 3 ||
            indexCount % 3 != 0 || stages == nullptr || stageCount == 0 || stageCount > HOST_GFX_STAGES) {
            return;
        }
        HostGfxState state = {};
        state.textures[0] = texture != nullptr ? rvl3DSGetJUTTexture(texture) : nullptr;
        if (texture != nullptr && state.textures[0] == nullptr) return;
        // Static: about 22 KB, too much for the drawing thread's stack. Drawing is single-threaded.
        static HostGfxVertex sVertices[kStageIndexedVertices];
        std::memset(sVertices, 0, vertexCount * sizeof(HostGfxVertex));
        for (u32 i = 0; i < vertexCount; i++) {
            transformToClip(positions[i], sVertices[i], color);
            if (texture != nullptr && texcoords != nullptr) {
                rvl3DSMapJUTTextureCoord(texture, texcoords[i].x, texcoords[i].y, sVertices[i].s0, sVertices[i].t0);
            }
        }
        state.stageCount = static_cast< u8 >(stageCount);
        for (u32 i = 0; i < stageCount; i++) {
            state.stages[i] = stages[i];
        }
        applyDirectState(state);
        hostGfxDraw(sVertices, vertexCount, indices, indexCount, HOST_GFX_TRIANGLES, &state);
    }

    u32 stageConstant(u32 rgba) {
        return ((rgba >> 24) & 0xFF) | ((rgba >> 8) & 0xFF00) | ((rgba << 8) & 0xFF0000) | ((rgba & 0xFF) << 24);
    }

    void drawTexturedColorLerpTriangles(const TVec3f* positions, const TVec2f* texcoords, const u32* colors, JUTTexture* texture,
                                        u32 count, u32 color0, u32 color1) {
        if (positions == nullptr || texcoords == nullptr || colors == nullptr || texture == nullptr || count < 3 || count > 192 ||
            count % 3 != 0) return;
        void* handle = rvl3DSGetJUTTexture(texture);
        if (handle == nullptr) return;

        HostGfxVertex vertices[192];
        std::memset(vertices, 0, count * sizeof(HostGfxVertex));  // the vertices used only (the array is 8 KB)
        for (u32 i = 0; i < count; i++) {
            transformToClip(positions[i], vertices[i], colors[i]);
            rvl3DSMapJUTTextureCoord(texture, texcoords[i].x, texcoords[i].y, vertices[i].s0, vertices[i].t0);
        }

        HostGfxState state = {};
        state.textures[0] = handle;
        state.stageCount = 2;
        // GX TEV evaluates (1 - texture.rgb) * C0 + texture.rgb * C1.
        // PICA interpolates s0*s2 + s1*(1-s2), so stage 0 seeds C1 and
        // stage 1 interpolates from that value toward C0 using sampled texture RGB.
        state.stages[0].colorSrc[0] = 0x0E;
        state.stages[0].colorFunc = 0;
        state.stages[0].constant = color1;
        state.stages[0].alphaSrc[0] = 0x00;
        state.stages[0].alphaFunc = 0;
        state.stages[1].colorSrc[0] = 0x0F;
        state.stages[1].colorSrc[1] = 0x0E;
        state.stages[1].colorSrc[2] = 0x03;
        state.stages[1].colorOp[2] = 0x00;
        state.stages[1].colorFunc = 0x04;
        state.stages[1].constant = color0;
        state.stages[1].alphaSrc[0] = 0x0F;
        state.stages[1].alphaFunc = 0;
        applyDirectState(state);  // the caller's alpha compare (WhirlPoolAccelerator: GX_GREATER 0)
        hostGfxDraw(vertices, count, nullptr, count, HOST_GFX_TRIANGLES, &state);
    }

    void drawMaskedTexturedTriangles(const TVec3f* positions, const TVec2f* colorUVs, const TVec2f* maskUVs, JUTTexture* texture,
                                     JUTTexture* mask, u32 count) {
        if (positions == nullptr || colorUVs == nullptr || maskUVs == nullptr || texture == nullptr || mask == nullptr || count < 3 ||
            count > 192 || count % 3 != 0) return;
        void* colorHandle = rvl3DSGetJUTTexture(texture);
        void* maskHandle = rvl3DSGetJUTTexture(mask);
        if (colorHandle == nullptr || maskHandle == nullptr) return;

        HostGfxVertex vertices[192];
        std::memset(vertices, 0, count * sizeof(HostGfxVertex));  // the vertices used only (the array is 8 KB)
        for (u32 i = 0; i < count; i++) {
            transformToClip(positions[i], vertices[i], 0xFFFFFFFFu);
            rvl3DSMapJUTTextureCoord(texture, colorUVs[i].x, colorUVs[i].y, vertices[i].s0, vertices[i].t0);
            rvl3DSMapJUTTextureCoord(mask, maskUVs[i].x, maskUVs[i].y, vertices[i].s1, vertices[i].t1);
        }

        HostGfxState state = {};
        state.textures[0] = colorHandle;
        state.textures[1] = maskHandle;
        state.stageCount = 2;
        state.stages[0].colorSrc[0] = state.stages[0].alphaSrc[0] = 0x03;  // color texture 0
        state.stages[0].colorFunc = state.stages[0].alphaFunc = 0;  // replace
        state.stages[1].colorSrc[0] = 0x0F;  // pass previous stage color
        state.stages[1].colorFunc = 0;
        state.stages[1].alphaSrc[0] = 0x0F;  // previous alpha
        state.stages[1].alphaSrc[1] = 0x04;  // mask texture 1 alpha
        state.stages[1].alphaFunc = 1;  // multiply previous alpha by mask alpha
        applyDirectState(state);
        hostGfxDraw(vertices, count, nullptr, count, HOST_GFX_TRIANGLES, &state);
    }

    // drawMaskedTexturedTriangles as a triangle list of indexCount indices into vertexCount shared
    // vertices (at most 192): each vertex is transformed once (SpinDriverPathDrawer's ribbons).
    void drawMaskedTexturedIndexed(const TVec3f* positions, const TVec2f* colorUVs, const TVec2f* maskUVs, u32 vertexCount,
                                   const u16* indices, u32 indexCount, JUTTexture* texture, JUTTexture* mask) {
        if (positions == nullptr || colorUVs == nullptr || maskUVs == nullptr || indices == nullptr || texture == nullptr ||
            mask == nullptr || vertexCount == 0 || vertexCount > 192 || indexCount < 3 || indexCount % 3 != 0) return;
        void* colorHandle = rvl3DSGetJUTTexture(texture);
        void* maskHandle = rvl3DSGetJUTTexture(mask);
        if (colorHandle == nullptr || maskHandle == nullptr) return;

        HostGfxVertex vertices[192];
        std::memset(vertices, 0, vertexCount * sizeof(HostGfxVertex));
        for (u32 i = 0; i < vertexCount; i++) {
            transformToClip(positions[i], vertices[i], 0xFFFFFFFFu);
            rvl3DSMapJUTTextureCoord(texture, colorUVs[i].x, colorUVs[i].y, vertices[i].s0, vertices[i].t0);
            rvl3DSMapJUTTextureCoord(mask, maskUVs[i].x, maskUVs[i].y, vertices[i].s1, vertices[i].t1);
        }

        HostGfxState state = {};
        state.textures[0] = colorHandle;
        state.textures[1] = maskHandle;
        state.stageCount = 2;
        state.stages[0].colorSrc[0] = state.stages[0].alphaSrc[0] = 0x03;  // color texture 0
        state.stages[0].colorFunc = state.stages[0].alphaFunc = 0;  // replace
        state.stages[1].colorSrc[0] = 0x0F;  // pass previous stage color
        state.stages[1].colorFunc = 0;
        state.stages[1].alphaSrc[0] = 0x0F;  // previous alpha
        state.stages[1].alphaSrc[1] = 0x04;  // mask texture 1 alpha
        state.stages[1].alphaFunc = 1;  // multiply previous alpha by mask alpha
        applyDirectState(state);
        hostGfxDraw(vertices, vertexCount, indices, indexCount, HOST_GFX_TRIANGLES, &state);
    }

    void drawJ2DPictureTriangles(const TVec3f* positions, const TVec2f* texcoords, const u32* colors, JUTTexture* const* textures,
                                 const f32* ratios, u32 textureCount, u32 count) {
        if (positions == nullptr || texcoords == nullptr || colors == nullptr || textures == nullptr || ratios == nullptr || count < 3 ||
            count > 192 || count % 3 != 0 || textureCount == 0) {
            return;
        }

        if (textureCount > 3) return;

        HostGfxVertex vertices[192];
        std::memset(vertices, 0, count * sizeof(HostGfxVertex));  // the vertices used only (the array is 8 KB)
        void* handles[3] = {};
        for (u32 texture = 0; texture < textureCount; texture++) {
            handles[texture] = rvl3DSGetJUTTexture(textures[texture]);
            if (handles[texture] == nullptr) {
                return;
            }
        }

        for (u32 i = 0; i < count; i++) {
            transformToClip(positions[i], vertices[i], colors[i]);
            // A screen capture is stored on its side: each texture's own mapping.
            rvl3DSMapJUTTextureCoord(textures[0], texcoords[i].x, texcoords[i].y, vertices[i].s0, vertices[i].t0);
            rvl3DSMapJUTTextureCoord(textureCount > 1 ? textures[1] : nullptr, texcoords[i].x, texcoords[i].y, vertices[i].s1, vertices[i].t1);
            rvl3DSMapJUTTextureCoord(textureCount > 2 ? textures[2] : nullptr, texcoords[i].x, texcoords[i].y, vertices[i].s2, vertices[i].t2);
        }

        HostGfxState state = {};
        for (u32 texture = 0; texture < textureCount; texture++) {
            state.textures[texture] = handles[texture];
        }
        // As J2DPicture's GX stages: texture 0, then each further texture blended in by its ratio
        // (previous * (1 - k) + texture * k, colour and alpha), then the vertex colour multiplied
        // in. PICA's interpolate is source 0 * source 2 + source 1 * (1 - source 2).
        state.stageCount = textureCount + 1;
        state.stages[0].colorSrc[0] = 0x03;
        state.stages[0].colorFunc = 0;
        state.stages[0].alphaSrc[0] = 0x03;
        state.stages[0].alphaFunc = 0;
        for (u32 texture = 1; texture < textureCount; texture++) {
            HostGfxStage& stage = state.stages[texture];
            const u32 k = static_cast<u32>(ratios[texture] * 255.0f);
            stage.colorSrc[0] = stage.alphaSrc[0] = static_cast<u8>(0x03 + texture);
            stage.colorSrc[1] = stage.alphaSrc[1] = 0x0F;
            stage.colorSrc[2] = stage.alphaSrc[2] = 0x0E;
            stage.colorFunc = stage.alphaFunc = 4;
            stage.constant = k << 24 | k << 16 | k << 8 | k;
        }
        HostGfxStage& modulate = state.stages[textureCount];
        modulate.colorSrc[0] = modulate.alphaSrc[0] = 0x0F;
        modulate.colorSrc[1] = modulate.alphaSrc[1] = 0x00;
        modulate.colorFunc = modulate.alphaFunc = 1;
        applyDirectState(state);
        hostGfxDraw(vertices, count, nullptr, count, HOST_GFX_TRIANGLES, &state);
    }

    // JPA particles (platform/3ds/gx/particle.cpp): the combiners of particle_tev.inc, with the prm
    // colour as the vertex colour, the env colour (or white) as the constant, texture 0 the
    // particle's with (s0, t0) and texture 1 the second texture with (s1, t1).
    void particleVertexToClip(const TVec3f& position, HostGfxVertex& vertex, u32 color) {
        viewToClip(position, vertex, color);
        vertex.s0 = vertex.t0 = vertex.s1 = vertex.t1 = vertex.s2 = vertex.t2 = 0.0f;
    }

    void drawParticleTriangles(HostGfxVertex* vertices, const TVec2f* uv0, const TVec2f* uv1, u32 count, const u16* indices, u32 indexCount,
                               JUTTexture* texture0, JUTTexture* texture1, u32 constant, const HostGfxStage* stages, u32 stageCount,
                               u8 bufferAlpha) {
        if (vertices == nullptr || uv0 == nullptr || count < 3 || indexCount < 3 || stageCount == 0 || stageCount > HOST_GFX_STAGES) {
            return;
        }
        HostGfxState state = {};
        state.textures[0] = texture0 != nullptr ? rvl3DSGetJUTTexture(texture0) : nullptr;
        state.textures[1] = texture1 != nullptr ? rvl3DSGetJUTTexture(texture1) : nullptr;
        state.stageCount = static_cast< u8 >(stageCount);
        for (u32 i = 0; i < stageCount; i++) {
            state.stages[i] = stages[i];
            state.stages[i].constant = ((constant >> 24) & 0xFF) | ((constant >> 8) & 0xFF00) | ((constant << 8) & 0xFF0000) |
                                       ((constant & 0xFF) << 24);
        }
        state.bufferUpdateAlpha = bufferAlpha;
        for (u32 i = 0; i < count; i++) {
            rvl3DSMapJUTTextureCoord(texture0, uv0[i].x, uv0[i].y, vertices[i].s0, vertices[i].t0);
            if (uv1 != nullptr) {
                rvl3DSMapJUTTextureCoord(texture1, uv1[i].x, uv1[i].y, vertices[i].s1, vertices[i].t1);
            }
        }
        applyDirectState(state);
        hostGfxDraw(vertices, count, indices, indexCount, HOST_GFX_TRIANGLES, &state);
    }

    void drawParticleDistortedTriangles(HostGfxVertex* vertices, const TVec2f* uv0, const TVec2f* uv1, u32 count, const u16* indices, u32 indexCount,
                                        JUTTexture* texture, JUTTexture* distortion, const f32* mean, const f32* step, u32 constant, const HostGfxStage* stages,
                                        u32 stageCount) {
        if (vertices == nullptr || uv0 == nullptr || uv1 == nullptr || count < 3 || indexCount < 3 || stageCount == 0 ||
            stageCount > HOST_GFX_STAGES) {
            return;
        }
        HostGfxState state = {};
        state.textures[0] = state.textures[1] = rvl3DSGetJUTTexture(texture);
        state.textures[2] = rvl3DSGetJUTTexture(distortion);
        state.stageCount = static_cast< u8 >(stageCount);
        for (u32 i = 0; i < stageCount; i++) {
            state.stages[i] = stages[i];
            if (i >= 2) {
                state.stages[i].constant = ((constant >> 24) & 0xFF) | ((constant >> 8) & 0xFF00) | ((constant << 8) & 0xFF0000) |
                                           ((constant & 0xFF) << 24);
            }
        }
        for (u32 i = 0; i < count; i++) {
            rvl3DSMapJUTTextureCoord(texture, uv0[i].x + mean[0] - step[0], uv0[i].y + mean[1] - step[1], vertices[i].s0, vertices[i].t0);
            rvl3DSMapJUTTextureCoord(texture, uv0[i].x + mean[0] + step[0], uv0[i].y + mean[1] + step[1], vertices[i].s1, vertices[i].t1);
            rvl3DSMapJUTTextureCoord(distortion, uv1[i].x, uv1[i].y, vertices[i].s2, vertices[i].t2);
        }
        applyDirectState(state);
        hostGfxDraw(vertices, count, indices, indexCount, HOST_GFX_TRIANGLES, &state);
    }

    // View-space units per Wii frame buffer pixel (640 wide) at a view-space position, through the
    // DirectDraw view and projection: GX point sizes and line widths are in pixels.
    f32 directViewUnitsPerPixel(const TVec3f& position) {
        TVec3f view;
        PSMTXMultVec(mViewMtx, &position, &view);
        const f32 w = view.x * sDirectProjection[3][0] + view.y * sDirectProjection[3][1] + view.z * sDirectProjection[3][2] +
                      sDirectProjection[3][3];
        const f32 sx = sDirectProjection[0][0];
        return sx != 0.0f ? 2.0f * (w != 0.0f ? (w < 0.0f ? -w : w) : 1.0f) / (640.0f * (sx < 0.0f ? -sx : sx)) : 0.0f;
    }

    // Immediate geometry -----------------------------------------------------------------------

    namespace {
        const u32 kImmediateVertices = 1024;
        HostGfxVertex sImmediate[kImmediateVertices];
        u32 sImmediateCount, sImmediatePrimitive, sImmediateTexComponent;
        bool sImmediateOverflowReported;
        JUTTexture* sImmediateTexture;
        u32 sImmediateCombine = DIRECT_COMBINE_VERTEX;
        u32 sImmediateKonst = 0xFFFFFFFFu;
        // Inputs of the CPU texture coordinate generation, per vertex.
        f32 sImmediatePosition[kImmediateVertices][3];
        f32 sImmediateNormal[kImmediateVertices][3];
        struct ImmediateTexGen {
            bool enabled, projective;
            u32 source;
            Mtx matrix;
        };
        ImmediateTexGen sImmediateTexGen[3];
        HostGfxStage sImmediateStages[HOST_GFX_STAGES];
        u32 sImmediateStageCount;
        JUTTexture* sImmediateTextures[3];
        void* sImmediateHandles[3];
        u32 sImmediateVertexColor = 0xFFFFFFFFu;
    }  // namespace

    // A J3D material's combiners and Citro3D textures (NativeMaterial3DS.hpp).
    void setDirectStagesFromState(const HostGfxState& rState) {
        sImmediateStageCount = rState.stageCount < HOST_GFX_STAGES ? rState.stageCount : HOST_GFX_STAGES;
        for (u32 i = 0; i < sImmediateStageCount; i++) {
            sImmediateStages[i] = rState.stages[i];
        }
        for (u32 unit = 0; unit < 3; unit++) {
            sImmediateHandles[unit] = rState.textures[unit];
        }
        sImmediateCombine = DIRECT_COMBINE_STATE;
    }

    // The colour of immediate vertices sent without one (GX's channel colour for them).
    void setDirectVertexColor(u32 color) {
        sImmediateVertexColor = color;
    }

    // MR::drawSimpleModel: the model's shapes in the colour setDirectVertexColor chose, through the
    // view matrix loaded last (loadDirectPosMtx, setDirectViewMtx), with the DirectDraw state.
    void drawDirectSimpleModel(J3DModelData* pModelData) {
        flushDirectPrimitive();
        HostGfxState state = {};
        state.stageCount = 1;
        state.stages[0].colorSrc[0] = state.stages[0].alphaSrc[0] = 0x00;  // the material colour
        state.stages[0].colorFunc = state.stages[0].alphaFunc = 0;
        applyDirectState(state);
        rvlNativeDrawSimpleModel(pModelData, ::mViewMtx, state, sImmediateVertexColor);
    }

    // drawDirectSimpleModel with a view matrix for each position matrix slot (null: the view).
    void drawDirectSimpleModelSlots(J3DModelData* pModelData, const f32 (*const* pSlots)[4]) {
        flushDirectPrimitive();
        HostGfxState state = {};
        state.stageCount = 1;
        state.stages[0].colorSrc[0] = state.stages[0].alphaSrc[0] = 0x00;  // the material colour
        state.stages[0].colorFunc = state.stages[0].alphaFunc = 0;
        applyDirectState(state);
        rvlNativeDrawSimpleModelSlots(pModelData, ::mViewMtx, pSlots, state, sImmediateVertexColor);
    }

    void directNormal(f32 x, f32 y, f32 z) {
        if (sImmediateCount != 0) {
            f32* n = sImmediateNormal[sImmediateCount - 1];
            n[0] = x;
            n[1] = y;
            n[2] = z;
        }
    }

    void setDirectTexGen(u32 unit, u32 source, const f32 (*pMtx)[4], bool projective) {
        if (unit >= 3) {
            return;
        }
        ImmediateTexGen& g = sImmediateTexGen[unit];
        g.enabled = pMtx != nullptr;
        g.projective = projective;
        g.source = source;
        if (pMtx != nullptr) {
            PSMTXCopy(pMtx, g.matrix);
        }
    }

    void setDirectStages(const HostGfxStage* pStages, u32 count, JUTTexture* pTexture0, JUTTexture* pTexture1, JUTTexture* pTexture2) {
        sImmediateStageCount = count < HOST_GFX_STAGES ? count : HOST_GFX_STAGES;
        for (u32 i = 0; i < sImmediateStageCount; i++) {
            sImmediateStages[i] = pStages[i];
        }
        sImmediateTextures[0] = pTexture0;
        sImmediateTextures[1] = pTexture1;
        sImmediateTextures[2] = pTexture2;
        sImmediateCombine = DIRECT_COMBINE_STAGES;
    }

    // Constant (0xAABBGGRR) of one of the setDirectStages stages, as GXSetTevColor/GXSetTevKColor.
    void setDirectStageConstant(u32 stage, u32 constant) {
        if (stage < HOST_GFX_STAGES) {
            sImmediateStages[stage].constant = constant;
        }
    }

    void setDirectTexture(JUTTexture* pTexture) {
        sImmediateTexture = pTexture;
    }

    void setDirectCombine(u32 mode, u32 konst) {
        sImmediateCombine = mode;
        sImmediateKonst = konst;
        sImmediateVertexColor = 0xFFFFFFFFu;
        for (u32 unit = 0; unit < 3; unit++) {
            sImmediateTexGen[unit].enabled = false;
        }
    }

    // GXLoadPosMtxImm for the current matrix (GX_PNMTX0): the immediate vertices' view matrix.
    void loadDirectPosMtx(const f32 (*pMtx)[4], u32 id) {
        if (id == 0) {
            PSMTXCopy(pMtx, ::mViewMtx);
        }
    }

    void beginDirectPrimitive(u32 primitive, u32 count) {
        (void)count;
        if (sImmediateCount != 0) {
            endDirectPrimitive();  // GX needs no GXEnd: the previous primitive is complete
        }
        sImmediatePrimitive = primitive;
        sImmediateCount = 0;
    }

    void flushDirectPrimitive() {
        if (sImmediateCount != 0) {
            endDirectPrimitive();
        }
    }

    void directPosition(f32 x, f32 y, f32 z) {
        if (sImmediateCount == kImmediateVertices) {
            if (!sImmediateOverflowReported) {
                hostLog("DirectDraw: immediate primitive over %lu vertices, the rest dropped\n", (unsigned long)kImmediateVertices);
                sImmediateOverflowReported = true;
            }
            return;
        }
        sImmediatePosition[sImmediateCount][0] = x;
        sImmediatePosition[sImmediateCount][1] = y;
        sImmediatePosition[sImmediateCount][2] = z;
        sImmediateNormal[sImmediateCount][0] = sImmediateNormal[sImmediateCount][1] = 0.0f;
        sImmediateNormal[sImmediateCount][2] = 1.0f;
        HostGfxVertex& v = sImmediate[sImmediateCount++];
        v = HostGfxVertex();
        transformToClip(TVec3f(x, y, z), v, sImmediateVertexColor);
        sImmediateTexComponent = 0;
    }

    // GXCmd1f32 after a position: texture coordinate s, then t.
    void directTexComponent(f32 value) {
        if (sImmediateCount != 0) {
            HostGfxVertex& v = sImmediate[sImmediateCount - 1];
            (sImmediateTexComponent++ == 0 ? v.s0 : v.t0) = value;
        }
    }

    void directColor(u32 color) {
        if (sImmediateCount != 0) {
            HostGfxVertex& v = sImmediate[sImmediateCount - 1];
            v.rgba[0] = static_cast< u8 >(color >> 24);
            v.rgba[1] = static_cast< u8 >(color >> 16);
            v.rgba[2] = static_cast< u8 >(color >> 8);
            v.rgba[3] = static_cast< u8 >(color);
        }
    }

    void directTexCoord(f32 s, f32 t) {
        if (sImmediateCount != 0) {
            sImmediate[sImmediateCount - 1].s0 = s;
            sImmediate[sImmediateCount - 1].t0 = t;
        }
    }

    void endDirectPrimitive() {
        const u32 count = sImmediateCount;
        sImmediateCount = 0;
        for (u32 unit = 0; unit < 3; unit++) {
            const ImmediateTexGen& g = sImmediateTexGen[unit];
            if (!g.enabled) {
                continue;
            }
            for (u32 k = 0; k < count; k++) {
                HostGfxVertex& v = sImmediate[k];
                f32 in[4] = {0.0f, 0.0f, 1.0f, 1.0f};
                if (g.source == DIRECT_TEXGEN_TEX0) {
                    in[0] = v.s0;
                    in[1] = v.t0;
                } else {
                    const f32* p = g.source == DIRECT_TEXGEN_NRM ? sImmediateNormal[k] : sImmediatePosition[k];
                    in[0] = p[0];
                    in[1] = p[1];
                    in[2] = p[2];
                }
                f32 out[3];
                for (u32 r = 0; r < 3; r++) {
                    out[r] = g.matrix[r][0] * in[0] + g.matrix[r][1] * in[1] + g.matrix[r][2] * in[2] + g.matrix[r][3] * in[3];
                }
                if (g.projective && out[2] != 0.0f) {
                    out[0] /= out[2];
                    out[1] /= out[2];
                }
                f32* st = unit == 0 ? &v.s0 : unit == 1 ? &v.s1 : &v.s2;
                st[0] = out[0];
                st[1] = out[1];
            }
        }
        // A screen capture is stored on its side: the coordinates go through its mapping.
        for (u32 unit = 0; unit < 3; unit++) {
            const JUTTexture* texture = sImmediateCombine == DIRECT_COMBINE_STAGES ? sImmediateTextures[unit]
                                        : sImmediateCombine == DIRECT_COMBINE_STATE || sImmediateCombine == DIRECT_COMBINE_VERTEX || unit != 0
                                            ? nullptr
                                            : sImmediateTexture;
            if (texture == nullptr || !rvl3DSIsEfbCopy(texture)) {
                continue;
            }
            for (u32 k = 0; k < count; k++) {
                HostGfxVertex& v = sImmediate[k];
                f32* st = unit == 0 ? &v.s0 : unit == 1 ? &v.s1 : &v.s2;
                f32 u, w;
                rvl3DSMapJUTTextureCoord(texture, st[0], st[1], u, w);
                st[0] = u;
                st[1] = w;
            }
        }
        enum { PRIMARY = 0x00, TEXTURE = 0x03, CONSTANT = 0x0E, REPLACE = 0, MODULATE = 1 };
        HostGfxState state = {};
        state.stageCount = 1;
        HostGfxStage& stage = state.stages[0];
        if (sImmediateCombine == DIRECT_COMBINE_STATE) {
            for (u32 unit = 0; unit < 3; unit++) {
                state.textures[unit] = sImmediateHandles[unit];
            }
            state.stageCount = sImmediateStageCount;
            for (u32 i = 0; i < sImmediateStageCount; i++) {
                state.stages[i] = sImmediateStages[i];
            }
        } else if (sImmediateCombine == DIRECT_COMBINE_STAGES) {
            for (u32 unit = 0; unit < 3; unit++) {
                if (sImmediateTextures[unit] != nullptr) {
                    state.textures[unit] = rvl3DSGetJUTTexture(sImmediateTextures[unit]);
                    if (state.textures[unit] == nullptr) {
                        return;
                    }
                }
            }
            state.stageCount = sImmediateStageCount;
            for (u32 i = 0; i < sImmediateStageCount; i++) {
                state.stages[i] = sImmediateStages[i];
            }
        } else if (sImmediateCombine != DIRECT_COMBINE_VERTEX) {
            state.textures[0] = sImmediateTexture != nullptr ? rvl3DSGetJUTTexture(sImmediateTexture) : nullptr;
            if (state.textures[0] == nullptr) {
                return;
            }
        }
        switch (sImmediateCombine) {
        case DIRECT_COMBINE_TEXTURE_X_VERTEX:
            stage.colorSrc[0] = stage.alphaSrc[0] = TEXTURE;
            stage.colorSrc[1] = stage.alphaSrc[1] = PRIMARY;
            stage.colorFunc = stage.alphaFunc = MODULATE;
            break;
        case DIRECT_COMBINE_TEXTURE_X_VERTEX_TEX_ALPHA:
            stage.colorSrc[0] = TEXTURE;
            stage.colorSrc[1] = PRIMARY;
            stage.colorFunc = MODULATE;
            stage.alphaSrc[0] = TEXTURE;
            stage.alphaFunc = REPLACE;
            break;
        case DIRECT_COMBINE_TEXTURE_X_VERTEX_KONST_ALPHA:
            stage.colorSrc[0] = TEXTURE;
            stage.colorSrc[1] = PRIMARY;
            stage.colorFunc = MODULATE;
            stage.alphaSrc[0] = CONSTANT;
            stage.alphaFunc = REPLACE;
            stage.constant = (sImmediateKonst & 0xFF) << 24;
            break;
        case DIRECT_COMBINE_TEXTURE:
            stage.colorSrc[0] = stage.alphaSrc[0] = TEXTURE;
            stage.colorFunc = stage.alphaFunc = REPLACE;
            break;
        case DIRECT_COMBINE_STAGES:
        case DIRECT_COMBINE_STATE:
            break;
        default:
            stage.colorSrc[0] = stage.alphaSrc[0] = PRIMARY;
            stage.colorFunc = stage.alphaFunc = REPLACE;
            break;
        }
        applyDirectState(state);
        switch (sImmediatePrimitive) {
        case GX_TRIANGLES:
            hostGfxDraw(sImmediate, count, nullptr, count - count % 3, HOST_GFX_TRIANGLES, &state);
            break;
        case GX_TRIANGLESTRIP:
            hostGfxDraw(sImmediate, count, nullptr, count, HOST_GFX_STRIP, &state);
            break;
        case GX_TRIANGLEFAN:
            hostGfxDraw(sImmediate, count, nullptr, count, HOST_GFX_FAN, &state);
            break;
        case GX_QUADS: {
            static u16 sQuadIndices[kImmediateVertices / 4 * 6];
            u32 n = 0;
            for (u32 q = 0; q + 3 < count; q += 4) {
                static const u16 kOrder[6] = {0, 1, 2, 0, 2, 3};
                for (u32 k = 0; k < 6; k++) {
                    sQuadIndices[n++] = static_cast< u16 >(q + kOrder[k]);
                }
            }
            hostGfxDraw(sImmediate, count, sQuadIndices, n, HOST_GFX_TRIANGLES, &state);
            break;
        }
        default:
            // Lines and points have no 3DS primitive; the callers that need lines draw them with
            // drawLine (quads one framebuffer pixel wide).
            break;
        }
    }

    void setupDirectImmediate(JUTTexture* pTexture, u32 combine, u32 konst, u32 blendType, u32 blendSource, u32 blendDestination,
                              u32 alphaCompare, u8 alphaReference, bool zEnable, u32 zCompare, bool zUpdate, u32 cullMode) {
        setupDirect(1, 1, 0);
        setDirectTexture(pTexture);
        setDirectCombine(combine, konst);
        u8 equation, source, destination;
        int logic;
        GXToCitro3D::blendMode(blendType, blendSource, blendDestination, GX_LO_NOOP, equation, source, destination, logic);
        setDirectBlendState(equation, source, destination);
        setDirectAlphaTest(GXToCitro3D::alphaTest(alphaCompare), alphaReference);
        setDirectDepthMode(zEnable, GXToCitro3D::depthTest(zCompare), zUpdate);
        bool cullAll;
        setDirectCullMode(GXToCitro3D::cullMode(cullMode, cullAll));
    }

    void drawNativeColoredTriangles(const TVec3f* positions, const u32* colors, u32 count) {
        if (positions == nullptr || colors == nullptr || count < 3 || count > 192 || count % 3 != 0) return;
        HostGfxVertex vertices[192];
        std::memset(vertices, 0, count * sizeof(HostGfxVertex));  // the vertices used only (the array is 8 KB)
        for (u32 i = 0; i < count; i++) {
            transformToClip(positions[i], vertices[i], colors[i]);
        }

        HostGfxState state = {};
        state.stageCount = 1;
        state.stages[0].colorSrc[0] = 0x00;
        state.stages[0].colorFunc = 0;
        state.stages[0].alphaSrc[0] = 0x00;
        state.stages[0].alphaFunc = 0;
        applyDirectState(state);
        hostGfxDraw(vertices, count, nullptr, count, HOST_GFX_TRIANGLES, &state);
    }
#endif
    void sendPoint(const TVec3f& rPoint, u32 color) NO_INLINE {
        // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
        // (DETAILS.md, section 5).
#ifdef __3DS__
        (void)rPoint;
        (void)color;
#else
        GXPosition3f32(rPoint.x, rPoint.y, rPoint.z);
        GXColor1u32(color);
#endif
    }

    void sendPoint(const TVec3f& rVec) NO_INLINE {
        // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
        // (DETAILS.md, section 5).
#ifdef __3DS__
        (void)rVec;
#else
        GXPosition3f32(rVec.x, rVec.y, rVec.z);
#endif
    }

#ifdef __3DS__
    void drawLine(const TVec3f& rStart, const TVec3f& rEnd, u32 color) {
#ifdef __3DS__
        drawLineDirect(rStart, rEnd, color, 1.0f);
#else
        GXBegin(GX_LINES, GX_VTXFMT0, 2);
        {
            GXPosition3f32(rStart.x, rStart.y, rStart.z);
            GXColor1u32(color);
            GXPosition3f32(rEnd.x, rEnd.y, rEnd.z);
            GXColor1u32(color);
        }

        GXEnd();
#endif
    }

    void drawLineWidth(const TVec3f& rStart, const TVec3f& rEnd, u32 color, u8 gxWidth) {
#ifdef __3DS__
        // GX line width is specified in sixths of a Wii framebuffer pixel.
        drawLineDirect(rStart, rEnd, color, static_cast<f32>(gxWidth) / 6.0f);
#else
        GXSetLineWidth(gxWidth, GX_TO_ZERO);
        drawLine(rStart, rEnd, color);
#endif
    }

    void drawCircle(const TVec3f& rCenter, const TVec3f& rNormal, f32 radius, u32 color, u32 segments) {
        TVec3f axis;

        if (rNormal.x != 0.0f) {
            axis.set< f32 >(rNormal.y, -rNormal.x, 0.0f);
        } else {
            axis.set< f32 >(0.0f, -rNormal.z, rNormal.y);
        }
#else
    void drawLine(const TVec3f& rStart, const TVec3f& rEnd, u32 color) {
        GXBegin(GX_LINES, GX_VTXFMT0, 2);
        {
            GXPosition3f32(rStart.x, rStart.y, rStart.z);
            GXColor1u32(color);
            GXPosition3f32(rEnd.x, rEnd.y, rEnd.z);
            GXColor1u32(color);
        }

        GXEnd();
    }

    void drawCircle(const TVec3f& rCenter, const TVec3f& rNormal, f32 radius, u32 color, u32 segments) {
        TVec3f axis;

        if (rNormal.x != 0.0f) {
            axis.set< f32 >(rNormal.y, -rNormal.x, 0.0f);
        } else {
            axis.set< f32 >(0.0f, -rNormal.z, rNormal.y);
        }
#endif

        MR::normalizeOrZero(&axis);
        drawCircle(rCenter, rNormal, axis, radius, color, segments);
    }

    void drawCircle(const TVec3f& rCenter, const TVec3f& rNormal, const TVec3f& rAxis, f32 radius, u32 color, u32 segments) {
        // 3DS: the same drawing through DirectDraw (Citro3D) instead of GX.
#ifdef __3DS__
        if (segments == 0 || segments > 256) return;
        TVec3f axis(rAxis);
        Mtx rotation;
        PSMTXRotAxisRad(rotation, rNormal, 6.2831855f / segments);
        TVec3f previous(rCenter + axis * radius);
        for (u32 i = 0; i < segments; i++) {
            PSMTXMultVec(rotation, &axis, &axis);
            TVec3f next(rCenter + axis * radius);
            drawLineDirect(previous, next, color, 1.0f);
            previous = next;
        }
#else
        TVec3f axis(rAxis);
        Mtx rotation;
        PSMTXRotAxisRad(rotation, rNormal, 6.2831855f / segments);

        GXBegin(GX_LINESTRIP, GX_VTXFMT0, segments + 1);
        for (u32 i = 0; i <= segments; i++) {
            TVec3f point(rCenter + axis * radius);
            GXPosition3f32(point.x, point.y, point.z);
            GXColor1u32(color);
            PSMTXMultVec(rotation, axis, axis);
        }

        GXEnd();
#endif
    }

    void drawFillCircle(const TVec3f& rCenter, f32 radius, u32 centerColor, u32 edgeColor, u32 segments) {
        // 3DS: the same drawing through DirectDraw (Citro3D) instead of GX.
#ifdef __3DS__
        if (segments == 0 || segments > 256) return;
        TVec3f points[258];
        u32 colors[258];
        points[0] = rCenter;
        colors[0] = centerColor;
        for (u32 i = 0; i <= segments; i++) {
            const f32 angle = 2.0f * ((static_cast<f32>(i) / segments) * JGeometry::TUtil<f32>::PI());
            points[i + 1].set< f32 >(rCenter.x - radius * MR::cos(angle), rCenter.y + radius * MR::sin(angle), rCenter.z);
            colors[i + 1] = edgeColor;
        }
        drawFanDirect(points, colors, segments + 2);
#else
        TVec3f point;
        point.z = rCenter.z;
        GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, segments + 2);
        GXPosition3f32(rCenter.x, rCenter.y, rCenter.z);
        GXColor1u32(centerColor);

        for (u32 i = 0; i <= segments; i++) {
            point.x = rCenter.x - radius * MR::cos(2.0f * ((static_cast< f32 >(i) / segments) * JGeometry::TUtil< f32 >::PI()));
            point.y = rCenter.y + radius * MR::sin(2.0f * ((static_cast< f32 >(i) / segments) * JGeometry::TUtil< f32 >::PI()));
            GXPosition3f32(point.x, point.y, point.z);
            GXColor1u32(edgeColor);
        }

        GXEnd();
#endif
    }

    void drawFillCircle(const TVec3f& rCenter, const TVec3f& rNormal, f32 radius, u32 color, u32 segments) {
        // 3DS: the vertices go into an array of at most 256 segments.
#ifdef __3DS__
        if (segments == 0 || segments > 256) return;
#endif
        TVec3f axis;
        if (rNormal.x != 0.0f) {
            axis.set< f32 >(rNormal.y, -rNormal.x, 0.0f);
        } else {
            axis.set< f32 >(0.0f, -rNormal.z, rNormal.y);
        }

        MR::normalizeOrZero(&axis);
        Mtx rotation;
        PSMTXRotAxisRad(rotation, rNormal, (6.2831855f / segments));

        // 3DS: the same drawing through DirectDraw (Citro3D) instead of GX.
#ifdef __3DS__
        if (segments == 0 || segments > 256) return;
        TVec3f points[258];
        u32 colors[258];
        points[0] = rCenter;
        colors[0] = color;
        for (u32 i = 0; i <= segments; i++) {
            points[i + 1] = rCenter + axis * radius;
            colors[i + 1] = color;
            PSMTXMultVec(rotation, &axis, &axis);
        }
        drawFanDirect(points, colors, segments + 2);
#else
        GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, segments + 2);
        {
            GXPosition3f32(rCenter.x, rCenter.y, rCenter.z);
            GXColor1u32(color);

            for (u32 i = 0; i <= segments; i++) {
                TVec3f point(rCenter + axis * radius);
                PSMTXMultVec(rotation, axis, axis);
                GXPosition3f32(point.x, point.y, point.z);
                GXColor1u32(color);
            }
        }

        GXEnd();
#endif
    }

    void drawFillFan(const TVec3f& rCenter, const TVec3f& rNormal, const TVec3f& rAxis, u32 color, f32 startAngle, f32 endAngle, u32 segments) {
        // 3DS: the vertices go into an array of at most 256 segments.
#ifdef __3DS__
        if (segments == 0 || segments > 256) return;
#endif
        Mtx startRotation;
        PSMTXRotAxisRad(startRotation, rNormal, startAngle);
        Mtx stepRotation;
        PSMTXRotAxisRad(stepRotation, rNormal, (endAngle - startAngle) / (segments));
        TVec3f axis;
        PSMTXMultVec(startRotation, rAxis, axis);

        // 3DS: the same drawing through DirectDraw (Citro3D) instead of GX.
#ifdef __3DS__
        if (segments == 0 || segments > 256) return;
        TVec3f points[258];
        u32 colors[258];
        points[0] = rCenter;
        colors[0] = color;
        for (u32 i = 0; i <= segments; i++) {
            points[i + 1] = rCenter + axis;
            colors[i + 1] = color;
            PSMTXMultVec(stepRotation, &axis, &axis);
        }
        drawFanDirect(points, colors, segments + 2);
#else
        GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, segments + 2);
        {
            GXPosition3f32(rCenter.x, rCenter.y, rCenter.z);
            GXColor1u32(color);

            for (u32 i = 0; i <= segments; i++) {
                TVec3f point(axis);
                point += rCenter;
                GXPosition3f32(point.x, point.y, point.z);
                GXColor1u32(color);
                PSMTXMultVec(stepRotation, axis, axis);
            }
        }

        GXEnd();
#endif
    }

    void drawCylinder(const TVec3f& rBase, const TVec3f& rHeight, f32 radius, u32 baseColor, u32 topColor, u32 segments) {
        // 3DS: the vertices go into an array of at most 256 segments.
#ifdef __3DS__
        if (segments == 0 || segments > 256) return;
#endif
        TVec3f axis;

        if (rHeight.x != 0.0f) {
            axis.set< f32 >(rHeight.y, -rHeight.x, 0.0f);
        } else {
            axis.set< f32 >(0.0f, -rHeight.z, rHeight.y);
        }

        if (MR::isNearZero(axis)) {
            return;
        }

        MR::normalizeOrZero(&axis);
        TVec3f top(rBase);
        top += rHeight;
        Mtx rotation;
        PSMTXRotAxisRad(rotation, rHeight, (TWO_PI / segments));

        // 3DS: the same drawing through DirectDraw (Citro3D) instead of GX.
#ifdef __3DS__
        TVec3f positions[514];
        u32 colors[514];
        for (u32 i = 0; i <= segments; i++) {
            TVec3f basePoint(rBase + axis * radius);
            TVec3f topPoint(top + axis * radius);
            positions[i * 2] = basePoint;
            positions[i * 2 + 1] = topPoint;
            colors[i * 2] = baseColor;
            colors[i * 2 + 1] = topColor;
            PSMTXMultVec(rotation, &axis, &axis);
        }
        drawStripDirect(positions, colors, 2 * (segments + 1));
#else
        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 2 * (segments + 1));
        for (u32 i = 0; i <= segments; i++) {
            TVec3f basePoint(rBase + axis * radius);
            TVec3f topPoint(top + axis * radius);
            GXPosition3f32(basePoint.x, basePoint.y, basePoint.z);
            GXColor1u32(baseColor);
            GXPosition3f32(topPoint.x, topPoint.y, topPoint.z);
            GXColor1u32(topColor);
            PSMTXMultVec(rotation, axis, axis);
        }

        GXEnd();
#endif
    }

    void drawSpherePart(const TPos3f& rMtx, f32 radius, f32 startAzimuth, f32 endAzimuth, f32 startPolar, f32 endPolar, u32 color,
                        u32 azimuthSegments, u32 polarSegments) {
        // 3DS: the vertices go into an array of at most 256 segments.
#ifdef __3DS__
        if (azimuthSegments == 0 || polarSegments == 0 || azimuthSegments > 256 || polarSegments > 256) return;
#endif
        TVec3f side;
        side.set< f32 >(rMtx(0, 0), rMtx(1, 0), rMtx(2, 0));
        TVec3f up;
        up.set< f32 >(rMtx(0, 1), rMtx(1, 1), rMtx(2, 1));
        TVec3f front;
        front.set< f32 >(rMtx(0, 2), rMtx(1, 2), rMtx(2, 2));
        TVec3f center;
        rMtx.getTrans(center);
        f32 azimuthSin, azimuthCos;
        azimuthSin = MR::sin(startAzimuth);
        azimuthCos = MR::cos(startAzimuth);
        TVec3f previousRadial((side * azimuthCos + up * azimuthSin) * radius);

        for (u32 i = 1; i <= azimuthSegments; i++) {
            f32 azimuth = startAzimuth + (static_cast< f32 >(i) / azimuthSegments) * (endAzimuth - startAzimuth);
            azimuthSin = MR::sin(azimuth);
            azimuthCos = MR::cos(azimuth);
            TVec3f radial((side * azimuthCos + up * azimuthSin) * radius);

            // 3DS: the same drawing through DirectDraw (Citro3D) instead of GX.
#ifdef __3DS__
            TVec3f positions[514];
            u32 colors[514];
            for (u32 j = 0; j <= polarSegments; j++) {
                const f32 polar = startPolar + (static_cast<f32>(j) / polarSegments) * (endPolar - startPolar);
                const f32 polarSin = MR::sin(polar);
                const f32 polarCos = MR::cos(polar);
                const TVec3f ringCenter(center + front * (polarCos * radius));
                TVec3f previousPoint(previousRadial);
                TVec3f point(radial);
                previousPoint *= polarSin;
                point *= polarSin;
                previousPoint += ringCenter;
                point += ringCenter;
                positions[j * 2] = previousPoint;
                positions[j * 2 + 1] = point;
                colors[j * 2] = colors[j * 2 + 1] = color;
            }
            drawStripDirect(positions, colors, 2 * (polarSegments + 1));
#else
            GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 2 * (polarSegments + 1));
            for (u32 j = 0; j <= polarSegments; j++) {
                f32 polar = (startPolar + (static_cast< f32 >(j) / polarSegments) * (endPolar - startPolar));
                f32 polarSin = MR::sin(polar);
                f32 polarCos = MR::cos(polar);
                TVec3f ringCenter(center + front * (polarCos * radius));
                TVec3f previousPoint(previousRadial);
                previousPoint *= polarSin;
                TVec3f point(radial);
                point *= polarSin;
                previousPoint += ringCenter;
                point += ringCenter;
                GXPosition3f32(previousPoint.x, previousPoint.y, previousPoint.z);
                GXColor1u32(color);
                GXPosition3f32(point.x, point.y, point.z);
                GXColor1u32(color);
            }

            GXEnd();
#endif

            previousRadial = radial;
        }
    }

    void drawSphere(const TVec3f& rCenter, f32 radius, u32 color, u32 segments) {
        TVec3f normal;
        normal = MR::getCamZdir();
        drawFillCircle(rCenter, normal, radius, color, segments);
    }

    void drawSphere3D(TVec3f center, f32 radius, u32 color, u32 segments) {
        TPos3f mtx;
        mtx.identity();
        mtx.setTrans(center);
        drawSpherePart(mtx, radius, 0.0f, TWO_PI, 0.0f, PI, color, segments, segments);
    }

    void drawTexture(const TVec2f& rPosition, JUTTexture* pTexture, const TVec2f& rSize) {
        // 3DS: the textured quad through the Citro3D draw below, fully opaque.
#ifdef __3DS__
        drawTexture(rPosition, pTexture, rSize, 255);
#else
        pTexture->load(GX_TEXMAP0);

        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        {
            GXPosition3f32(rPosition.x, rPosition.y, 0.0f);
            GXTexCoord2f32(0.0f, 0.0f);
            GXPosition3f32(rPosition.x + rSize.x, rPosition.y, 0.0f);
            GXTexCoord2f32(1.0f, 0.0f);
            GXPosition3f32(rPosition.x + rSize.x, rPosition.y + rSize.y, 0.0f);
            GXTexCoord2f32(1.0f, 1.0f);
            GXPosition3f32(rPosition.x, rPosition.y + rSize.y, 0.0f);
            GXTexCoord2f32(0.0f, 1.0f);
        }

        GXEnd();
#endif
    }

#ifdef __3DS__
    void drawTexture(const TVec2f& rPosition, JUTTexture* pTexture, const TVec2f& rSize, u8 alpha) {
        const TVec3f positions[4] = {TVec3f(rPosition.x, rPosition.y, 0.0f), TVec3f(rPosition.x + rSize.x, rPosition.y, 0.0f),
                                     TVec3f(rPosition.x + rSize.x, rPosition.y + rSize.y, 0.0f),
                                     TVec3f(rPosition.x, rPosition.y + rSize.y, 0.0f)};
        const f32 uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
        drawTexturedQuadDirect(positions, uv, pTexture, alpha);
    }

    void drawHostTexture(const TVec2f& rPosition, const TVec2f& rSize, void* pTexture, f32 u, f32 v) {
        if (pTexture == nullptr) return;
        const TVec3f positions[4] = {TVec3f(rPosition.x, rPosition.y, 0.0f), TVec3f(rPosition.x + rSize.x, rPosition.y, 0.0f),
                                     TVec3f(rPosition.x + rSize.x, rPosition.y + rSize.y, 0.0f),
                                     TVec3f(rPosition.x, rPosition.y + rSize.y, 0.0f)};
        const f32 uv[4][2] = {{0.0f, 0.0f}, {u, 0.0f}, {u, v}, {0.0f, v}};
        const u8 order[6] = {0, 1, 2, 0, 2, 3};
        HostGfxVertex vertices[6] = {};
        for (u32 i = 0; i < 6; i++) {
            transformToClip(positions[order[i]], vertices[i], 0xFFFFFFFFu);
            vertices[i].s0 = uv[order[i]][0];
            vertices[i].t0 = uv[order[i]][1];
        }
        HostGfxState state = {};
        state.textures[0] = pTexture;
        state.stageCount = 1;
        state.stages[0].colorSrc[0] = 0x03;  // the texture
        state.stages[0].colorFunc = 0;
        state.stages[0].alphaSrc[0] = 0x00;  // opaque (the vertex alpha)
        state.stages[0].alphaFunc = 0;
        applyDirectState(state);
        hostGfxDraw(vertices, 6, nullptr, 6, HOST_GFX_TRIANGLES, &state);
    }

    // The texture over the rectangle, sampled at up to three offsets (in texture coordinates) and
    // summed, each times weight / 255 (the Wii's blur: texture matrices moving texcoord 0, one
    // TEV stage per tap). Drawn with the DirectDraw state; three taps per draw.
    void drawTextureTaps(const TVec2f& rPosition, JUTTexture* pTexture, const TVec2f& rSize, const f32 (*pOffsets)[2], u32 count,
                         u8 weight) {
        void* handle = pTexture != nullptr ? rvl3DSGetJUTTexture(pTexture) : nullptr;
        if (handle == nullptr || pOffsets == nullptr || count == 0) {
            return;
        }
        count = count > 3 ? 3 : count;
        const f32 corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
        const u8 order[6] = {0, 1, 2, 0, 2, 3};
        HostGfxVertex vertices[6] = {};
        for (u32 i = 0; i < 6; i++) {
            const f32* corner = corners[order[i]];
            transformToClip(TVec3f(rPosition.x + corner[0] * rSize.x, rPosition.y + corner[1] * rSize.y, 0.0f), vertices[i], 0xFFFFFFFFu);
            f32* uv[3][2] = {{&vertices[i].s0, &vertices[i].t0}, {&vertices[i].s1, &vertices[i].t1}, {&vertices[i].s2, &vertices[i].t2}};
            for (u32 tap = 0; tap < count; tap++) {
                rvl3DSMapJUTTextureCoord(pTexture, corner[0] + pOffsets[tap][0], corner[1] + pOffsets[tap][1], *uv[tap][0], *uv[tap][1]);
            }
        }
        HostGfxState state = {};
        state.stageCount = static_cast< u8 >(count);
        const u32 constant = weight | (weight << 8) | (static_cast< u32 >(weight) << 16) | (static_cast< u32 >(weight) << 24);
        for (u32 tap = 0; tap < count; tap++) {
            state.textures[tap] = handle;
            HostGfxStage& stage = state.stages[tap];
            // Colour and alpha alike: the texture of this tap times its weight, plus the taps so
            // far (modulate, then multiply-add).
            stage.colorSrc[0] = stage.alphaSrc[0] = static_cast< u8 >(0x03 + tap);
            stage.colorSrc[1] = stage.alphaSrc[1] = 0x0E;
            stage.colorSrc[2] = stage.alphaSrc[2] = tap == 0 ? 0x0E : 0x0F;
            stage.colorFunc = stage.alphaFunc = tap == 0 ? 1 : 8;
            stage.constant = constant;
        }
        applyDirectState(state);
        hostGfxDraw(vertices, 6, nullptr, 6, HOST_GFX_TRIANGLES, &state);
    }

    // The texture over the rectangle (unit 0, through a capture's mapping) with the given combiner
    // stages and DirectDraw's fragment state; with farDepth the rectangle lies on the far plane (the
    // Wii's GXSetZScaleOffset(0, 1)).
    void drawTextureStages(const TVec2f& rPosition, JUTTexture* pTexture, const TVec2f& rSize, const HostGfxStage* pStages, u32 count,
                           bool farDepth) {
        void* handle = pTexture != nullptr ? rvl3DSGetJUTTexture(pTexture) : nullptr;
        if (handle == nullptr || pStages == nullptr || count == 0 || count > HOST_GFX_STAGES) {
            return;
        }
        const f32 corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
        const u8 order[6] = {0, 1, 2, 0, 2, 3};
        HostGfxVertex vertices[6] = {};
        for (u32 i = 0; i < 6; i++) {
            const f32* corner = corners[order[i]];
            transformToClip(TVec3f(rPosition.x + corner[0] * rSize.x, rPosition.y + corner[1] * rSize.y, 0.0f), vertices[i], 0xFFFFFFFFu);
            if (farDepth) {
                vertices[i].z = 0.0f;  // the far plane (3DS depth 0)
            }
            rvl3DSMapJUTTextureCoord(pTexture, corner[0], corner[1], vertices[i].s0, vertices[i].t0);
        }
        HostGfxState state = {};
        state.textures[0] = handle;
        state.stageCount = static_cast< u8 >(count);
        for (u32 i = 0; i < count; i++) {
            state.stages[i] = pStages[i];
        }
        applyDirectState(state);
        hostGfxDraw(vertices, 6, nullptr, 6, HOST_GFX_TRIANGLES, &state);
    }

    // The texture's taps, averaged, blended by alpha over the rectangle at a fixed 3DS depth: with a
    // depth test this reaches only what lies beyond (or before) that depth.
    void drawTextureTapsAtDepth(const TVec2f& rPosition, JUTTexture* pTexture, const TVec2f& rSize, const f32 (*pOffsets)[2], u32 count, u8 alpha,
                                f32 depth) {
        void* handle = pTexture != nullptr ? rvl3DSGetJUTTexture(pTexture) : nullptr;
        if (handle == nullptr || pOffsets == nullptr || count == 0) {
            return;
        }
        count = count > 3 ? 3 : count;
        const f32 corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
        const u8 order[6] = {0, 1, 2, 0, 2, 3};
        HostGfxVertex vertices[6] = {};
        for (u32 i = 0; i < 6; i++) {
            const f32* corner = corners[order[i]];
            transformToClip(TVec3f(rPosition.x + corner[0] * rSize.x, rPosition.y + corner[1] * rSize.y, 0.0f), vertices[i], 0xFFFFFFFFu);
            vertices[i].z = -depth * vertices[i].w;  // clip space keeps z in [-w, 0], and 0 is the far plane
            f32* uv[3][2] = {{&vertices[i].s0, &vertices[i].t0}, {&vertices[i].s1, &vertices[i].t1}, {&vertices[i].s2, &vertices[i].t2}};
            for (u32 tap = 0; tap < count; tap++) {
                rvl3DSMapJUTTextureCoord(pTexture, corner[0] + pOffsets[tap][0], corner[1] + pOffsets[tap][1], *uv[tap][0], *uv[tap][1]);
            }
        }
        const u32 weight = 255 / count + (255 % count != 0 ? 1 : 0);
        HostGfxState state = {};
        state.stageCount = static_cast< u8 >(count);
        for (u32 tap = 0; tap < count; tap++) {
            state.textures[tap] = handle;
            HostGfxStage& stage = state.stages[tap];
            stage.colorSrc[0] = static_cast< u8 >(0x03 + tap);
            stage.colorSrc[1] = 0x0E;
            stage.colorSrc[2] = tap == 0 ? 0x0E : 0x0F;
            stage.colorFunc = tap == 0 ? 1 : 8;  // modulate, then multiply-add
            stage.alphaSrc[0] = 0x0E;
            stage.alphaFunc = 0;
            stage.constant = weight | (weight << 8) | (weight << 16) | (static_cast< u32 >(alpha) << 24);
        }
        applyDirectState(state);
        hostGfxDraw(vertices, 6, nullptr, 6, HOST_GFX_TRIANGLES, &state);
    }

    // The texture over the rectangle where its mask value exceeds threshold, black elsewhere: the
    // Wii's bloom mask (an I8, R8, G8 or B8 copy, then GX_TEV_COMP_R8_GT against the threshold).
    // The mask value goes in the alpha (weights / 256 per channel, plus 16 / 255 as the I8 copy
    // adds) and the alpha test compares it, over a black rectangle.
    void drawTextureBrightPass(const TVec2f& rPosition, JUTTexture* pTexture, const TVec2f& rSize, u8 threshold, const u8* pWeights) {
        void* handle = pTexture != nullptr ? rvl3DSGetJUTTexture(pTexture) : nullptr;
        if (handle == nullptr || pWeights == nullptr) {
            return;
        }
        const f32 corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
        const u8 order[6] = {0, 1, 2, 0, 2, 3};
        HostGfxVertex vertices[6] = {};
        for (u32 i = 0; i < 6; i++) {
            const f32* corner = corners[order[i]];
            transformToClip(TVec3f(rPosition.x + corner[0] * rSize.x, rPosition.y + corner[1] * rSize.y, 0.0f), vertices[i], 0xFFFFFFFFu);
            rvl3DSMapJUTTextureCoord(pTexture, corner[0], corner[1], vertices[i].s0, vertices[i].t0);
        }
        // The black rectangle.
        HostGfxState state = {};
        state.stageCount = 1;
        state.stages[0].colorSrc[0] = 0x0E;
        state.stages[0].alphaSrc[0] = 0x0E;
        state.stages[0].constant = 0xFF000000u;
        applyDirectState(state);
        hostGfxDraw(vertices, 6, nullptr, 6, HOST_GFX_TRIANGLES, &state);

        // The texture, its mask value in the alpha: r * wr + g * wg + b * wb (alpha operands
        // GPU_TEVOP_A_SRC_R, _G, _B). A weight of 255 takes the channel as it is; the others are
        // the I8 copy's 1 / 256 steps in the combiners' 1 / 255 ones.
        const bool luminance = pWeights[0] != 255 && pWeights[1] != 255 && pWeights[2] != 255;
        state = HostGfxState();
        state.textures[0] = handle;
        state.stageCount = 3;
        static const u8 kChannel[3] = {0x02, 0x04, 0x06};
        for (u32 c = 0; c < 3; c++) {
            HostGfxStage& stage = state.stages[c];
            const u32 w = pWeights[c] == 255 ? 255 : (pWeights[c] * 255 + 128) / 256;
            stage.colorSrc[0] = c == 0 ? 0x03 : 0x0F;
            stage.alphaSrc[0] = 0x03;
            stage.alphaOp[0] = kChannel[c];
            stage.alphaSrc[1] = 0x0E;
            stage.alphaSrc[2] = c == 0 ? 0x0E : 0x0F;
            stage.alphaFunc = c == 0 ? 1 : 8;  // modulate, then multiply-add
            stage.constant = w << 24;
        }
        applyDirectState(state);
        const u32 offset = luminance ? 16 : 0;
        if (threshold < offset) {
            state.alphaTestFunc = 1;  // GPU_ALWAYS: Y is at least 16
        } else {
            state.alphaTestFunc = 6;  // GPU_GREATER
            state.alphaRef = threshold - offset;
        }
        hostGfxDraw(vertices, 6, nullptr, 6, HOST_GFX_TRIANGLES, &state);
    }
#endif
    void drawTexture3D(const TVec3f& rCenter, const TVec3f& rNormal, const TVec3f& rUp, f32 width, f32 height, JUTTexture* pTexture, bool flipX,
                       bool flipY) {
        // 3DS: the textured quad through the Citro3D draw below, fully opaque.
#ifdef __3DS__
        drawTexture3D(rCenter, rNormal, rUp, width, height, pTexture, flipX, flipY, 255);
#else
        TVec3f p3;
        TVec3f p2;
        TVec3f p1;
        TVec3f p0;
        TVec3f right;
        TVec3f up;

        if (pTexture != nullptr) {
            pTexture->load(GX_TEXMAP0);
        }

        right.cross(rNormal, rUp);
        MR::normalizeOrZero(&right);
        up.cross(right, rNormal);
        MR::normalizeOrZero(&up);
        p0 = rCenter - right * height + up * width;
        p1 = rCenter + right * height + up * width;
        p2 = rCenter + right * height - up * width;
        p3 = rCenter - right * height - up * width;

        f32 left = flipX ? 1.0f : 0.0f;
        f32 rightTex = flipX ? 0.0f : 1.0f;
        f32 top = flipY ? 1.0f : 0.0f;
        f32 bottom = flipY ? 0.0f : 1.0f;
        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        GXPosition3f32(p0.x, p0.y, p0.z);
        GXTexCoord2f32(left, top);
        sendPoint(p1);
        GXTexCoord2f32(rightTex, top);
        sendPoint(p2);
        GXTexCoord2f32(rightTex, bottom);
        sendPoint(p3);
        GXTexCoord2f32(left, bottom);
        GXEnd();
#endif
    }
    // 3DS: a textured quad in 3D with an alpha, through Citro3D.
#ifdef __3DS__
    void drawTexture3D(const TVec3f& rCenter, const TVec3f& rNormal, const TVec3f& rUp, f32 width, f32 height,
                       JUTTexture* pTexture, bool flipX, bool flipY, u8 alpha) {
        TVec3f right;
        right.cross(rNormal, rUp);
        MR::normalizeOrZero(&right);
        TVec3f up;
        up.cross(right, rNormal);
        MR::normalizeOrZero(&up);

        const TVec3f positions[4] = {rCenter - right * height + up * width, rCenter + right * height + up * width,
                                     rCenter + right * height - up * width, rCenter - right * height - up * width};
        const f32 left = flipX ? 1.0f : 0.0f;
        const f32 rightTex = flipX ? 0.0f : 1.0f;
        const f32 top = flipY ? 1.0f : 0.0f;
        const f32 bottom = flipY ? 0.0f : 1.0f;
        const f32 uv[4][2] = {{left, top}, {rightTex, top}, {rightTex, bottom}, {left, bottom}};
        drawTexturedQuadDirect(positions, uv, pTexture, alpha);
    }
#endif

    void drawFillBox(const TVec3f& rMin, const TVec3f& rMax, u32 color) {
        // 3DS: the same drawing through DirectDraw (Citro3D) instead of GX.
#ifdef __3DS__
        const TVec3f positions[6] = {TVec3f(rMin.x, rMin.y, rMin.z), TVec3f(rMax.x, rMin.y, rMin.z),
                                      TVec3f(rMax.x, rMax.y, rMax.z), TVec3f(rMin.x, rMin.y, rMin.z),
                                      TVec3f(rMax.x, rMax.y, rMax.z), TVec3f(rMin.x, rMax.y, rMax.z)};
        const u32 colors[6] = {color, color, color, color, color, color};
        drawTrianglesDirect(positions, colors, 6);
#else
        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        {
            GXPosition3f32(rMin.x, rMin.y, rMin.z);
            GXColor1u32(color);
            GXPosition3f32(rMax.x, rMin.y, rMin.z);
            GXColor1u32(color);
            GXPosition3f32(rMax.x, rMax.y, rMax.z);
            GXColor1u32(color);
            GXPosition3f32(rMin.x, rMax.y, rMax.z);
            GXColor1u32(color);
        }

        GXEnd();
#endif
    }

    void drawFillBox(const TVec2f& rMin, const TVec2f& rMax, u32 color) {
        // 3DS: the same drawing through DirectDraw (Citro3D) instead of GX.
#ifdef __3DS__
        const TVec3f positions[6] = {TVec3f(rMin.x, rMin.y, 0.0f), TVec3f(rMax.x, rMin.y, 0.0f),
                                      TVec3f(rMax.x, rMax.y, 0.0f), TVec3f(rMin.x, rMin.y, 0.0f),
                                      TVec3f(rMax.x, rMax.y, 0.0f), TVec3f(rMin.x, rMax.y, 0.0f)};
        const u32 colors[6] = {color, color, color, color, color, color};
        drawTrianglesDirect(positions, colors, 6);
#else
        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        {
            GXPosition3f32(rMin.x, rMin.y, 0.0f);
            GXColor1u32(color);
            GXPosition3f32(rMax.x, rMin.y, 0.0f);
            GXColor1u32(color);
            GXPosition3f32(rMax.x, rMax.y, 0.0f);
            GXColor1u32(color);
            GXPosition3f32(rMin.x, rMax.y, 0.0f);
            GXColor1u32(color);
        }

        GXEnd();
#endif
    }

    void drawFillBox3D(const TVec3f& rCenter, const TVec3f& rHeight, const TVec3f& rSide, const TVec3f& rFront, u32 color) {
        TVec3f p7, p6, p5, p4, p3, p2, p1, p0;
        p0 = rCenter + rSide - rFront;
        p1 = rCenter - rSide - rFront;
        p2 = rCenter - rSide + rFront;
        p3 = rCenter + rSide + rFront;
        p4 = p0 + rHeight;
        p5 = p1 + rHeight;
        p6 = p2 + rHeight;
        p7 = p3 + rHeight;

        // 3DS: the same drawing through DirectDraw (Citro3D) instead of GX.
#ifdef __3DS__
        const TVec3f quads[6][4] = {{p0, p1, p2, p3}, {p7, p4, p0, p3}, {p6, p2, p1, p5},
                                    {p0, p4, p5, p1}, {p6, p7, p3, p2}, {p6, p5, p4, p7}};
        TVec3f positions[36];
        u32 colors[36];
        u32 out = 0;
        for (u32 face = 0; face < 6; face++) {
            positions[out] = quads[face][0]; colors[out++] = color;
            positions[out] = quads[face][1]; colors[out++] = color;
            positions[out] = quads[face][2]; colors[out++] = color;
            positions[out] = quads[face][0]; colors[out++] = color;
            positions[out] = quads[face][2]; colors[out++] = color;
            positions[out] = quads[face][3]; colors[out++] = color;
        }
        drawTrianglesDirect(positions, colors, out);
#else
        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        GXPosition3f32(p0.x, p0.y, p0.z);
        GXColor1u32(color);
        sendPoint(p1, color);
        sendPoint(p2, color);
        sendPoint(p3, color);
        GXEnd();

        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        sendPoint(p7, color);
        sendPoint(p4, color);
        GXPosition3f32(p0.x, p0.y, p0.z);
        GXColor1u32(color);
        sendPoint(p3, color);
        GXEnd();

        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        sendPoint(p6, color);
        sendPoint(p2, color);
        sendPoint(p1, color);
        sendPoint(p5, color);
        GXEnd();

        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        GXPosition3f32(p0.x, p0.y, p0.z);
        GXColor1u32(color);
        sendPoint(p4, color);
        sendPoint(p5, color);
        sendPoint(p1, color);
        GXEnd();

        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        sendPoint(p6, color);
        sendPoint(p7, color);
        sendPoint(p3, color);
        sendPoint(p2, color);
        GXEnd();

        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        sendPoint(p6, color);
        sendPoint(p5, color);
        sendPoint(p4, color);
        sendPoint(p7, color);
        GXEnd();
#endif
    }

    void cameraInit3D() {
        MR::loadProjectionMtx();
        // 3DS: DirectDraw keeps the projection itself (no GX projection register).
#ifdef __3DS__
        copyDirectProjection(*MR::getCameraProjectionMtx().toCMtx44());
#endif
    }

    void cameraInit2D() {
        static TVec3f camLoc(MR::getScreenWidth() / 2.0f, MR::getScreenHeight() / 2.0f, -30.0f);
        static TVec3f objPt(MR::getScreenWidth() / 2.0f, MR::getScreenHeight() / 2.0f, 0.0f);
        static TVec3f up(0, -10, 0);

        f32 width, nearZ, farZ, height;
        nearZ = 0.0f;
        farZ = 1.0f;
        width = MR::getScreenWidth() / 2;
        height = MR::getScreenHeight() / 2;
        Mtx44 projection;
        C_MTXOrtho(projection, height, -height, -width, width, nearZ, -farZ);
        // 3DS: DirectDraw keeps the projection itself (no GX projection register).
#ifdef __3DS__
        copyDirectProjection(projection);
#endif
        // Wii only: GX state, display lists and material setup; the 3DS draws this through Citro3D
        // (DETAILS.md, section 5).
#ifndef __3DS__
        GXSetProjection(projection, GX_ORTHOGRAPHIC);
        MR::setDefaultViewportAndScissor();
#endif
    }

    void mixFogColor(TVec3f position, f32 radius, u32 packedColor) {
        // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
        // (DETAILS.md, section 5).
#ifdef __3DS__
        (void)position;
        (void)radius;
        (void)packedColor;
        return;
#else
        f32 nearZ = MR::getNearZ();
        f32 farZ = MR::getFarZ();
        GXColor color;
        setGXColor(packedColor, &color);
        f32 start;
        f32 end;
        MR::calcFogStartEnd(position, radius, &start, &end);
        GXSetFog(GX_FOG_PERSP_LIN, start, end, nearZ, farZ, color);
#endif
    }

    namespace {
        u8 tileBuffer[0x800] ATTRIBUTE_ALIGN(32);
    }

    inline void tileConversion8Impl(u8* pTexture, u32 width, u32 height, u8* pTileBuffer) {
        u8* pDest = pTexture;
        u8* pBuffer = pTileBuffer;
        u32 offset, row, y, x, sourceOffset;
        offset = 0;

        for (y = 0; y < height; y += 4) {
            for (x = 0; x < width; x += 8) {
                for (row = 0; row < 4; row++) {
                    sourceOffset = ((y + row) * width) + x;
                    *reinterpret_cast< u32* >(&pBuffer[offset]) = *reinterpret_cast< const u32* >(&pTexture[sourceOffset]);
                    *reinterpret_cast< u32* >(&pBuffer[offset + 4]) = *reinterpret_cast< const u32* >(&pTexture[sourceOffset + 4]);
                    offset += 8;
                }
            }

            MR::copyMemory(pDest, pBuffer, width * 4);
            pDest += width * 4;
            offset = 0;
        }
    }

    void tileConversion8(u8* pTexture, u32 width, u32 height) {
        tileConversion8Impl(pTexture, width, height, tileBuffer);
    }

    inline void tileConversion16Impl(u16* pTexture, u32 width, u32 height, u8* pBuffer) {
        u8* pDest = reinterpret_cast< u8* >(pTexture);
        u8* const pBufferLocal = pBuffer;
        u32 offset, y, row, column, x;
        offset = 0;
        for (y = 0; y < height; y += 4, pDest += 32) {
            for (x = 0; x < width; x += 4) {
                for (row = 0; row < 4; row++) {
                    for (column = 0; column < 4; column++) {
                        pBufferLocal[offset] = pTexture[(y + row) * width + (x + column)];
                        offset++;
                    }
                }
            }

            MR::copyMemory(pDest, pBufferLocal, 32);
            offset = 0;
        }
    }

    void tileConversion16(u16* pTexture, u32 width, u32 height) {
        tileConversion16Impl(pTexture, width, height, tileBuffer);
    }

    u32 getTexel32(const JUTTexture* pTexture, u32 x, u32 y) {
        return getTexel32(pTexture->mImage, pTexture->getWidth(), x, y);
    }

    u32 getTexel32(const u8* pTexture, u32 width, u32 x, u32 y) {
        u32 offset = ((width << 4) & ~0x3F) * (y >> 2) + (((x & 3) << 1) + (((y & 3) << 3) + ((x << 4) & ~0x3F)));
        return (pTexture[offset] | (pTexture[offset + 0x21] << 8)) | ((pTexture[offset + 1] << 24) | (pTexture[offset + 0x20] << 16));
    }

    void setTexel32(JUTTexture* pTexture, u32 x, u32 y, u32 color) {
        setTexel32(pTexture->mImage, pTexture->getWidth(), x, y, color);
    }

    void setTexel32(u8* pTexture, u32 width, u32 x, u32 y, u32 color) {
        u32 offset = ((width << 4) & ~0x3F) * (y >> 2) + (((x & 3) << 1) + (((y & 3) << 3) + ((x << 4) & ~0x3F)));
        pTexture[offset] = color;
        pTexture[offset + 1] = color >> 24;
        pTexture[offset + 0x20] = color >> 16;
        pTexture[offset + 0x21] = color >> 8;
    }

    void invProject(TVec3f* pDest, const TVec3f& rScreen, MtxPtr pView, const f32* pProjection, const f32* pViewport, bool normalizedZ) {
        f32 screenZ;
        if (normalizedZ == false) {
            screenZ = rScreen.z / 16777215.0f;
        } else {
            screenZ = rScreen.z;
        }

        f32 depth = screenZ - pViewport[5];
        f32 w;
        f32 projectedZ = (depth * pProjection[6]) / (depth + pProjection[5] * (pViewport[5] - pViewport[4]));
        w = pProjection[5] / (pProjection[6] - projectedZ);
        f32 x;
        if (MR::isScreen16Per9()) {
            x = ((rScreen.x * MR::getFrameBufferWidth() / MR::getScreenWidth() - (pViewport[0] + pViewport[2] / 2.0f)) / w) / (pViewport[2] / 2.0f);
        } else {
            x = ((rScreen.x - (pViewport[0] + pViewport[2] / 2.0f)) / w) / (pViewport[2] / 2.0f);
        }

        f32 y = (-(rScreen.y - (pViewport[1] + pViewport[3] / 2.0f)) / w) / (pViewport[3] / 2.0f);
        TVec3f viewPos;
        if (pProjection[0] == 0.0f) {
            viewPos.z = (projectedZ - pProjection[6]) / pProjection[5];
            viewPos.x = (x - viewPos.z * pProjection[2]) / pProjection[1];
            viewPos.y = (y - viewPos.z * pProjection[4]) / pProjection[3];
        } else {
            viewPos.z = (projectedZ - pProjection[6]) / pProjection[5];
            viewPos.x = (x - pProjection[2]) / pProjection[1];
            viewPos.y = (y - pProjection[4]) / pProjection[3];
        }

        Mtx inverseView;
        PSMTXInverse(pView, inverseView);
        PSMTXMultVec(inverseView, viewPos, pDest);
    }

    void project2D(TVec3f* pDest, const TVec3f& rPosition) {
        // 3DS: the position projected with the camera's matrices directly (GX's viewport and
        // projection are not kept).
#ifdef __3DS__
        TVec3f view;
        PSMTXMultVec(MR::getCameraViewMtx(), &rPosition, &view);
        const TProj3f& projection = MR::getCameraProjectionMtx();
        const f32 x = view.x * projection.mMtx[0][0] + view.y * projection.mMtx[0][1] + view.z * projection.mMtx[0][2] + projection.mMtx[0][3];
        const f32 y = view.x * projection.mMtx[1][0] + view.y * projection.mMtx[1][1] + view.z * projection.mMtx[1][2] + projection.mMtx[1][3];
        const f32 z = view.x * projection.mMtx[2][0] + view.y * projection.mMtx[2][1] + view.z * projection.mMtx[2][2] + projection.mMtx[2][3];
        const f32 w = view.x * projection.mMtx[3][0] + view.y * projection.mMtx[3][1] + view.z * projection.mMtx[3][2] + projection.mMtx[3][3];
        if (w == 0.0f) {
            pDest->set(0.0f, 0.0f, 0.0f);
            return;
        }
        pDest->x = (x / w + 1.0f) * MR::getScreenWidth() * 0.5f;
        pDest->y = (1.0f - y / w) * MR::getScreenHeight() * 0.5f;
        pDest->z = z / w;
        return;
#else
        f32 projection[7];
        f32 viewport[6];
        GXGetViewportv(viewport);
        const TProj3f& rProjection = MR::getCameraProjectionMtx();
        projection[0] = 0.0f;
        projection[1] = rProjection.mMtx[0][0];
        projection[2] = rProjection.mMtx[0][2];
        projection[3] = rProjection.mMtx[1][1];
        projection[4] = rProjection.mMtx[1][2];
        projection[5] = rProjection.mMtx[2][2];
        projection[6] = rProjection.mMtx[2][3];
        GXProject(rPosition.x, rPosition.y, rPosition.z, MR::getCameraViewMtx(), projection, viewport, &pDest->x, &pDest->y, &pDest->z);
        if (MR::isScreen16Per9()) {
            pDest->x *= static_cast< f32 >(MR::getScreenWidth()) / MR::getFrameBufferWidth();
        }
#endif
    }

    void project2D(TVec2f* pDest, const TVec3f& rPosition) {
        TVec3f screen;
        project2D(&screen, rPosition);
        pDest->x = screen.x;
        pDest->y = screen.y;
    }

    void fix2Dpos(TVec3f* pPosition) {
        if (MR::isScreen16Per9()) {
            pPosition->x *= static_cast< f32 >(MR::getScreenWidth()) / MR::getFrameBufferWidth();
        }
    }

};  // namespace TDDraw

void DirectDraw_FORCE_MATCH(TPos3f* pMtx, TVec3f* pVec) {
    pMtx->getTrans(*pVec);
    pMtx->setTrans(*pVec);
}

void DirectDraw_FORCE_MATCH_CTOR(int x, int y, int z) {
    TVec3f v(x, y, z);
}
