#include "Game/Screen/WaterCameraFilter.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Map/WaterAreaHolder.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Screen/MissLayout.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/MtxUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
// 3DS: JUTTexture3DS.hpp, GPU textures for JUTTextures; WaterRender3DS.hpp, water drawn through
// Citro3D.
#ifdef __3DS__
#include "JUTTexture3DS.hpp"
#include "WaterRender3DS.hpp"
#endif
#include <JSystem/JGeometry/TMatrix.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <JSystem/JUtility/JUTVideo.hpp>
#include <revolution/gx/GXBump.h>
#include <revolution/gx/GXEnum.h>
#include <revolution/gx/GXRegs.h>
#include <revolution/gx/GXStruct.h>
#include <revolution/gx/GXTransform.h>

namespace NrvWaterCameraFilter {
    NEW_NERVE(WaterCameraFilterNrvAir, WaterCameraFilter, Air);
    NEW_NERVE(WaterCameraFilterNrvAirToWater, WaterCameraFilter, AirToWater);
    NEW_NERVE(WaterCameraFilterNrvWater, WaterCameraFilter, Water);
    NEW_NERVE(WaterCameraFilterNrvWaterToAir, WaterCameraFilter, WaterToAir);
};  // namespace NrvWaterCameraFilter

namespace {
    s32 getFrameBufferWidth() {
        return MR::getFrameBufferWidth();
    }

    s32 getScreenHeight() {
        return MR::getScreenHeight();
    }
};  // namespace

static GXColor sDefaultColor;

WaterCameraFilter::WaterCameraFilter()
    : LiveActor("水中カメラフィルター"), mWaterAlpha(), mWavePhase(), mWaterColor(::sDefaultColor), mScreenTex(), mFilterTex() {
}

void WaterCameraFilter::init(const JMapInfoIter& rIter) {
    MR::connectToScene(this, MR::MovementType_MapObj, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_WaterCameraFilter);
    MR::invalidateClipping(this);
    initNerve(GET_NERVE(WaterCameraFilter, WaterCameraFilterNrvAir));
    initScreenTex();
    JUTTexture* pFilterTex = new JUTTexture(MR::loadTexFromArc("WaterCameraFilter.arc", "WaterCameraFilter.bti"), 0);
    mFilterTex = pFilterTex;
    makeActorAppeared();
}

namespace {
    s32 getScreenTexHeight() {
        return MR::getScreenHeight();
    }
};  // namespace

void WaterCameraFilter::initScreenTex() {
#ifdef __3DS__
    // The screen capture lives on the GPU (rvl3DSCaptureJUTTextureRegion): the texture only names
    // it, so its own image is the smallest.
    MR::CurrentHeapRestorer restorer(MR::getSceneHeapGDDR3());
    mScreenTex = new JUTTexture(8, 8, GX_TF_RGB565);
#else
    MR::CurrentHeapRestorer restorer(MR::getSceneHeapGDDR3());
    mScreenTex = new JUTTexture(::getFrameBufferWidth(), ::getScreenHeight(), GX_TF_RGB565);
#endif
}

void WaterCameraFilter::exeAirToWater() {
    if (!MR::isCameraInWater()) {
        setNerve(GET_NERVE(WaterCameraFilter, WaterCameraFilterNrvWaterToAir));
    } else {
        mWaterAlpha += 20.0f;
        if (MR::isDemoActive() || mWaterAlpha >= 255.0f) {
            mWaterAlpha = 255.0f;
            setNerve(GET_NERVE(WaterCameraFilter, WaterCameraFilterNrvWater));
        }
    }
}

void WaterCameraFilter::exeWaterToAir() {
    if (MR::isCameraInWater()) {
        setNerve(GET_NERVE(WaterCameraFilter, WaterCameraFilterNrvAirToWater));
    } else {
        mWaterAlpha -= 20.0f;
        if (MR::isDemoActive() || mWaterAlpha <= 0.0f) {
            mWaterAlpha = 0.0f;
            setNerve(GET_NERVE(WaterCameraFilter, WaterCameraFilterNrvAir));
        }
    }
}

static GXColor sWaterColor = {0x32, 0x32, 0, 0};

void WaterCameraFilter::control() {
    if (MR::isCameraInWater()) {
        mWavePhase += 0.5f;

        if (mWavePhase > 360.0f) {
            mWavePhase -= 360.0f;
        }

        f32 depth = MR::clamp(WaterAreaFunction::getCameraWaterDepth(), 0.0f, 3000.0f);
        Color8 color(MR::lerp(::sDefaultColor, ::sWaterColor, depth / 3000.0f));
        mWaterColor.mGXColor = color.mGXColor;
    }
}

void WaterCameraFilter::draw() const {
    if (isNerve(GET_NERVE(WaterCameraFilter, WaterCameraFilterNrvAir))) {
        return;
    }

#ifdef __3DS__
    // The Wii's pass (below, loadMaterial): the screen copied, then drawn back over itself as
    // screen - C0 at alpha mWaterAlpha (blended, alpha test > 0, no Z), the screen read through
    // an indirect stage: the filter texture (I8, so the same offset in s and t) at TEXCOORD0
    // moves it by 0.05 * (I - 128) texels. The PICA has no indirect stage: the offsets are taken
    // on the CPU at the corners of a 40 x 30 grid over the screen (texel by texel bilinear, as
    // GX samples, mirrored), the moved coordinates interpolated in between.
    void* screen = nullptr;
    const ResTIMG* filter = mFilterTex != nullptr ? mFilterTex->getTexInfo() : nullptr;
    if (mScreenTex != nullptr) {
        rvl3DSCaptureJUTTextureRegion(mScreenTex, 0.0f, 0.0f, MR::getFrameBufferWidth(), MR::getScreenHeight());
        screen = rvl3DSGetJUTTexture(mScreenTex);
    }
    if (screen == nullptr || filter == nullptr || filter->mFormat != GX_TF_I8) {
        return;
    }
    const u8 alpha = mWaterAlpha;
    if (alpha == 0) {
        return;
    }
    TVec2f pos(0.0f, 0.0f);
    MR::calcScreenPosition(&pos, *MR::getPlayerCenterPos());
    pos.x /= MR::getScreenWidth();
    pos.y /= MR::getScreenHeight();
    const f32 centerU = 1.0f - pos.x;
    const f32 centerV = 1.0f - pos.y;
    TMtx34f textureMtx;
    textureMtx.identity();
    TPos3f translation;
    translation.identity();
    translation.setTrans(TVec3f(-0.5f, -0.5f, 0.0f));
    TPos3f rotation;
    rotation.identity();
    MR::makeMtxRotate(rotation, 0.0f, 0.0f, mWavePhase);
    MR::multMtx(textureMtx, translation, rotation);
    translation.setTrans(TVec3f(0.5f, 0.5f, 0.0f));
    MR::multMtx(textureMtx, textureMtx, translation);

    const u8* image = reinterpret_cast< const u8* >(filter) + filter->mImageDataOffset;
    const s32 texWidth = filter->mWidth, texHeight = filter->mHeight;
    auto texel = [&](s32 x, s32 y) -> f32 {
        // GX_MIRROR, then the I8 tiles of 8 x 4.
        x = ((x % (2 * texWidth)) + 2 * texWidth) % (2 * texWidth);
        y = ((y % (2 * texHeight)) + 2 * texHeight) % (2 * texHeight);
        if (x >= texWidth) x = 2 * texWidth - 1 - x;
        if (y >= texHeight) y = 2 * texHeight - 1 - y;
        return image[((y >> 2) * (texWidth >> 3) + (x >> 3)) * 32 + (y & 3) * 8 + (x & 7)];
    };
    const f32 width = MR::getFrameBufferWidth(), height = MR::getScreenHeight();
    enum { COLUMNS = 40, ROWS = 30 };
    auto makeVertex = [&](s32 column, s32 row, HostGfxVertex& v) {
        const f32 u = static_cast< f32 >(column) / COLUMNS, w = static_cast< f32 >(row) / ROWS;
        v.x = u * width / 320.0f - 1.0f;
        v.y = 1.0f - w * height / 228.0f;
        v.z = 0.0f;
        v.w = 1.0f;
        // TEXCOORD0: TEX0 (centerU -+ 0.5, centerV -+ 0.5 at the corners) by textureMtx.
        const f32 s0 = centerU - 0.5f + u, t0 = centerV - 0.5f + w;
        const f32 q = textureMtx[2][0] * s0 + textureMtx[2][1] * t0 + textureMtx[2][2] + textureMtx[2][3];
        f32 s = textureMtx[0][0] * s0 + textureMtx[0][1] * t0 + textureMtx[0][2] + textureMtx[0][3];
        f32 t = textureMtx[1][0] * s0 + textureMtx[1][1] * t0 + textureMtx[1][2] + textureMtx[1][3];
        if (q != 0.0f) {
            s /= q;
            t /= q;
        }
        const f32 x = s * texWidth - 0.5f, y = t * texHeight - 0.5f;
        const s32 x0 = static_cast< s32 >(floorf(x)), y0 = static_cast< s32 >(floorf(y));
        const f32 fx = x - x0, fy = y - y0;
        const f32 value = (texel(x0, y0) * (1.0f - fx) + texel(x0 + 1, y0) * fx) * (1.0f - fy) +
                          (texel(x0, y0 + 1) * (1.0f - fx) + texel(x0 + 1, y0 + 1) * fx) * fy;
        const f32 offset = 0.05f * (static_cast< s32 >(value) - 128);
        // TEXCOORD1: TEX1 (0 to 1 over the screen) moved by the offset, in screen texels.
        rvl3DSMapJUTTextureCoord(mScreenTex, u + offset / width, w + offset / height, v.s0, v.t0);
    };
    HostGfxState state = {};
    state.textures[0] = screen;
    state.stageCount = 1;
    state.stages[0].colorSrc[0] = 0x03;  // texture 0
    state.stages[0].colorSrc[1] = 0x0E;  // C0
    state.stages[0].colorFunc = 5;       // subtract
    state.stages[0].alphaSrc[0] = 0x0E;  // A0
    state.stages[0].constant = mWaterColor.r | (mWaterColor.g << 8) | (mWaterColor.b << 16) | (static_cast< u32 >(alpha) << 24);
    state.blendSrc = 6;        // GPU_SRC_ALPHA
    state.blendDst = 7;        // GPU_ONE_MINUS_SRC_ALPHA
    state.alphaTestFunc = 6;   // GPU_GREATER
    state.alphaRef = 0;
    state.depthFunc = 1;       // GPU_ALWAYS
    state.writeMask = 0x0F;
    state.scissor[2] = 640;
    state.scissor[3] = 456;
    for (s32 row = 0; row < ROWS; row++) {
        HostGfxVertex strip[2 * (COLUMNS + 1)] = {};
        for (s32 column = 0; column <= COLUMNS; column++) {
            makeVertex(column, row, strip[2 * column]);
            makeVertex(column, row + 1, strip[2 * column + 1]);
        }
        hostGfxDraw(strip, 2 * (COLUMNS + 1), nullptr, 2 * (COLUMNS + 1), HOST_GFX_STRIP, &state);
    }
#else
    GXRenderModeObj* pRenderMode = JUTVideo::getManager()->getRenderMode();
    GXSetCopyFilter(GX_FALSE, pRenderMode->sample_pattern, GX_FALSE, pRenderMode->vfilter);
    mScreenTex->capture(0, 0, GX_TF_RGB565, false, 0);
    GXSetCopyFilter(GX_FALSE, pRenderMode->sample_pattern, GX_TRUE, pRenderMode->vfilter);
    loadMaterial();
    TVec2f pos;
    pos.x = 0.0f;
    pos.y = 0.0f;
    TVec3f* pPlayerCenter = MR::getPlayerCenterPos();
    MR::calcScreenPosition(&pos, *pPlayerCenter);
    pos.x /= MR::getScreenWidth();
    pos.y /= MR::getScreenHeight();

    f32 halfSize = 0.5f;
    f32 centerU = 1.0f - pos.x;
    f32 centerV = 1.0f - pos.y;

    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);
    {
        GXPosition2u16(0, 0);
        GXTexCoord2f32(centerU - halfSize, centerV - halfSize);
        GXTexCoord2f32(0.0f, 0.0f);

        GXPosition2u16(MR::getFrameBufferWidth(), 0);
        GXTexCoord2f32(centerU + halfSize, centerV - halfSize);
        GXTexCoord2f32(1.0f, 0.0f);

        GXPosition2u16(0, MR::getScreenHeight());
        GXTexCoord2f32(centerU - halfSize, centerV + halfSize);
        GXTexCoord2f32(0.0f, 1.0f);

        s32 height = MR::getScreenHeight();
        GXPosition2u16(MR::getFrameBufferWidth(), height);
        GXTexCoord2f32(centerU + halfSize, centerV + halfSize);
        GXTexCoord2f32(1.0f, 1.0f);
    }

    GXEnd();

    MR::loadProjectionMtx();
    MR::loadViewMtx();
#endif
}

// Wii only: GX state, display lists and material setup; the 3DS draws this through Citro3D
// (DETAILS.md, section 5).
#ifndef __3DS__
void WaterCameraFilter::loadMaterial() const {
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_U16, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX1, GX_DIRECT);
    Mtx44 projection;
    s32 height = MR::getScreenHeight();
    C_MTXOrtho(projection, 0.0f, height, 0.0f, MR::getFrameBufferWidth(), -10000.0f, 10000.0f);
    GXSetProjection(projection, GX_ORTHOGRAPHIC);
    TMtx34f modelView;
    modelView.identity();
    GXLoadPosMtxImm(modelView, 0);
    GXSetCurrentMtx(0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(2);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_TEX0, 0x1E, 0, 0x7D);
    GXSetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX3x4, GX_TG_TEX1, 0x3C, 0, 0x7D);
    TVec2f screenPos;
    screenPos.x = 0.0f;
    screenPos.y = 0.0f;
    MR::calcScreenPosition(&screenPos, *MR::getPlayerCenterPos());
    screenPos.x /= MR::getScreenWidth();
    screenPos.y /= MR::getScreenHeight();
    TVec3f screenPosNorm;
    MR::calcNormalizedScreenPosition(&screenPosNorm, *MR::getPlayerCenterPos());
    TMtx34f textureMtx;
    textureMtx.identity();
    TPos3f translation;
    translation.identity();
    translation.setTrans(TVec3f(-0.5f, -0.5f, 0.0f));
    TPos3f rotation;
    rotation.identity();
    MR::makeMtxRotate(rotation, 0.0f, 0.0f, mWavePhase);
    MR::multMtx(textureMtx, translation, rotation);
    translation.setTrans(TVec3f(0.5f, 0.5f, 0.0f));
    MR::multMtx(textureMtx, textureMtx, translation);
    GXLoadTexMtxImm(textureMtx, 0x1E, GX_MTX3x4);
    mFilterTex->load(GX_TEXMAP0);
    mScreenTex->load(GX_TEXMAP1);
    GXSetNumIndStages(1);
    GXSetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0, GX_TEXMAP0);
    GXSetTevIndWarp(GX_TEVSTAGE0, GX_INDTEXSTAGE0, 1, 0, GX_ITM_0);
    f32 ind[2][3];
    ind[0][0] = 0.05f;
    ind[0][1] = 0.0f;
    ind[0][2] = 0.0f;
    ind[1][0] = 0.0f;
    ind[1][1] = 0.05f;
    ind[1][2] = 0.0f;
    GXSetIndTexMtx(GX_ITM_0, ind, 0);
    GXSetTevColor(GX_TEVREG0, Color8(mWaterColor.r, mWaterColor.g, mWaterColor.b, mWaterAlpha));
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_C0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, 1, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_A0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 1, GX_TEVPREV);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
    GXSetAlphaCompare(GX_GREATER, 0, GX_AOP_OR, GX_GREATER, 0);
    GXSetZMode(0, GX_ALWAYS, 0);
    GXSetZCompLoc(0);
    GXSetCullMode(GX_CULL_NONE);
    GXSetClipMode(GX_CLIP_ENABLE);
}
#endif

void WaterCameraFilter::exeWater() {
    if (!MR::isCameraInWater()) {
        setNerve(GET_NERVE(WaterCameraFilter, WaterCameraFilterNrvWaterToAir));
    }
}

void WaterCameraFilter::exeAir() {
    if (MR::isCameraInWater()) {
        setNerve(GET_NERVE(WaterCameraFilter, WaterCameraFilterNrvAirToWater));
    }
}
