#include "Game/MapObj/BigBubbleDrawer.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Util.hpp"
#include <JSystem/JGeometry/TMatrix.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>

namespace {
    static Color8 sTevColor0(0xE0E0E0FF);
    Color8 sTevColor1(0xFF000000);
    TVec3f sEnvMatTrans(1.0f, 1.0f, 1.5f);
};  // namespace

BigBubbleDrawer::BigBubbleDrawer() {
    mBubbleTexture = nullptr;
    mRainbowTexture = nullptr;
    _8 = 0.0f;
    mBubbleTexture = new JUTTexture(MR::loadTexFromArc("BigBubble.arc", "Bubble.bti"), 0);
    mRainbowTexture = new JUTTexture(MR::loadTexFromArc("BigBubble.arc", "Rainbow.bti"), 0);
}

void BigBubbleDrawer::setUpDraw() const {
#ifdef __3DS__
    // The Wii material below: rainbow texture (texture coordinate 0); plus C0 x the bubble
    // texture's alpha (normals through the environment matrix, projective); then toward the
    // bubble's warning colour C1 by A1 (BigBubble::draw sets stages 2-4), alpha toward 7/8.
    enum { TEXTURE0 = 0x03, TEXTURE1 = 0x04, CONSTANT = 0x0E, PREVIOUS = 0x0F };
    enum { REPLACE = 0, MODULATE = 1, ADD = 2, INTERPOLATE = 4, MULTIPLY_ADD = 8, OP_ALPHA = 0x02 };
    HostGfxStage stages[5] = {};
    stages[0].colorSrc[0] = stages[0].alphaSrc[0] = TEXTURE0;
    stages[0].colorFunc = stages[0].alphaFunc = REPLACE;
    stages[1].colorSrc[0] = CONSTANT;
    stages[1].colorSrc[1] = TEXTURE1;
    stages[1].colorOp[1] = OP_ALPHA;
    stages[1].colorSrc[2] = PREVIOUS;
    stages[1].colorFunc = MULTIPLY_ADD;
    stages[1].alphaSrc[0] = TEXTURE1;
    stages[1].alphaFunc = REPLACE;
    stages[1].constant = ::sTevColor0.r | (::sTevColor0.g << 8) | (::sTevColor0.b << 16) | (static_cast< u32 >(::sTevColor0.a) << 24);
    stages[2].colorSrc[0] = CONSTANT;
    stages[2].colorSrc[1] = PREVIOUS;
    stages[2].colorSrc[2] = CONSTANT;
    stages[2].colorOp[2] = OP_ALPHA;
    stages[2].colorFunc = INTERPOLATE;
    stages[2].alphaSrc[0] = PREVIOUS;
    stages[2].alphaFunc = REPLACE;
    stages[3].colorSrc[0] = stages[4].colorSrc[0] = PREVIOUS;
    stages[3].colorFunc = stages[4].colorFunc = REPLACE;
    stages[3].alphaSrc[0] = stages[4].alphaSrc[0] = PREVIOUS;
    stages[3].alphaSrc[1] = stages[4].alphaSrc[1] = CONSTANT;
    stages[3].alphaFunc = MODULATE;
    stages[4].alphaFunc = ADD;
    stages[3].constant = 0xFF000000u;  // warning off: alpha x 1, + 0
    TDDraw::setDirectStages(stages, 5, mRainbowTexture, mBubbleTexture, nullptr);
    TPos3f mtx;
    mtx.identity();
    TDDraw::setDirectTexGen(0, TDDraw::DIRECT_TEXGEN_TEX0, mtx.toMtxPtr(), false);
    mtx.set(MR::getCameraViewMtx());
    mtx.setTrans(::sEnvMatTrans);
    TDDraw::setDirectTexGen(1, TDDraw::DIRECT_TEXGEN_NRM, mtx.toMtxPtr(), true);
#else
    setUpMaterial();
#endif
}

#ifndef __3DS__  // the 3DS material is setUpDraw's Citro3D stages and texture generation
void BigBubbleDrawer::setUpVertexFormat() const {
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_POS_XY, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_POS_XYZ, GX_F32, 0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
}

void BigBubbleDrawer::setUpTexture() const {
    GXSetNumTexGens(2);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, 0x1E, 0, 0x7D);
    GXSetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX3x4, GX_TG_NRM, 0x21, 0, 0x7D);
    TPos3f mtx;
    mtx.identity();
    GXLoadTexMtxImm(mtx, 0x1E, GX_MTX2x4);

    mtx.set(MR::getCameraViewMtx());
    mtx.setTrans(::sEnvMatTrans);

    GXLoadTexMtxImm(mtx, 0x21, GX_MTX3x4);
    mRainbowTexture->load(GX_TEXMAP0);
    mBubbleTexture->load(GX_TEXMAP1);
}

void BigBubbleDrawer::setUpTevStage() const {
    GXSetNumTevStages(3);
    GXSetTevColor(GX_TEVREG0, (GXColor&)::sTevColor0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_TEXC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 0, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_TEXA, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 0, GX_TEVPREV);
    GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_C0, GX_CC_TEXA, GX_CC_CPREV);
    GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 1, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_TEXA, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 0, GX_TEVPREV);
    GXSetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE2, GX_CC_CPREV, GX_CC_C1, GX_CC_A1, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 1, GX_TEVPREV);
    GXSetTevKAlphaSel(GX_TEVSTAGE2, GX_TEV_KASEL_7_8);
    GXSetTevAlphaIn(GX_TEVSTAGE2, GX_CA_APREV, GX_CA_KONST, GX_CA_A1, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 0, GX_TEVPREV);
}

void BigBubbleDrawer::setUpPixelFormat() const {
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetColorUpdate(1);
    GXSetAlphaUpdate(0);
    GXSetCoPlanar(0);
    GXSetZMode(1, GX_LEQUAL, 0);
    GXSetZCompLoc(0);
    GXSetCullMode(GX_CULL_BACK);
    GXSetClipMode(GX_CLIP_ENABLE);
}

void BigBubbleDrawer::setUpMaterial() const {
    setUpVertexFormat();
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, 0, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    GXSetChanCtrl(GX_COLOR1A1, 0, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    setUpTexture();
    setUpTevStage();
    setUpPixelFormat();
    GXLoadPosMtxImm(MR::getCameraViewMtx(), 0);

    Mtx normalMtx;
    PSMTXIdentity(normalMtx);
    GXLoadNrmMtxImm(normalMtx, 0);
    GXSetCurrentMtx(0);
    MR::loadProjectionMtx();
}
#endif
