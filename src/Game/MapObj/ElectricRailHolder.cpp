#include "Game/MapObj/ElectricRailHolder.hpp"
#include "Game/LiveActor/HitSensor.hpp"
#include "Game/LiveActor/ModelObj.hpp"
#include "Game/MapObj/ElectricRailMoving.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/ModelUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/RailUtil.hpp"
// 3DS: DirectDraw, TDDraw's Citro3D draws.
#ifdef __3DS__
#include "Game/Util/DirectDraw.hpp"
#endif
// 3DS: NativeMaterial3DS.hpp, a model's material for immediate geometry.
#ifdef __3DS__
#include "NativeMaterial3DS.hpp"
#endif

void ElectricRailHolder_FORCE_MATCH_SDATA2() {
    (void)30.0f;
}

namespace {
    const f32 cHitSensorRadius = 30.0f;

    ElectricRailHolder* getHolder() {
        return MR::getSceneObj< ElectricRailHolder >(SceneObj_ElectricRailHolder);
    }

    bool isNearRail(const LiveActor* pActor, const HitSensor* pSensor, TVec3f* pRailPos, TVec3f* pRailDir) NO_INLINE {
        TVec3f railPos;
        f32 coord = MR::calcNearestRailPos(&railPos, pActor, pSensor->mPosition);

        const f32 sensorRadius = pSensor->mRadius;
        f32 radius = ::cHitSensorRadius + sensorRadius;

        if (railPos.squared(pSensor->mPosition) < radius * radius) {
            if (pRailPos != nullptr) {
                pRailPos->set(railPos);
            }

            if (pRailDir != nullptr) {
                MR::calcRailDirectionAtCoord(pRailDir, pActor, coord);
            }

            return true;
        }

        return false;
    }
};  // namespace

ElectricRailHolder::ElectricRailHolder(const char* pName) : LiveActor(pName), mIsActive() {
    MR::zeroMemory(&mRailModels, sizeof(mRailModels));
    MR::zeroMemory(&mRailItems, sizeof(mRailItems.mArray));
}

void ElectricRailHolder::calcAnim() {
    if (!mIsActive) {
        return;
    }

    for (s32 i = 0; i < ARRAY_SIZE(mRailModels); i++) {
        if (mRailModels[i] != nullptr) {
            mRailModels[i]->calcAnim();
        }
    }
}

void ElectricRailHolder::draw() const {
    if (!mIsActive) {
        return;
    }

#ifdef __3DS__
    // The rail model's material (as its display list loads it on the Wii) for the rails' own
    // immediate strips: texture coordinate 0 through the material's texture matrix.
    for (s32 i = 0; i < mRailItems.size(); i++) {
        RailItem* pItem = mRailItems[i];
        if (!MR::isValidDraw(pItem->mHost)) {
            continue;
        }
        J3DMatPacket* pPacket = pItem->mMatPacket;
        HostGfxState state;
        f32 texMtx[3][3][4];
        u32 rasterColor;
        if (!rvlNativeMaterialState(pPacket->getMaterial(), pPacket->getShapePacket(), state, texMtx, rasterColor)) {
            continue;
        }
        TDDraw::setupDirect(1, 1, 0);
        TDDraw::setDirectStagesFromState(state);
        TDDraw::setDirectVertexColor(rasterColor);
        TDDraw::setDirectTexGen(0, TDDraw::DIRECT_TEXGEN_TEX0, texMtx[0], false);
        TDDraw::setDirectBlendState(state.blendEquation, state.blendSrc, state.blendDst);
        TDDraw::setDirectAlphaTest(state.alphaTestFunc, state.alphaRef);
        TDDraw::setDirectDepthMode(state.depthTest != 0, state.depthFunc, (state.writeMask & 0x10) != 0);
        TDDraw::setDirectCullMode(state.cull);
        pItem->mHost->draw();
    }
    TDDraw::closeDirect();
    return;
#endif
    initDraw();

    for (s32 i = 0; i < mRailItems.size(); i++) {
        RailItem* pItem = mRailItems[i];

        if (!MR::isValidDraw(pItem->mHost)) {
            continue;
        }

        J3DMatPacket* pPacket = pItem->mMatPacket;
        pItem->mMatPacket->callDL();

        if (pPacket->mpShapePacket->mpDisplayListObj != nullptr) {
            pPacket->mpShapePacket->mpDisplayListObj->callDL();
        }

        pItem->mHost->draw();
    }
}

bool ElectricRailHolder::isTouchRail(const HitSensor* pSensor, TVec3f* pVec1, TVec3f* pVec2) const {
    if (!mIsActive) {
        return false;
    }

    for (s32 i = 0; i < mRailItems.size(); i++) {
        RailItem* pItem = mRailItems[i];

        if (!MR::isValidDraw(pItem->mHost)) {
            continue;
        }

        if (pItem->mRailType == ElectricRail_RedMoving || pItem->mRailType == ElectricRail_YellowMoving) {
            if (pItem->mHost->isTouchRail(pSensor, pVec1, pVec2)) {
                return true;
            }
        } else {
            if (::isNearRail(pItem->mHost, pSensor, pVec1, pVec2)) {
                return true;
            }
        }
    }

    return false;
}

void ElectricRailHolder::control() {
    mIsActive = false;

    for (s32 i = 0; i < mRailItems.size(); i++) {
        if (MR::isValidDraw(mRailItems[i]->mHost)) {
            mIsActive = true;
            break;
        }
    }

    if (!mIsActive) {
        return;
    }

    for (s32 i = 0; i < 4; i++) {
        if (mRailModels[i] != nullptr) {
            mRailModels[i]->movement();
        }
    }
}

void ElectricRailHolder::initDraw() const {
#ifndef __3DS__  // 3DS: the rails draw through TDDraw
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_CLR_RGBA, GX_RGBA6, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_CLR_RGBA, GX_RGBA6, 0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXLoadPosMtxImm(MR::getCameraViewMtx(), GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, 30, GX_FALSE, 125);
#endif
}

void ElectricRailFunction::registerRail(const LiveActor* pActor, ElectricRailType railType) {
    MR::createSceneObj(SceneObj_ElectricRailHolder);
    ::getHolder()->registerRail(pActor, railType);
}

bool ElectricRailFunction::isTouchRail(const HitSensor* pSensor, TVec3f* pVec1, TVec3f* pVec2) {
    if (!MR::isExistSceneObj(SceneObj_ElectricRailHolder)) {
        return false;
    }

    return ::getHolder()->isTouchRail(pSensor, pVec1, pVec2);
}

f32 ElectricRailFunction::getHitSensorRadius() {
    return ::cHitSensorRadius;
}

u32 ElectricRailFunction::calcDisplayListSize(u32 size, s32 count) {
    return (((size * count + 3) / 32) + 2) * 32;
}

void ElectricRailHolder::init(const JMapInfoIter& rIter) {
    MR::connectToScene(this, MR::MovementType_MapObjDecoration, MR::CalcAnimType_MapObj, MR::DrawBufferType_None, MR::DrawType_ElectricRailHolder);

    MR::invalidateClipping(this);

    MR::registerDemoSimpleCastAll(this);

    MR::createAdaptorAndConnectToDrawBloomModel("電撃レールブルーム描画", MR::Functor(this, &ElectricRailHolder::draw));

    makeActorAppeared();
}

void ElectricRailHolder::registerRail(const LiveActor* pActor, ElectricRailType railType) {
    if (mRailModels[railType] == nullptr) {
        mRailModels[railType] = createModel(railType);
    }

    RailItem* pItem = new RailItem();
    (*mRailItems.end()) = pItem;
    (*mRailItems.end())->mRailType = railType;
    (*mRailItems.end())->mHost = static_cast< const ElectricRailMoving* >(pActor);

    J3DModel* pModel = MR::getJ3DModel(mRailModels[railType]);
    J3DMatPacket* pPacket = pModel->getMatPacket(MR::getMaterialNo(pModel, "SparkMat"));
    (*mRailItems.end())->mMatPacket = pPacket;

    mRailItems.mCount++;
}

ModelObj* ElectricRailHolder::createModel(ElectricRailType railType) const {
    ModelObj* pModel;
    const char* pModelName;
    const char* pName;

    switch (railType) {
    case ElectricRail_Red:
        pModelName = "ElectricRail";
        pName = "電撃レールモデル(黄)";
        pModel = new ModelObj(pName, pModelName, nullptr, MR::DrawBufferType_None, MR::MovementType_None, MR::CalcAnimType_None, false);
        pModel->initWithoutIter();

        MR::startBtk(pModel, "ElectricRail");
        MR::startBrk(pModel, "ElectricRail");
        MR::setBrkFrameAndStop(pModel, 0.0f);

        break;
    case ElectricRail_Yellow:
        pModelName = "ElectricRail";
        pName = "電撃レールモデル(赤)";
        pModel = new ModelObj(pName, pModelName, nullptr, MR::DrawBufferType_None, MR::MovementType_None, MR::CalcAnimType_None, false);
        pModel->initWithoutIter();

        MR::startBtk(pModel, "ElectricRail");
        MR::startBrk(pModel, "ElectricRail");
        MR::setBrkFrameAndStop(pModel, 1.0f);

        break;
    case ElectricRail_YellowMoving:
        pModelName = "ElectricRailMoving";
        pName = "移動電撃レールモデル(黄)";
        pModel = new ModelObj(pName, pModelName, nullptr, MR::DrawBufferType_None, MR::MovementType_None, MR::CalcAnimType_None, false);
        pModel->initWithoutIter();

        MR::startBtk(pModel, "ElectricRailMoving");
        MR::startBrk(pModel, "ElectricRailMoving");
        MR::setBrkFrameAndStop(pModel, 0.0f);

        break;
    case ElectricRail_RedMoving:;
        pModelName = "ElectricRailMoving";
        pName = "移動電撃レールモデル(赤)";
        pModel = new ModelObj(pName, pModelName, nullptr, MR::DrawBufferType_None, MR::MovementType_None, MR::CalcAnimType_None, false);
        pModel->initWithoutIter();

        MR::startBtk(pModel, "ElectricRailMoving");
        MR::startBrk(pModel, "ElectricRailMoving");
        MR::setBrkFrameAndStop(pModel, 1.0f);

        break;
    }

    return pModel;
}
