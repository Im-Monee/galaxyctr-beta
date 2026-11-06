#include "Game/Screen/CopyFilterNegater.hpp"
#include <JSystem/JUtility/JUTVideo.hpp>

CopyFilterNegater::CopyFilterNegater() {
#ifndef __3DS__
    mRenderObj = JUTVideo::getManager()->getRenderMode();
    GXSetCopyFilter(GX_FALSE, mRenderObj->sample_pattern, GX_FALSE, mRenderObj->vfilter);
#else
    mRenderObj = nullptr;
#endif
}

CopyFilterNegater::~CopyFilterNegater() {
#ifndef __3DS__
    GXSetCopyFilter(GX_FALSE, mRenderObj->sample_pattern, GX_TRUE, mRenderObj->vfilter);
#endif
}
