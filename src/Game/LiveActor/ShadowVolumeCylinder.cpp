#include "Game/LiveActor/ShadowVolumeCylinder.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/MtxUtil.hpp"
// 3DS: DirectDraw, TDDraw's Citro3D draws.
#ifdef __3DS__
#include "Game/Util/DirectDraw.hpp"
#endif
#include <revolution/gx.h>

namespace {
    const f32 sModelScale = 100.0f;
};  // namespace

ShadowVolumeCylinder::~ShadowVolumeCylinder() {
}

ShadowVolumeCylinder::ShadowVolumeCylinder() : ShadowVolumeModel("影描画[ボリューム円柱]") {
    mRadius = 100.0f;
    initVolumeModel("ShadowVolumeCylinder");
}

void ShadowVolumeCylinder::setRadius(f32 radius) {
    mRadius = radius;
}

void ShadowVolumeCylinder::loadModelDrawMtx() const {
    ShadowController* controller = getController();
    TVec3f position;
    calcBaseDropPosition(&position);
    TVec3f direction;
    controller->getDropDir(&direction);
    TVec3f up(-direction);
    TPos3f mtx;
    MR::makeMtxUpNoSupportPos(&mtx, up, position);

    f32 radius = mRadius / ::sModelScale;
    if (controller->isFollowHostScale()) {
        radius *= controller->getHost()->mScale.x;
    }

    TVec3f scale(radius, calcBaseDropLength() / ::sModelScale, radius);
    MR::preScaleMtx(mtx, scale);
    // 3DS: the same drawing through DirectDraw (Citro3D) instead of GX.
#ifdef __3DS__
    TDDraw::setModelMtxDirect(mtx.toMtxPtr());
#else
    PSMTXConcat(MR::getCameraViewMtx(), mtx, mtx);
    GXLoadPosMtxImm(mtx, 0);
#endif
}

#ifdef __3DS__
void ShadowVolumeCylinder::drawShape() const {
    // The volume model, as on the Wii (see ShadowVolumeSphere::drawShape).
    ShadowVolumeModel::drawShape();
}

#endif