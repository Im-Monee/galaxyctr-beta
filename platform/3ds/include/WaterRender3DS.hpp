#pragma once

#ifdef __3DS__

#include "Game/Util/CameraUtil.hpp"
#include "JUTTexture3DS.hpp"
#include "host.h"
#include <JSystem/JUtility/JUTTexture.hpp>
#include <cmath>
#include <cstring>

// ##############################################################################################
// # Water surfaces through Citro3D                                                             #
// #                                                                                            #
// # For OceanBowl and similar water: GX fog in the register form HostGfxState carries (gfx.c   #
// # builds the table from it), camera-space vertices put into clip space, the water's pixel    #
// # state, and its draws through host.h.                                                       #
// ##############################################################################################
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

    // ##########################################################################################
    // # The screen behind the water, divided per pixel                                         #
    // #                                                                                        #
    // # The capture coordinate is the position through MR::loadTexProjectionMtx (0 to 1 over  #
    // # the screen, t down), then onto the capture stored on its side (map from                #
    // # rvl3DSGetNativeTIMGCapture: u = c0 + c1 t, v = c2 + c3 s). GX divides it per pixel.    #
    // # Divided at the vertices, Loopdeeloop's track went grey: its triangles are large and    #
    // # reach past the screen, so between the vertices the capture was read far from the      #
    // # water's place, past the image (owner, CIA 0.22.0 and 0.22.10 videos; azure only where  #
    // # the triangles were small). So the capture goes on unit 0, the one the GPU divides per   #
    // # pixel, as homogeneous (s, t, q) with q = w, and the waves move to units 1 and 2         #
    // # (makeWaterState).                                                                      #
    // ##########################################################################################
    // A vertex made by makeVertex with the waves on (s0, t0) and (s1, t1): the waves to (s1, t1)
    // and (s2, t2), the capture to (s0, t0, q0).
    inline void mapScreen(HostGfxVertex& vertex, const float* map) {
        vertex.s2 = vertex.s1;
        vertex.t2 = vertex.t1;
        vertex.s1 = vertex.s0;
        vertex.t1 = vertex.t0;
        // s * w and t * w of the screen position, then the capture's affine map times w.
        const float w = vertex.w, sw = 0.5f * w + 0.5f * vertex.x, tw = 0.5f * w - 0.5f * vertex.y;
        vertex.s0 = map[0] * w + map[1] * tw;
        vertex.t0 = map[2] * w + map[3] * sw;
        vertex.q0 = w;
    }

    // ##########################################################################################
    // # The waves multiplied, as the Wii's stage 1                                             #
    // #                                                                                        #
    // # The Wii's stage 1 (GX_CC_ZERO, GX_CC_TEXC, GX_CC_CPREV, GX_CC_ZERO, divided by 2) is   #
    // # the two wave samples multiplied and halved. They were averaged here instead, a much    #
    // # lighter grey that hid the blue of the screen behind: Loopdeeloop's track stayed grey   #
    // # (owner, CIA 0.22.0 to 0.22.11). The 3DS has no halving scale, so it takes a stage of   #
    // # its own.                                                                               #
    // ##########################################################################################
    // The stages of the Wii's water materials (OceanRingDrawer, OceanBowl loadMaterial) the 3DS
    // can do: the waves sampled twice, multiplied and halved, then, with a screen capture, the
    // capture times the water's colour added, clamped. Alpha from the vertices. The indirect warp
    // of the capture and the compare stage (glints) have no 3DS equivalent. Without a capture the
    // waves are on units 0 and 1; with one (vertices through mapScreen) on units 1 and 2, the
    // capture on unit 0 in projection mode.
    inline HostGfxState makeWaterState(void* waves, void* screen, unsigned char red, unsigned char green, unsigned char blue,
                                       bool additive) {
        HostGfxState state = makeState(waves, waves, screen, true, false);
        std::memset(state.stages, 0, sizeof(state.stages));
        const unsigned char wavesA = screen != nullptr ? 0x04 : 0x03, wavesB = screen != nullptr ? 0x05 : 0x04;
        HostGfxStage& product = state.stages[0];
        product.colorSrc[0] = wavesA;  // the waves' first sample
        product.colorSrc[1] = wavesB;  // the second
        product.colorFunc = 1;         // modulate
        product.alphaSrc[0] = 0x00;    // vertex alpha
        product.alphaFunc = 0;         // replace
        HostGfxStage& half = state.stages[1];
        half.colorSrc[0] = 0x0F;  // previous
        half.colorSrc[1] = 0x0E;  // constant 0.5
        half.colorFunc = 1;       // modulate
        half.alphaSrc[0] = 0x0F;
        half.alphaFunc = 0;
        half.constant = 0x80808080ul;
        HostGfxStage& behind = state.stages[2];
        behind.colorSrc[0] = 0x03;  // texture 0, the screen
        behind.colorSrc[1] = 0x0E;  // the water's colour
        behind.colorSrc[2] = 0x0F;  // previous
        behind.colorFunc = 8;       // multiply-add
        behind.alphaSrc[0] = 0x0F;
        behind.alphaFunc = 0;
        behind.constant = 0xFF000000ul | (static_cast< unsigned long >(blue) << 16) | (static_cast< unsigned long >(green) << 8) | red;
        state.stageCount = screen != nullptr ? 3 : 2;
        if (screen != nullptr) {
            state.textures[0] = screen;
            state.textures[1] = waves;
            state.textures[2] = waves;
            state.projectUnit0 = 1;
        }
        state.blendDst = additive ? 1 : 7;  // GPU_ONE or GPU_ONE_MINUS_SRC_ALPHA
        state.alphaTestFunc = 6;            // GPU_GREATER than 0, as GXSetAlphaCompare(GX_GREATER, 0)
        state.alphaRef = 0;
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
