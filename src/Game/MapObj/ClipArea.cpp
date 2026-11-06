#include "Game/MapObj/ClipArea.hpp"
#include "Game/MapObj/ClipAreaHolder.hpp"
#include "Game/MapObj/ClipAreaShape.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util.hpp"
// 3DS: GXToCitro3D.hpp, GX values as 3DS GPU values; DirectDraw, TDDraw's Citro3D draws.
#ifdef __3DS__
#include "GXToCitro3D.hpp"
#include "Game/Util/DirectDraw.hpp"
#endif

namespace {
    static const Color8 sShapeColor(0, 0, 0, 4);
    static const Color8 sDebugShapeColor(255, 0, 0, 128);
};  // namespace

ClipArea::ClipArea(const char* pName) : LiveActor(pName), mShape() {
    mBaseMtx.identity();
    MR::createClipAreaHolder();
    MR::addClipArea(this);
}

void ClipArea::init(const JMapInfoIter& rIter) {
    initBaseMatrix(rIter);
    MR::connectToScene(this, MR::MovementType_None, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_ClipArea);
}

void ClipArea::initBaseMatrix(const JMapInfoIter& rIter) {
    MR::initDefaultPos(this, rIter);
    MR::makeMtxTR(mBaseMtx, this);
}

void ClipArea::draw() const {
    if (!MR::isActiveClipArea()) {
        return;
    }

#ifdef __3DS__
    // The volume counts in the destination alpha only (cleared by the pre-draw, ClipAreaHolder):
    // its back faces (GX_CULL_FRONT) add the shape colour's alpha, its front faces (GX_CULL_BACK)
    // take it away again (GX_BM_SUBTRACT: destination - source), where the volume is behind the
    // scene (setupShadowVolumeDraw's GX_GEQUAL, kept for the next areas). FallOutFieldDraw reads it.
    bool cullAll;
    TDDraw::setDirectVertexColor((static_cast< u32 >(::sShapeColor.r) << 24) | (::sShapeColor.g << 16) | (::sShapeColor.b << 8) |
                                 ::sShapeColor.a);
    TDDraw::setDirectPixelWriteMask(GXToCitro3D::WRITE_ALPHA);
    TDDraw::setDirectCullMode(GXToCitro3D::cullMode(GX_CULL_FRONT, cullAll));
    TDDraw::setDirectBlendState(GXToCitro3D::EQUATION_ADD, GXToCitro3D::FACTOR_ONE, GXToCitro3D::FACTOR_ONE);
    mShape->drawVolumeShape(mBaseMtx, mScale);
    TDDraw::setDirectCullMode(GXToCitro3D::cullMode(GX_CULL_BACK, cullAll));
    TDDraw::setDirectBlendState(GXToCitro3D::EQUATION_REVERSE_SUBTRACT, GXToCitro3D::FACTOR_ONE, GXToCitro3D::FACTOR_ONE);
    mShape->drawVolumeShape(mBaseMtx, mScale);
    TDDraw::setDirectPixelWriteMask(GXToCitro3D::WRITE_RGB);
    TDDraw::setDirectCullMode(GXToCitro3D::CULL_NONE);
    TDDraw::setDirectBlendState(GXToCitro3D::EQUATION_ADD, GXToCitro3D::FACTOR_ONE, GXToCitro3D::FACTOR_ZERO);
    TDDraw::resetViewMtxDirect();
    return;
#else
    GXSetTevColor(GX_TEVREG0, ::sShapeColor);
    GXSetColorUpdate(GX_FALSE);
    GXSetDstAlpha(GX_FALSE, 0);
    GXSetCullMode(GX_CULL_FRONT);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_COPY);
    mShape->drawVolumeShape(mBaseMtx, mScale);
    GXSetCullMode(GX_CULL_BACK);
    GXSetBlendMode(GX_BM_SUBTRACT, GX_BL_ZERO, GX_BL_ZERO, GX_LO_COPY);
    mShape->drawVolumeShape(mBaseMtx, mScale);
#endif
}

void ClipArea::setShape(ClipAreaShape* pShape) {
    mShape = pShape;
}

bool ClipArea::isInArea(const TVec3f& rParam1, f32 param2) const {
    return mShape->isInArea(rParam1, param2, mBaseMtx, mScale);
}

namespace MR {
    void setClipSphereArea(ClipArea* pClipArea) {
        pClipArea->setShape(new ClipAreaShapeSphere());
    }

    void setClipCenterBoxArea(ClipArea* pClipArea) {
        pClipArea->setShape(new ClipAreaShapeBox(0));
    }

    void setClipBottomBoxArea(ClipArea* pClipArea) {
        pClipArea->setShape(new ClipAreaShapeBox(1));
    }

    void setClipTopConeArea(ClipArea* pClipArea) {
        pClipArea->setShape(new ClipAreaShapeCone(0));
    }

    void setClipBottomConeArea(ClipArea* pClipArea) {
        pClipArea->setShape(new ClipAreaShapeCone(1));
    }
};  // namespace MR
