#pragma once

#ifdef __3DS__

#include "Game/Util/CameraUtil.hpp"
#include "JUTTexture3DS.hpp"
#include "host.h"
#include <JSystem/JUtility/JUTTexture.hpp>
#include <cmath>
#include <cstring>

namespace WaterRender3DS {
    inline unsigned int packFogFloat(float value) {
        unsigned int bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return ((bits >> 12) & 0x7FF) | (((bits >> 23) & 0xFF) << 11) | (((bits >> 31) & 1) << 19);
    }

    inline void setPerspectiveLinearFog(HostGfxState& state, float start, float end, float nearZ, float farZ,
                                        unsigned char red, unsigned char green, unsigned char blue) {
        if (farZ == nearZ || end == start) { state.fogParamC = 0xF1000000; return; }
        const float a0 = (farZ * nearZ) / ((farZ - nearZ) * (end - start));
        float bMantissa = farZ / (farZ - nearZ);
        int bExponent = 0;
        while (bMantissa > 1.0f) { bMantissa *= 0.5f; bExponent++; }
        while (bMantissa > 0.0f && bMantissa < 0.5f) { bMantissa *= 2.0f; bExponent--; }
        const float a = a0 / std::ldexp(1.0f, bExponent + 1);
        const float c = start / (end - start);
        const unsigned int b = static_cast<unsigned int>(bMantissa * 8388638.0f);
        state.fogParamA = 0xEE000000 | packFogFloat(a);
        state.fogParamB = 0xEF000000 | (b & 0xFFFFFF);
        state.fogParamShift = 0xF0000000 | (static_cast<unsigned int>(bExponent + 1) & 0x1F);
        state.fogParamC = 0xF1000000 | packFogFloat(c) | (2u << 21);
        state.fogColor = 0xF2000000 | (static_cast<unsigned int>(red) << 16) |
                         (static_cast<unsigned int>(green) << 8) | blue;
    }

    inline HostGfxVertex makeVertex(const TVec3f& position, float s0, float t0, float s1 = 0.0f, float t1 = 0.0f,
                                    float s2 = 0.0f, float t2 = 0.0f, unsigned char alpha = 255) {
        TVec3f view;
        MR::getCameraViewMtx().mult(position, view);
        const Mtx44& projection = *MR::getCameraProjectionMtx().toCMtx44();

        HostGfxVertex vertex = {};
        vertex.x = view.x * projection[0][0] + view.z * projection[0][2];
        vertex.y = view.y * projection[1][1] + view.z * projection[1][2];
        vertex.z = view.z * projection[2][2] + projection[2][3];
        vertex.w = -view.z;
        vertex.rgba[0] = 255;
        vertex.rgba[1] = 255;
        vertex.rgba[2] = 255;
        vertex.rgba[3] = alpha;
        vertex.s0 = s0;
        vertex.t0 = t0;
        vertex.s1 = s1;
        vertex.t1 = t1;
        vertex.s2 = s2;
        vertex.t2 = t2;
        return vertex;
    }

    inline HostGfxState makeState(void* texture0, void* texture1 = nullptr, void* texture2 = nullptr, bool blend = true,
                                  bool depthWrite = false, unsigned long constant = 0xFFFFFFFFul) {
        HostGfxState state = {};
        state.textures[0] = texture0;
        state.textures[1] = texture1;
        state.textures[2] = texture2;
        state.stageCount = texture2 != nullptr ? 3 : texture1 != nullptr ? 2 : 1;
        state.blendEquation = 0;
        state.blendSrc = blend ? 6 : 1;  // GPU_SRC_ALPHA or GPU_ONE
        state.blendDst = blend ? 7 : 0;  // GPU_ONE_MINUS_SRC_ALPHA or GPU_ZERO
        state.alphaTestFunc = 1;
        state.depthTest = 1;
        state.depthFunc = 7;  // GX_LEQUAL with the 3DS depth reversed (GPU_GEQUAL)
        state.writeMask = 0x0F | (depthWrite ? 0x10 : 0);
        state.cull = 0;
        state.scissor[2] = 640;
        state.scissor[3] = 456;

        for (unsigned int i = 0; i < state.stageCount; i++) {
            HostGfxStage& stage = state.stages[i];
            const unsigned char textureSource = static_cast<unsigned char>(0x03 + i);
            const unsigned char previous = i == 0 ? 0x00 : 0x0F;
            stage.colorSrc[0] = texture0 != nullptr || i > 0 ? textureSource : 0x00;
            stage.colorSrc[1] = previous;
            stage.colorSrc[2] = previous;
            stage.colorOp[0] = 0;
            stage.colorOp[1] = 0;
            stage.colorOp[2] = 0;
            stage.colorFunc = (i == 0 && texture0 == nullptr) ? 0 : 1;
            stage.colorScale = 0;
            stage.alphaSrc[0] = stage.colorSrc[0];
            stage.alphaSrc[1] = previous;
            stage.alphaSrc[2] = previous;
            stage.alphaOp[0] = 0;
            stage.alphaOp[1] = 0;
            stage.alphaOp[2] = 0;
            stage.alphaFunc = stage.colorFunc;
            stage.alphaScale = 0;
            stage.constant = constant;
        }
        return state;
    }

    inline void draw(const HostGfxVertex* vertices, unsigned long count, const unsigned short* indices, unsigned long indexCount,
                     int primitive, const HostGfxState& state) {
        hostGfxDraw(vertices, count, indices, indexCount, primitive, &state);
    }

    inline void drawStrip(const HostGfxVertex* vertices, unsigned long count, void* texture0, void* texture1 = nullptr,
                          void* texture2 = nullptr, bool blend = true, bool depthWrite = false) {
        HostGfxState state = makeState(texture0, texture1, texture2, blend, depthWrite);
        draw(vertices, count, nullptr, count, HOST_GFX_STRIP, state);
    }
}  // namespace WaterRender3DS

#endif
