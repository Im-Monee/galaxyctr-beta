#include "Game/Player/J3DModelX.hpp"
#ifdef __3DS__
#include "Game/LiveActor/ActorLightCtrl.hpp"
#endif
#include "Game/System/ShapePacketUserData.hpp"
#include "Game/Util/DirectDraw.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/MtxUtil.hpp"
#include "Game/Util/SchedulerUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp>
#include <JSystem/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3DGraphBase/J3DShapeDraw.hpp>
#include <JSystem/J3DGraphBase/J3DShapeMtx.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JUtility/JUTVideo.hpp>
#include <revolution/gd.h>
#ifdef __3DS__
#include "GXDelta3DS.hpp"
#include "GXToCitro3D.hpp"
#endif
#ifdef __3DS__
bool rvlNativeBegin(J3DMaterial*, J3DShapePacket*, bool);
bool rvlNativePacket(J3DShapePacket*, const ActorLightInfo*);
const ActorLightInfo* rvlNativeLoadedLight();

namespace {
    s32 sVisibleDraws;

    // The sixteen mode display lists the Wii constructor builds (below, under #else), as GX
    // changes: same calls, same values. No draw of the game selects lists 7-9 (light in the alpha,
    // alpha-count passes). List 6 looks the screen capture up by the normal, repeated (the cosmic
    // ghost's turtle shell, JetTurtleShadow::drawType1): a 3DS capture is a rectangle of a larger
    // texture, clamped, and cannot repeat, so that draw is skipped until captures can be drawn
    // into a texture of their own. Where the Wii counts or masks in the destination alpha the
    // 3DS uses the stencil: list 3 (silhouette, see MR::fillSilhouetteColor), 10
    // (CollisionShadow's volume box) and 15 (Mario's dark mask).
    const u32 kUnsupportedModes = (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9);
    RvlGXDelta sModeDeltas[16];
    bool sModeDeltasReady;

    void buildModeDeltas() {
        for (u32 i = 0; i < 16; i++) {
            sModeDeltas[i].clear();
        }
        RvlGXDelta* d = &sModeDeltas[0];
        d->setCull(GX_CULL_FRONT);

        d = &sModeDeltas[1];
        d->setAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        d->setZMode(false, GX_ALWAYS, false);
        d->setGenMode(0, 1, 1, GX_CULL_BACK);
        d->setChanCtrl(GX_COLOR0A0, false, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
        d->setBlend(GX_BM_NONE, GX_BL_ZERO, GX_BL_INVSRCALPHA, GX_LO_CLEAR, false, true);
        d->setDstAlpha(true, 0xFF);
        d->setTevAlphaIn(GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ADDHALF, GX_CS_DIVIDE_2);

        d = &sModeDeltas[2];
        d->setZMode(true, GX_LEQUAL, true);
        d->setBlend(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR, true, false);
        d->setGenMode(1, 0, 1, GX_CULL_FRONT);
        d->setChanCtrl(GX_COLOR0A0, false, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
        d->setTevOrder(GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
        d->setTevColorIn(GX_CC_TEXC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1);
        d->setTevAlphaIn(GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ADDHALF, GX_CS_DIVIDE_2);

        d = &sModeDeltas[3];
        d->setAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        d->setZMode(true, GX_GEQUAL, false);
        d->setGenMode(0, 1, 1, GX_CULL_BACK);
        d->setChanCtrl(GX_COLOR0A0, false, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
        // GX_BM_NONE with the alpha alone updated to 0x90: the stencil marks the pixels instead.
        d->setBlend(GX_BM_NONE, GX_BL_ZERO, GX_BL_INVSRCALPHA, GX_LO_CLEAR, false, false);
        d->setStencil(GXToCitro3D::TEST_ALWAYS, 1, GXToCitro3D::STENCIL_KEEP, GXToCitro3D::STENCIL_KEEP, GXToCitro3D::STENCIL_REPLACE);
        d->setTevAlphaIn(GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ADDHALF, GX_CS_DIVIDE_2);

        d = &sModeDeltas[4];
        d->setAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        d->setZMode(true, GX_LEQUAL, false);
        d->setGenMode(0, 1, 1, GX_CULL_FRONT);
        d->setChanCtrl(GX_COLOR0A0, false, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
        d->setBlend(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR, true, false);
        d->setTevRegister(GX_TEVREG0, 255, 255, 0, 255);
        d->setTevColorIn(GX_CC_C0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1);
        d->setTevAlphaIn(GX_CA_A0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1);

        d = &sModeDeltas[5];
        d->setAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        d->setZMode(true, GX_ALWAYS, true);
        d->setGenMode(0, 1, 1, GX_CULL_FRONT);
        d->setBlend(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR, false, false);
        d->setChanCtrl(GX_COLOR0A0, false, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_CLAMP, GX_AF_NONE);
        d->setTevColorIn(GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1);
        d->setTevAlphaIn(GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1);
        d->setTevOrder(GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);

        // GX_BM_BLEND (DSTALPHA, INVDSTALPHA) over the box's 0/255 mask, which GXSetDstAlpha(1, 0)
        // clears: the model where the stencil is set, clearing it.
        d = &sModeDeltas[10];
        d->setBlend(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP, true, false);
        d->setZMode(true, GX_LEQUAL, false);
        d->setStencil(GXToCitro3D::TEST_NOTEQUAL, 0, GXToCitro3D::STENCIL_KEEP, GXToCitro3D::STENCIL_KEEP, GXToCitro3D::STENCIL_ZERO);

        d = &sModeDeltas[11];
        d->setZMode(true, GX_LEQUAL, true);
        d->setGenMode(1, 0, 1, GX_CULL_FRONT);
        d->setAlphaCompare(GX_GREATER, 0x20, GX_AOP_AND, GX_ALWAYS, 0);
        d->setBlend(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_NOOP, true, true);
        d->setChanCtrl(GX_COLOR0A0, false, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
        d->setTevColorIn(GX_CC_TEXC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2);
        d->setTevOrder(GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);

        d = &sModeDeltas[12];
        d->setZMode(true, GX_LEQUAL, false);
        d->setGenMode(1, 0, 1, GX_CULL_BACK);
        d->setAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        d->setBlend(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_NOOP, true, false);
        d->setChanCtrl(GX_COLOR0A0, false, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
        d->setTevColorIn(GX_CC_ZERO, GX_CC_ONE, GX_CC_C0, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1);
        d->setTevAlphaIn(GX_CA_A0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1);
        d->setTevOrder(GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);

        d = &sModeDeltas[13];
        d->setAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        d->setZMode(true, GX_GEQUAL, false);
        d->setGenMode(0, 1, 1, GX_CULL_BACK);
        d->setChanCtrl(GX_COLOR0A0, false, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
        d->setBlend(GX_BM_BLEND, GX_BL_ONE, GX_BL_SRCALPHA, GX_LO_CLEAR, true, false);
        d->setTevRegister(GX_TEVREG0, 35, 25, 25, 5);
        d->setTevColorIn(GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1);
        d->setTevAlphaIn(GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1);

        d = &sModeDeltas[14];
        d->setDstAlpha(true, 0x40);

        // GX_BM_BLEND (INVDSTALPHA, DSTALPHA): the model where the dark mask left the alpha at 0,
        // the frame buffer where it wrote 255. The mask is in the stencil (drawDarkMask).
        d = &sModeDeltas[15];
        d->setBlend(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR, true, false);
        d->setStencil(GXToCitro3D::TEST_EQUAL, 0, GXToCitro3D::STENCIL_KEEP, GXToCitro3D::STENCIL_KEEP, GXToCitro3D::STENCIL_KEEP);
        sModeDeltasReady = true;
    }
}

J3DModelXVisibleDraw::J3DModelXVisibleDraw() {
    sVisibleDraws++;
}

J3DModelXVisibleDraw::~J3DModelXVisibleDraw() {
    sVisibleDraws--;
}
#endif

class J3DMtxBuffer2 : public J3DMtxBuffer {
public:
    void rotationMtx(MtxPtr mtx) {
        mpDrawMtxArr[1][mCurrentViewNo] = reinterpret_cast< Mtx* >(mtx);
    }
    void calcNrmMtx2();
    void calcDrawMtx2(u32, const Vec&, const Mtx&, J3DMtxBuffer*);
    void calcDrawMtx3(u32, const Vec&, const Mtx&, J3DMtxBuffer*, const TVec3f&, const TVec3f&);
};

class J3DShapeX : public J3DShape {};

class J3DShapePacketX : public J3DShapePacket {};

void J3DModelX::viewCalc2() {
    J3DMtxBuffer2* buffer = static_cast< J3DMtxBuffer2* >(mMtxBuffer);
    buffer->swapDrawMtx();
    buffer->swapNrmMtx();
    buffer->calcDrawMtx(getMtxCalcMode(), mBaseScale, mBaseTransformMtx);
    buffer->calcNrmMtx2();

    calcBBoardMtx();
    calcBumpMtx();
    DCStoreRangeNoSync(getDrawMtxPtr(), mModelData->getDrawMtxNum() * sizeof(Mtx));
    prepareShapePackets();
}

void J3DModelX::setDrawView(u32 view) {
    static_cast< J3DMtxBuffer2* >(mMtxBuffer)->rotationMtx(*mExtraMtxBuffer[view + _DC]);
}

void J3DModelX::setDrawViewBuffer(MtxPtr mtx) {
    mMtxBuffer->mpDrawMtxArr[1][mMtxBuffer->mCurrentViewNo] = reinterpret_cast< Mtx* >(mtx);
}

void J3DModelX::copyAnmMtxBuffer(const J3DModelX* model) {
    mMtxBuffer->mpAnmMtx = model->mMtxBuffer->mpAnmMtx;
}

void J3DModelX::viewCalc3(u32 view, MtxPtr mtx) {
    J3DMtxBuffer2* buffer = static_cast< J3DMtxBuffer2* >(mMtxBuffer);
    buffer->rotationMtx(*mExtraMtxBuffer[view + _DC]);

    if (mtx) {
        buffer->calcDrawMtx(getMtxCalcMode(), mBaseScale, reinterpret_cast< const Mtx& >(mtx));
    } else {
        buffer->calcDrawMtx(getMtxCalcMode(), mBaseScale, mBaseTransformMtx);
    }

    if (!(view & 1)) {
        buffer->swapNrmMtx();
        buffer->calcNrmMtx2();
    }

    calcBBoardMtx();
    calcBumpMtx();
    DCStoreRangeNoSync(getDrawMtxPtr(), mModelData->getDrawMtxNum() * sizeof(Mtx));
    prepareShapePackets();
}

void J3DModelX::viewCalcRef(u32 view, J3DModel* model) {
    J3DMtxBuffer2* buffer = static_cast< J3DMtxBuffer2* >(mMtxBuffer);
    buffer->rotationMtx(*mExtraMtxBuffer[view + _DC]);
    buffer->calcDrawMtx2(getMtxCalcMode(), mBaseScale, mBaseTransformMtx, model->getMtxBuffer());
    DCStoreRangeNoSync(getDrawMtxPtr(), mModelData->getDrawMtxNum() * sizeof(Mtx));
    prepareShapePackets();
}

void J3DModelX::viewCalcRefPos(u32 view, J3DModel* model, const TVec3f& pos, const TVec3f& normal) {
    J3DMtxBuffer2* buffer = static_cast< J3DMtxBuffer2* >(mMtxBuffer);
    buffer->rotationMtx(*mExtraMtxBuffer[view + _DC]);
    buffer->calcDrawMtx3(getMtxCalcMode(), mBaseScale, mBaseTransformMtx, model->getMtxBuffer(), pos, normal);
    DCStoreRangeNoSync(getDrawMtxPtr(), mModelData->getDrawMtxNum() * sizeof(Mtx));
    prepareShapePackets();
}

void J3DMtxBuffer2::calcNrmMtx2() {
    u16 count = mJointTree->getDrawMtxNum();

    for (u16 i = 0; i < count; i++) {
        J3DPSCalcInverseTranspose(*getDrawMtx(i), *getNrmMtx(i));
    }

    DCStoreRange(getNrmMtxPtr(), count * sizeof(Mtx33));
}

void J3DMtxBuffer2::calcDrawMtx2(u32 mode, const Vec& scale, const Mtx& base, J3DMtxBuffer* buffer) {
    Mtx viewBase;
    J3DCalcViewBaseMtx(j3dSys.getViewMtx(), scale, base, viewBase);

    for (u16 i = 0, count = mJointTree->getDrawFullWgtMtxNum(); i < count; i++) {
        PSMTXConcat(viewBase, buffer->getAnmMtx(mJointTree->getDrawMtxIndex(i)), *getDrawMtx(i));
    }

    if (mJointTree->getDrawMtxNum() > mJointTree->getDrawFullWgtMtxNum()) {
        J3DPSMtxArrayConcat(viewBase, buffer->getWeightAnmMtx(0), *getDrawMtx(mJointTree->getDrawFullWgtMtxNum()), mJointTree->getWEvlpMtxNum());
    }
}

void J3DMtxBuffer2::calcDrawMtx3(u32 mode, const Vec& scale, const Mtx& base, J3DMtxBuffer* buffer, const TVec3f& pos, const TVec3f& normal) {
    Mtx viewBase;
    J3DCalcViewBaseMtx(j3dSys.getViewMtx(), scale, base, viewBase);
    u16 count = mJointTree->getDrawFullWgtMtxNum();

    for (u16 i = 0; i < count; i++) {
        Mtx jointMtx;
        TPos3f orientation;
        Mtx flatten;
        TVec3f trans;
        TVec3f planar;

        MR::extractMtxTrans(buffer->getAnmMtx(mJointTree->getDrawMtxIndex(i)), &trans);
        MR::vecKillElement(trans - pos, normal, &planar);
        trans = pos + planar;

        PSMTXCopy(buffer->getAnmMtx(mJointTree->getDrawMtxIndex(i)), jointMtx);
        MR::setMtxTrans(jointMtx, 0.0f, 0.0f, 0.0f);
        MR::makeMtxUpNoSupportPos(&orientation, normal, TVec3f(0.0f, 0.0f, 0.0f));
        PSMTXConcat(orientation.toMtxPtr(), jointMtx, jointMtx);
        PSMTXScale(flatten, 1.0f, 0.1f, 1.0f);
        PSMTXConcat(flatten, jointMtx, jointMtx);
        PSMTXInverse(orientation.toMtxPtr(), orientation.toMtxPtr());
        PSMTXConcat(orientation.toMtxPtr(), jointMtx, jointMtx);
        MR::setMtxTrans(jointMtx, trans.x, trans.y, trans.z);
        PSMTXConcat(viewBase, jointMtx, *getDrawMtx(i));
    }
}

void J3DModelX::directDraw(J3DModel* model) {
    j3dSys.setModel(this);
    if (J3DModel::checkFlag(4)) {
        j3dSys.onFlag(4);
    } else {
        j3dSys.offFlag(4);
    }
    if (J3DModel::checkFlag(8)) {
        j3dSys.onFlag(8);
    } else {
        j3dSys.offFlag(8);
    }
    mModelData->syncJ3DSysFlags();
    j3dSys.setTexture(mModelData->getTexture());

    bool fog = false;
    if (_1D0) {
        fog = true;
        _1D0--;
    }
    for (u16 i = 0; i < mModelData->getMaterialNum(); i++) {
        J3DMaterial* material = mModelData->getMaterialNodePointer(i);
        _1C0 = i;
        if (model) {
            drawIn(material, fog, model->getBaseTRMtx(), model);
        } else {
            drawIn(material, fog, mBaseTransformMtx, nullptr);
        }
    }

    J3DShape::resetVcdVatCache();
}

void J3DModelX::drawIn(J3DMaterial* material, bool fog, MtxPtr base, J3DModel* model) {
    if (!material) {
        return;
    }
    if (!material->getShape()) {
        return;
    }
    if (material->getShape()->checkFlag(1)) {
        return;
    }
    J3DMatPacket* matPacket = getMatPacket(material->getIndex());
    J3DShapePacket* packet = model ? model->getShapePacket(material->getShape()->getIndex()) : getShapePacket(material->getShape()->getIndex());
#ifdef __3DS__
    // The Wii calls the selected mode lists, then the dynamic list and the material's texture
    // list (below): here they are GX changes the native draw applies over the material.
    if (!sModeDeltasReady) {
        buildModeDeltas();
    }
    RvlGXDelta delta;
    delta.clear();
    bool special = false;
    for (u32 i = 0; i <= 15; i++) {
        if (*reinterpret_cast< u32* >(&mFlags) & (1u << (31 - i))) {
            if (kUnsupportedModes & (1 << i)) {
                return;
            }
            delta.merge(sModeDeltas[i]);
            special = true;
        }
    }
    if (_1B8) {
        _1B4 = _1B8;
        _1B8 = nullptr;
    }
    if (_1B4 && _1BC) {
        delta.merge(*reinterpret_cast< const RvlGXDelta* >(_1B4));
    }
    u32 materialIndex = _1C0;
    if (_1C8[materialIndex]) {
        _1C4[materialIndex] = _1C8[materialIndex];
        _1C8[materialIndex] = nullptr;
    }
    if (_1C4[materialIndex] && _1CC[materialIndex]) {
        delta.merge(*reinterpret_cast< const RvlGXDelta* >(_1C4[materialIndex]));
    }
    if (model != nullptr || sVisibleDraws > 0 || special) {
        rvlNativeSetDelta(&delta);
        // Lit by the light loaded last, as GX does: SceneExecutor loads the player's light before
        // DrawType_Player. With none, the rim-light alpha channel was unlit and Mario turned pale.
        // The material packet's material, as the Wii calls its display list: MarioActor::copyMaterial
        // lends the face model Mario's own "Face" material that way.
        J3DMaterial* drawn = matPacket->mpMaterial != nullptr ? matPacket->mpMaterial : material;
        if (rvlNativeBegin(drawn, packet, false)) {
            rvlNativePacket(packet, rvlNativeLoadedLight());
        }
        rvlNativeSetDelta(nullptr);
    }
    return;
#endif
    j3dSys.setMatPacket(matPacket);
    matPacket->callDL();

    if (fog) {
        TVec3f pos;
        MR::extractMtxTrans(mBaseTransformMtx, &pos);
        TDDraw::mixFogColor(pos, _1D4, _1D8);
    }

    packet->getShape()->loadPreDrawSetting();
    if (packet->getDisplayListObj()) {
        packet->getDisplayListObj()->callDL();
    }
    if (MR::getJ3DShapePacketUserData(packet)) {
#ifndef __3DS__
        MR::getJ3DShapePacketUserData(packet)->callDL();
#endif
    }

    for (u32 i = 0; i <= 15; i++) {
#ifdef __3DS__
        // Metrowerks allocates bitfields from the top bit, GCC from the bottom: the Wii's bit i
        // is field (31 - i) here (bits 0-15 are the flags _1F down to _10, e.g. _1E = shadow).
        if (*reinterpret_cast< u32* >(&mFlags) & (1u << (31 - i))) {
#else
        if (*reinterpret_cast< u32* >(&mFlags) & (1 << i)) {
#endif
#ifndef __3DS__
            GXCallDisplayList(mDisplayLists[i], mDisplayListSizes[i]);
#endif
        }
    }

    if (_1B8) {
        _1B4 = _1B8;
        _1B8 = nullptr;
    }
    if (_1B4 && _1BC) {
#ifndef __3DS__
        GXCallDisplayList(_1B4, _1BC);
#endif
    }

    u32 index = _1C0;
    if (_1C8[index]) {
        _1C4[index] = _1C8[index];
        _1C8[index] = nullptr;
    }
    if (_1C4[index] && _1CC[index]) {
#ifndef __3DS__
        GXCallDisplayList(_1C4[index], _1CC[index]);
#endif
    }
    if (_120) {
        _120(_128, _1C0);
    }

    J3DShape* original;
    if (_12C) {
        J3DShapePacket* replacement = _12C->getShapePacket(material->getShape()->getIndex());
        original = packet->getShape();
        packet->setShape(replacement->getShape());
        packet->mpShape->mDrawMtx = original->mDrawMtx;
        packet->mpShape->mDrawMtxData = original->mDrawMtxData;
        packet->mpShape->mCurrentMtx = material->mCurrentMtx;
    }

    shapePacketDrawFast(static_cast< J3DShapePacketX* >(packet));
    if (_12C) {
        packet->mpShape = original;
    }
}

bool J3DModelX::simpleDrawSetup(J3DMaterial* material) {
#ifdef __3DS__
    (void)material;
    return false;
#else
    if (!material) {
        return false;
    }
    if (!material->getShape()) {
        return false;
    }
    if (material->getShape()->checkFlag(1)) {
        return false;
    }

    J3DShape::resetVcdVatCache();
    J3DMatPacket* matPacket = getMatPacket(material->getIndex());
    J3DShapePacket* packet = getShapePacket(material->getShape()->getIndex());
    j3dSys.setMatPacket(matPacket);
    matPacket->callDL();
    packet->getShape()->loadPreDrawSetting();
    if (packet->getDisplayListObj()) {
        packet->getDisplayListObj()->callDL();
    }
    return true;
#endif
}

void J3DModelX::simpleDrawShape(J3DMaterial* material) {
#ifdef __3DS__
    (void)material;
#else
    getShapePacket(material->getShape()->getIndex())->drawFast();
#endif
}

void J3DModelX::storeDisplayList(GDLObj* obj, u32 index) {
#ifdef __3DS__
    (void)obj;
    (void)index;
#else
    GDPadCurr32();
    u32 size = (GDGetGDLObjOffset(obj) + 31) & ~31;
    mDisplayLists[index] = new (32) u8[size];
    MR::copyMemory(mDisplayLists[index], obj->start, size);
    DCStoreRange(mDisplayLists[index], size);
    mDisplayListSizes[index] = GDGetGDLObjOffset(obj);
    GDInitGDLObj(obj, obj->start, obj->length);
#endif
}

J3DModelX::J3DModelX(J3DModelData* data, u32 flags, u32 bufferFlags) : J3DModel(data, flags, bufferFlags) {
    MR::ProhibitSchedulerAndInterrupts guard(false);
    _DC = 0;
    _DD = 0;
    _120 = nullptr;
    _128 = nullptr;
    _12C = nullptr;
    mShapeCallback = nullptr;
    _1E4 = 0;
    _1E5 = 0;

 #ifndef __3DS__
    GDLObj obj;
    u8 buffer[512] ATTRIBUTE_ALIGN(32);
    GDInitGDLObj(&obj, buffer, sizeof(buffer));
    GDSetCurrent(&obj);

    GDSetCullMode(GX_CULL_FRONT);
    storeDisplayList(&obj, 0);

    GDSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GDSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GDSetGenMode2(0, 1, 1, 0, GX_CULL_BACK);
    GDSetTevDirect(GX_TEVSTAGE0);
    GDSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
    GDSetBlendModeEtc(GX_BM_NONE, GX_BL_ZERO, GX_BL_INVSRCALPHA, GX_LO_CLEAR, GX_FALSE, GX_TRUE, GX_FALSE);
    GDSetDstAlpha(GX_TRUE, 0xFF);
    GDSetTevAlphaCalcAndSwap(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ADDHALF, GX_CS_DIVIDE_2, GX_TRUE,
                             GX_TEVPREV, GX_TEV_SWAP0, GX_TEV_SWAP0);
    storeDisplayList(&obj, 1);

    GDSetCullMode(GX_CULL_FRONT);
    GDSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GDSetBlendModeEtc(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR, GX_TRUE, GX_FALSE, GX_FALSE);
    GDSetGenMode2(1, 0, 1, 0, GX_CULL_FRONT);
    GDSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
    GDSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
    GDSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR_NULL);
    GDSetTevColorCalc(GX_TEVSTAGE0, GX_CC_TEXC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GDSetTevAlphaCalcAndSwap(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ADDHALF, GX_CS_DIVIDE_2, GX_TRUE,
                             GX_TEVPREV, GX_TEV_SWAP0, GX_TEV_SWAP0);
    storeDisplayList(&obj, 2);

    GDSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GDSetZMode(GX_TRUE, GX_GEQUAL, GX_FALSE);
    GDSetGenMode2(0, 1, 1, 0, GX_CULL_BACK);
    GDSetTevDirect(GX_TEVSTAGE0);
    GDSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
    GDSetBlendModeEtc(GX_BM_NONE, GX_BL_ZERO, GX_BL_INVSRCALPHA, GX_LO_CLEAR, GX_FALSE, GX_TRUE, GX_FALSE);
    GDSetDstAlpha(GX_TRUE, 0x90);
    GDSetTevAlphaCalcAndSwap(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ADDHALF, GX_CS_DIVIDE_2, GX_TRUE,
                             GX_TEVPREV, GX_TEV_SWAP0, GX_TEV_SWAP0);
    storeDisplayList(&obj, 3);

    GDSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GDSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    GDSetGenMode2(0, 1, 1, 0, GX_CULL_FRONT);
    GDSetTevDirect(GX_TEVSTAGE0);
    GDSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
    GDSetBlendModeEtc(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR, GX_TRUE, GX_FALSE, GX_FALSE);
    static GXColor highlightColor = {255, 255, 0, 255};
    GDSetTevColor(GX_TEVREG0, highlightColor);
    GDSetTevColorCalc(GX_TEVSTAGE0, GX_CC_C0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GDSetTevAlphaCalcAndSwap(GX_TEVSTAGE0, GX_CA_A0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV,
                             GX_TEV_SWAP0, GX_TEV_SWAP0);
    storeDisplayList(&obj, 4);

    GDSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GDSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    GDSetGenMode2(0, 1, 1, 0, GX_CULL_FRONT);
    GDSetBlendModeEtc(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR, GX_FALSE, GX_FALSE, GX_FALSE);
    GDSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_CLAMP, GX_AF_NONE);
    GDSetTevColorCalc(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GDSetTevAlphaCalcAndSwap(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV,
                             GX_TEV_SWAP0, GX_TEV_SWAP0);
    GDSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    storeDisplayList(&obj, 5);

    GDSetTexLookupMode(GX_TEXMAP0, GX_REPEAT, GX_REPEAT, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GDSetTexImgAttr(GX_TEXMAP0, MR::getScreenWidth(), JUTVideo::getManager()->getEfbHeight(), GX_TF_RGB565);
    GDSetTexImgPtr(GX_TEXMAP0, const_cast< u8* >(reinterpret_cast< const u8* >(MR::getScreenResTIMG())) + MR::getScreenResTIMG()->mImageDataOffset);
    GDSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_NRM, GX_TRUE, GX_IDENTITY);
    storeDisplayList(&obj, 6);

    GXColor ambientColor = {0, 0, 0, 0};
    GXColor materialColor = {0, 0, 0, 255};
    GDSetChanAmbColor(GX_ALPHA0, ambientColor);
    GDSetChanMatColor(GX_ALPHA0, materialColor);
    GDSetBlendModeEtc(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP, GX_FALSE, GX_TRUE, GX_FALSE);
    GDSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GDSetChanCtrl(GX_COLOR0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
    GDSetChanCtrl(GX_ALPHA0, GX_TRUE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0, GX_DF_CLAMP, GX_AF_NONE);
    GDSetTevAlphaCalcAndSwap(GX_TEVSTAGE0, GX_CA_KONST, GX_CA_ZERO, GX_CA_RASA, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                             GX_TEVPREV, GX_TEV_SWAP0, GX_TEV_SWAP0);
    storeDisplayList(&obj, 7);

    GDSetCullMode(GX_CULL_FRONT);
    GDSetBlendModeEtc(GX_BM_BLEND, GX_BL_ZERO, GX_BL_ZERO, GX_LO_NOOP, GX_FALSE, GX_TRUE, GX_FALSE);
    GDSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GDSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    GXColor subtractColor = {0, 0, 0, 252};
    GDSetTevColor(GX_TEVREG0, subtractColor);
    GDSetTevAlphaCalcAndSwap(GX_TEVSTAGE0, GX_CA_A0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV,
                             GX_TEV_SWAP0, GX_TEV_SWAP0);
    storeDisplayList(&obj, 8);

    GDSetCullMode(GX_CULL_BACK);
    GDSetBlendModeEtc(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_NOOP, GX_FALSE, GX_TRUE, GX_FALSE);
    GDSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GDSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    GXColor addColor = {0, 0, 0, 4};
    GDSetTevColor(GX_TEVREG0, addColor);
    GDSetTevAlphaCalcAndSwap(GX_TEVSTAGE0, GX_CA_A0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV,
                             GX_TEV_SWAP0, GX_TEV_SWAP0);
    storeDisplayList(&obj, 9);

    GDSetBlendModeEtc(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_INVDSTALPHA, GX_LO_NOOP, GX_TRUE, GX_TRUE, GX_FALSE);
    GDSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    GDSetDstAlpha(GX_TRUE, 0);
    storeDisplayList(&obj, 10);

    GDSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GDSetGenMode2(1, 0, 1, 0, GX_CULL_FRONT);
    GDSetAlphaCompare(GX_GREATER, 0x20, GX_AOP_AND, GX_ALWAYS, 0);
    GDSetBlendModeEtc(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_NOOP, GX_TRUE, GX_TRUE, GX_FALSE);
    GDSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
    GDSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
    GDSetTevColorCalc(GX_TEVSTAGE0, GX_CC_TEXC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_DIVIDE_2, GX_TRUE, GX_TEVPREV);
    GDSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR_NULL);
    storeDisplayList(&obj, 11);

    GDSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    GDSetGenMode2(1, 0, 1, 0, GX_CULL_BACK);
    GDSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GDSetBlendModeEtc(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_NOOP, GX_TRUE, GX_FALSE, GX_FALSE);
    GDSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
    GDSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
    GDSetTevColorCalc(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ONE, GX_CC_C0, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GDSetTevAlphaCalcAndSwap(GX_TEVSTAGE0, GX_CA_A0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV,
                             GX_TEV_SWAP0, GX_TEV_SWAP0);
    GDSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR_NULL);
    storeDisplayList(&obj, 12);

    GDSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GDSetZMode(GX_TRUE, GX_GEQUAL, GX_FALSE);
    GDSetGenMode2(0, 1, 1, 0, GX_CULL_BACK);
    GDSetTevDirect(GX_TEVSTAGE0);
    GDSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
    GDSetBlendModeEtc(GX_BM_BLEND, GX_BL_ONE, GX_BL_SRCALPHA, GX_LO_CLEAR, GX_TRUE, GX_FALSE, GX_FALSE);
    static GXColor tintColor = {35, 25, 25, 5};
    GDSetTevColor(GX_TEVREG0, tintColor);
    GDSetTevColorCalc(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GDSetTevAlphaCalcAndSwap(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV,
                             GX_TEV_SWAP0, GX_TEV_SWAP0);
    storeDisplayList(&obj, 13);

    GDSetDstAlpha(GX_TRUE, 0x40);
    storeDisplayList(&obj, 14);

    GDSetBlendModeEtc(GX_BM_BLEND, GX_BL_INVDSTALPHA, GX_BL_DSTALPHA, GX_LO_CLEAR, GX_TRUE, GX_FALSE, GX_FALSE);
    storeDisplayList(&obj, 15);
 #endif

    _1E0 = -1;
    _1DC = 0x32323232;
    _1D8 = 0xFF0000FF;
    mFlags.clear();
    _1D4 = 0.0f;
    _1D0 = 0;
    _1B4 = nullptr;
    _1B8 = nullptr;
    _1BC = 0;

    u32 count = mModelData->getMaterialNum();
    _1C4 = new u8*[count];
    _1C8 = new u8*[count];
    _1CC = new u16[count];
    for (u32 i = 0; i < count; i++) {
        _1C4[i] = nullptr;
        _1C8[i] = nullptr;
        _1CC[i] = 0;
    }
    _DC = 0;
}

void J3DModelX::shapePacketDrawFast(J3DShapePacketX* packet) const {
#ifdef __3DS__
    (void)packet;
    return;
#else
    if (!(packet->mFlags & 0x10) && packet->getShape()) {
        packet->prepareDraw();
        if (packet->getTexMtxObj()) {
            J3DDifferedTexMtx::sTexGenBlock = packet->getShape()->mMaterial->mTexGenBlock;
            J3DDifferedTexMtx::sTexMtxObj = packet->getTexMtxObj();
        } else {
            J3DDifferedTexMtx::sTexGenBlock = nullptr;
        }
        shapeDrawFast(static_cast< J3DShapeX* >(packet->getShape()));
    }
#endif
}

void J3DModelX::shapeDrawFast(J3DShapeX* shape) const {
#ifdef __3DS__
    (void)shape;
    return;
#else
    if (J3DShape::sOldVcdVatCmd != shape->mVcdVatCmd) {
        GXCallDisplayList(shape->mVcdVatCmd, 0xC0);
        J3DShape::sOldVcdVatCmd = shape->mVcdVatCmd;
    }

    if (J3DShape::sEnvelopeFlag && !shape->mHasPNMTXIdx) {
        shape->mCurrentMtx.load();
    }

    shape->setArrayAndBindPipeline();

    if (!(shape->mFlags & 0x200)) {
        if (J3DShapeMtx::sLODFlag) {
            J3DShapeMtx::resetMtxLoadCache();
        }
        u16 count = shape->mMtxGroupNum;
        for (u16 i = 0; i < count; i++) {
            if (shape->mShapeMtx[i]) {
                shape->mShapeMtx[i]->load();
            }
            if (mShapeCallback) {
                mShapeCallback(shape);
            }
            if (shape->mShapeDraw[i]) {
                shape->mShapeDraw[i]->draw();
            }
        }
    } else {
        J3DFifoLoadPosMtxImm(*j3dSys.getShapePacket()->getBaseMtxPtr(), 0);
        J3DFifoLoadNrmMtxImm(*j3dSys.getShapePacket()->getBaseMtxPtr(), 0);
        u16 count = shape->mMtxGroupNum;
        for (u16 i = 0; i < count; i++) {
            if (shape->mShapeDraw[i]) {
                shape->mShapeDraw[i]->draw();
            }
        }
    }
#endif
}

J3DModelX::~J3DModelX() {
}
