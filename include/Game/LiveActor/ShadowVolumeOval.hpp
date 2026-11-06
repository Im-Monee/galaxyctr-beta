#pragma once

#include "Game/LiveActor/ShadowVolumeModel.hpp"

class ShadowVolumeOval : public ShadowVolumeModel {
public:
    ShadowVolumeOval();
    ~ShadowVolumeOval();

    virtual void loadModelDrawMtx() const;
    virtual bool isDraw() const;
#ifdef __3DS__
    virtual void drawShape() const override;
#endif

    void setSize(const TVec3f&);

    /* 0x20 */ TVec3f mSize;
};
