#include "Game/Screen/ImageEffectLocalUtil.hpp"
#include "Game/NameObj/NameObjAdaptor.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Screen/BloomEffect.hpp"
#include "Game/Util/Color.hpp"
#include "Game/Util/DrawUtil.hpp"
#ifdef __3DS__
#include "Game/Util/DirectDraw.hpp"
#endif
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include <JSystem/JUtility/JUTTexture.hpp>
#ifdef __3DS__
#include "GXToCitro3D.hpp"
#include "JUTTexture3DS.hpp"
#endif

namespace {
    void setTextureTrans(f32 x, f32 y) {
#ifndef __3DS__
        TPos3f transMtx;
        transMtx.makeTrans(x, y, 0.0f);
        GXLoadTexMtxImm(transMtx.toMtxPtr(), GX_TEXMTX0, GX_MTX2x4);
#else
        (void)x;
        (void)y;
#endif
    }
};  // namespace

void MR::connectToSceneImageEffect(NameObj* pObj) {
    MR::connectToScene(pObj, MR::MovementType_ImageEffect, MR::CalcAnimType_Environment, MR::DrawBufferType_None, MR::DrawType_ImageEffect);
}

void MR::connectToSceneImageEffectMovement(NameObj* pObj) {
    MR::connectToScene(pObj, MR::MovementType_ImageEffect, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_None);
}

void ImageEffectLocalUtil::capture(JUTTexture* pTexture, s32 divisions, s32 tile, GXTexFmt format, bool clear, u8 filter) {
#ifdef __3DS__
    if (pTexture == nullptr || divisions <= 0 || tile < 0 || tile >= divisions * divisions) {
        return;
    }
    // As the Wii's JUTTexture::capture(x, y, format, mipmap = clear, clear = filter): the copy
    // starts at the tile and has the texture's size, twice it with mipmap (a 2x reduction); the
    // texture keeps the copied rectangle (RGBA8) and is drawn stretched. A clearing copy leaves
    // the rectangle black (the copy clear colour the image effects set).
    (void)format;
    const f32 scale = clear ? 2.0f : 1.0f;
    const f32 x0 = static_cast< f32 >((tile % divisions) * MR::getFrameBufferWidth() / divisions);
    const f32 y0 = static_cast< f32 >((tile / divisions) * MR::getFrameBufferHeight() / divisions);
    const f32 x1 = x0 + pTexture->getWidth() * scale;
    const f32 y1 = y0 + pTexture->getHeight() * scale;
    rvl3DSCaptureJUTTextureRegion(pTexture, x0, y0, x1, y1);
    if (filter) {
        MR::fillScreenSetup(Color8(0, 0, 0, 255).mGXColor);
        MR::fillScreenArea(TVec2s(static_cast< s16 >(x0), static_cast< s16 >(y0)), TVec2s(static_cast< s16 >(x1), static_cast< s16 >(y1)));
    }
#else
    pTexture->capture((tile % divisions) * MR::getFrameBufferWidth() / divisions, (tile / divisions) * MR::getFrameBufferHeight() / divisions, format,
                      clear, filter);
#endif
}

void ImageEffectLocalUtil::setupDrawTexture() {
#ifdef __3DS__
    // The Wii's projection in frame buffer pixels (0 to 640): the image effects draw their
    // textures over frame buffer rectangles, as they capture them. View mode 2 is in screen units
    // (832 wide in 16:9), which drew a full-screen texture over 640 / 832 of the width (CameraCover
    // on camera cuts, CIA 0.13.8).
    TDDraw::setupDirect(1, 1, 3);
    Mtx44 projectionMtx;
    C_MTXOrtho(projectionMtx, 0.0f, MR::getFrameBufferHeight(), 0.0f, MR::getFrameBufferWidth(), -10000.0f, 10000.0f);
    TDDraw::setDirectProjectionMtx(projectionMtx);
    Mtx viewMtx;
    PSMTXIdentity(viewMtx);
    TDDraw::setDirectViewMtx(viewMtx);
    TDDraw::setDirectDepthMode(false, 5, false);
    TDDraw::setDirectCullMode(0);
#else
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
    GXSetZTexture(GX_ZT_DISABLE, GX_TF_Z24X8, 0);
    GXSetNumIndStages(0);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetTevDirect(GX_TEVSTAGE1);
    GXSetTevDirect(GX_TEVSTAGE2);
    GXSetTevDirect(GX_TEVSTAGE3);
    GXSetTevDirect(GX_TEVSTAGE4);
    GXSetTevDirect(GX_TEVSTAGE5);
    GXSetTevDirect(GX_TEVSTAGE6);
    GXSetTevDirect(GX_TEVSTAGE7);
    GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, Color8(0, 0, 0, 0));
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetZCompLoc(GX_TRUE);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetDither(GX_FALSE);
#endif
}

void ImageEffectLocalUtil::drawTexture(JUTTexture* pTexture, s32 divisions, s32 tile, u8 intensity, ETexDrawType texDrawType) {
#ifdef __3DS__
    if (pTexture == nullptr || divisions <= 0 || tile < 0 || tile >= divisions * divisions) {
        return;
    }
    const f32 width = static_cast<f32>(MR::getFrameBufferWidth()) / divisions;
    const f32 height = static_cast<f32>(MR::getFrameBufferHeight()) / divisions;
    const s32 row = tile / divisions;
    const s32 column = tile % divisions;
    const TVec2f position(column * width, row * height);
    {
        // The Wii's blend mode per type: none; one, one; source alpha; source colour (as GX reads
        // GX_BL_SRCCLR in the source slot: the destination colour), zero.
        static const u32 kModes[4][3] = {{GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO},
                                         {GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE},
                                         {GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA},
                                         {GX_BM_BLEND, GX_BL_SRCCLR, GX_BL_ZERO}};
        const u32* mode = kModes[static_cast< u32 >(texDrawType) & 3];
        u8 equation, source, destination;
        int logic;
        GXToCitro3D::blendMode(mode[0], mode[1], mode[2], GX_LO_CLEAR, equation, source, destination, logic);
        TDDraw::setDirectBlendState(equation, source, destination);
    }
    if (texDrawType == TexDrawType_2) {
        TDDraw::drawTexture(position, pTexture, TVec2f(width, height), intensity);  // intensity as alpha
    } else {
        // The Wii's material colour (intensity, intensity, intensity) times the texture.
        static const f32 kNoOffset[1][2] = {{0.0f, 0.0f}};
        TDDraw::drawTextureTaps(position, pTexture, TVec2f(width, height), kNoOffset, 1, intensity);
    }
#else
    pTexture->load(GX_TEXMAP0);

    if (texDrawType == TexDrawType_2) {
        GXSetChanMatColor(GX_COLOR0A0, Color8(255, 255, 255, intensity));
        GXSetChanAmbColor(GX_COLOR0A0, Color8(0, 0, 0, 255));
    } else {
        GXSetChanMatColor(GX_COLOR0A0, Color8(intensity, intensity, intensity, 255));
        GXSetChanAmbColor(GX_COLOR0A0, Color8(0, 0, 0, 255));
    }

    switch (texDrawType) {
    case TexDrawType_0:
        GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR);
        break;
    case TexDrawType_1:
        GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
        break;
    case TexDrawType_2:
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
        break;
    case TexDrawType_3:
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCCLR, GX_BL_ZERO, GX_LO_CLEAR);
        break;
    }

    sendTextureVertex(divisions, tile);
#endif
}

void ImageEffectLocalUtil::sendTextureVertex(s32 divisions, s32 tile) {
#ifdef __3DS__
    return;
#else
    s32 row = tile / divisions;
    s32 column = tile % divisions;
    f32 width = static_cast< f32 >(MR::getFrameBufferWidth()) / divisions;
    f32 height = static_cast< f32 >(MR::getFrameBufferHeight()) / divisions;
    f32 left = column * width;
    f32 right = (column + 1) * width;
    f32 bottom = (row + 1) * height;
    f32 top = row * height;

    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);
    GXPosition3f32(left, bottom, 0.0f);
    GXTexCoord2f32(0.0f, 1.0f);

    GXPosition3f32(left, top, 0.0f);
    GXTexCoord2f32(0.0f, 0.0f);

    GXPosition3f32(right, bottom, 0.0f);
    GXTexCoord2f32(1.0f, 1.0f);

    GXPosition3f32(right, top, 0.0f);
    GXTexCoord2f32(1.0f, 0.0f);
    GXEnd();
#endif
}

#pragma push
#pragma global_optimizer off
void ImageEffectLocalUtil::blurTexture(JUTTexture* pTexture, s32 divisions, s32 tile, u32 sampleCount, f32 radius, f32 intensity) {
#ifdef __3DS__
    if (pTexture == nullptr || sampleCount == 0 || divisions <= 0 || tile < 0 || tile >= divisions * divisions) {
        return;
    }
    // The Wii's taps on a circle (texture matrix translations), three per draw: the first draw
    // replaces, the others add.
    const f32 aspect = MR::isScreen16Per9() ? 1.333f : 1.0f;
    const f32 width = static_cast< f32 >(MR::getFrameBufferWidth()) / divisions;
    const f32 height = static_cast< f32 >(MR::getFrameBufferHeight()) / divisions;
    const TVec2f position((tile % divisions) * width, (tile / divisions) * height);
    const s32 sampleIntensity = 255.0f * intensity / sampleCount;
    for (u32 i = 0; i < sampleCount; i += 3) {
        f32 offsets[3][2];
        u32 count = 0;
        for (; count < 3 && i + count < sampleCount; count++) {
            const f32 angle = 2.0f * ((i + count) * JGeometry::TUtil< f32 >::PI()) / sampleCount;
            offsets[count][0] = radius * JMACosRadian(angle);
            offsets[count][1] = radius * aspect * JMASinRadian(angle);
        }
        u8 equation, source, destination;
        int logic;
        GXToCitro3D::blendMode(i == 0 ? GX_BM_NONE : GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR, equation, source, destination, logic);
        TDDraw::setDirectBlendState(equation, source, destination);
        TDDraw::drawTextureTaps(position, pTexture, TVec2f(width, height), offsets, count, static_cast< u8 >(sampleIntensity));
    }
    return;
#else
    f32 aspect = MR::isScreen16Per9() ? 1.333f : 1.0f;
    f32 verticalRadius = radius * aspect;
    s32 sampleIntensity = 255.0f * intensity / sampleCount;

    for (u32 i = 0; i < sampleCount; i++) {
        f32 angle = 2.0f * (i * JGeometry::TUtil< f32 >::PI()) / sampleCount;
        ::setTextureTrans(radius * JMACosRadian(angle), verticalRadius * JMASinRadian(angle));
        ETexDrawType type;
        if (i == 0) {
            type = TexDrawType_0;
        } else {
            type = TexDrawType_1;
        }

        drawTexture(pTexture, divisions, tile, sampleIntensity, type);
    }

    ::setTextureTrans(0.0f, 0.0f);
#endif
}
#pragma pop

void MR::connectToSceneNormalBloom(BloomEffect* pBloomEffect) {
    MR::connectToScene(pBloomEffect, MR::MovementType_None, MR::CalcAnimType_Environment, MR::DrawBufferType_None, MR::DrawType_None);

    NameObjAdaptor* pPreDrawAdaptor = new NameObjAdaptor("BloomEffect::preDraw");

    pPreDrawAdaptor->connectToDraw(MR::Functor(pBloomEffect, &BloomEffect::preDraw));
    MR::connectToScene(pPreDrawAdaptor, MR::MovementType_None, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_BloomEffectPreDraw);

    NameObjAdaptor* pPostDrawAdaptor = new NameObjAdaptor("BloomEffect::postDraw");

    pPostDrawAdaptor->connectToDraw(MR::Functor(pBloomEffect, &BloomEffect::postDraw));
    MR::connectToScene(pPostDrawAdaptor, MR::MovementType_None, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_BloomEffectPostDraw);
}
