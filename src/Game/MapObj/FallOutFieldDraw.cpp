#include "Game/MapObj/FallOutFieldDraw.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Map/StageSwitch.hpp"
#include "Game/MapObj/ClipAreaHolder.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Screen/ImageEffectLocalUtil.hpp"
#include "Game/Screen/ScreenAlphaCapture.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/Color.hpp"
#include "Game/Util/JMapUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#ifdef __3DS__
#include "Game/Util/DirectDraw.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include "GXToCitro3D.hpp"
#endif

void FallOutFieldDraw_FORCE_MATCH_SDATA2() {
    (void)0.25f;
}

namespace {
    inline void drawFillScreen(GXColor color) {
        GXSetTevColor(GX_TEVREG0, color);
        ImageEffectLocalUtil::sendTextureVertex(1, 0);
    }
};  // namespace

namespace {
    static GXTevOp sAlphaTevOperater = GX_TEV_COMP_A8_GT;
    static GXBlendMode sFillBlendMode = GX_BM_BLEND;
    static GXBlendFactor sFillSrcFactor = GX_BL_SRCALPHA;
    static GXBlendFactor sFillDstFactor = GX_BL_INVSRCALPHA;
    static GXLogicOp sFillLogicOp = GX_LO_NOOP;
    // static const s32 sBlurCount = _;
    // static const f32 sBlurRadius = _;
    // static const f32 sBlurIntensity = _;
    static Color8 sFillColor = Color8(0, 0, 0, 255);
    static Color8 sEdgeColor = Color8(0, 46, 200, 128);
};  // namespace

FallOutFieldDraw::FallOutFieldDraw(const char* pName) : NameObj(pName), _C() {
    MR::createClipAreaHolder();
    MR::connectToScene(this, MR::MovementType_None, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_FallOutFieldDraw);
    MR::createScreenAlphaSceneObj(2, 0.5f);
    MR::createScreenAlphaSceneObj(3, 0.25f);
    MR::createScreenAlphaSceneObj(4, 0.25f);
}

void FallOutFieldDraw::setUpFillScreen() const {
#ifndef __3DS__  // 3DS: only used by the Wii draw()
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_C0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevKColor(GX_KCOLOR0, Color8(0, 0, 0, 1));
    GXSetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);

    GXTevAlphaArg alphaA = _C ? GX_CA_TEXA : GX_CA_KONST;
    GXTevAlphaArg alphaB = _C ? GX_CA_KONST : GX_CA_TEXA;

    GXSetTevAlphaIn(GX_TEVSTAGE0, alphaA, alphaB, GX_CA_A0, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, ::sAlphaTevOperater, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    GXSetZCompLoc(GX_FALSE);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetZScaleOffset(0.0f, 1.0f);
    GXSetBlendMode(::sFillBlendMode, ::sFillSrcFactor, ::sFillDstFactor, ::sFillLogicOp);
#endif
}

void FallOutFieldDraw::setUpEdgeAndClearAlpha() const {
#ifndef __3DS__  // 3DS: only used by the Wii draw()
    GXSetTevColor(GX_TEVREG1, ::sEdgeColor);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_C1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_A1, GX_CA_TEXA, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetDstAlpha(GX_TRUE, 0);
#endif
}

void FallOutFieldDraw::init(const JMapInfoIter& rIter) {
    if (!MR::isValidInfo(rIter)) {
        return;
    }

    bool arg0;
    MR::getJMapInfoArg0WithInit(rIter, &arg0);
    _C = arg0;

    if (MR::isExistStageSwitchAppear(rIter)) {
        MR::listenNameObjStageSwitchOnOffAppear(this, MR::createStageSwitchCtrl(this, rIter), MR::Functor(this, &FallOutFieldDraw::activate),
                                                MR::Functor(this, &FallOutFieldDraw::deactivate));
        MR::deactivateClipArea();
    } else {
        MR::activateClipArea();
    }
}

void FallOutFieldDraw::activate() {
    MR::activateClipArea();
}

void FallOutFieldDraw::deactivate() {
    MR::deactivateClipArea();
}

#ifdef __3DS__
namespace {
    // The Wii's alpha comparison with K0's alpha 1 (GX_TEV_COMP_A8_GT: inside, the clip areas'
    // count above 1; outside, below) as a steep ramp, x64, the combiners having no comparison:
    // the counts are multiples of 4. Colour: the constant (black for the fill).
    u32 buildMaskStages(HostGfxStage* pStages, bool inside, u32 color) {
        u32 count = 0;
        for (u32 i = 0; i < 3; i++) {
            HostGfxStage& stage = pStages[count++];
            stage = HostGfxStage();
            stage.colorSrc[0] = i == 0 ? 0x0E : 0x0F;  // constant, then previous
            stage.alphaSrc[0] = i == 0 ? 0x03 : 0x0F;  // texture alpha, then previous
            stage.alphaScale = 2;                      // x4
            stage.constant = color;
        }
        if (!inside) {
            HostGfxStage& stage = pStages[count++];
            stage = HostGfxStage();
            stage.colorSrc[0] = 0x0F;
            stage.alphaSrc[0] = 0x0F;
            stage.alphaOp[0] = 1;  // 1 - previous alpha
        }
        return count;
    }
}  // namespace
#endif

void FallOutFieldDraw::draw() const {
#ifdef __3DS__
    // The Wii's steps with the same captures, tiles and blend modes; the TEV each step sets is
    // given as combiner stages. The mask and its blur are written to the alpha alone, as the Wii
    // does, so the tiles they use keep the scene's colour.
    if (!MR::isActiveClipArea()) {
        return;
    }

    JUTTexture* pScreenAlphaTexture2 = MR::getScreenAlphaTexture(2);
    JUTTexture* pScreenAlphaTexture3 = MR::getScreenAlphaTexture(3);
    JUTTexture* pScreenAlphaTexture4 = MR::getScreenAlphaTexture(4);
    const f32 width = MR::getFrameBufferWidth();
    const f32 height = MR::getFrameBufferHeight();
    ImageEffectLocalUtil::setupDrawTexture();
    // capture(texture, 0, 1, ...): the Wii's division by 0 gives 0 (Broadway's divw), so the copy
    // starts at (0, 0), as division 1, tile 0.
    ImageEffectLocalUtil::capture(pScreenAlphaTexture2, 1, 0, GX_CTF_A8, true, 0);

    HostGfxStage stages[4];
    u32 count = buildMaskStages(stages, _C, 0);
    TDDraw::setDirectPixelWriteMask(GXToCitro3D::WRITE_ALPHA);
    TDDraw::setDirectBlendState(GXToCitro3D::EQUATION_ADD, GXToCitro3D::FACTOR_ONE, GXToCitro3D::FACTOR_ZERO);
    TDDraw::drawTextureStages(TVec2f(0.0f, 0.0f), pScreenAlphaTexture2, TVec2f(width / 2, height / 2), stages, count, false);
    ImageEffectLocalUtil::capture(pScreenAlphaTexture3, 2, 0, GX_CTF_A8, true, 0);

    // blurTexture(texture3, 4, 4, 5, 0.004, 1): the TEV reads the texture alone (colour and
    // alpha TEXA), so the five taps are summed as they are, the first replacing.
    {
        const u32 kSamples = 5;
        const f32 kRadius = 0.004f;
        const f32 aspect = MR::isScreen16Per9() ? 1.333f : 1.0f;
        const TVec2f position(0.0f, height / 4);
        for (u32 i = 0; i < kSamples; i += 3) {
            f32 offsets[3][2];
            u32 taps = 0;
            for (; taps < 3 && i + taps < kSamples; taps++) {
                const f32 angle = 2.0f * ((i + taps) * JGeometry::TUtil< f32 >::PI()) / kSamples;
                offsets[taps][0] = kRadius * JMACosRadian(angle);
                offsets[taps][1] = kRadius * aspect * JMASinRadian(angle);
            }
            TDDraw::setDirectBlendState(GXToCitro3D::EQUATION_ADD, GXToCitro3D::FACTOR_ONE,
                                        i == 0 ? GXToCitro3D::FACTOR_ZERO : GXToCitro3D::FACTOR_ONE);
            TDDraw::drawTextureTaps(position, pScreenAlphaTexture3, TVec2f(width / 4, height / 4), offsets, taps, 255);
        }
    }
    ImageEffectLocalUtil::capture(pScreenAlphaTexture4, 4, 4, GX_CTF_A8, false, 0);

    // setUpEdgeAndClearAlpha: the edge colour, alpha the blurred mask times the edge's alpha,
    // blended over the screen.
    {
        HostGfxStage edge = HostGfxStage();
        edge.colorSrc[0] = 0x0E;
        edge.alphaSrc[0] = 0x03;
        edge.alphaSrc[1] = 0x0E;
        edge.alphaFunc = 1;  // modulate
        const Color8& c = ::sEdgeColor;
        edge.constant = c.r | (c.g << 8) | (c.b << 16) | (static_cast< u32 >(c.a) << 24);
        TDDraw::setDirectPixelWriteMask(GXToCitro3D::WRITE_RGB);
        TDDraw::setDirectBlendState(GXToCitro3D::EQUATION_ADD, GXToCitro3D::FACTOR_SRC_ALPHA, GXToCitro3D::FACTOR_ONE_MINUS_SRC_ALPHA);
        TDDraw::drawTextureStages(TVec2f(0.0f, 0.0f), pScreenAlphaTexture4, TVec2f(width, height), &edge, 1, false);
    }

    // setUpFillScreen: the fill colour where the unblurred mask is set (alpha above 0), on the far
    // plane of the depth buffer (GXSetZScaleOffset(0, 1) with GX_ALWAYS, written).
    {
        const Color8& c = ::sFillColor;
        count = buildMaskStages(stages, _C, c.r | (c.g << 8) | (c.b << 16));
        TDDraw::setDirectDepthMode(true, GXToCitro3D::TEST_ALWAYS, true);
        TDDraw::setDirectAlphaTest(GXToCitro3D::alphaTest(GX_GREATER), 0);
        TDDraw::drawTextureStages(TVec2f(0.0f, 0.0f), pScreenAlphaTexture2, TVec2f(width, height), stages, count, true);
    }
    TDDraw::closeDirect();
    MR::loadViewMtx();
    MR::loadProjectionMtx();
#else
    if (!MR::isActiveClipArea()) {
        return;
    }

    ImageEffectLocalUtil::setupDrawTexture();

    JUTTexture* pScreenAlphaTexture2 = MR::getScreenAlphaTexture(2);
    JUTTexture* pScreenAlphaTexture3 = MR::getScreenAlphaTexture(3);
    JUTTexture* pScreenAlphaTexture4 = MR::getScreenAlphaTexture(4);

    ImageEffectLocalUtil::capture(pScreenAlphaTexture2, 0, 1, GX_CTF_A8, true, 0);

    GXSetTevKColor(GX_KCOLOR0, Color8(0, 0, 0, 1));
    GXSetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
    GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0_A);
    GXSetTevColor(GX_TEVREG0, Color8(255, 255, 255, 255));

    GXTevColorArg colorA = _C ? GX_CC_TEXA : GX_CC_KONST;
    GXTevColorArg colorB = _C ? GX_CC_KONST : GX_CC_TEXA;

    GXSetTevColorIn(GX_TEVSTAGE0, colorA, colorB, GX_CC_ZERO, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_COMP_RGB8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

    GXTevAlphaArg alphaA = _C ? GX_CA_TEXA : GX_CA_KONST;
    GXTevAlphaArg alphaB = _C ? GX_CA_KONST : GX_CA_TEXA;

    GXSetTevAlphaIn(GX_TEVSTAGE0, alphaA, alphaB, GX_CA_A0, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_COMP_A8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetColorUpdate(GX_FALSE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetDstAlpha(GX_FALSE, 0);
    GXSetZScaleOffset(0.0f, 0.0f);

    ImageEffectLocalUtil::drawTexture(pScreenAlphaTexture2, 2, 0, 255, ImageEffectLocalUtil::TexDrawType_0);
    ImageEffectLocalUtil::capture(pScreenAlphaTexture3, 2, 0, GX_CTF_A8, true, 0);

    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_TEXA, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_TEXA, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

    ImageEffectLocalUtil::blurTexture(pScreenAlphaTexture3, 4, 4, 5, 0.004f, 1.0f);
    ImageEffectLocalUtil::capture(pScreenAlphaTexture4, 4, 4, GX_CTF_A8, false, 0);
    setUpEdgeAndClearAlpha();
    ImageEffectLocalUtil::drawTexture(pScreenAlphaTexture4, 1, 0, 255, ImageEffectLocalUtil::TexDrawType_2);
    MR::loadScreenAlphaTexture(2, GX_TEXMAP0);
    setUpFillScreen();

    ::drawFillScreen(::sFillColor);

    MR::loadViewMtx();
    MR::loadProjectionMtx();

    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetDstAlpha(GX_TRUE, 0);
    GXSetZScaleOffset(1.0f, 0.0f);
#endif
}

namespace MR {
    NameObj* createFallOutFieldDraw(const char*) {
        return MR::createSceneObj(SceneObj_FallOutFieldDraw);
    }
};  // namespace MR
