#include "Game/MapObj/ClipFieldFillDraw.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Screen/ScreenAlphaCapture.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DirectDraw.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/JMapUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
// 3DS: GXToCitro3D.hpp, GX values as 3DS GPU values.
#ifdef __3DS__
#include "GXToCitro3D.hpp"
#endif

ClipFieldFillDraw::ClipFieldFillDraw(const char* pName)
    // 3DS: the fill's blending as plain numbers: the 3DS draw translates them (GXToCitro3D.hpp).
#ifdef __3DS__
    : NameObj(pName), mFillBlendMode(), mFillSrcFactor(), mFillDstFactor(), mFillLogicOp(),
      _1C(255, 230, 80, 200), _20(255, 255, 255, 255), _24(), _25() {
#else
    : NameObj(pName), mFillBlendMode(GX_BM_BLEND), mFillSrcFactor(GX_BL_SRCALPHA), mFillDstFactor(GX_BL_ONE), mFillLogicOp(GX_LO_NOOP),
      _1C(255, 230, 80, 200), _20(255, 255, 255, 255), _24(), _25() {
#endif
    MR::connectToScene(this, MR::MovementType_None, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_ClipFieldFillDraw);
    MR::createScreenAlphaSceneObj(1, 1.0f);
}

void ClipFieldFillDraw::init(const JMapInfoIter& rIter) {
    if (MR::isValidInfo(rIter)) {
        return;
    }
}

void ClipFieldFillDraw::setModeSubColor() {
    // 3DS: GX_BM_SUBTRACT, GX_BL_ONE, GX_BL_ONE, GX_LO_NOOP as plain numbers, translated by the 3DS
    // draw.
#ifdef __3DS__
    mFillBlendMode = static_cast< decltype(mFillBlendMode) >(2);
    mFillDstFactor = static_cast< decltype(mFillDstFactor) >(1);
    mFillSrcFactor = static_cast< decltype(mFillSrcFactor) >(1);
    mFillLogicOp = static_cast< decltype(mFillLogicOp) >(0);
#else
    mFillBlendMode = GX_BM_SUBTRACT;
    mFillDstFactor = GX_BL_ONE;
    mFillSrcFactor = GX_BL_ONE;
    mFillLogicOp = GX_LO_NOOP;
#endif
}

// Wii only: GX state, display lists and material setup; the 3DS draws this through Citro3D
// (DETAILS.md, section 5).
#ifndef __3DS__
void ClipFieldFillDraw::setUpFillScreen() const {
    Mtx44 projectionMtx;
    C_MTXOrtho(projectionMtx, 0.0f, MR::getScreenHeight(), 0.0f, MR::getScreenWidth(), -1.0f, 1.0f);
    GXSetProjection(projectionMtx, GX_ORTHOGRAPHIC);

    TMtx34f posMtx;
    posMtx.identity();
    GXLoadPosMtxImm((MtxPtr)&posMtx, GX_PNMTX0);
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

    GXTevAlphaArg alphaA;
    GXTevAlphaArg alphaB;

    if (_25) {
        alphaA = GX_CA_TEXA;
        alphaB = GX_CA_KONST;
    } else {
        alphaA = GX_CA_KONST;
        alphaB = GX_CA_TEXA;
    }

    GXSetTevKColor(GX_KCOLOR0, Color8(0, 0, 0, 1));
    GXSetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
    GXSetTevAlphaIn(GX_TEVSTAGE0, alphaA, alphaB, GX_CA_A0, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_COMP_RGB8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetZMode(GX_TRUE, GX_ALWAYS, GX_FALSE);
    GXSetZCompLoc(GX_FALSE);
    GXSetCullMode(GX_CULL_BACK);
    GXSetDither(GX_FALSE);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetBlendMode(mFillBlendMode, mFillSrcFactor, mFillDstFactor, mFillLogicOp);
}

void ClipFieldFillDraw::sendFillScreen(GXColor color) const {
    GXSetTevColor(GX_TEVREG0, color);

    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);
    {
        u16 screenWidth = MR::getScreenWidth();
        u16 screenHeight = MR::getScreenHeight();

        GXPosition2u16(0, 0);
        GXTexCoord2f32(0.0f, 0.0f);

        GXPosition2u16(screenWidth, 0);
        GXTexCoord2f32(1.0f, 0.0f);

        GXPosition2u16(0, screenHeight);
        GXTexCoord2f32(0.0f, 1.0f);

        GXPosition2u16(screenWidth, screenHeight);
        GXTexCoord2f32(1.0f, 1.0f);
    }

    GXEnd();
}
#endif

void ClipFieldFillDraw::draw() const {
#ifdef __3DS__
    JUTTexture* alphaTexture = MR::getScreenAlphaTexture(1);
    TDDraw::setupDirect(1, 1, 3);
    {
        Mtx44 projection;
        C_MTXOrtho(projection, 0.0f, MR::getScreenHeight(), 0.0f, MR::getScreenWidth(), -1.0f, 1.0f);
        TDDraw::setDirectProjectionMtx(projection);
        Mtx identity;
        PSMTXIdentity(identity);
        TDDraw::setDirectViewMtx(identity);
        u8 equation, source, destination;
        int logic;
        GXToCitro3D::blendMode(mFillBlendMode, mFillSrcFactor, mFillDstFactor, mFillLogicOp, equation, source, destination, logic);
        TDDraw::setDirectBlendState(equation, source, destination);
    }
    TDDraw::setDirectAlphaTest(GXToCitro3D::alphaTest(GX_GREATER), 0);
    TDDraw::setDirectDepthMode(true, GXToCitro3D::depthTest(GX_ALWAYS), false);
    TDDraw::setDirectCullMode(_24 ? 1 : 0);
    const f32 width = static_cast< f32 >(MR::getScreenWidth());
    const f32 height = static_cast< f32 >(MR::getScreenHeight());
    const TVec3f positions[6] = {
        TVec3f(0.0f, 0.0f, 0.0f), TVec3f(width, 0.0f, 0.0f), TVec3f(width, height, 0.0f),
        TVec3f(0.0f, 0.0f, 0.0f), TVec3f(width, height, 0.0f), TVec3f(0.0f, height, 0.0f),
    };
    const TVec2f texcoords[6] = {
        TVec2f(0.0f, 0.0f), TVec2f(1.0f, 0.0f), TVec2f(1.0f, 1.0f),
        TVec2f(0.0f, 0.0f), TVec2f(1.0f, 1.0f), TVec2f(0.0f, 1.0f),
    };
    // The Wii's TEV: colour C0; alpha (a > b ? A0 : 0) with a, b = TEXA, 1/255 (or swapped when
    // _25 is off). The comparison as combiners: max(a - b, 0) times 4 four times (256: 1 from
    // 1/255 up), then times A0. C0 is the vertex colour.
    HostGfxStage stages[5] = {};
    stages[0].colorSrc[0] = 0x00;  // primary: C0
    stages[0].alphaSrc[0] = _25 ? 0x03 : 0x0E;
    stages[0].alphaSrc[1] = _25 ? 0x0E : 0x03;
    stages[0].alphaFunc = 5;  // subtract (clamped at 0)
    stages[0].alphaScale = 2;  // x4
    stages[0].constant = 0x01000000u;  // alpha 1/255
    for (u32 i = 1; i < 4; i++) {
        stages[i].colorSrc[0] = 0x0F;
        stages[i].alphaSrc[0] = 0x0F;
        stages[i].alphaScale = 2;
    }
    stages[4].colorSrc[0] = 0x0F;
    stages[4].alphaSrc[0] = 0x0F;
    stages[4].alphaSrc[1] = 0x00;  // primary: A0
    stages[4].alphaFunc = 1;  // modulate
    TDDraw::drawStageTriangles(positions, texcoords, 6, alphaTexture, static_cast< u32 >(_1C), stages, 5);
    MR::clearAlphaBuffer(0);
    return;
#else
    MR::loadScreenAlphaTexture(1, GX_TEXMAP0);
    setUpFillScreen();
    sendFillScreen(_1C);
    MR::loadViewMtx();
    MR::loadProjectionMtx();
    GXSetZScaleOffset(1.0f, 0.0f);
    MR::clearAlphaBuffer(0);
    MR::loadViewMtx();
    MR::loadProjectionMtx();
    GXSetAlphaUpdate(GX_FALSE);
    GXSetColorUpdate(GX_TRUE);

    if (_24) {
        TDDraw::setup(0, 1, 0);
        GXSetCullMode(GX_CULL_FRONT);
        GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    }
#endif
}

namespace MR {
    NameObj* createClipFieldFillDraw(const char* pParam1) {
        return static_cast< ClipFieldFillDraw* >(MR::createSceneObj(SceneObj_ClipFieldFillDraw));
    }

    NameObj* createVolumeDrawSpotLight(const char* pParam1) {
        ClipFieldFillDraw* clipFieldFillDraw;

        clipFieldFillDraw = static_cast< ClipFieldFillDraw* >(createClipFieldFillDraw(pParam1));
        clipFieldFillDraw->_25 = false;
        clipFieldFillDraw->_24 = false;
        clipFieldFillDraw->setModeSubColor();
        clipFieldFillDraw->_1C = static_cast< u32 >(Color8(70, 35, 11, 80));
        clipFieldFillDraw->_20 = static_cast< u32 >(Color8(255, 255, 128, 80));

        return clipFieldFillDraw;
    }
};  // namespace MR
