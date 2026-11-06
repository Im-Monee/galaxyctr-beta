#pragma once

#include "Game/LiveActor/ShadowVolumeModel.hpp"

class ShadowVolumeOvalPole : public ShadowVolumeModel {
public:
    ShadowVolumeOvalPole();

    virtual ~ShadowVolumeOvalPole();
    virtual void loadModelDrawMtx() const;
#ifdef __3DS__
    virtual void drawShape() const override;
#endif

    void setSize(const TVec3f&);

    /* 0x20 */ TVec3f mSize;
};
#ifdef __3DS__

#endif