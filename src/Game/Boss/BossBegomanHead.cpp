// 3DS: host.h, the port's host interface (hostGfxNextFrameDrawn).
#ifdef __3DS__
#include <host.h>
#endif
#include "Game/Boss/BossBegomanHead.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util/JointController.hpp"
#include "Game/Util/LiveActorUtil.hpp"

namespace {
    static const f32 hSpikeRotate = 0.23f;
    static const f32 hSpikeRotateTurn = 0.2f;
};  // namespace

namespace NrvBossBegomanHead {
    NEW_NERVE(HostTypeNrvDemoWait, BossBegomanHead, DemoWait);
    NEW_NERVE(HostTypeNrvOpeningDemo, BossBegomanHead, OpeningDemo);
    NEW_NERVE(HostTypeNrvOnWait, BossBegomanHead, OnWait);
    NEW_NERVE(HostTypeNrvOffWait, BossBegomanHead, OffWait);
    NEW_NERVE(HostTypeNrvSwitchOn, BossBegomanHead, SwitchOn);
    NEW_NERVE(HostTypeNrvSwitchOff, BossBegomanHead, SwitchOff);
    NEW_NERVE(HostTypeNrvTurn, BossBegomanHead, Turn);
    NEW_NERVE(HostTypeNrvTurnEnd, BossBegomanHead, TurnEnd);
};  // namespace NrvBossBegomanHead

BossBegomanHead::BossBegomanHead(LiveActor* pParent, MtxPtr pMtx)
    : PartsModel(pParent, "スイッチ頭", "BossBegomanHead", pMtx, MR::DrawBufferType_Enemy, false), mAngle(), mJointDelegator() {
#ifdef __3DS__
    mShownAngle3DS = 0.0f;
    mLastAngle3DS = 0.0f;
#endif
}

void BossBegomanHead::init(const JMapInfoIter& rIter) {
    initNerve(GET_NERVE(BossBegomanHead, HostTypeNrvDemoWait));

    mJointDelegator = MR::createJointDelegatorWithNullChildFunc(this, &BossBegomanHead::calcJointEdge, "Edge");

    PartsModel::init(rIter);
    MR::initLightCtrl(this);
    appear();
}

bool BossBegomanHead::isSwitchOn() {
    return isNerve(GET_NERVE(BossBegomanHead, HostTypeNrvDemoWait)) || isNerve(GET_NERVE(BossBegomanHead, HostTypeNrvOpeningDemo)) ||
           isNerve(GET_NERVE(BossBegomanHead, HostTypeNrvOnWait)) || isNerve(GET_NERVE(BossBegomanHead, HostTypeNrvSwitchOn));
}

bool BossBegomanHead::isEdgeOut() {
    return isNerve(GET_NERVE(BossBegomanHead, HostTypeNrvOffWait)) || isNerve(GET_NERVE(BossBegomanHead, HostTypeNrvSwitchOff)) ||
           isNerve(GET_NERVE(BossBegomanHead, HostTypeNrvTurn)) || isNerve(GET_NERVE(BossBegomanHead, HostTypeNrvTurnEnd));
}

void BossBegomanHead::setOpeningDemo() {
    setNerve(GET_NERVE(BossBegomanHead, HostTypeNrvOpeningDemo));
}

void BossBegomanHead::trySwitchPushTrample() {
    setNerve(GET_NERVE(BossBegomanHead, HostTypeNrvSwitchOn));
}

void BossBegomanHead::tryForceRecover() {
    setNerve(GET_NERVE(BossBegomanHead, HostTypeNrvSwitchOff));
}

void BossBegomanHead::tryTurn() {
    setNerve(GET_NERVE(BossBegomanHead, HostTypeNrvTurn));
}

void BossBegomanHead::tryTurnEnd() {
    setNerve(GET_NERVE(BossBegomanHead, HostTypeNrvTurnEnd));
}

void BossBegomanHead::exeDemoWait() {
    if (MR::isFirstStep(this)) {
        MR::startAction(this, "DemoWait");
    }
}

void BossBegomanHead::exeOpeningDemo() {
    if (MR::isFirstStep(this)) {
        MR::startAction(this, "OpeningDemo");
    }
}

void BossBegomanHead::exeOnWait() {
    if (MR::isFirstStep(this)) {
        MR::startAction(this, "OnWait");
    }
}

void BossBegomanHead::exeOffWait() {
    if (MR::isFirstStep(this)) {
        MR::startAction(this, "OffWait");
    }

    mAngle -= ::hSpikeRotate;
}

void BossBegomanHead::exeSwitchOn() {
    if (MR::isFirstStep(this)) {
        MR::startAction(this, "On");
    }

    MR::setNerveAtBckStopped(this, GET_NERVE(BossBegomanHead, HostTypeNrvOnWait));
}

void BossBegomanHead::exeSwitchOff() {
    if (MR::isFirstStep(this)) {
        MR::startAction(this, "Off");
    }

    MR::setNerveAtBckStopped(this, GET_NERVE(BossBegomanHead, HostTypeNrvOffWait));
}

void BossBegomanHead::exeTurn() {
    if (MR::isFirstStep(this)) {
        MR::startAction(this, "Turn");
    }

    mAngle -= ::hSpikeRotateTurn;
}

void BossBegomanHead::exeTurnEnd() {
    if (MR::isFirstStep(this)) {
        MR::startAction(this, "TurnEnd");
    }

    mAngle -= ::hSpikeRotate;

    MR::setNerveAtBckStopped(this, GET_NERVE(BossBegomanHead, HostTypeNrvOffWait));
}

void BossBegomanHead::calcAndSetBaseMtx() {
    PartsModel::calcAndSetBaseMtx();
    mJointDelegator->registerCallBack();
}

bool BossBegomanHead::calcJointEdge(TPos3f* pMtx, const JointControllerInfo&) {
    TPos3f v9;
#ifdef __3DS__
    // ##############################################################################################
    // # The spike ring's turn, one step per drawn image                                            #
    // #                                                                                            #
    // # The ring has 9 spikes, one every 40 degrees, and turns 0.23 radians (13.2 degrees) a loop. #
    // # Drawing one loop in three (the port when it is behind), each image showed it 39.5 degrees  #
    // # on, almost one spike: the spikes looked still while the trail under them moved (owner,     #
    // # Topmaniac, CIA 0.21.15). The angle shown moves on by the loop's turn only in loops that    #
    // # are drawn: each image shows one step, as each of the Wii's does. With every loop drawn it  #
    // # is the game's angle. The ring's angle is only seen: the edge's attack does not read it.    #
    // ##############################################################################################
    if (hostGfxNextFrameDrawn()) {
        mShownAngle3DS += mAngle - mLastAngle3DS;
    }
    mLastAngle3DS = mAngle;
    v9.makeRotate(TVec3f(0.0f, 1.0f, 0.0f), mShownAngle3DS);
#else
    v9.makeRotate(TVec3f(0.0f, 1.0f, 0.0f), mAngle);
#endif
    pMtx->concat(*pMtx, v9);

    return true;
}
