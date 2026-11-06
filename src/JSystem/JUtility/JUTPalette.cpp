#include "JSystem/JUtility/JUTPalette.hpp"
#include <revolution/os.h>

void JUTPalette::storeTLUT(_GXTlut param_0, ResTLUT* tlut) {
    if (tlut == nullptr) {
        OSPanic(__FILE__, 35, "JUTTexture: TLUT is NULL\n");
    }
    mName = param_0;
    mFormat = tlut->mFormat;
    mTransparency = tlut->mTransparency;
    mLutNum = tlut->mLutNum;
    mColorTable = tlut + 8;
    // Wii only: GX state, display lists and material setup; the 3DS draws this through Citro3D
    // (DETAILS.md, section 5).
#ifndef __3DS__
    GXInitTlutObj(&mObj, mColorTable, static_cast< GXTlutFmt >(mFormat), mLutNum);
#endif
}

void JUTPalette::storeTLUT(_GXTlut param_0, _GXTlutFmt param_1, JUTTransparency param_2, u16 param_3, void* param_4) {
    mName = param_0;
    mFormat = param_1;
    mTransparency = param_2;
    mLutNum = param_3;
    mColorTable = static_cast< ResTLUT* >(param_4);
    // Wii only: GX state, display lists and material setup; the 3DS draws this through Citro3D
    // (DETAILS.md, section 5).
#ifndef __3DS__
    GXInitTlutObj(&mObj, mColorTable, static_cast< GXTlutFmt >(mFormat), mLutNum);
#endif
}

bool JUTPalette::load() {
    bool check = mLutNum != 0;
    // 3DS: no GX palette to load: palettes are decoded with their textures (rvl/jut_texture.cpp).
#ifdef __3DS__
    return check;
#else
    if (check) {
        GXLoadTlut(&mObj, mName);
    }

    return check;
#endif
}
