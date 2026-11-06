#pragma once

#include "Game/LiveActor/ShadowVolumeModel.hpp"

class ShadowVolumeSphere : public ShadowVolumeModel {
public:
    ShadowVolumeSphere();

    virtual ~ShadowVolumeSphere();
    virtual void loadModelDrawMtx() const;
    virtual bool isDraw() const;
    // 3DS: the volume drawn through DirectDraw instead of a GX display list.
#ifdef __3DS__
    virtual void drawShape() const override;
#endif

    void setRadius(f32);

    /* 0x20 */ f32 mRadius;
};