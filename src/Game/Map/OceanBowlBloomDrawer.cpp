#include "Game/Map/OceanBowlBloomDrawer.hpp"
#include "Game/Map/OceanBowl.hpp"
#include "Game/Map/OceanBowlPoint.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
// 3DS: WaterRender3DS.hpp, water drawn through Citro3D.
#ifdef __3DS__
#include "WaterRender3DS.hpp"
#endif
#include <revolution/gx/GXVert.h>

OceanBowlBloomDrawer::OceanBowlBloomDrawer(OceanBowl* pHost) : NameObj("オーシャンボウル[ブルーム描画]"), mHost(pHost) {
}

void OceanBowlBloomDrawer::init(const JMapInfoIter&) {
    MR::connectToScene(this, MR::MovementType_None, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_OceanBowlBloomDrawer);
}

void OceanBowlBloomDrawer::draw() const {
    if (mHost->mIsClipped || !MR::isValidDraw(mHost) || !MR::isCameraInWater()) {
        return;
    }

    // 3DS: the same drawing through DirectDraw (Citro3D) instead of GX.
#ifdef __3DS__
    void* water = rvl3DSGetJUTTexture(mHost->mWaterTex);
    for (s32 x = 0; x < 24; x++) {
        HostGfxVertex vertices[50];
        u16 even = static_cast<u16>(x * 50);
        u16 odd = even + 1;
        for (s32 y = 0; y < 25; y++) {
            OceanBowlPoint* pointA = mHost->getPoint(x + 1, y);
            OceanBowlPoint* pointB = mHost->getPoint(x, y);
            u32 a = static_cast<u32>(even) * 2;
            u32 b = static_cast<u32>(odd) * 2;
            vertices[y * 2] = WaterRender3DS::makeVertex(pointA->mVertexPosition, mHost->mVtxTex0Array[a] + mHost->mTexU0,
                                                         mHost->mVtxTex0Array[a + 1] + mHost->mTexV0, mHost->mVtxTex1Array[a] + mHost->mTexU1,
                                                         mHost->mVtxTex1Array[a + 1] + mHost->mTexV1, 0.0f, 0.0f, pointA->mAlpha);
            vertices[y * 2 + 1] = WaterRender3DS::makeVertex(pointB->mVertexPosition, mHost->mVtxTex0Array[b] + mHost->mTexU0,
                                                             mHost->mVtxTex0Array[b + 1] + mHost->mTexV0,
                                                             mHost->mVtxTex1Array[b] + mHost->mTexU1,
                                                             mHost->mVtxTex1Array[b + 1] + mHost->mTexV1, 0.0f, 0.0f, pointB->mAlpha);
            even += 2;
            odd += 2;
        }
        WaterRender3DS::drawStrip(vertices, 50, water, water, nullptr, true, false);
    }
#else
    mHost->loadMaterialBloom();
#ifndef __3DS__

#endif
    OceanBowlPoint* pPoint2;
    u16 zero = 0;
    u16 one = 1;
    for (s32 x = 0; x < 24; x++) {
        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 50);
        for (s32 y = 0; y < 25; y++) {
            pPoint2 = mHost->getPoint(x, y);
            OceanBowlPoint* pPoint = mHost->getPoint(x + 1, y);
            GXPosition3f32(pPoint->mVertexPosition.x, pPoint->mVertexPosition.y, pPoint->mVertexPosition.z);
            GXColor4u8(0xFF, 0xFF, 0xFF, mHost->getPoint(x + 1, y)->mAlpha);
            GXTexCoord2s16(zero, zero);

            GXPosition3f32(pPoint2->mVertexPosition.x, pPoint2->mVertexPosition.y, pPoint2->mVertexPosition.z);
            GXColor4u8(0xFF, 0xFF, 0xFF, mHost->getPoint(x, y)->mAlpha);
            GXTexCoord2s16(one, one);

            zero += 2;
            one += 2;
        }
        GXEnd();
    }
#endif
}
