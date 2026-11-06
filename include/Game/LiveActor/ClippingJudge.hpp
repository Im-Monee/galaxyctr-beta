#pragma once

#include "Game/NameObj/NameObj.hpp"
#include <JSystem/JGeometry/THex.hpp>

class ClippingJudge : public NameObj {
public:
    ClippingJudge(const char*);

    virtual ~ClippingJudge();
    virtual void init(const JMapInfoIter&);

    virtual void movement();

    bool isJudgedToClipFrustum(const TVec3f&, f32) const;
    bool isJudgedToClipFrustum(const TVec3f&, f32, s32) const;
    void calcViewingVolume(THex3f*, f32);

    /* 0x0C */ THex3f mFrustum;
    /* 0x06C */ THex3f mClipFrustums[0x8];
    /* 0x36C */ f32 mClipDistances[0x8];
    // 3DS: the actor clipping test with the frustums scaled for the lowered detail
    // (ClippingJudge.cpp, hostPerfDetail).
#ifdef __3DS__
    bool isJudgedToClipActor(const TVec3f&, f32, s32) const;
    void updateActorScale();

    THex3f mActorFrustums[0x8];
    f32 mActorScale;
#endif
};

namespace MR {
    ClippingJudge* getClippingJudge();
};  // namespace MR
