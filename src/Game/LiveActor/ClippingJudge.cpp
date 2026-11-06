#include "Game/LiveActor/ClippingJudge.hpp"
#include "Game/LiveActor/ClippingDirector.hpp"
#include "Game/Util.hpp"
// 3DS: host.h, the port's host interface.
#ifdef __3DS__
#include "host.h"
#endif

void ClippingJudge_FORCE_MATCH_SDATA2() {
    0.0f;
}

ClippingJudge::ClippingJudge(const char* pName) : NameObj(pName), mFrustum() {
    for (s32 i = 0; i < 8; i++) {
        mClipDistances[i] = -1.0f;
    }

    mClipDistances[0] = 0.0f;
    mClipDistances[1] = 60000.0f;
    mClipDistances[2] = 50000.0f;
    mClipDistances[3] = 40000.0f;
    mClipDistances[4] = 30000.0f;
    mClipDistances[5] = 20000.0f;
    mClipDistances[6] = 10000.0f;
    mClipDistances[7] = 5000.0f;
    // 3DS: no scaling until the detail is lowered.
#ifdef __3DS__
    mActorScale = 1.0f;
#endif
}

void ClippingJudge::init(const JMapInfoIter& rIter) {
}

void ClippingJudge::movement() {
    calcViewingVolume(&mFrustum, MR::getFarZ());

    for (u32 i = 1; i < 8; i++) {
        calcViewingVolume(&mClipFrustums[i], mClipDistances[i]);
    }

#ifdef __3DS__
    // With the 96 MB of an Old 3DS (hostDebugFarClip is 0 otherwise: the Wii's distances), actor
    // clipping (an actor clipped neither moves, animates nor draws) takes its own volumes: no level reaches past farclip
    // metres (the Wii's "far max" goes to the camera's far plane, 8 km, and levels 1 to 3 to
    // 400-600 m), and while the game runs behind the clock every level shrinks to 40%, never
    // under 50 m. mFrustum and mClipFrustums keep the Wii distances for the game's own tests.
    updateActorScale();
    f32 farClip = 100.0f * hostDebugFarClip();
    for (u32 i = 0; i < 8; i++) {
        f32 distance = i == 0 ? MR::getFarZ() : mClipDistances[i];
        if (farClip > 0.0f) {
            if (distance > farClip) {
                distance = farClip;
            }
            f32 scaled = distance * mActorScale;
            distance = scaled > 5000.0f ? scaled : (distance < 5000.0f ? distance : 5000.0f);
        }
        calcViewingVolume(&mActorFrustums[i], distance);
    }
#endif
}

#ifdef __3DS__
// The host's detail level (hostPerfDetail): it falls while the game runs behind the clock, down
// to 40% of the distances, and rises back once it keeps up.
void ClippingJudge::updateActorScale() {
    mActorScale = hostPerfDetail();
}

bool ClippingJudge::isJudgedToClipActor(const TVec3f& rVec, f32 radius, s32 level) const {
    return !mActorFrustums[level].mayIntersectBall3(rVec, radius);
}
#endif

bool ClippingJudge::isJudgedToClipFrustum(const TVec3f& rVec, f32 a2) const {
    return !mFrustum.mayIntersectBall3(rVec, a2);
}

bool ClippingJudge::isJudgedToClipFrustum(const TVec3f& rVec, f32 a2, s32 index) const {
    return (!index) ? !mFrustum.mayIntersectBall3(rVec, a2) : !mClipFrustums[index].mayIntersectBall3(rVec, a2);
}

void ClippingJudge::calcViewingVolume(THex3f* pVolume, f32 farClip) {
    f32 nearClip = 500.0f;
    if (MR::isSubjectiveCameraOnForObjClipping()) {
        nearClip = 100.0f;
    }

    f32 aspect = MR::getAspect();
    f32 fovy = MR::getFovy();
    TPos3f cameraMtx;
    cameraMtx.setPositionFromLookAt(MR::getCameraViewMtx());
    f32 height = nearClip * static_cast< f32 >(tan(fovy * (JGeometry::TUtil< f32 >::PI() / 360.0f)));
    f32 width = aspect * height;

    TVec3f front;
    cameraMtx.getZDir(front);
    TVec3f position;
    TVec3f bottomRight(width, -height, nearClip);
    TVec3f topRight(width, height, nearClip);
    TVec3f topLeft(-width, height, nearClip);
    TVec3f bottomLeft(-width, -height, nearClip);
    cameraMtx.getTrans(position);
    cameraMtx.mult(topRight, topRight);
    cameraMtx.mult(topLeft, topLeft);
    cameraMtx.mult(bottomLeft, bottomLeft);
    cameraMtx.mult(bottomRight, bottomRight);

    pVolume->mPlanes[3].set(position, topLeft, topRight);
    pVolume->mPlanes[1].set(position, topRight, bottomRight);
    pVolume->mPlanes[2].set(position, bottomRight, bottomLeft);
    pVolume->mPlanes[0].set(position, bottomLeft, topLeft);

    TVec3f back;
    back.negate(front);
    TVec3f planePosition;
    planePosition.scaleAdd(farClip, front, position);
    pVolume->mPlanes[5].set(back, planePosition);
    planePosition.scaleAdd(nearClip, front, position);
    pVolume->mPlanes[4].set(front, planePosition);
}

namespace MR {
    ClippingJudge* getClippingJudge() {
        return getClippingDirector()->mJudge;
    }
};  // namespace MR

ClippingJudge::~ClippingJudge() {
}
