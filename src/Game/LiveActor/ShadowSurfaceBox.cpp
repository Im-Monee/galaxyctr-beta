#include "Game/LiveActor/ShadowSurfaceBox.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/LiveActor/ShadowController.hpp"
#include "Game/LiveActor/ShadowSurfaceDrawer.hpp"
// 3DS: headers the 3DS code of this file uses (CameraUtil.hpp).
#ifdef __3DS__
#include "Game/Util/CameraUtil.hpp"
#endif
#include "Game/Util/DirectDraw.hpp"
#include <JSystem/JGeometry/TMatrix.hpp>
#include <JSystem/JGeometry/TVec.hpp>
// Wii only: the GX headers of the Wii's drawing code.
#ifndef __3DS__
#include <revolution/gx/GXEnum.h>
#include <revolution/gx/GXGeometry.h>
#include <revolution/gx/GXVert.h>
#endif
#include <revolution/mtx.h>

ShadowSurfaceBox::~ShadowSurfaceBox() {
}

ShadowSurfaceBox::ShadowSurfaceBox() : ShadowSurfaceDrawer("影描画[水面ボックス]"), mSize(100.0f) {
}

void ShadowSurfaceBox::setSize(const TVec3f& pSize) {
    mSize.set(pSize);
}

void ShadowSurfaceBox::draw() const {
    MtxPtr baseMtx;
    ShadowController* controller = getController();

    if (!controller->isProjected() || !controller->isDraw()) {
        return;
    }

    TPos3f modelMtx;
    baseMtx = controller->_18;
    TVec3f side(baseMtx[0][0], baseMtx[1][0], baseMtx[2][0]);
    TVec3f up(baseMtx[0][1], baseMtx[1][1], baseMtx[2][1]);
    TVec3f front(baseMtx[0][2], baseMtx[1][2], baseMtx[2][2]);
    TVec3f normal;
    controller->getProjectionNormal(&normal);
    f32 sideDot = normal.dot(side);
    f32 upDot = normal.dot(up);
    f32 frontDot = normal.dot(front);
    TVec3f size(mSize);
    TVec3f position;

    if (controller->isFollowHostScale()) {
        TVec3f scale(controller->getHost()->mScale);
        scale /= 2.0f;
        size *= scale;
    } else {
        size /= 2.0f;
    }

    side -= normal * sideDot;
    up -= normal * upDot;
    front -= normal * frontDot;
    side.scale(size.x);
    up.scale(size.y);
    front.scale(size.z);
    modelMtx.setXYZDir(side, up, front);
    controller->getProjectionPos(&position);
    modelMtx.setTrans(position + normal);
    // 3DS: the model matrix for the DirectDraw (Citro3D) draw.
#ifdef __3DS__
    TDDraw::setModelMtxDirect(modelMtx.toMtxPtr());
#else
    TDDraw::setModelMtx(modelMtx.toMtxPtr());
#endif

    sideDot = sideDot >= 0.0f ? 1.0f : -1.0f;
    upDot = upDot >= 0.0f ? 1.0f : -1.0f;
    frontDot = frontDot >= 0.0f ? 1.0f : -1.0f;

    // 3DS: the same drawing through DirectDraw (Citro3D) instead of GX.
#ifdef __3DS__
    const TVec3f positions[18] = {
        TVec3f(sideDot, -sideDot, -1.0f), TVec3f(sideDot, -sideDot, 1.0f), TVec3f(sideDot, sideDot, 1.0f),
        TVec3f(sideDot, -sideDot, -1.0f), TVec3f(sideDot, sideDot, 1.0f), TVec3f(sideDot, sideDot, -1.0f),
        TVec3f(-1.0f, upDot, -upDot), TVec3f(1.0f, upDot, -upDot), TVec3f(1.0f, upDot, upDot),
        TVec3f(-1.0f, upDot, -upDot), TVec3f(1.0f, upDot, upDot), TVec3f(-1.0f, upDot, upDot),
        TVec3f(-frontDot, -1.0f, frontDot), TVec3f(-frontDot, 1.0f, frontDot), TVec3f(frontDot, 1.0f, frontDot),
        TVec3f(-frontDot, -1.0f, frontDot), TVec3f(frontDot, 1.0f, frontDot), TVec3f(frontDot, -1.0f, frontDot),
    };
    const u32 colors[18] = {
        0x80808080, 0x80808080, 0x80808080, 0x80808080, 0x80808080, 0x80808080,
        0x80808080, 0x80808080, 0x80808080, 0x80808080, 0x80808080, 0x80808080,
        0x80808080, 0x80808080, 0x80808080, 0x80808080, 0x80808080, 0x80808080,
    };
    TDDraw::drawDirectTriangles(positions, colors, 18);
    TDDraw::setViewMtx(MR::getCameraViewMtx());
#else
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(sideDot, -sideDot, -1.0f);
    GXColor1u32(0x80);
    GXPosition3f32(sideDot, -sideDot, 1.0f);
    GXColor1u32(0x80);
    GXPosition3f32(sideDot, sideDot, 1.0f);
    GXColor1u32(0x80);
    GXPosition3f32(sideDot, sideDot, -1.0f);
    GXColor1u32(0x80);
    GXEnd();

    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(-1.0f, upDot, -upDot);
    GXColor1u32(0x80);
    GXPosition3f32(1.0f, upDot, -upDot);
    GXColor1u32(0x80);
    GXPosition3f32(1.0f, upDot, upDot);
    GXColor1u32(0x80);
    GXPosition3f32(-1.0f, upDot, upDot);
    GXColor1u32(0x80);
    GXEnd();

    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(-frontDot, -1.0f, frontDot);
    GXColor1u32(0x80);
    GXPosition3f32(-frontDot, 1.0f, frontDot);
    GXColor1u32(0x80);
    GXPosition3f32(frontDot, 1.0f, frontDot);
    GXColor1u32(0x80);
    GXPosition3f32(frontDot, -1.0f, frontDot);
    GXColor1u32(0x80);
    GXEnd();
#endif
}
