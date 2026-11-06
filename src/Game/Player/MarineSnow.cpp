#include "Game/Player/MarineSnow.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DirectDraw.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/MtxUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
// 3DS: JUTTexture3DS.hpp, GPU textures for JUTTextures; host.h, the port's host interface.
#ifdef __3DS__
#include "JUTTexture3DS.hpp"
#include "host.h"
#endif
#ifdef __3DS__
namespace {
    static const u32 kMarineSnowBatchQuads = 128;
    static HostGfxVertex sMarineSnowBatch[kMarineSnowBatchQuads * 6];

    void flushMarineSnowBatch(u32& vertexCount, const HostGfxState& state) {
        if (vertexCount == 0) return;
        hostGfxDraw(sMarineSnowBatch, vertexCount, nullptr, vertexCount, HOST_GFX_TRIANGLES, &state);
        vertexCount = 0;
    }

    void makeMarineSnowVertex(const TVec3f& world, float s, float t, u8 alpha, const Mtx44& projection, HostGfxVertex& vertex) {
        TVec3f view;
        MR::getCameraViewMtx().mult(world, view);
        vertex.x = view.x * projection[0][0] + view.z * projection[0][2];
        vertex.y = view.y * projection[1][1] + view.z * projection[1][2];
        vertex.z = view.z * projection[2][2] + projection[2][3];
        vertex.w = -view.z;
        vertex.rgba[0] = vertex.rgba[1] = vertex.rgba[2] = 255;
        vertex.rgba[3] = alpha;
        vertex.s0 = s;
        vertex.t0 = t;
    }

    void appendMarineSnowQuad(const TVec3f& cellOrigin, const TVec3f& particle, const TVec3f& diagonalUp,
                              const TVec3f& diagonalDown, u8 alpha, const Mtx44& projection, u32& vertexCount,
                              const HostGfxState& state) {
        if (vertexCount + 6 > kMarineSnowBatchQuads * 6) flushMarineSnowBatch(vertexCount, state);
        const TVec3f points[4] = {cellOrigin + particle - diagonalDown, cellOrigin + particle + diagonalUp,
                                  cellOrigin + particle + diagonalDown, cellOrigin + particle - diagonalUp};
        const float uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
        const u8 order[6] = {0, 1, 2, 0, 2, 3};
        for (u32 i = 0; i < 6; i++) {
            const u8 corner = order[i];
            makeMarineSnowVertex(points[corner], uv[corner][0], uv[corner][1], alpha, projection, sMarineSnowBatch[vertexCount++]);
        }
    }
}  // namespace
#endif

MarineSnow::MarineSnow() : mParticleCount(16), mParticlePositions(new (32) TVec3f[mParticleCount]), mCellSize(1000.0f) {
    for (s32 idx = 0; idx < mParticleCount; idx++) {
        TVec3f randVec;
        randVec.set(MR::getRandom() * mCellSize, MR::getRandom() * mCellSize, MR::getRandom() * mCellSize);
        mParticlePositions[idx] = randVec;
    }

    mFadeStep = 0;

    mTexture = new JUTTexture(MR::loadTexFromArc("MarineSnow"), 0);
}

void MarineSnow::view() {
    if (mFadeStep < 60) {
        mFadeStep++;
    }
}

void MarineSnow::clear() {
    if (mFadeStep != 0) {
        mFadeStep--;
    }
}

void MarineSnow::draw(const TVec3f& rPosition, const TVec3f& rUp, f32 surfaceDistance) const {
#ifdef __3DS__
    if (mFadeStep == 0) return;
    const f32 fadeRate = static_cast<f32>(mFadeStep) / 60.0f;
    const s32 originX = static_cast<s32>((rPosition.x + mCellSize / 2.0f) / mCellSize) * mCellSize;
    const s32 originY = static_cast<s32>((rPosition.y + mCellSize / 2.0f) / mCellSize) * mCellSize;
    const s32 originZ = static_cast<s32>((rPosition.z + mCellSize / 2.0f) / mCellSize) * mCellSize;

    void* texture = rvl3DSGetJUTTexture(mTexture);
    if (texture == nullptr) return;
    const Mtx44& projection = *MR::getCameraProjectionMtx().toCMtx44();
    HostGfxState state = {};
    state.textures[0] = texture;
    state.stageCount = 1;
    state.stages[0].colorSrc[0] = 0x03;
    state.stages[0].colorFunc = 0;
    state.stages[0].alphaSrc[0] = 0x03;
    state.stages[0].alphaSrc[1] = 0x00;
    state.stages[0].alphaFunc = 1;
    state.blendSrc = 6;
    state.blendDst = 7;
    state.alphaTestFunc = 1;
    state.depthTest = 1;
    state.depthFunc = 7;  // GPU_GEQUAL, matching GX_LEQUAL with reversed 3DS depth.
    state.writeMask = 0x07;
    state.scissor[2] = 640;
    state.scissor[3] = 456;

    static u8 phase = 0;
    phase = (phase + 1) & 3;
    const u32 start = (mParticleCount / 2) * phase;
    const u32 end = start + mParticleCount / 2;
    const TVec3f cameraForward = -MR::getCamZdir();
    const TVec3f cameraUp = MR::getCamYdir();
    TVec3f billboardRight;
    billboardRight.cross(cameraForward, cameraUp);
    MR::normalizeOrZero(&billboardRight);
    TVec3f billboardUp;
    billboardUp.cross(billboardRight, cameraForward);
    MR::normalizeOrZero(&billboardUp);
    const TVec3f diagonalUp = (billboardRight + billboardUp) * 10.0f;
    const TVec3f diagonalDown = (billboardRight - billboardUp) * 10.0f;

    u32 vertexCount = 0;
    for (s32 i = -4; i < 4; i++) {
        for (s32 j = -4; j < 4; j++) {
            for (s32 k = -4; k < 4; k++) {
                TVec3f cellOrigin(originX + i * mCellSize, originY + j * mCellSize, originZ + k * mCellSize);
                const TVec3f surfaceOffset = cellOrigin + rUp * mCellSize - rPosition;
                if (surfaceOffset.dot(rUp) > surfaceDistance) continue;

                const f32 cellDistance = static_cast<f32>((i >= 0 ? i : -i) + (j >= 0 ? j : -j) + (k >= 0 ? k : -k));
                u32 particleStride = 1;
                if (cellDistance > 3.0f) particleStride = 2;
                if (cellDistance > 5.0f) particleStride = 4;
                if (cellDistance > 8.0f) particleStride = 8;

                f32 opacity = 1.0f - cellDistance / 12.0f;
                if (opacity < 0.0f) opacity = 0.0f;
                opacity *= opacity * opacity;
                const u8 alpha = static_cast<u8>(242.0f * opacity * fadeRate);
                if (alpha == 0) continue;

                // The original opacity > 3 check is never true after clamping to [0, 1].
                for (u32 point = 0; point < mParticleCount; point += particleStride) {
                    appendMarineSnowQuad(cellOrigin, mParticlePositions[point], diagonalUp, diagonalDown, alpha, projection,
                                         vertexCount, state);
                }
            }
        }
    }
    flushMarineSnowBatch(vertexCount, state);
#else
    if (mFadeStep == 0) {
        return;
    }

    f32 fadeRate = static_cast< f32 >(mFadeStep) / 60.0f;

    s32 cellX = static_cast< s32 >((rPosition.x + mCellSize / 2.0f) / mCellSize);
    s32 originX = cellX * mCellSize;
    s32 cellY = static_cast< s32 >((rPosition.y + mCellSize / 2.0f) / mCellSize);
    s32 originY = cellY * mCellSize;
    s32 cellZ = static_cast< s32 >((rPosition.z + mCellSize / 2.0f) / mCellSize);
    s32 originZ = cellZ * mCellSize;

    TDDraw::setup(0, 1, 0);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    GXSetPointSize(27, GX_TO_ZERO);

    static u8 phase = 0;
    phase = (phase + 1) & 3;
    u32 pointCount;
    u32 start = (mParticleCount / 2) * phase;
    u32 end = start + mParticleCount / 2;
    pointCount = mParticleCount;

    TVec3f cameraForward = -MR::getCamZdir();
    TVec3f cameraUp = MR::getCamYdir();

    TVec3f billboardRight;
    billboardRight.cross(cameraForward, cameraUp);
    MR::normalizeOrZero(&billboardRight);

    TVec3f billboardUp;
    billboardUp.cross(billboardRight, cameraForward);
    MR::normalizeOrZero(&billboardUp);

    TVec3f diagonalUp = (billboardRight + billboardUp) * 10.0f;
    TVec3f diagonalDown = (billboardRight - billboardUp) * 10.0f;

    TDDraw::setup(1, 1, 0);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);

    mTexture->load(GX_TEXMAP0);

    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_A0, GX_CA_TEXA, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

    for (s32 i = -4; i < 4; i++) {
        for (s32 j = -4; j < 4; j++) {
            for (s32 k = -4; k < 4; k++) {
                TVec3f cellOrigin;
                cellOrigin.x = originX + i * mCellSize;
                cellOrigin.y = originY + j * mCellSize;
                cellOrigin.z = originZ + k * mCellSize;
                TVec3f surfaceOffset = cellOrigin + rUp * mCellSize - rPosition;

                if (surfaceOffset.dot(rUp) > surfaceDistance) {
                    continue;
                }

                f32 cellDistance = (i >= 0 ? i : -i) + (j >= 0 ? j : -j) + (k >= 0 ? k : -k);

                s32 particleStride = 1;
                if (cellDistance > 3.0f) {
                    particleStride = 2;
                }

                if (cellDistance > 5.0f) {
                    particleStride = 4;
                }

                if (cellDistance > 8.0f) {
                    particleStride = 8;
                }

                f32 opacity = 1.0f - cellDistance / 12.0f;
                if (opacity < 0.0f) {
                    opacity = 0.0f;
                }

                opacity *= opacity * opacity;
                u32 alpha = 242.0f * opacity * fadeRate;

                if (alpha == 0) {
                    continue;
                }

                GXColor color = {0, 0, 0, alpha};
                GXSetTevColor(GX_TEVREG0, color);

                TMtx34f mtx;
                MtxPtr pTranslationMtx = MR::tmpMtxTrans(cellOrigin);
                MR::multMtx(mtx, pTranslationMtx, MR::getCameraViewMtx());
                GXLoadPosMtxImm(mtx, 0);

                u16 lastParticle;
                u16 firstParticle;

                if (opacity > 3.0f) {
                    firstParticle = start;
                    lastParticle = end;
                } else {
                    firstParticle = 0;
                    lastParticle = pointCount;
                }

                for (u16 point = firstParticle; point < lastParticle; point += particleStride) {
                    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
                    {
                        TDDraw::sendPoint(mParticlePositions[point] - diagonalDown);
                        GXTexCoord2f32(0.0f, 0.0f);

                        TDDraw::sendPoint(mParticlePositions[point] + diagonalUp);
                        GXTexCoord2f32(1.0f, 0.0f);

                        TDDraw::sendPoint(mParticlePositions[point] + diagonalDown);
                        GXTexCoord2f32(1.0f, 1.0f);

                        TDDraw::sendPoint(mParticlePositions[point] - diagonalUp);
                        GXTexCoord2f32(0.0f, 1.0f);
                    }

                    GXEnd();
                }
            }
        }
    }
#endif
}

MarineSnow* MarineSnow_FORCE_MATCH() {
    return new MarineSnow();
}
