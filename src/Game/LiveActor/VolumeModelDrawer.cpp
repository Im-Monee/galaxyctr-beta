#include "Game/LiveActor/VolumeModelDrawer.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/System/ResourceHolder.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DrawUtil.hpp"
// 3DS: DirectDraw, TDDraw's Citro3D draws.
#ifdef __3DS__
#include "Game/Util/DirectDraw.hpp"
#endif
#include "Game/Util/MtxUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
// 3DS: headers the 3DS code of this file uses (cfloat).
#ifdef __3DS__
#include <cfloat>
#endif
#include <cstdio>
// 3DS: GXToCitro3D.hpp, GX values as 3DS GPU values.
#ifdef __3DS__
#include "GXToCitro3D.hpp"
#endif

VolumeModelDrawInit::VolumeModelDrawInit() : NameObj("ボリュームモデル描画初期化") {
    MR::registerPreDrawFunction(MR::Functor(&MR::setupShadowVolumeDraw), MR::DrawType_VolumeModel);
}

VolumeModelDrawer::VolumeModelDrawer(const char* pName, const char* pFileName, MtxPtr mtx)
    : NameObj(pName), mMtx(), mModelData(), mColor(0xFF, 0xA9, 0, 0xFF) {
    MR::createSceneObj(SceneObj_VolumeModelDrawInit);
    mMtx = mtx;
    char buf[0x100];
    snprintf(buf, sizeof(buf), "%s.arc", pFileName);
    mModelData = (J3DModelData*)MR::createAndAddResourceHolder(buf)->mModelResTable->getRes(pFileName);
}

void VolumeModelDrawer::draw() const {
#ifdef __3DS__
    if (mModelData == nullptr || mMtx == nullptr) {
        return;
    }

    // The Wii's four passes over the model with the alpha as a mask: cleared under the model,
    // set where its back faces are behind the scene, cleared again where its front faces are
    // (what remains is the scene inside the volume), then the colour added there
    // (GX_BL_DSTALPHA, GX_BL_ONE) once, the alpha written back to 0. The mask is the stencil here.
    bool cullAll;
    const u32 color = (static_cast< u32 >(mColor.r) << 24) | (mColor.g << 16) | (mColor.b << 8) | mColor.a;
    TDDraw::setModelMtxDirect(mMtx);
    TDDraw::setDirectVertexColor(color);
    TDDraw::setDirectPixelWriteMask(0);
    TDDraw::setDirectCullMode(GXToCitro3D::CULL_NONE);
    TDDraw::setDirectDepthMode(false, GXToCitro3D::TEST_ALWAYS, false);
    TDDraw::setDirectStencil(true, GXToCitro3D::TEST_ALWAYS, 0, 0xFF, 0xFF, GXToCitro3D::STENCIL_KEEP, GXToCitro3D::STENCIL_KEEP,
                             GXToCitro3D::STENCIL_ZERO);
    MR::drawSimpleModel(mModelData);
    TDDraw::setDirectDepthMode(true, GXToCitro3D::depthTest(GX_GEQUAL), false);
    TDDraw::setDirectCullMode(GXToCitro3D::cullMode(GX_CULL_FRONT, cullAll));
    TDDraw::setDirectStencil(true, GXToCitro3D::TEST_ALWAYS, 1, 0xFF, 0xFF, GXToCitro3D::STENCIL_KEEP, GXToCitro3D::STENCIL_KEEP,
                             GXToCitro3D::STENCIL_REPLACE);
    MR::drawSimpleModel(mModelData);
    TDDraw::setDirectCullMode(GXToCitro3D::cullMode(GX_CULL_BACK, cullAll));
    TDDraw::setDirectStencil(true, GXToCitro3D::TEST_ALWAYS, 0, 0xFF, 0xFF, GXToCitro3D::STENCIL_KEEP, GXToCitro3D::STENCIL_KEEP,
                             GXToCitro3D::STENCIL_ZERO);
    MR::drawSimpleModel(mModelData);
    TDDraw::setDirectCullMode(GXToCitro3D::CULL_NONE);
    TDDraw::setDirectDepthMode(false, GXToCitro3D::TEST_ALWAYS, false);
    TDDraw::setDirectPixelWriteMask(GXToCitro3D::WRITE_RGB);
    TDDraw::setDirectBlendState(GXToCitro3D::EQUATION_ADD, GXToCitro3D::FACTOR_ONE, GXToCitro3D::FACTOR_ONE);
    TDDraw::setDirectStencil(true, GXToCitro3D::TEST_NOTEQUAL, 0, 0xFF, 0xFF, GXToCitro3D::STENCIL_KEEP, GXToCitro3D::STENCIL_KEEP,
                             GXToCitro3D::STENCIL_ZERO);
    MR::drawSimpleModel(mModelData);
    TDDraw::setDirectStencil(false, 0, 0, 0, 0, 0, 0, 0);
    TDDraw::setDirectBlendState(GXToCitro3D::EQUATION_ADD, GXToCitro3D::FACTOR_ONE, GXToCitro3D::FACTOR_ZERO);
    TDDraw::setDirectPixelWriteMask(0x0F);
    TDDraw::setDirectDepthMode(true, GXToCitro3D::depthTest(GX_GEQUAL), false);  // setupShadowVolumeDraw's, for the next drawer
    TDDraw::resetViewMtxDirect();
#else
    MR::loadProjectionMtx();
    loadModelDrawMtx();
    GXSetCurrentMtx(0);
    GXSetColorUpdate(0);
    GXSetAlphaUpdate(1);
    GXSetDstAlpha(1, 0);
    GXSetCullMode(GX_CULL_NONE);
    GXColor color_1 = mColor;
    GXSetTevColor(GX_TEVREG0, color_1);
    GXSetZMode(0, GX_ALWAYS, 0);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    MR::drawSimpleModel(mModelData);
    GXSetDstAlpha(0, 0);
    GXSetZMode(1, GX_GEQUAL, 0);
    GXSetCullMode(GX_CULL_FRONT);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ZERO, GX_LO_COPY);
    MR::drawSimpleModel(mModelData);
    GXSetCullMode(GX_CULL_BACK);
    GXSetBlendMode(GX_BM_SUBTRACT, GX_BL_ZERO, GX_BL_ZERO, GX_LO_COPY);
    MR::drawSimpleModel(mModelData);
    GXColor color_2 = mColor;
    GXSetTevColor(GX_TEVREG0, color_2);
    GXSetCullMode(GX_CULL_NONE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_ONE, GX_LO_COPY);
    GXSetColorUpdate(1);
    GXSetDstAlpha(1, 0);
    GXSetZMode(0, GX_ALWAYS, 0);
    MR::drawSimpleModel(mModelData);
#endif
}

void VolumeModelDrawer::loadModelDrawMtx() const {
    // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
    // (DETAILS.md, section 5).
#ifdef __3DS__
    return;
#else
    Mtx drawMtx;
    MR::multMtx(drawMtx, mMtx, MR::getCameraViewMtx().mMtx);
    GXLoadPosMtxImm(drawMtx, 0);
#endif
}
