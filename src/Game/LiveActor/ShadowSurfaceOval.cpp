#include "Game/LiveActor/ShadowSurfaceOval.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/LiveActor/ShadowController.hpp"
#include "Game/LiveActor/ShadowSurfaceDrawer.hpp"
#include "Game/Util/Color.hpp"
#include "Game/Util/DirectDraw.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/ModelUtil.hpp"
#include "Game/Util/MtxUtil.hpp"
#include <JSystem/JGeometry/TMatrix.hpp>
#include <JSystem/JGeometry/TVec.hpp>
#include <revolution/gx/GXEnum.h>
#include <revolution/gx/GXLighting.h>
#ifdef __3DS__
#include <cmath>
#endif

ShadowSurfaceOval::ShadowSurfaceOval() : ShadowSurfaceDrawer("影描画[水面楕円影]"), mModelData(), mSize(100.0f, 100.0f, 200.0f), mColor(0, 0, 0, 64) {
    mModelData = MR::getJ3DModelData("ShadowVolumeSphere");
}

void ShadowSurfaceOval::setColor(Color8 color) {
    mColor.set(color);
}

void ShadowSurfaceOval::setAlpha(u8 alpha) {
    mColor.a = alpha;
}

void ShadowSurfaceOval::setSize(const TVec3f& pSize) {
    mSize = pSize;
}

void ShadowSurfaceOval::draw() const {
    MtxPtr baseMtx;
    ShadowController* controller = getController();
    baseMtx = controller->_18;

    if (!controller->isProjected() || !controller->isDraw()) {
        return;
    }

    TVec3f size(mSize / 100.0f);
    if (controller->isFollowHostScale()) {
        size *= controller->getHost()->mScale;
    }

    TVec3f pos, normal;
    controller->getProjectionPos(&pos);
    controller->getProjectionNormal(&normal);
#ifdef __3DS__
    TPos3f mtx;
    mtx.setInline(baseMtx);
    MR::preScaleMtx(mtx, size);
    MR::flattenMtx(mtx, normal);
    mtx.setTrans(pos);
    TDDraw::setModelMtxDirect(mtx.toMtxPtr());
    TDDraw::setDirectCullMode(0);
    const f32 pi2 = 6.2831853071795864769f;
    const u32 rgb = (static_cast<u32>(mColor.r) << 24) | (static_cast<u32>(mColor.g) << 16) | (static_cast<u32>(mColor.b) << 8);
    for (u32 ring = 0; ring < 6; ring++) {
        TVec3f positions[192];
        u32 colors[192];
        const f32 inner = static_cast<f32>(ring) / 6.0f;
        const f32 outer = static_cast<f32>(ring + 1) / 6.0f;
        const u8 innerAlpha = static_cast<u8>((static_cast<u32>(mColor.a) * (6 - ring)) / 6);
        const u8 outerAlpha = static_cast<u8>((static_cast<u32>(mColor.a) * (5 - ring)) / 6);
        for (u32 segment = 0; segment < 32; segment++) {
            const f32 a0 = pi2 * segment / 32.0f;
            const f32 a1 = pi2 * (segment + 1) / 32.0f;
            const TVec3f i0(100.0f * inner * cosf(a0), 100.0f * inner * sinf(a0), 0.0f);
            const TVec3f i1(100.0f * inner * cosf(a1), 100.0f * inner * sinf(a1), 0.0f);
            const TVec3f o0(100.0f * outer * cosf(a0), 100.0f * outer * sinf(a0), 0.0f);
            const TVec3f o1(100.0f * outer * cosf(a1), 100.0f * outer * sinf(a1), 0.0f);
            const u32 base = segment * 6;
            positions[base + 0] = i0; positions[base + 1] = o0; positions[base + 2] = o1;
            positions[base + 3] = i0; positions[base + 4] = o1; positions[base + 5] = i1;
            colors[base + 0] = rgb | innerAlpha; colors[base + 1] = rgb | outerAlpha; colors[base + 2] = rgb | outerAlpha;
            colors[base + 3] = rgb | innerAlpha; colors[base + 4] = rgb | outerAlpha; colors[base + 5] = rgb | innerAlpha;
        }
        TDDraw::drawDirectTriangles(positions, colors, ARRAY_SIZE(positions));
    }
    TDDraw::resetViewMtxDirect();
    TDDraw::setDirectCullMode(2);
#else
    GXSetChanMatColor(GX_COLOR0A0, mColor);
    TPos3f mtx;
    mtx.setInline(baseMtx);
    MR::preScaleMtx(mtx, size);
    MR::flattenMtx(mtx, normal);
    mtx.setTrans(pos);
    TDDraw::setModelMtx(mtx.toMtxPtr());
    MR::drawSimpleModel(mModelData);
    MR::preScaleMtx(mtx, 0.9f);
    TDDraw::setModelMtx(mtx.toMtxPtr());
    MR::drawSimpleModel(mModelData);
    MR::preScaleMtx(mtx.toMtxPtr(), 0.9f);
    TDDraw::setModelMtx(mtx.toMtxPtr());
    MR::drawSimpleModel(mModelData);
    GXSetChanMatColor(GX_COLOR0A0, Color8(128));
#endif
}

ShadowSurfaceOval::~ShadowSurfaceOval() {
}
