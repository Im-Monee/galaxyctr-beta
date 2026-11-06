#include "Game/Screen/CopyFilterNegater.hpp"
#include <JSystem/JUtility/JUTVideo.hpp>

CopyFilterNegater::CopyFilterNegater() {
    // 3DS: no render mode and no copy filter to change.
#ifndef __3DS__
    mRenderObj = JUTVideo::getManager()->getRenderMode();
    GXSetCopyFilter(GX_FALSE, mRenderObj->sample_pattern, GX_FALSE, mRenderObj->vfilter);
#else
    mRenderObj = nullptr;
#endif
}

CopyFilterNegater::~CopyFilterNegater() {
    // Wii only: GX state, display lists and material setup; the 3DS draws this through Citro3D
    // (DETAILS.md, section 5).
#ifndef __3DS__
    GXSetCopyFilter(GX_FALSE, mRenderObj->sample_pattern, GX_TRUE, mRenderObj->vfilter);
#endif
}
