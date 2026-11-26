#pragma once

#include "Game/Effect/SimpleEffectObj.hpp"

class AstroEffectObj : public SimpleEffectObj {
public:
    AstroEffectObj(const char*);

    virtual ~AstroEffectObj();
    virtual void init(const JMapInfoIter&);
    virtual bool receiveOtherMsg(u32, HitSensor*, HitSensor*);
    virtual void setStateMove();
#ifdef __3DS__
    // 3DS: the effect stops while the object is clipped (out of view, or past its far distance)
    // and starts again when it is back, as SimpleEffectObj does for the objects that ask for it.
    // The full Comet Observatory burns 73 torches (AstroTorchLightRed) and their flames were
    // simulated behind the camera too (owner's choice, 25 Nov 2026).
    virtual bool isSyncClipping() const {
        return true;
    }
#endif

    void selectNrvWait();

    s32 _9C;
};
