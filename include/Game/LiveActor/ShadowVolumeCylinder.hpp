#pragma once

#include "Game/LiveActor/ShadowVolumeModel.hpp"

class ShadowVolumeCylinder : public ShadowVolumeModel {
public:
    ShadowVolumeCylinder();

    virtual ~ShadowVolumeCylinder();
    virtual void loadModelDrawMtx() const;
    // 3DS: the volume drawn through DirectDraw instead of a GX display list.
#ifdef __3DS__
    virtual void drawShape() const override;
#endif

    void setRadius(f32);

    /* 0x20 */ f32 mRadius;
};