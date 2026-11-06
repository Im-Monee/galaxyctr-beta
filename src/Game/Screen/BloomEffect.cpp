#include "Game/Screen/BloomEffect.hpp"
// 3DS: host.h, the port's host interface; DirectDraw, TDDraw's Citro3D draws; GXToCitro3D.hpp, GX
// values as 3DS GPU values; JUTTexture3DS.hpp, GPU textures for JUTTextures.
#ifdef __3DS__
#include "host.h"
#include "Game/Util/DirectDraw.hpp"
#include "GXToCitro3D.hpp"
#include "JUTTexture3DS.hpp"
#endif
#include "Game/Screen/CopyFilterNegater.hpp"
#include "Game/Screen/ImageEffectLocalUtil.hpp"
#include "Game/Screen/ImageEffectResource.hpp"
#include "Game/Screen/ImageEffectSystemHolder.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/Color.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "JSystem/JUtility/JUTVideo.hpp"
#include "revolution/gx/GXEnum.h"
#include "revolution/gx/GXFrameBuf.h"
#include "revolution/gx/GXStruct.h"
#include "revolution/gx/GXTev.h"
#include <JSystem/JUtility/JUTTexture.hpp>

namespace {
    struct RadAndOfs {
        /* 0x00 */ f32 _0;
        /* 0x04 */ f32 _4;
    };

    static const RadAndOfs sL1RadAndOfs[] = {
        {0.01f, 0.0f},
        {0.02f, 0.52f},
    };
    static const RadAndOfs sL2RadAndOfs[] = {
        {0.04f, 0.0f},
        {0.07f, 0.0f},
        {0.09f, 0.0f},
    };
    static const GXTexMtx sTexMtxID[] = {
        GX_TEXMTX0, GX_TEXMTX1, GX_TEXMTX2, GX_TEXMTX3, GX_TEXMTX4, GX_TEXMTX5, GX_TEXMTX6, GX_TEXMTX7,
    };
    static const u32 sL1RoundPoints = 6;
    static const u32 sL2RoundPoints = 12;
    // static const f32 sBloomIntensity = _;
    // static const f32 sMaskThreshold = _;
    // static const ... sBlurType = _;
    // static const s32 sFadeCounterMax = _;
};  // namespace

BloomEffect::BloomEffect(const char* pName) : ImageEffectBase(pName), _14(255), _18(1.0f / ::sL1RoundPoints), _1C(1.0f / ::sL2RoundPoints), _20(128) {
    MR::connectToSceneNormalBloom(this);
    MR::createImageEffectSystemHolder();
    MR::getImageEffectResource()->createBloomTexture();

    _24 = MR::getImageEffectResource()->_0;
    _28 = MR::getImageEffectResource()->_4;
    _2C = MR::getImageEffectResource()->_C;
    _30 = MR::getImageEffectResource()->_10;
    _34 = MR::getImageEffectResource()->_14;
    _38 = MR::getImageEffectResource()->_18;
    _3C = _2C;
    _40 = MR::getImageEffectResource()->_1C;
    _44 = MR::getImageEffectResource()->_20;
    _48 = new Mtx[::sL1RoundPoints * ARRAY_SIZE(::sL1RadAndOfs)];
    _4C = new Mtx[::sL2RoundPoints * ARRAY_SIZE(::sL2RadAndOfs)];

    for (u32 i = 0; i < ARRAY_SIZE(::sL1RadAndOfs); i++) {
        initBlurMtx(&_48[i * ::sL1RoundPoints], ::sL1RoundPoints, ::sL1RadAndOfs[i]._0, ::sL1RadAndOfs[i]._4);
    }

    for (u32 i = 0; i < ARRAY_SIZE(::sL2RadAndOfs); i++) {
        initBlurMtx(&_4C[i * ::sL2RoundPoints], ::sL2RoundPoints, ::sL2RadAndOfs[i]._0, ::sL2RadAndOfs[i]._4);
    }

    PPCSync();
}

void BloomEffect::preDraw() const {
#ifdef __3DS__
    // As on the Wii: the scene so far is kept in _24 and the screen cleared, so that the models
    // drawn for the bloom come next on black; postDraw draws the scene back.
    if (!isSomething()) {
        return;
    }

    ImageEffectLocalUtil::capture(_24, 1, 0, GX_TF_RGBA8, false, 0);
    MR::fillScreen(Color8(0, 0, 0, 0xFF).mGXColor);
    MR::loadViewMtx();
    MR::loadProjectionMtx();
    return;
#else
    if (!isSomething()) {
        return;
    }

    {
        CopyFilterNegater copyFilterNegator;
        ImageEffectLocalUtil::capture(_24, 1, 0, GX_TF_RGBA8, false, 0);
    }

    MR::fillScreen(Color8(0, 0, 0, 0xFF).mGXColor);
    MR::loadViewMtx();
    MR::loadProjectionMtx();
    GXSetClipMode(GX_CLIP_ENABLE);
#endif
}

void BloomEffect::postDraw() const {
#ifdef __3DS__
    // The Wii's chain below in frame buffer tiles: the bloom models' image reduced (tile 0 of 4),
    // kept where its luminance passes _20 (tile 2), blurred on the two rings of sL1RadAndOfs
    // (tile 3) and the three of sL2RadAndOfs (tile 20 of 8), the two blurs summed at the bloom
    // intensity (tile 7), then the scene of preDraw with the bloom added. The mask compares
    // through the alpha test (drawTextureBrightPass).
    if (!isSomething()) {
        return;
    }

    ImageEffectLocalUtil::capture(_28, 1, 0, GX_TF_RGB565, true, 0);
    initDraw();
    drawTexture(_28, 4, 0, 0xFF, DrawType_0);
    ImageEffectLocalUtil::capture(_2C, 4, 0, GX_TF_RGB565, false, 0);
    {
        static const u8 kLuminance[3] = {66, 129, 25};  // the Wii's I8 copy: Y of YUV
        const f32 width = static_cast< f32 >(MR::getFrameBufferWidth()) / 4;
        const f32 height = static_cast< f32 >(MR::getFrameBufferHeight()) / 4;
        u8 equation, source, destination;
        int logic;
        GXToCitro3D::blendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR, equation, source, destination, logic);
        TDDraw::setDirectBlendState(equation, source, destination);
        TDDraw::drawTextureBrightPass(TVec2f(2 * width, 0.0f), _2C, TVec2f(width, height), _20, kLuminance);
    }
    ImageEffectLocalUtil::capture(_44, 4, 2, GX_TF_RGB565, false, 0);
    blurTexture(_44, _48, 4, 3, 6, _18, 2, &::sL1RadAndOfs[0]._0);

    ImageEffectLocalUtil::capture(_30, 4, 3, GX_TF_RGB565, false, 0);
    ImageEffectLocalUtil::capture(_34, 4, 3, GX_TF_RGB565, true, 0);
    blurTexture(_34, _4C, 8, 20, 12, _1C, 3, &::sL2RadAndOfs[0]._0);

    ImageEffectLocalUtil::capture(_38, 8, 20, GX_TF_RGB565, false, 0);
    const u8 intensity = _14 * get_10();
    drawTexture(_30, 4, 7, intensity, DrawType_0);
    drawTexture(_38, 4, 7, intensity, DrawType_1);

    ImageEffectLocalUtil::capture(_3C, 4, 7, GX_TF_RGB565, false, 0);
    drawTexture(_24, 1, 0, 0xFF, DrawType_0);
    drawTexture(_3C, 1, 0, 0xFF, DrawType_1);
    TDDraw::closeDirect();
    return;
#else
    if (!isSomething()) {
        return;
    }

    GXRenderModeObj* pRenderObj = JUTVideo::getManager()->getRenderMode();
    GXSetCopyFilter(GX_FALSE, pRenderObj->sample_pattern, GX_FALSE, pRenderObj->vfilter);

    ImageEffectLocalUtil::capture(_28, 1, 0, GX_TF_RGB565, true, 0);
    initDraw();
    GXSetCopyClear(Color8(0, 0, 0, 0xFF), 0xFFFFFF);
    drawTexture(_28, 4, 0, 0xFF, DrawType_0);

    ImageEffectLocalUtil::capture(_2C, 4, 0, GX_TF_RGB565, false, 0);
    ImageEffectLocalUtil::capture(_40, 4, 0, GX_TF_I8, false, 0);
    drawTexture(_2C, 4, 2, 0xFF, DrawType_0);

    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_TEXC, GX_CC_RASC, GX_CC_ONE, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_COMP_R8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    drawTexture(_40, 4, 2, _20, DrawType_2);

    GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    ImageEffectLocalUtil::capture(_44, 4, 2, GX_TF_RGB565, false, 0);
    blurTexture(_44, _48, 4, 3, 6, _18, 2, &::sL1RadAndOfs[0]._0);

    ImageEffectLocalUtil::capture(_30, 4, 3, GX_TF_RGB565, false, 0);
    ImageEffectLocalUtil::capture(_34, 4, 3, GX_TF_RGB565, true, 0);
    blurTexture(_34, _4C, 8, 20, 12, _1C, 3, &::sL2RadAndOfs[0]._0);

    ImageEffectLocalUtil::capture(_38, 8, 20, GX_TF_RGB565, false, 0);
    const u8 intensity = _14 * get_10();
    drawTexture(_30, 4, 7, intensity, DrawType_0);
    drawTexture(_38, 4, 7, intensity, DrawType_1);

    ImageEffectLocalUtil::capture(_3C, 4, 7, GX_TF_RGB565, false, 0);
    GXSetCopyFilter(GX_FALSE, pRenderObj->sample_pattern, GX_TRUE, pRenderObj->vfilter);

    drawTexture(_24, 1, 0, 0xFF, DrawType_0);
    drawTexture(_3C, 1, 0, 0xFF, DrawType_1);
#endif
}

u8 BloomEffect::getIntensity1Default() const {
    return 42;
}

u8 BloomEffect::getIntensity2Default() const {
    return 21;
}

bool BloomEffect::isForEachModel() const {
    return true;
}

void BloomEffect::initDraw() const {
#ifdef __3DS__
    ImageEffectLocalUtil::setupDrawTexture();  // 2D frame buffer coordinates, no depth, no culling
    return;
#else
    MR::reinitGX();
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);

    Mtx44 projectionMtx;
    C_MTXOrtho(projectionMtx, 0.0f, MR::getFrameBufferHeight(), 0.0f, MR::getFrameBufferWidth(), -10000.0f, 10000.0f);
    GXSetProjection(projectionMtx, GX_ORTHOGRAPHIC);

    MR::loadViewMtxFor2DModel();
    GXClearVtxDesc();
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);

    TMtx34f posMtx;
    posMtx.identity();
    GXLoadPosMtxImm(posMtx.toMtxPtr(), GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);

    GXSetCullMode(GX_CULL_NONE);
    GXSetClipMode(GX_CLIP_ENABLE);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(1);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0, GX_FALSE, GX_PTIDENTITY);
    GXLoadTexMtxImm(posMtx.toMtxPtr(), GX_TEXMTX0, GX_MTX2x4);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GXSetNumIndStages(0);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, Color8(0, 0, 0, 0));
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetZCompLoc(GX_TRUE);
    GXSetDither(GX_FALSE);
#endif
}

void BloomEffect::drawTexture(JUTTexture* pTexture, s32 param2, s32 param3, u8 intensity, BLOOM_TEX_DRAW_TYPE param5) const {
#ifdef __3DS__
    // The texture over tile param3 of param2 x param2, times the material colour intensity, with
    // the type's blending (replace; add; times the frame buffer).
    static const u32 kModes[3][3] = {{GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO},
                                     {GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE},
                                     {GX_BM_BLEND, GX_BL_SRCCLR, GX_BL_ZERO}};
    const u32* mode = kModes[param5 <= DrawType_2 ? param5 : DrawType_0];
    u8 equation, source, destination;
    int logic;
    GXToCitro3D::blendMode(mode[0], mode[1], mode[2], GX_LO_CLEAR, equation, source, destination, logic);
    TDDraw::setDirectBlendState(equation, source, destination);
    const f32 width = static_cast< f32 >(MR::getFrameBufferWidth()) / param2;
    const f32 height = static_cast< f32 >(MR::getFrameBufferHeight()) / param2;
    static const f32 kNoOffset[1][2] = {{0.0f, 0.0f}};
    TDDraw::drawTextureTaps(TVec2f((param3 % param2) * width, (param3 / param2) * height), pTexture, TVec2f(width, height), kNoOffset,
                            1, intensity);
    return;
#else
    pTexture->load(GX_TEXMAP0);
    GXSetChanMatColor(GX_COLOR0A0, Color8(intensity, intensity, intensity, 255));

    switch (param5) {
    case DrawType_0:
        GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR);
        break;
    case DrawType_1:
        GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
        break;
    case DrawType_2:
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCCLR, GX_BL_ZERO, GX_LO_CLEAR);
        break;
    }

    s32 row = param3 / param2;
    s32 column = param3 % param2;
    f32 width = static_cast< f32 >(MR::getFrameBufferWidth()) / param2;
    f32 height = static_cast< f32 >(MR::getFrameBufferHeight()) / param2;
    f32 left = column * width;
    f32 right = (column + 1) * width;
    f32 bottom = (row + 1) * height;
    f32 top = row * height;

    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);
    {
        GXPosition3f32(left, bottom, -1.0f);
        GXTexCoord2f32(0.0f, 1.0f);

        GXPosition3f32(left, top, -1.0f);
        GXTexCoord2f32(0.0f, 0.0f);

        GXPosition3f32(right, bottom, -1.0f);
        GXTexCoord2f32(1.0f, 1.0f);

        GXPosition3f32(right, top, -1.0f);
        GXTexCoord2f32(1.0f, 0.0f);
    }
    GXEnd();
#endif
}

void BloomEffect::blurTexture(JUTTexture* pTexture, Mtx* pMtx, s32 param3, s32 param4, u32 param5, f32 param6, u32 param7, const f32* pParam8) const {
#ifdef __3DS__
    // The taps of the param7 rings of param5 matrices (initBlurMtx: texture coordinate
    // translations), each times param6, summed over tile param4 of param3 x param3: the first
    // draw replaces, the others add (as initBlur and drawBlur set the blending).
    (void)pParam8;
    const f32 width = static_cast< f32 >(MR::getFrameBufferWidth()) / param3;
    const f32 height = static_cast< f32 >(MR::getFrameBufferHeight()) / param3;
    const TVec2f position((param4 % param3) * width, (param4 / param3) * height);
    const u8 weight = static_cast< u8 >(param6 * 255.0f);
    const u32 taps = param5 * param7;
    for (u32 i = 0; i < taps; i += 3) {
        f32 offsets[3][2];
        u32 count = 0;
        for (; count < 3 && i + count < taps; count++) {
            offsets[count][0] = pMtx[i + count][0][3];
            offsets[count][1] = pMtx[i + count][1][3];
        }
        u8 equation, source, destination;
        int logic;
        GXToCitro3D::blendMode(i == 0 ? GX_BM_NONE : GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR, equation, source, destination, logic);
        TDDraw::setDirectBlendState(equation, source, destination);
        TDDraw::drawTextureTaps(position, pTexture, TVec2f(width, height), offsets, count, weight);
    }
    return;
#else
    initBlur(pTexture, 8, param6);

    for (s32 i = 0; i < param7; i++) {
        drawBlur(param3, param4, &pMtx[i * param5], 8, param5);
    }

    GXSetNumTexGens(1);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
#endif
}

void BloomEffect::initBlurMtx(Mtx* pMtx, u32 count, f32 radius, f32 offset) const {
    f32 invAspect = 1.0f / MR::getAspect();
    radius = -radius;

    for (u32 i = 0; i < count; i++) {
        PSMTXTrans(pMtx[i], invAspect * (radius * JMACosRadian(offset + i * JMath::TAngleConstant_< f32 >::RADIAN_DEG360() / count)),
                   radius * JMASinRadian(offset + i * JMath::TAngleConstant_< f32 >::RADIAN_DEG360() / count), 0.0f);
    }

    DCStoreRangeNoSync(pMtx, sizeof(Mtx) * count);
}

void BloomEffect::initBlur(JUTTexture* pTexture, u32 param2, f32 intensity) const {
    // 3DS: nothing to do here; the bloom is drawn by the 3DS branch of the effect (the PICA has no
    // texture matrices to set).
#ifdef __3DS__
    (void)pTexture; (void)param2; (void)intensity;
    return;
#else
    GXSetNumTexGens(param2);

    for (u32 i = 0; i < param2; i++) {
        GXSetTexCoordGen2(GXTexCoordID(i), GX_TG_MTX3x4, GX_TG_TEX0, ::sTexMtxID[i], GX_FALSE, GX_PTIDENTITY);
    }

    GXSetNumTevStages(param2);

    for (u32 i = 0; i < param2; i++) {
        GXSetTevOrder(GXTevStageID(i), GXTexCoordID(i), GX_TEXMAP0, GX_COLOR0A0);
        GXSetTevColorIn(GXTevStageID(i), GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_CPREV);
        GXSetTevColorOp(GXTevStageID(i), GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
        GXSetTevAlphaIn(GXTevStageID(i), GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
        GXSetTevAlphaOp(GXTevStageID(i), GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    }

    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
    GXSetTevColorOp(GXTevStageID(param2), GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetChanMatColor(GX_COLOR0A0, Color8(intensity * 255.0f, intensity * 255.0f, intensity * 255.0f, 255));
    pTexture->load(GX_TEXMAP0);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR);
#endif
}

void BloomEffect::drawBlur(s32 divisions, s32 tile, Mtx* pMtx, u32 stageCount, u32 matrixCount) const {
    // 3DS: nothing to do here; the bloom is drawn by the 3DS branch of the effect.
#ifdef __3DS__
    (void)divisions; (void)tile; (void)pMtx; (void)stageCount; (void)matrixCount;
    return;
#else
    GXSetArray(GX_TEX_MTX_ARRAY, pMtx, sizeof(Mtx));

    s32 row = tile / divisions;
    s32 column = tile % divisions;
    f32 width = static_cast< f32 >(MR::getFrameBufferWidth()) / divisions;
    f32 height = static_cast< f32 >(MR::getFrameBufferHeight()) / divisions;
    f32 left = column * width;
    f32 right = (column + 1) * width;
    f32 bottom = (row + 1) * height;
    f32 top = row * height;

    GXSetNumTexGens(stageCount);
    GXSetNumTevStages(stageCount);

    for (u32 i = 0; i < matrixCount; i += stageCount) {
        u32 remaining = matrixCount - i;
        if (remaining < stageCount) {
            GXSetNumTexGens(remaining);
            GXSetNumTevStages(remaining);
            stageCount = remaining;
        }

        for (u32 j = 0; j < stageCount; j++) {
            GXLoadTexMtxIndx(i + j, ::sTexMtxID[j], GX_MTX3x4);
        }

        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);
        GXPosition3f32(left, bottom, -1.0f);
        GXTexCoord2f32(0.0f, 1.0f);

        GXPosition3f32(left, top, -1.0f);
        GXTexCoord2f32(0.0f, 0.0f);

        GXPosition3f32(right, bottom, -1.0f);
        GXTexCoord2f32(1.0f, 1.0f);

        GXPosition3f32(right, top, -1.0f);
        GXTexCoord2f32(1.0f, 0.0f);
        GXEnd();

        if (i == 0) {
            GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
        }
    }
#endif
}
