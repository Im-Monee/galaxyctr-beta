#include "Game/Effect/ParticleDrawExecutor.hpp"
#include "Game/Effect/EffectSystem.hpp"
#include "Game/NameObj/NameObjAdaptor.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/Color.hpp"
#ifdef __3DS__
#include "Game/Util/DirectDraw.hpp"
#endif
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include <JSystem/J3DGraphBase/J3DSys.hpp>
#include <JSystem/JParticle/JPAEmitterManager.hpp>
#include <JSystem/JUtility/JUTVideo.hpp>

namespace {
    void connectToSceneDrawAdaptor(NameObjAdaptor* pAdaptor, const MR::FunctorBase& rFunctor, int drawType) NO_INLINE {
        pAdaptor->connectToDraw(rFunctor);
        MR::connectToScene(pAdaptor, -1, -1, -1, drawType);
        MR::registerPreDrawFunction(MR::Functor(&ParticleDrawExecutor::initDraw), drawType);
    }
};  // namespace

ParticleDrawExecutor::ParticleDrawExecutor(const EffectSystem* pSystem, bool createAdaptors)
    : mHost(pSystem), _4(), _8(), _C(), _10(), _14(), _18(), _1C(), _20(true), _21() {
    if (createAdaptors) {
        initDrawAdaptor();
    }
}

void ParticleDrawExecutor::initDraw() {
#ifndef __3DS__
    GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, Color8(0, 0, 0, 0));
#endif
}

void ParticleDrawExecutor::draw3D() const {
    drawWithViewMtx3D(MR::getCameraViewMtx());
}

void ParticleDrawExecutor::draw2D() const {
    if (_20) {
        TPos3f cameraMtx;
        cameraMtx.identity();
        JPADrawInfo drawInfo(cameraMtx.toMtxPtr());

        f32 width;
        f32 height = static_cast< s32 >(JUTVideo::getManager()->getEfbHeight());
        width = MR::isScreen16Per9() ? MR::getScreenWidth() : 608.0f;
        width *= 0.5f;
        height *= 0.5f;

        Mtx44 projection;
        C_MTXOrtho(projection, height, -height, -width, width, -1000.0f, 1000.0f);
#ifdef __3DS__
        JPASetDirectProjection(&projection);
#else
        GXSetProjection(projection, GX_ORTHOGRAPHIC);
        GXSetCullMode(GX_CULL_NONE);
        GXSetZMode(GX_FALSE, GX_NEVER, GX_FALSE);
#endif
        C_MTXLightOrtho(drawInfo.mPrjMtx, height, -height, -width, width, 0.5f, 0.5f, 0.5f, 0.5f);
        mHost->mEmitterManager->draw(&drawInfo, 6);
#ifndef __3DS__
        GXSetClipMode(GX_CLIP_ENABLE);
#endif
        mHost->mEmitterManager->draw(&drawInfo, 7);
#ifdef __3DS__
        JPASetDirectProjection(NULL);
#else
        GXSetClipMode(GX_CLIP_ENABLE);
#endif
    }
}

void ParticleDrawExecutor::drawIndirect() const {
    if (_20) {
        JPADrawInfo drawInfo(j3dSys.getViewMtx(), MR::getFovy(), MR::getAspect());
        mHost->mEmitterManager->draw(&drawInfo, 2);
#ifndef __3DS__
        GXSetClipMode(GX_CLIP_ENABLE);
#endif
    }
}

void ParticleDrawExecutor::drawAfterIndirect() const {
    drawWithViewMtxAfterIndirect(MR::getCameraViewMtx());
}

void ParticleDrawExecutor::drawFor2DModel() const {
    if (_20) {
        JPADrawInfo drawInfo(j3dSys.getViewMtx());
        mHost->mEmitterManager->draw(&drawInfo, 8);
#ifndef __3DS__
        GXSetClipMode(GX_CLIP_ENABLE);
#endif
    }
}

void ParticleDrawExecutor::drawForBloomEffect() const {
    drawWithViewMtxForBloomEffect(MR::getCameraViewMtx());
}

void ParticleDrawExecutor::drawAfterImageEffect() const {
    drawWithViewMtxAfterImageEffect(MR::getCameraViewMtx());
}

void ParticleDrawExecutor::drawWithViewMtx3D(const TPos3f& rViewMtx) const {
    if (_20) {
        JPADrawInfo drawInfo(rViewMtx);
        mHost->mEmitterManager->draw(&drawInfo, 0);
#ifndef __3DS__
        GXSetClipMode(GX_CLIP_ENABLE);
#endif
        mHost->mEmitterManager->draw(&drawInfo, 1);
#ifndef __3DS__
        GXSetClipMode(GX_CLIP_ENABLE);
#endif
    }
}

void ParticleDrawExecutor::drawWithViewMtxAfterIndirect(const TPos3f& rViewMtx) const {
    if (_20) {
        JPADrawInfo drawInfo(rViewMtx);
        mHost->mEmitterManager->draw(&drawInfo, 3);
#ifndef __3DS__
        GXSetClipMode(GX_CLIP_ENABLE);
#endif
    }
}

void ParticleDrawExecutor::drawWithViewMtxForBloomEffect(const TPos3f& rViewMtx) const {
    if (_20) {
        JPADrawInfo drawInfo(rViewMtx);
        mHost->mEmitterManager->draw(&drawInfo, 4);
#ifndef __3DS__
        GXSetClipMode(GX_CLIP_ENABLE);
#endif
    }
}

void ParticleDrawExecutor::drawWithViewMtxAfterImageEffect(const TPos3f& rViewMtx) const {
    if (_20) {
        JPADrawInfo drawInfo(rViewMtx);
        mHost->mEmitterManager->draw(&drawInfo, 5);
#ifndef __3DS__
        GXSetClipMode(GX_CLIP_ENABLE);
#endif
    }
}

void ParticleDrawExecutor::initDrawAdaptor() {
    _4 = new NameObjAdaptor("3Dパーティクル");
    ::connectToSceneDrawAdaptor(_4, MR::Functor(this, &ParticleDrawExecutor::draw3D), MR::DrawType_EffectDraw3D);

    _8 = new NameObjAdaptor("2Dパーティクル");
    ::connectToSceneDrawAdaptor(_8, MR::Functor(this, &ParticleDrawExecutor::draw2D), MR::DrawType_EffectDraw2D);

    _C = new NameObjAdaptor("インダイレクトパーティクル");
    ::connectToSceneDrawAdaptor(_C, MR::Functor(this, &ParticleDrawExecutor::drawIndirect), MR::DrawType_EffectDrawIndirect);

    _10 = new NameObjAdaptor("インダイレクト後パーティクル");
    ::connectToSceneDrawAdaptor(_10, MR::Functor(this, &ParticleDrawExecutor::drawAfterIndirect), MR::DrawType_EffectDrawAfterIndirect);

    _14 = new NameObjAdaptor("2Dモデル用パーティクル");
    ::connectToSceneDrawAdaptor(_14, MR::Functor(this, &ParticleDrawExecutor::drawFor2DModel), MR::DrawType_EffectDrawFor2DModel);

    _18 = new NameObjAdaptor("ブルーム用パーティクル");
    ::connectToSceneDrawAdaptor(_18, MR::Functor(this, &ParticleDrawExecutor::drawForBloomEffect), MR::DrawType_EffectDrawForBloomEffect);

    _1C = new NameObjAdaptor("イメージエフェクト後パーティクル");
    ::connectToSceneDrawAdaptor(_1C, MR::Functor(this, &ParticleDrawExecutor::drawAfterImageEffect), MR::DrawType_EffectDrawAfterImageEffect);
}
