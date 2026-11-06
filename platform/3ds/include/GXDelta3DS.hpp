#pragma once

// ##############################################################################################
// # GX state changes as data                                                                   #
// #                                                                                            #
// # GX state changes a J3D draw makes after loading its material, as data. On the Wii they are #
// # small display lists called between the material and the shape: J3DModelX's sixteen mode    #
// # lists (shadow capture, silhouette, reflection, blur...) and Mario's dynamic lists (alpha,  #
// # light, rainbow colours, eye texture). The native renderer applies them over the material's #
// # translated state (platform/3ds/gx/native.cpp, gx_delta.cpp). Values are the GX ones.       #
// ##############################################################################################

#include <revolution/types.h>

struct ResTIMG;

struct RvlGXDelta {
    enum {
        CULL = 1 << 0,           // GXSetCullMode / GDSetGenMode2's cull
        ZMODE = 1 << 1,          // GXSetZMode
        ALPHA_COMPARE = 1 << 2,  // GXSetAlphaCompare
        BLEND = 1 << 3,          // GXSetBlendMode with colour and alpha update (GDSetBlendModeEtc)
        DST_ALPHA = 1 << 4,      // GXSetDstAlpha
        TEV_COLOR = 1 << 5,      // stage 0 colour inputs and operation
        TEV_ALPHA = 1 << 6,      // stage 0 alpha inputs and operation
        GENMODE = 1 << 7,        // GDSetGenMode2: texgen, channel and stage counts
        TEV_ORDER = 1 << 8,      // stage 0 order
        CHANNEL_COLOR0 = 1 << 9, // GXSetChanCtrl of COLOR0
        CHANNEL_ALPHA0 = 1 << 10,
        MAT_COLOR = 1 << 11,     // GXSetChanMatColor of COLOR0A0 (matMask: rgb 1, alpha 2)
        AMB_COLOR = 1 << 12,     // GXSetChanAmbColor of COLOR0A0 (ambMask)
        FOG = 1 << 13,           // GXSetFog
        STENCIL = 1 << 14,       // 3DS only: where the Wii counts in the destination alpha
        TEXTURE = 1 << 15,       // GXSetTexImgPtr of one texture map (another image)
        TEV_REGISTER = 1 << 16,  // GXSetTevColor of the registers in registerMask
    };

    u32 mask;
    u8 cull;
    u8 zEnable, zFunc, zUpdate;
    u8 alphaComp0, alphaRef0, alphaCompareOp, alphaComp1, alphaRef1;
    u8 blendType, blendSrc, blendDst, logicOp, colorUpdate, alphaUpdate;
    u8 dstAlphaEnable, dstAlpha;
    u8 colorIn[4], colorOp, colorBias, colorScale;
    u8 alphaIn[4], alphaOp, alphaBias, alphaScale;
    u8 texGens, channels, stages;
    u8 orderCoord, orderMap, orderChannel;
    u8 chanEnable[2], chanAmbSrc[2], chanMatSrc[2], chanLights[2], chanDiffuse[2], chanAttn[2];  // colour 0, alpha 0
    u8 matMask, ambMask;
    u8 matColor[4], ambColor[4];  // RGBA
    u8 fogType;
    u8 fogColor[4];
    f32 fogStart, fogEnd, fogNear, fogFar;
    u8 stencil[8];  // enabled, GPU_TESTFUNC, reference, mask, write mask, fail, zfail, zpass (3DS values)
    u8 textureMap;
    const ResTIMG* texture;
    u8 registerMask;        // bit r: GX_TEVPREV + r
    s16 registers[4][4];    // RGBA (GXColorS10)

    void clear();
    // Applies d's changes over these ones (a later display list overrides an earlier one).
    void merge(const RvlGXDelta& d);

    void setCull(u32 mode);
    void setZMode(bool enable, u32 func, bool update);
    void setAlphaCompare(u32 comp0, u8 ref0, u32 op, u32 comp1, u8 ref1);
    void setBlend(u32 type, u32 src, u32 dst, u32 op, bool colorUpdate, bool alphaUpdate);
    void setDstAlpha(bool enable, u8 alpha);
    void setTevColorIn(u32 a, u32 b, u32 c, u32 d, u32 op, u32 bias, u32 scale);
    void setTevAlphaIn(u32 a, u32 b, u32 c, u32 d, u32 op, u32 bias, u32 scale);
    void setGenMode(u32 texGens, u32 channels, u32 stages, u32 cull);
    void setTevOrder(u32 coord, u32 map, u32 channel);
    void setChanCtrl(u32 channel, bool enable, u32 ambSrc, u32 matSrc, u32 lights, u32 diffuse, u32 attn);
    void setMatColor(u32 channel, u8 r, u8 g, u8 b, u8 a);
    void setAmbColor(u32 channel, u8 r, u8 g, u8 b, u8 a);
    void setFog(u32 type, f32 start, f32 end, f32 nearZ, f32 farZ, u8 r, u8 g, u8 b, u8 a);
    void setStencil(u8 func, u8 ref, u8 fail, u8 zfail, u8 zpass);
    void setTexture(u32 map, const ResTIMG* pTexture);
    void setTevRegister(u32 reg, s16 r, s16 g, s16 b, s16 a);
};

// The changes of the next native draws (null: none). The pointer is read at each draw.
void rvlNativeSetDelta(const RvlGXDelta* pDelta);

// The next native draws through this projection, mapped onto the viewport (x, y, width, height
// in Wii frame buffer pixels, as GXSetViewport) and cut to the scissor rectangle (x, y, width,
// height, as GXSetScissor). Null restores the camera projection and the whole screen.
void rvlNativeSetViewport(const f32 (*pProjection)[4], const f32* pViewport, const u16* pScissor);

// Folds a GXSetViewport rectangle into a projection: the clip space the viewport maps onto the
// whole 640x456 frame buffer (GX's viewport transform), so a full-screen target draws into it.
void rvlViewportProjection(f32 (*pOut)[4], const f32 (*pProjection)[4], f32 x, f32 y, f32 width, f32 height);
