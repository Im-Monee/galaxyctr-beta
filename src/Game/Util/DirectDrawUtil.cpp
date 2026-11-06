#include "Game/Util/DirectDrawUtil.hpp"

extern "C" {
void GXPosition3f32(f32, f32, f32);
void GXNormal3f32(f32, f32, f32);
void GXTexCoord2f32(f32, f32);
}

u32 mVtxMode;
u32 mLightingFlag;
u16 mLightingMask;

namespace MR {
    void ddSetVtxFormat(u32 format) {
        mVtxMode = format;
        // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
        // (DETAILS.md, section 5).
#ifdef __3DS__
        return;
#else
        u32 mode = mVtxMode;
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        if ((mode & 1) != 0) {
            GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
        }

        if ((mode & 2) != 0) {
            GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        }

        if ((mode & 4) != 0) {
            GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        }

        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_RGBA6, 0);
        if ((mode & 1) != 0) {
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_POS_XY, GX_RGBA6, 0);
        }

        if ((mode & 2) != 0) {
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_POS_XYZ, GX_RGBA6, 0);
        }

        if ((mode & 4) != 0) {
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_POS_XYZ, GX_RGBA8, 0);
        }

        if (mLightingFlag != 0) {
            ddLightingOn(mLightingMask);
        } else {
            ddLightingOff();
        }

        ddChangeTev();
#endif
    }

    void ddChangeTev() {
        // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
        // (DETAILS.md, section 5).
#ifdef __3DS__
        return;
#else
        if ((mVtxMode & 2) != 0) {
            GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
            GXSetNumTexGens(1);
            GXSetNumTevStages(1);
            if (mLightingFlag != 0) {
                GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
                GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
            } else {
                GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
                GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
            }
        } else {
            if ((mVtxMode & 8) != 0) {
                GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_NRM, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
                GXSetNumTexGens(1);
                GXSetNumTevStages(1);
                if (mLightingFlag != 0) {
                    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
                    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
                } else {
                    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
                    GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
                }
            } else {
                GXSetNumTexGens(0);
                GXSetNumTevStages(1);
                GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
                GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
            }
        }
#endif
    }

    void ddSendVtxData(const TVec3f& rVec1, const TVec3f& rVec2, const TVec2f& rVec3) {
        // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
        // (DETAILS.md, section 5).
#ifdef __3DS__
        (void)rVec1;
        (void)rVec2;
        (void)rVec3;
#else
        GXPosition3f32(rVec1.x, rVec1.y, rVec1.z);
        if ((mVtxMode & 1) != 0) {
            GXNormal3f32(rVec2.x, rVec2.y, rVec2.z);
        }
#ifndef __3DS__

#endif
        if ((mVtxMode & 2) != 0) {
            GXTexCoord2f32(rVec3.x, rVec3.y);
        }
#endif
    }

    void ddSendVtxData(const TVec3f& rVec1, const TVec2f& rVec2) {
        // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
        // (DETAILS.md, section 5).
#ifdef __3DS__
        (void)rVec1;
        (void)rVec2;
#else
        GXPosition3f32(rVec1.x, rVec1.y, rVec1.z);
        if ((mVtxMode & 2) != 0) {
            GXTexCoord2f32(rVec2.x, rVec2.y);
        }
#endif
    }

    void ddLightingOn(u16 light) {
        mLightingFlag = 1;
        mLightingMask = light;
        // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
        // (DETAILS.md, section 5).
#ifdef __3DS__
        return;
#else
        if ((mVtxMode & 4) != 0) {
            GXSetChanCtrl(GX_COLOR0A0, GX_TRUE, GX_SRC_VTX, GX_SRC_VTX, mLightingMask, GX_DF_CLAMP, GX_AF_NONE);
        } else {
            GXSetChanCtrl(GX_COLOR0A0, GX_TRUE, GX_SRC_REG, GX_SRC_REG, mLightingMask, GX_DF_CLAMP, GX_AF_NONE);
        }
#endif
    }

    void ddLightingOff() {
        mLightingFlag = 0;
        // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
        // (DETAILS.md, section 5).
#ifdef __3DS__
        return;
#else
        if ((mVtxMode & 4) != 0) {
            GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
        } else {
            GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
        }
#endif
    }
};  // namespace MR

void DirectDrawUtil_FORCE_MATCH(const TVec3f& rNormal) {
    // 3DS: nothing to do here: the Wii branch works with GX, which the 3DS build does not have
    // (DETAILS.md, section 5).
#ifndef __3DS__
    GXNormal3f32(rNormal.x, rNormal.y, rNormal.z);
#else
    (void)rNormal;
#endif
}
