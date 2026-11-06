#include "Game/LiveActor/ShadowVolumeDrawer.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/DrawUtil.hpp"
#ifdef __3DS__
#include "Game/Util/DirectDraw.hpp"
#endif
#include "Game/Util/ObjUtil.hpp"
#ifdef __3DS__
#include "GXToCitro3D.hpp"
#endif

namespace {
    static Color8 sShapeColor(0xC0, 0xC0, 0, 4);
    static Color8 sDebugShapeColor(0xFF, 0, 0, 0x80);
};  // namespace

ShadowVolumeDrawInit::ShadowVolumeDrawInit() : NameObj("シャドウボリューム描画初期化") {
    MR::registerPreDrawFunction(MR::Functor(&MR::setupShadowVolumeDraw), MR::DrawType_ShadowVolume);
}

ShadowVolumeDrawer::ShadowVolumeDrawer(const char* pName) : ShadowDrawer(pName) {
    mStartDrawShapeOffset = 0.0f;
    mEndDrawShapeOffset = 0.0f;
    mIsCutDropShadow = false;
    MR::createSceneObj(SceneObj_ShadowVolumeDrawInit);
    MR::connectToScene(this, MR::MovementType_None, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_ShadowVolume);
}

void ShadowVolumeDrawer::setStartDrawShepeOffset(f32 offs) {
    mStartDrawShapeOffset = offs;
}

void ShadowVolumeDrawer::setEndDrawShepeOffset(f32 offs) {
    mEndDrawShapeOffset = offs;
}

void ShadowVolumeDrawer::onCutDropShadow() {
    mIsCutDropShadow = true;
}

void ShadowVolumeDrawer::offCutDropShadow() {
    mIsCutDropShadow = false;
}

void ShadowVolumeDrawer::calcBaseDropPosition(TVec3f* pVec) const {
    calcBaseDropPosition(pVec, getController());
}

void ShadowVolumeDrawer::calcBaseDropPosition(TVec3f* pPosition, const ShadowController* pController) const {
    TVec3f position;
    TVec3f direction;
    pController->getDropPos(&position);
    pController->getDropDir(&direction);
    pPosition->set(position + direction * mStartDrawShapeOffset);
}

f32 ShadowVolumeDrawer::calcBaseDropLength() const {
    return calcBaseDropLength(getController());
}

f32 ShadowVolumeDrawer::calcBaseDropLength(const ShadowController* pController) const {
    f32 length = pController->getDropLength();

    if (mIsCutDropShadow) {
        if (pController->isProjected()) {
            length = pController->getProjectionLength();
        }
    }

    f32 endOffset = mEndDrawShapeOffset;
    f32 startOffset = mStartDrawShapeOffset;
    length += -startOffset + endOffset;
    return length;
}

void ShadowVolumeDrawer::loadModelDrawMtx() const {
}

void ShadowVolumeDrawer::drawShape() const {
}

bool ShadowVolumeDrawer::isDraw() const {
    return getController()->isDraw();
}

void ShadowVolumeDrawer::draw() const {
    if (isDraw()) {
        loadModelDrawMtx();
#ifdef __3DS__
        // The Wii counts in the destination alpha: back faces add the shape colour's 4
        // (GX_CULL_FRONT, GX_BM_BLEND ONE ONE), front faces take it away (GX_CULL_BACK,
        // GX_BM_SUBTRACT), both where the volume is behind the scene (setupShadowVolumeDraw's
        // GX_GEQUAL). The 3DS frame buffer alpha holds the materials' alpha: the count goes in the
        // stencil, saturated like the alpha, and MR::fillSilhouetteColor darkens where it is not 0.
        bool cullAll;
        TDDraw::setDirectPixelWriteMask(0);
        TDDraw::setDirectDestinationAlpha(false, 0);
        TDDraw::setDirectCullMode(GXToCitro3D::cullMode(GX_CULL_FRONT, cullAll));
        TDDraw::setDirectStencil(true, GXToCitro3D::TEST_ALWAYS, 0, 0xFF, 0xFF, GXToCitro3D::STENCIL_KEEP, GXToCitro3D::STENCIL_KEEP,
                                 GXToCitro3D::STENCIL_INCR);
        drawShape();
        TDDraw::setDirectCullMode(GXToCitro3D::cullMode(GX_CULL_BACK, cullAll));
        TDDraw::setDirectStencil(true, GXToCitro3D::TEST_ALWAYS, 0, 0xFF, 0xFF, GXToCitro3D::STENCIL_KEEP, GXToCitro3D::STENCIL_KEEP,
                                 GXToCitro3D::STENCIL_DECR);
        drawShape();
        TDDraw::setDirectStencil(false, 0, 0, 0, 0, 0, 0, 0);
        TDDraw::setDirectPixelWriteMask(0x0F);
        TDDraw::setDirectCullMode(0);
        // setupShadowVolumeDraw's state (Z test) stays for the next volumes, as on the Wii; only
        // the view matrix a drawer loaded goes back to the camera's.
        TDDraw::resetViewMtxDirect();
#else
        GXColor shapeColor = reinterpret_cast< GXColor& >(::sShapeColor);
        GXSetTevColor(GX_TEVREG0, shapeColor);
        GXSetColorUpdate(0);
        GXSetDstAlpha(0, 0);
        GXSetCullMode(GX_CULL_FRONT);
        GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_COPY);
        drawShape();
        GXSetCullMode(GX_CULL_BACK);
        GXSetBlendMode(GX_BM_SUBTRACT, GX_BL_ZERO, GX_BL_ZERO, GX_LO_COPY);
        drawShape();
        GXSetCullMode(GX_CULL_NONE);
        GXSetColorUpdate(1);
        GXSetDstAlpha(1, 0);
#endif
    }
}

ShadowVolumeDrawInit::~ShadowVolumeDrawInit() {
}
