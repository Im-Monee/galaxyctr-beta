// RvlGXDelta (GXDelta3DS.hpp): GX state changes recorded as data, and the viewport folding the
// native renderer uses for draws into part of the frame buffer.

#include "GXDelta3DS.hpp"
#include <cstring>

void RvlGXDelta::clear() {
    std::memset(this, 0, sizeof(*this));
}

void RvlGXDelta::merge(const RvlGXDelta& d) {
    const u32 m = d.mask;
    if (m & CULL) {
        cull = d.cull;
    }
    if (m & ZMODE) {
        zEnable = d.zEnable;
        zFunc = d.zFunc;
        zUpdate = d.zUpdate;
    }
    if (m & ALPHA_COMPARE) {
        alphaComp0 = d.alphaComp0;
        alphaRef0 = d.alphaRef0;
        alphaCompareOp = d.alphaCompareOp;
        alphaComp1 = d.alphaComp1;
        alphaRef1 = d.alphaRef1;
    }
    if (m & BLEND) {
        blendType = d.blendType;
        blendSrc = d.blendSrc;
        blendDst = d.blendDst;
        logicOp = d.logicOp;
        colorUpdate = d.colorUpdate;
        alphaUpdate = d.alphaUpdate;
    }
    if (m & DST_ALPHA) {
        dstAlphaEnable = d.dstAlphaEnable;
        dstAlpha = d.dstAlpha;
    }
    if (m & TEV_COLOR) {
        std::memcpy(colorIn, d.colorIn, sizeof(colorIn));
        colorOp = d.colorOp;
        colorBias = d.colorBias;
        colorScale = d.colorScale;
    }
    if (m & TEV_ALPHA) {
        std::memcpy(alphaIn, d.alphaIn, sizeof(alphaIn));
        alphaOp = d.alphaOp;
        alphaBias = d.alphaBias;
        alphaScale = d.alphaScale;
    }
    if (m & GENMODE) {
        texGens = d.texGens;
        channels = d.channels;
        stages = d.stages;
    }
    if (m & TEV_ORDER) {
        orderCoord = d.orderCoord;
        orderMap = d.orderMap;
        orderChannel = d.orderChannel;
    }
    for (u32 c = 0; c < 2; c++) {
        if (m & (CHANNEL_COLOR0 << c)) {
            chanEnable[c] = d.chanEnable[c];
            chanAmbSrc[c] = d.chanAmbSrc[c];
            chanMatSrc[c] = d.chanMatSrc[c];
            chanLights[c] = d.chanLights[c];
            chanDiffuse[c] = d.chanDiffuse[c];
            chanAttn[c] = d.chanAttn[c];
        }
    }
    if (m & MAT_COLOR) {
        for (u32 i = 0; i < 4; i++) {
            if (d.matMask & (i < 3 ? 1 : 2)) {
                matColor[i] = d.matColor[i];
            }
        }
        matMask |= d.matMask;
    }
    if (m & AMB_COLOR) {
        for (u32 i = 0; i < 4; i++) {
            if (d.ambMask & (i < 3 ? 1 : 2)) {
                ambColor[i] = d.ambColor[i];
            }
        }
        ambMask |= d.ambMask;
    }
    if (m & FOG) {
        fogType = d.fogType;
        std::memcpy(fogColor, d.fogColor, sizeof(fogColor));
        fogStart = d.fogStart;
        fogEnd = d.fogEnd;
        fogNear = d.fogNear;
        fogFar = d.fogFar;
    }
    if (m & STENCIL) {
        std::memcpy(stencil, d.stencil, sizeof(stencil));
    }
    if (m & TEXTURE) {
        textureMap = d.textureMap;
        texture = d.texture;
    }
    if (m & TEV_REGISTER) {
        for (u32 r = 0; r < 4; r++) {
            if (d.registerMask & (1 << r)) {
                std::memcpy(registers[r], d.registers[r], sizeof(registers[r]));
            }
        }
        registerMask |= d.registerMask;
    }
    mask |= m;
}

void RvlGXDelta::setCull(u32 mode) {
    mask |= CULL;
    cull = static_cast< u8 >(mode);
}

void RvlGXDelta::setZMode(bool enable, u32 func, bool update) {
    mask |= ZMODE;
    zEnable = enable;
    zFunc = static_cast< u8 >(func);
    zUpdate = update;
}

void RvlGXDelta::setAlphaCompare(u32 comp0, u8 ref0, u32 op, u32 comp1, u8 ref1) {
    mask |= ALPHA_COMPARE;
    alphaComp0 = static_cast< u8 >(comp0);
    alphaRef0 = ref0;
    alphaCompareOp = static_cast< u8 >(op);
    alphaComp1 = static_cast< u8 >(comp1);
    alphaRef1 = ref1;
}

void RvlGXDelta::setBlend(u32 type, u32 src, u32 dst, u32 op, bool colorUpdateEnable, bool alphaUpdateEnable) {
    mask |= BLEND;
    blendType = static_cast< u8 >(type);
    blendSrc = static_cast< u8 >(src);
    blendDst = static_cast< u8 >(dst);
    logicOp = static_cast< u8 >(op);
    colorUpdate = colorUpdateEnable;
    alphaUpdate = alphaUpdateEnable;
}

void RvlGXDelta::setDstAlpha(bool enable, u8 alpha) {
    mask |= DST_ALPHA;
    dstAlphaEnable = enable;
    dstAlpha = alpha;
}

void RvlGXDelta::setTevColorIn(u32 a, u32 b, u32 c, u32 d, u32 op, u32 bias, u32 scale) {
    mask |= TEV_COLOR;
    colorIn[0] = static_cast< u8 >(a);
    colorIn[1] = static_cast< u8 >(b);
    colorIn[2] = static_cast< u8 >(c);
    colorIn[3] = static_cast< u8 >(d);
    colorOp = static_cast< u8 >(op);
    colorBias = static_cast< u8 >(bias);
    colorScale = static_cast< u8 >(scale);
}

void RvlGXDelta::setTevAlphaIn(u32 a, u32 b, u32 c, u32 d, u32 op, u32 bias, u32 scale) {
    mask |= TEV_ALPHA;
    alphaIn[0] = static_cast< u8 >(a);
    alphaIn[1] = static_cast< u8 >(b);
    alphaIn[2] = static_cast< u8 >(c);
    alphaIn[3] = static_cast< u8 >(d);
    alphaOp = static_cast< u8 >(op);
    alphaBias = static_cast< u8 >(bias);
    alphaScale = static_cast< u8 >(scale);
}

void RvlGXDelta::setGenMode(u32 texGenCount, u32 channelCount, u32 stageCount, u32 cullMode) {
    mask |= GENMODE;
    texGens = static_cast< u8 >(texGenCount);
    channels = static_cast< u8 >(channelCount);
    stages = static_cast< u8 >(stageCount);
    setCull(cullMode);
}

void RvlGXDelta::setTevOrder(u32 coord, u32 map, u32 channel) {
    mask |= TEV_ORDER;
    orderCoord = static_cast< u8 >(coord);
    orderMap = static_cast< u8 >(map);
    orderChannel = static_cast< u8 >(channel);
}

void RvlGXDelta::setChanCtrl(u32 channel, bool enable, u32 ambSrc, u32 matSrc, u32 lights, u32 diffuse, u32 attn) {
    // GXChannelID: GX_COLOR0 0, GX_ALPHA0 2, GX_COLOR0A0 4 (channel 1 is not used by these lists).
    for (u32 c = 0; c < 2; c++) {
        bool set = c == 0 ? (channel == 0 || channel == 4) : (channel == 2 || channel == 4);
        if (!set) {
            continue;
        }
        mask |= CHANNEL_COLOR0 << c;
        chanEnable[c] = enable;
        chanAmbSrc[c] = static_cast< u8 >(ambSrc);
        chanMatSrc[c] = static_cast< u8 >(matSrc);
        chanLights[c] = static_cast< u8 >(lights);
        chanDiffuse[c] = static_cast< u8 >(diffuse);
        chanAttn[c] = static_cast< u8 >(attn);
    }
}

void RvlGXDelta::setMatColor(u32 channel, u8 r, u8 g, u8 b, u8 a) {
    u8 parts = channel == 0 ? 1 : channel == 2 ? 2 : channel == 4 ? 3 : 0;
    if (parts == 0) {
        return;
    }
    mask |= MAT_COLOR;
    matMask |= parts;
    if (parts & 1) {
        matColor[0] = r;
        matColor[1] = g;
        matColor[2] = b;
    }
    if (parts & 2) {
        matColor[3] = a;
    }
}

void RvlGXDelta::setAmbColor(u32 channel, u8 r, u8 g, u8 b, u8 a) {
    u8 parts = channel == 0 ? 1 : channel == 2 ? 2 : channel == 4 ? 3 : 0;
    if (parts == 0) {
        return;
    }
    mask |= AMB_COLOR;
    ambMask |= parts;
    if (parts & 1) {
        ambColor[0] = r;
        ambColor[1] = g;
        ambColor[2] = b;
    }
    if (parts & 2) {
        ambColor[3] = a;
    }
}

void RvlGXDelta::setFog(u32 type, f32 start, f32 end, f32 nearZ, f32 farZ, u8 r, u8 g, u8 b, u8 a) {
    mask |= FOG;
    fogType = static_cast< u8 >(type);
    fogStart = start;
    fogEnd = end;
    fogNear = nearZ;
    fogFar = farZ;
    fogColor[0] = r;
    fogColor[1] = g;
    fogColor[2] = b;
    fogColor[3] = a;
}

void RvlGXDelta::setStencil(u8 func, u8 ref, u8 fail, u8 zfail, u8 zpass) {
    mask |= STENCIL;
    stencil[0] = 1;
    stencil[1] = func;
    stencil[2] = ref;
    stencil[3] = 0xFF;
    stencil[4] = 0xFF;
    stencil[5] = fail;
    stencil[6] = zfail;
    stencil[7] = zpass;
}

void RvlGXDelta::setTexture(u32 map, const ResTIMG* pTexture) {
    mask |= TEXTURE;
    textureMap = static_cast< u8 >(map);
    texture = pTexture;
}

void RvlGXDelta::setTevRegister(u32 reg, s16 r, s16 g, s16 b, s16 a) {
    if (reg > 3) {
        return;
    }
    mask |= TEV_REGISTER;
    registerMask |= 1 << reg;
    registers[reg][0] = r;
    registers[reg][1] = g;
    registers[reg][2] = b;
    registers[reg][3] = a;
}

void rvlViewportProjection(f32 (*pOut)[4], const f32 (*pProjection)[4], f32 x, f32 y, f32 width, f32 height) {
    // GX: frame buffer x = x + (ndc.x + 1) / 2 * width, y = y + (1 - ndc.y) / 2 * height.
    const f32 sx = width / 640.0f, ox = (2.0f * x + width) / 640.0f - 1.0f;
    const f32 sy = height / 456.0f, oy = 1.0f - (2.0f * y + height) / 456.0f;
    f32 out[4][4];
    for (u32 k = 0; k < 4; k++) {
        out[0][k] = pProjection[0][k] * sx + pProjection[3][k] * ox;
        out[1][k] = pProjection[1][k] * sy + pProjection[3][k] * oy;
        out[2][k] = pProjection[2][k];
        out[3][k] = pProjection[3][k];
    }
    std::memcpy(pOut, out, sizeof(out));
}
