#include "Game/LiveActor/ShadowVolumeLine.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DirectDraw.hpp"
#include "Game/Util/MathUtil.hpp"

ShadowVolumeLine::ShadowVolumeLine() : ShadowVolumeDrawer("影描画[ボリュームライン]") {
    mFromShadowController = 0;
    mToShadowController = 0;
    mFromWidth = 100.0f;
    mToWidth = 100.0f;
}

void ShadowVolumeLine::loadModelDrawMtx() const {
#ifndef __3DS__
    GXLoadPosMtxImm(MR::getCameraViewMtx(), 0);
    GXSetCurrentMtx(0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
#endif
}

void ShadowVolumeLine::drawShape() const {
    const ShadowController* fromController = mFromShadowController;
    const ShadowController* toController = mToShadowController;
    TVec3f points[8];
    TVec3f from;
    TVec3f to;
    calcBaseDropPosition(&from, fromController);
    calcBaseDropPosition(&to, toController);
    TVec3f direction;
    MR::normalizeOrZero(to - from, &direction);
    if (MR::isNearZero(direction)) {
        return;
    }

    TVec3f fromDrop;
    TVec3f toDrop;
    fromController->getDropDir(&fromDrop);
    toController->getDropDir(&toDrop);
    TVec3f fromSide;
    fromSide.cross(fromDrop, direction);
    if (MR::isNearZero(fromSide)) {
        return;
    }

    MR::normalize(&fromSide);
    TVec3f toSide;
    toSide.cross(toDrop, direction);
    if (MR::isNearZero(toSide)) {
        return;
    }

    MR::normalize(&toSide);
    f32 fromLength = mFromWidth + calcBaseDropLength(fromController);
    f32 toLength = mToWidth + calcBaseDropLength(toController);
    points[0].scaleAdd(-mFromWidth, fromSide, from);
    points[1].scaleAdd(mFromWidth, fromSide, from);
    points[2].scaleAdd(fromLength, fromDrop, points[0]);
    points[3].scaleAdd(fromLength, fromDrop, points[1]);
    points[4].scaleAdd(-mToWidth, toSide, to);
    points[5].scaleAdd(mToWidth, toSide, to);
    points[6].scaleAdd(toLength, toDrop, points[4]);
    points[7].scaleAdd(toLength, toDrop, points[5]);

#ifdef __3DS__
    const u8 indices[] = {1, 5, 7, 3, 0, 2, 6, 4, 0, 1, 2, 3, 6, 7, 4, 5, 0, 1};
    TVec3f triangles[36];
    u32 colors[36];
    u32 count = 0;
    for (u32 quad = 0; quad < 2; quad++) {
        const u32 base = quad * 4;
        triangles[count++] = points[indices[base]];
        triangles[count++] = points[indices[base + 1]];
        triangles[count++] = points[indices[base + 2]];
        triangles[count++] = points[indices[base]];
        triangles[count++] = points[indices[base + 2]];
        triangles[count++] = points[indices[base + 3]];
    }
    for (u32 i = 0; i < 8; i++) {
        const u32 a = indices[8 + i], b = indices[8 + i + 1], c = indices[8 + i + 2];
        if ((i & 1) == 0) {
            triangles[count++] = points[a]; triangles[count++] = points[b];
        } else {
            triangles[count++] = points[b]; triangles[count++] = points[a];
        }
        triangles[count++] = points[c];
    }
    for (u32 i = 0; i < count; i++) colors[i] = 0xC0C00004;
    TDDraw::drawDirectTriangles(triangles, colors, count);
#else
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    TDDraw::sendPoint(points[1]);
    TDDraw::sendPoint(points[5]);
    TDDraw::sendPoint(points[7]);
    TDDraw::sendPoint(points[3]);
    GXEnd();

    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    TDDraw::sendPoint(points[0]);
    TDDraw::sendPoint(points[2]);
    TDDraw::sendPoint(points[6]);
    TDDraw::sendPoint(points[4]);
    GXEnd();

    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 10);
    TDDraw::sendPoint(points[0]);
    TDDraw::sendPoint(points[1]);
    TDDraw::sendPoint(points[2]);
    TDDraw::sendPoint(points[3]);
    TDDraw::sendPoint(points[6]);
    TDDraw::sendPoint(points[7]);
    TDDraw::sendPoint(points[4]);
    TDDraw::sendPoint(points[5]);
    TDDraw::sendPoint(points[0]);
    TDDraw::sendPoint(points[1]);
    GXEnd();
#endif
}

void ShadowVolumeLine::setFromShadowController(const ShadowController* pController) {
    mFromShadowController = pController;
}

void ShadowVolumeLine::setToShadowController(const ShadowController* pController) {
    mToShadowController = pController;
}

void ShadowVolumeLine::setFromWidth(f32 width) {
    mFromWidth = width;
}

void ShadowVolumeLine::setToWidth(f32 width) {
    mToWidth = width;
}

ShadowVolumeLine::~ShadowVolumeLine() {
}
