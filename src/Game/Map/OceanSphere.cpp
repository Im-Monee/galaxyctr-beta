#include "Game/Map/OceanSphere.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Map/OceanSpherePoint.hpp"
#include "Game/Map/WaterAreaHolder.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util/ActorMovementUtil.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/SchedulerUtil.hpp"
#include <JSystem/JGeometry/TMatrix.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#ifdef __3DS__
#include "WaterRender3DS.hpp"
#endif
#include <revolution/gx/GXVert.h>

GXColor sOceanSphereTevReg0Face = {0x48, 0x80, 0xBE, 0x1C};
GXColor sOceanSphereTevReg1FrontDefault = {0x00, 0x51, 0x70, 0x6F};
GXColor sOceanSphereTevReg1FrontTear = {0x00, 0x64, 0xFF, 0x6F};
GXColor sOceanSphereTevReg2Face = {0xFF, 0xFF, 0xFF, 0xFF};
GXColor sOceanSphereKColor0Face = {0x78, 0xFF, 0xFF, 0x00};
GXColor sOceanSphereTevReg0Back = {0x48, 0x80, 0xBE, 0x1C};
GXColor sOceanSphereTevReg1BackDefault = {0x00, 0x51, 0x70, 0x6F};
GXColor sOceanSphereTevReg1BackTear = {0x00, 0xC3, 0xFF, 0x6F};
GXColor sOceanSphereTevReg2Back = {0xFF, 0xFF, 0xFF, 0xFF};
GXColor sOceanSphereKColor0Back = {0x78, 0xFF, 0xFF, 0x00};
GXColor sOceanSphereTevReg0Env = {0x00, 0x69, 0xB8, 0x14};
GXColor sOceanSphereTevReg1Env = {0x00, 0x00, 0x00, 0xFF};

#ifdef __3DS__
namespace {
    // The material drawSphere draws with (OceanSphere::draw sets it for each pass).
    HostGfxState sOceanSphereState;
    // The strips drawSphere built last (8 (n - 1)^2 vertices for n points a side, up to 20), so
    // that the face's second pass draws them again instead of transforming them twice.
    // The strips become one indexed triangle list, one draw a pass: drawn strip by strip they were
    // 152 draws a pass, up to 9 ms a frame on the console (CIA 0.12.14 log, draw:11).
    const u32 kOceanSphereVertices = 8 * 19 * 19;
    const u32 kOceanSphereStrips = 8 * 19;
    HostGfxVertex sOceanSphereVertices[kOceanSphereVertices];
    u16 sOceanSphereIndices[3 * kOceanSphereVertices];
    u32 sOceanSphereVertexCount;
    u32 sOceanSphereIndexCount;
    bool sOceanSphereReuse;

    // Strip triangle k keeps the GPU's alternating winding: (k, k+1, k+2), odd ones (k+1, k, k+2).
    void addOceanSphereStrip(u32 first, u32 count) {
        for (u32 k = 0; k + 2 < count; k++) {
            u16* triangle = &sOceanSphereIndices[sOceanSphereIndexCount];
            triangle[0] = static_cast< u16 >(first + ((k & 1) ? k + 1 : k));
            triangle[1] = static_cast< u16 >(first + ((k & 1) ? k : k + 1));
            triangle[2] = static_cast< u16 >(first + k + 2);
            sOceanSphereIndexCount += 3;
        }
    }

    void drawOceanSphereStrips(bool again = true) {
        // The later passes of a sphere share the first's vertices: copied for each, up to 11552 of
        // the frame's 16384 transient ones went on one sphere and the HUD, drawn last, went without
        // (under Good Egg's DiskGardenPlanet, CIA 0.18.7 log).
        if (sOceanSphereIndexCount != 0 &&
            (!again || !hostGfxDrawAgain(sOceanSphereIndices, sOceanSphereIndexCount, HOST_GFX_TRIANGLES, &sOceanSphereState))) {
            WaterRender3DS::draw(sOceanSphereVertices, sOceanSphereVertexCount, sOceanSphereIndices, sOceanSphereIndexCount,
                                 HOST_GFX_TRIANGLES, sOceanSphereState);
        }
    }

    unsigned long packConstant(GXColor color) {
        return color.r | (color.g << 8) | (color.b << 16) | (static_cast< unsigned long >(color.a) << 24);
    }

    enum { PRIMARY = 0x00, TEXTURE0 = 0x03, TEXTURE1 = 0x04, CONSTANT = 0x0E, PREVIOUS = 0x0F };
    enum { REPLACE = 0, MODULATE = 1, MULTIPLY_ADD = 8, ONE_MINUS = 1 };

    // loadMaterialFace: TEXC(0) * K0, then TEXC(1) * that * 2, then lerp(C1, C2, that) with C2
    // white as in both colour sets: C1 * (1 - that) + that, alpha A1. GX stage 2 adds C0 where
    // the red equals A0 (TEV_COMP_R8_EQ): makeGlintState draws that part after.
    // Blended, Z tested without update, GX_CULL_BACK (1) unless cull is given.
    HostGfxState makeFaceState(void* water, GXColor kColor0, GXColor tevReg1, u8 cull) {
        HostGfxState state = {};
        state.textures[0] = water;
        state.textures[1] = water;
        state.stageCount = 3;
        HostGfxStage& stage0 = state.stages[0];
        stage0.colorSrc[0] = TEXTURE0;
        stage0.colorSrc[1] = CONSTANT;
        stage0.colorFunc = MODULATE;
        stage0.alphaSrc[0] = PRIMARY;
        stage0.alphaFunc = REPLACE;
        stage0.constant = packConstant(kColor0);
        HostGfxStage& stage1 = state.stages[1];
        stage1.colorSrc[0] = TEXTURE1;
        stage1.colorSrc[1] = PREVIOUS;
        stage1.colorFunc = MODULATE;
        stage1.colorScale = 1;  // GPU_TEVSCALE_2
        stage1.alphaSrc[0] = PREVIOUS;
        stage1.alphaFunc = REPLACE;
        HostGfxStage& stage2 = state.stages[2];
        stage2.colorSrc[0] = CONSTANT;
        stage2.colorSrc[1] = PREVIOUS;
        stage2.colorSrc[2] = PREVIOUS;
        stage2.colorOp[1] = ONE_MINUS;
        stage2.colorFunc = MULTIPLY_ADD;
        stage2.alphaSrc[0] = CONSTANT;
        stage2.alphaFunc = REPLACE;
        stage2.constant = packConstant(tevReg1);
        state.blendSrc = 6;  // GPU_SRC_ALPHA
        state.blendDst = 7;  // GPU_ONE_MINUS_SRC_ALPHA
        state.alphaTestFunc = 1;  // GPU_ALWAYS
        state.depthTest = 1;
        state.depthFunc = 7;  // GX_LEQUAL with the 3DS depth reversed (GPU_GEQUAL)
        state.writeMask = 0x0F;
        state.cull = cull;
        state.scissor[2] = 640;
        state.scissor[3] = 456;
        return state;
    }

    // GX stage 2 of loadMaterialFace on the pixels where the red of TEXC(1) * TEXC(0) * K0 * 2
    // equals A0: that red goes in the alpha (GPU_TEVOP_A_SRC_R) and the alpha test keeps them. The
    // water texture is grey (9 to 145), so there green and blue are at most about 62 and adding C0
    // never passes 255: stage 3's lerp then gives (1 - C1) * C0 more than the face pass did, which
    // blended at A1 is a constant added over it. Colour only, same depth test and culling.
    HostGfxState makeGlintState(void* water, GXColor kColor0, GXColor tevReg0, GXColor tevReg1, u8 cull) {
        HostGfxState state = makeFaceState(water, kColor0, tevReg1, cull);
        state.stageCount = 2;
        HostGfxStage& stage0 = state.stages[0];
        stage0.colorSrc[0] = CONSTANT;
        stage0.colorFunc = REPLACE;
        stage0.alphaSrc[0] = TEXTURE0;
        stage0.alphaSrc[1] = CONSTANT;
        stage0.alphaOp[0] = 0x02;  // GPU_TEVOP_A_SRC_R
        stage0.alphaFunc = MODULATE;
        GXColor add;
        add.r = (255 - tevReg1.r) * tevReg0.r / 255 * tevReg1.a / 255;
        add.g = (255 - tevReg1.g) * tevReg0.g / 255 * tevReg1.a / 255;
        add.b = (255 - tevReg1.b) * tevReg0.b / 255 * tevReg1.a / 255;
        add.a = kColor0.r;
        stage0.constant = packConstant(add);
        HostGfxStage& stage1 = state.stages[1];
        stage1 = HostGfxStage();
        stage1.colorSrc[0] = PREVIOUS;
        stage1.colorFunc = REPLACE;
        stage1.alphaSrc[0] = TEXTURE1;
        stage1.alphaSrc[1] = PREVIOUS;
        stage1.alphaOp[0] = 0x02;
        stage1.alphaFunc = MODULATE;
        stage1.alphaScale = 1;  // GPU_TEVSCALE_2
        state.blendSrc = 1;  // GPU_ONE
        state.blendDst = 1;
        state.alphaTestFunc = 2;  // GPU_EQUAL
        state.alphaRef = tevReg0.a;
        state.writeMask = 0x07;
        return state;
    }

    // The face pass's strips (drawn last) again with makeGlintState.
    void drawOceanSphereGlint(void* water, GXColor kColor0, GXColor tevReg0, GXColor tevReg1, u8 cull) {
        if (tevReg1.a == 0) {
            return;
        }
        sOceanSphereState = makeGlintState(water, kColor0, tevReg0, tevReg1, cull);
        drawOceanSphereStrips();
    }

    // loadMaterialBack: lerp(C0, C1, TEXC) with C1 black, C0 * (1 - TEXC); opaque (GX_BL_ONE,
    // GX_BL_ZERO), Z updated, GX_CULL_FRONT (2).
    HostGfxState makeEnvState(void* envMap) {
        HostGfxState state = {};
        state.textures[0] = envMap;
        state.stageCount = 1;
        HostGfxStage& stage = state.stages[0];
        stage.colorSrc[0] = CONSTANT;
        stage.colorSrc[1] = TEXTURE0;
        stage.colorOp[1] = ONE_MINUS;
        stage.colorFunc = MODULATE;
        stage.alphaSrc[0] = CONSTANT;
        stage.alphaFunc = REPLACE;
        stage.constant = packConstant(sOceanSphereTevReg0Env);
        state.blendSrc = 1;  // GPU_ONE
        state.blendDst = 0;  // GPU_ZERO
        state.alphaTestFunc = 1;
        state.depthTest = 1;
        state.depthFunc = 7;
        state.writeMask = 0x1F;
        state.cull = 2;
        state.scissor[2] = 640;
        state.scissor[3] = 456;
        return state;
    }
}  // namespace
#endif

namespace NrvOceanSphere {
    NEW_NERVE(OceanSphereNrvRiseUp, OceanSphere, RiseUp);
    NEW_NERVE(OceanSphereNrvWait, OceanSphere, Wait);
};  // namespace NrvOceanSphere

namespace {
    Vec sAxisX = {1.0f, 0.0f, 0.0f};
    Vec sAxisY = {0.0f, 1.0f, 0.0f};
    Vec sAxisZ = {0.0f, 0.0f, 1.0f};

    TVec3f sPosAxisX(sAxisX);
    TVec3f sPosAxisY(sAxisY);
    TVec3f sPosAxisZ(sAxisZ);
};  // namespace

namespace {
    u32 calcDisplayListSize(u32 count, u32 stride) {
        count = (((count + 1) * count) & ~1) - count - 1;
        return (((((count - 1) * 3 + stride * count) >> 2) & 0x07FFFFFF) + 2) << 5;
    }

#ifndef __3DS__
    void beginDrawPolygon(int idx, s32 pointCount, bool useGD) {
        if (useGD) {
            GDOverflowCheck(1);
            *__GDCurrentDL->ptr++ = GX_TRIANGLESTRIP;
            GDOverflowCheck(2);
            u16 vtxCount = (u16)(((pointCount - 1 - idx) * 2 + 1));
            *__GDCurrentDL->ptr++ = (u8)(vtxCount >> 8);
            *__GDCurrentDL->ptr++ = (u8)vtxCount;
        } else {
            u16 vtxCount = (u16)(((pointCount - 1 - idx) * 2 + 1));
            GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, vtxCount);
        }
    }
#endif
};  // namespace

OceanSpherePlane::OceanSpherePlane(s32 pointCount, const TVec3f* pCenter, const TVec3f& rAxis1, const TVec3f& rAxis2, const TVec2f& rTex1,
                                   const TVec2f& rTex2, const TVec2f& rTex3) {
    mPoints = nullptr;
    mAxisPointCount = pointCount - 2;
    mGridPointCount = mAxisPointCount * mAxisPointCount;
    mPoints = new OceanSpherePoint*[mGridPointCount];

    TRot3f rot;
    rot.identity();
    TVec2f texCenter(0.5f, 0.5f);

    for (s32 row = 1; row < pointCount - 1; row++) {
        rot.setRotate(rAxis2, (static_cast< f32 >(row) / (pointCount - 1)) * (MR::pi() / 2.0f));
        TVec3f vec(rAxis1);
        rot.mult(vec, vec);
        f32 rowRate = static_cast< f32 >(row) / (pointCount - 1);
        TVec2f texA = texCenter * (1.0f - rowRate) + rTex1 * rowRate;
        TVec2f texB = texCenter * (1.0f - rowRate) + rTex2 * rowRate;
        TVec2f texC = texCenter * (1.0f - rowRate) + rTex3 * rowRate;

        for (s32 col = 1; col < 2 * row; col++) {
            rot.setRotate(rAxis1, (static_cast< f32 >(col) / (2 * row)) * MR::pi());
            TVec3f normal(vec);
            rot.mult(normal, normal);
            TVec2f tex(0.0f, 0.0f);
            s32 index;

            if (col <= row) {
                index = (row - 1) * mAxisPointCount + col - 1;
                f32 rate = static_cast< f32 >(col) / row;
                tex = texB * (1.0f - rate) + texA * rate;
            } else {
                index = (row - 1) * (mAxisPointCount + 1) - mAxisPointCount * (col - row);
                f32 rate = static_cast< f32 >(col - row) / row;
                tex = texA * (1.0f - rate) + texC * rate;
            }

            mPoints[index] = new OceanSpherePoint(pCenter, normal, row, col, tex);
        }
    }
}

void OceanSpherePlane::update(f32 radius, f32 wave1Time, f32 wave2Time) {
    for (s32 row = 0; row < mAxisPointCount; row++) {
        for (s32 col = 0; col < mAxisPointCount; col++) {
            getPoint(col, row)->updatePos(radius, wave1Time, wave2Time);
        }
    }
}

OceanSpherePlaneEdge::OceanSpherePlaneEdge(s32 pointCount, const TVec3f* pCenter, const TVec3f& rAxis1, const TVec3f& rAxis2, const TVec2f& rTex1,
                                           const TVec2f& rTex2) {
    mPoints = nullptr;
    mPointCount = pointCount - 2;
    mPoints = new OceanSpherePoint*[mPointCount];
    TVec3f axis = rAxis1.cross(rAxis2);
    MR::normalize(&axis);
    TRot3f rot;
    rot.identity();
    rot.setRotate(axis, (1.0f / (pointCount - 1)) * (MR::pi() / 2.0f));
    TVec3f vec(rAxis1);

    for (s32 i = 0; i < mPointCount; i++) {
        rot.mult(vec, vec);
        f32 rate = static_cast< f32 >(i + 1) / (pointCount - 1);
        TVec2f tex = rTex1 * (1.0f - rate) + rTex2 * rate;
        mPoints[i] = new OceanSpherePoint(pCenter, vec, i, i, tex);
    }
}

void OceanSpherePlaneEdge::update(f32 radius, f32 wave1Time, f32 wave2Time) {
    for (s32 i = 0; i < mPointCount; i++) {
        mPoints[i]->updatePos(radius, wave1Time, wave2Time);
    }
}

OceanSphere::OceanSphere(const char* pName)
    : LiveActor(pName), mRadius(), mRadiusTarget(), mPointCount(), mAxisPointPX(), mAxisPointNX(), mAxisPointPY(), mAxisPointNY(), mAxisPointPZ(),
      mAxisPointNZ(), mPlaneLeftUpper(), mPlaneRightUpper(), mPlaneLeftLower(), mPlaneRightLower(), mEdge0(), mEdge1(), mEdge2(), mEdge3(), mEdge4(),
      mEdge5(), mEdge6(), mEdge7(), mWaveTime1(), mWaveTime2(), mTexOffs0X(), mTexOffs0Y(), mTexOffs1X(), mTexOffs1Y(), mWaterTex(), mWaterEnvTex() {
    GXColor front = sOceanSphereTevReg1FrontDefault;
    mTevReg1Front = front;

    GXColor back = sOceanSphereTevReg1BackDefault;
    mTevReg1Back = back;

    mUseDisplayList = false;
    mDispListFaceLen = 0;
    mDispListFace = nullptr;
    mDispListBackLen = 0;
    mDispListBack = nullptr;
    mAlwaysUseRealDrawing = false;
    mIsCameraInside = false;
    mIsStartPosCamera = true;
    mIsTearDrop = false;
    mEnableStartPosCameraSwitch = false;
}

void OceanSphere::init(const JMapInfoIter& rIter) {
    MR::connectToScene(this, MR::MovementType_MapObj, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_OceanSphere);
    MR::initDefaultPos(this, rIter);
    mRadius = 100.0f * mScale.x;
    mRadiusTarget = mRadius;
    initPoints();
    updatePoints();
    initDisplayList();
    WaterAreaFunction::entryOceanSphere(this);

    mWaterTex = new JUTTexture(MR::loadTexFromArc("WaterWave.arc", "OceanSphere.bti"), 0);
    mWaterEnvTex = new JUTTexture(MR::loadTexFromArc("WaterWave.arc", "OceanSphereEnvRef.bti"), 0);

    MR::setClippingTypeSphere(this, mRadius);
    MR::setClippingFarMax(this);
    initNerve(GET_NERVE(OceanSphere, OceanSphereNrvWait));

    if (MR::isEqualStageName("SkullSharkGalaxy")) {
        mEnableStartPosCameraSwitch = true;
        mAlwaysUseRealDrawing = true;
    }

    if (MR::isEqualStageName("TearDropGalaxy")) {
        mIsTearDrop = true;
        mAlwaysUseRealDrawing = true;
        mIsStartPosCamera = false;
        GXColor front;
        GXColor back;
        GXColor frontTear = sOceanSphereTevReg1FrontTear;
        front = frontTear;
        mTevReg1Front = front;

        GXColor backTear = sOceanSphereTevReg1BackTear;
        back = backTear;
        mTevReg1Back = back;
    }

    if (MR::tryRegisterDemoCast(this, rIter)) {
        makeActorDead();
    } else {
        MR::registerDemoSimpleCastAll(this);
        makeActorAppeared();
    }
}

void OceanSphere::appear() {
    LiveActor::appear();
    mRadius = 1000.0f;
    setNerve(GET_NERVE(OceanSphere, OceanSphereNrvRiseUp));
}

void OceanSphere::exeWait() {
}

void OceanSphere::exeRiseUp() {
    f32 rate = MR::calcDemoPartStepRate("湧き水上昇");
    mRadius = 1000.0f + (rate * (mRadiusTarget - 1000.0f));

    if (MR::isDemoPartLastStep("湧き水上昇")) {
        setNerve(GET_NERVE(OceanSphere, OceanSphereNrvWait));
    }
}

bool OceanSphere::isInWater(const TVec3f& rPos) const {
    if (MR::isDead(this)) {
        return false;
    }

    return rPos.distance(mPosition) <= mRadius;
}

#pragma push
#pragma opt_propagation off
bool OceanSphere::calcWaterInfo(const TVec3f& rPos, const TVec3f& rGravity, WaterInfo* pInfo) const {
    const TVec3f& rPosition = mPosition;
    const f32 radius = mRadius;
    TVec3f offset = rPos - rPosition;
    const f32 alongGravity = MR::vecKillElement(offset, -rGravity, &offset);
    const f32 height = radius * MR::cos(offset.length() / radius * MR::pi() / 2.0f);
    pInfo->mCamWaterDepth = height - alongGravity;
    pInfo->_4 = height + alongGravity;

    TVec3f normal = rPos - rPosition;
    MR::normalizeOrZero(&normal);
    pInfo->mSurfaceNormal.set(normal);
    pInfo->mSurfacePos.set(rPosition + normal * radius);
    return true;
}
#pragma pop

void OceanSphere::initPoints() {
    if (mRadius <= 300.0f) {
        mPointCount = 8;
    } else if (mRadius <= 500.0f) {
        mPointCount = 10;
    } else if (mRadius <= 1000.0f) {
        mPointCount = 15;
    } else {
        mPointCount = 20;
    }

    OceanSpherePoint* pPoint;

    pPoint = new OceanSpherePoint(&mPosition, ::sPosAxisX, 0.0f, 0.0f, TVec2f(1.0f, 0.0f));
    mAxisPointPX = pPoint;

    pPoint = new OceanSpherePoint(&mPosition, -::sPosAxisX, 0.0f, 0.0f, TVec2f(0.0f, 1.0f));
    mAxisPointNX = pPoint;

    pPoint = new OceanSpherePoint(&mPosition, ::sPosAxisY, 0.0f, 0.0f, TVec2f(0.5f, 0.5f));
    mAxisPointPY = pPoint;

    pPoint = new OceanSpherePoint(&mPosition, -::sPosAxisY, 0.0f, 0.0f, TVec2f(0.5f, 0.5f));
    mAxisPointNY = pPoint;

    pPoint = new OceanSpherePoint(&mPosition, ::sPosAxisZ, 0.0f, 0.0f, TVec2f(1.0f, 1.0f));
    mAxisPointPZ = pPoint;

    pPoint = new OceanSpherePoint(&mPosition, -::sPosAxisZ, 0.0f, 0.0f, TVec2f(0.0f, 0.0f));
    mAxisPointNZ = pPoint;

    OceanSpherePlane* pPlane;

    pPlane = new OceanSpherePlane(mPointCount, &mPosition, ::sPosAxisY, -::sPosAxisX, TVec2f(0.0f, 1.0f), TVec2f(0.0f, 0.0f), TVec2f(1.0f, 1.0f));
    mPlaneLeftUpper = pPlane;

    pPlane = new OceanSpherePlane(mPointCount, &mPosition, ::sPosAxisY, ::sPosAxisX, TVec2f(1.0f, 0.0f), TVec2f(1.0f, 1.0f), TVec2f(0.0f, 0.0f));
    mPlaneRightUpper = pPlane;

    pPlane = new OceanSpherePlane(mPointCount, &mPosition, -::sPosAxisY, -::sPosAxisX, TVec2f(0.0f, 1.0f), TVec2f(1.0f, 1.0f), TVec2f(0.0f, 0.0f));
    mPlaneLeftLower = pPlane;

    pPlane = new OceanSpherePlane(mPointCount, &mPosition, -::sPosAxisY, ::sPosAxisX, TVec2f(1.0f, 0.0f), TVec2f(0.0f, 0.0f), TVec2f(1.0f, 1.0f));
    mPlaneRightLower = pPlane;

    mEdge0 = new OceanSpherePlaneEdge(mPointCount, &mPosition, ::sPosAxisY, ::sPosAxisZ, TVec2f(0.5f, 0.5f), TVec2f(1.0f, 1.0f));

    mEdge1 = new OceanSpherePlaneEdge(mPointCount, &mPosition, ::sPosAxisY, -::sPosAxisZ, TVec2f(0.5f, 0.5f), TVec2f(0.0f, 0.0f));

    mEdge2 = new OceanSpherePlaneEdge(mPointCount, &mPosition, -::sPosAxisY, ::sPosAxisZ, TVec2f(0.5f, 0.5f), TVec2f(1.0f, 1.0f));

    mEdge3 = new OceanSpherePlaneEdge(mPointCount, &mPosition, -::sPosAxisY, -::sPosAxisZ, TVec2f(0.5f, 0.5f), TVec2f(0.0f, 0.0f));

    mEdge4 = new OceanSpherePlaneEdge(mPointCount, &mPosition, ::sPosAxisZ, -::sPosAxisX, TVec2f(1.0f, 1.0f), TVec2f(0.0f, 1.0f));

    mEdge5 = new OceanSpherePlaneEdge(mPointCount, &mPosition, ::sPosAxisZ, ::sPosAxisX, TVec2f(1.0f, 1.0f), TVec2f(1.0f, 0.0f));

    mEdge6 = new OceanSpherePlaneEdge(mPointCount, &mPosition, -::sPosAxisZ, -::sPosAxisX, TVec2f(0.0f, 0.0f), TVec2f(0.0f, 1.0f));

    mEdge7 = new OceanSpherePlaneEdge(mPointCount, &mPosition, -::sPosAxisZ, ::sPosAxisX, TVec2f(0.0f, 0.0f), TVec2f(1.0f, 0.0f));
}

void OceanSphere::initDisplayList() {
#ifdef __3DS__
    // The 3DS path draws through WaterRender3DS/hostGfx directly and never reads the display
    // lists; building them here would emit GPU draws during scene init, so skip them entirely.
    mUseDisplayList = false;
    return;
#else
    MR::ProhibitSchedulerAndInterrupts prohibit(false);
    u32 sizeFace = ::calcDisplayListSize(mPointCount, 0x1C);
    mDispListFace = new (0x20) u8[sizeFace];
    DCInvalidateRange(mDispListFace, sizeFace);
    GDLObj obj;
    GDInitGDLObj(&obj, mDispListFace, sizeFace);
    __GDCurrentDL = &obj;
    drawSphere(false, true);
    GDPadCurr32();
    mDispListFaceLen = obj.ptr - obj.start;
    DCStoreRange(mDispListFace, sizeFace);
    u32 sizeBack = ::calcDisplayListSize(mPointCount, 0x18);
    mDispListBack = new (0x20) u8[sizeBack];
    DCInvalidateRange(mDispListBack, sizeBack);
    GDInitGDLObj(&obj, mDispListBack, sizeBack);
    __GDCurrentDL = &obj;
    drawSphere(true, true);
    GDPadCurr32();
    mDispListBackLen = obj.ptr - obj.start;
    DCStoreRange(mDispListBack, sizeBack);
#endif
}

void OceanSphere::control() {
    mIsCameraInside = false;

    if (WaterAreaFunction::getCameraWaterInfo()->mOceanSphere == this) {
        mIsCameraInside = true;

        if (mEnableStartPosCameraSwitch && mIsStartPosCamera && MR::isStartPosCameraEnd()) {
            mIsStartPosCamera = false;
        }
    }

    mWaveTime1 += -0.1f;
    mWaveTime2 += -0.1f;

    if (!mAlwaysUseRealDrawing && MR::calcDistanceToPlayer(this) > 10000.0f) {
        mUseDisplayList = true;
    } else {
        mUseDisplayList = false;
        updatePoints();
    }

    const f32 add = 0.0008f;
    const f32 sub = -0.0008f;

    mTexOffs0X = MR::repeat(add + mTexOffs0X, 0.0f, 1.0f);
    mTexOffs0Y = MR::repeat(add + mTexOffs0Y, 0.0f, 1.0f);
    mTexOffs1X = MR::repeat(add + mTexOffs1X, 0.0f, 1.0f);
    mTexOffs1Y = MR::repeat(sub + mTexOffs1Y, 0.0f, 1.0f);
}

void OceanSphere::updatePoints() {
    mAxisPointPX->updatePos(mRadius, mWaveTime1, mWaveTime2);
    mAxisPointNX->updatePos(mRadius, mWaveTime1, mWaveTime2);
    mAxisPointPY->updatePos(mRadius, mWaveTime1, mWaveTime2);
    mAxisPointNY->updatePos(mRadius, mWaveTime1, mWaveTime2);
    mAxisPointPZ->updatePos(mRadius, mWaveTime1, mWaveTime2);
    mAxisPointNZ->updatePos(mRadius, mWaveTime1, mWaveTime2);

    mPlaneLeftUpper->update(mRadius, mWaveTime1, mWaveTime2);
    mPlaneRightUpper->update(mRadius, mWaveTime1, mWaveTime2);
    mPlaneLeftLower->update(mRadius, mWaveTime1, mWaveTime2);
    mPlaneRightLower->update(mRadius, mWaveTime1, mWaveTime2);

    mEdge0->update(mRadius, mWaveTime1, mWaveTime2);
    mEdge1->update(mRadius, mWaveTime1, mWaveTime2);
    mEdge2->update(mRadius, mWaveTime1, mWaveTime2);
    mEdge3->update(mRadius, mWaveTime1, mWaveTime2);
    mEdge4->update(mRadius, mWaveTime1, mWaveTime2);
    mEdge5->update(mRadius, mWaveTime1, mWaveTime2);
    mEdge6->update(mRadius, mWaveTime1, mWaveTime2);
    mEdge7->update(mRadius, mWaveTime1, mWaveTime2);
}

OceanSpherePoint* OceanSphere::getPlanePointLeftUpper(int row, int col) const {
    int max;

    if (row != 0 && col != 0) {
        max = mPointCount - 1;

        if (row != max && col != max) {
            return mPlaneLeftUpper->getPoint(row - 1, col - 1);
        }
    }

    if (row == 0) {
        if (col == 0) {
            return mAxisPointPY;
        }

        max = mPointCount - 1;
        if (col == max) {
            return mAxisPointNZ;
        }

        return mEdge1->mPoints[col - 1];
    }

    if (col == 0) {
        max = mPointCount - 1;
        if (row == max) {
            return mAxisPointPZ;
        }

        return mEdge0->mPoints[row - 1];
    }

    max = mPointCount - 1;
    if (row == max) {
        if (col == max) {
            return mAxisPointNX;
        }

        return mEdge4->mPoints[col - 1];
    }

    return mEdge6->mPoints[row - 1];
}

OceanSpherePoint* OceanSphere::getPlanePointRightUpper(int row, int col) const {
    int max;

    if (row != 0 && col != 0) {
        max = mPointCount - 1;

        if (row != max && col != max) {
            return mPlaneRightUpper->getPoint(row - 1, col - 1);
        }
    }

    if (row == 0) {
        if (col == 0) {
            return mAxisPointPY;
        }

        max = mPointCount - 1;
        if (col == max) {
            return mAxisPointPZ;
        }

        return mEdge0->mPoints[col - 1];
    }

    if (col == 0) {
        max = mPointCount - 1;
        if (row == max) {
            return mAxisPointNZ;
        }

        return mEdge1->mPoints[row - 1];
    }

    max = mPointCount - 1;
    if (row == max) {
        if (col == max) {
            return mAxisPointPX;
        }

        return mEdge7->mPoints[col - 1];
    }

    return mEdge5->mPoints[row - 1];
}

OceanSpherePoint* OceanSphere::getPlanePointLeftLower(int row, int col) const {
    int max;

    if (row != 0 && col != 0) {
        max = mPointCount - 1;

        if (row != max && col != max) {
            return mPlaneLeftLower->getPoint(row - 1, col - 1);
        }
    }

    if (row == 0) {
        if (col == 0) {
            return mAxisPointNY;
        }

        max = mPointCount - 1;
        if (col == max) {
            return mAxisPointPZ;
        }

        return mEdge2->mPoints[col - 1];
    }

    if (col == 0) {
        max = mPointCount - 1;
        if (row == max) {
            return mAxisPointNZ;
        }

        return mEdge3->mPoints[row - 1];
    }

    max = mPointCount - 1;
    if (row == max) {
        if (col == max) {
            return mAxisPointNX;
        }

        return mEdge6->mPoints[col - 1];
    }

    return mEdge4->mPoints[row - 1];
}

OceanSpherePoint* OceanSphere::getPlanePointRightLower(int row, int col) const {
    int max;

    if (row != 0 && col != 0) {
        max = mPointCount - 1;

        if (row != max && col != max) {
            return mPlaneRightLower->getPoint(row - 1, col - 1);
        }
    }

    if (row == 0) {
        if (col == 0) {
            return mAxisPointNY;
        }

        max = mPointCount - 1;
        if (col == max) {
            return mAxisPointNZ;
        }

        return mEdge3->mPoints[col - 1];
    }

    if (col == 0) {
        max = mPointCount - 1;
        if (row == max) {
            return mAxisPointPZ;
        }

        return mEdge2->mPoints[row - 1];
    }

    max = mPointCount - 1;
    if (row == max) {
        if (col == max) {
            return mAxisPointPX;
        }

        return mEdge5->mPoints[col - 1];
    }

    return mEdge7->mPoints[row - 1];
}

void OceanSphere::draw() const {
    if (!MR::isValidDraw(this)) {
        return;
    }

#ifdef __3DS__
    // The Wii's passes (below), without the display lists.
    void* water = rvl3DSGetJUTTexture(mWaterTex);
    if (mIsStartPosCamera && !mIsCameraInside) {
        sOceanSphereState = makeEnvState(rvl3DSGetJUTTexture(mWaterEnvTex));
        drawSphere(true, false);
    }

    if (mIsStartPosCamera) {
        sOceanSphereState = makeFaceState(water, sOceanSphereKColor0Face, mTevReg1Front, mIsCameraInside ? 2 : 1);
        drawSphere(false, false);
        drawOceanSphereGlint(water, sOceanSphereKColor0Face, sOceanSphereTevReg0Face, mTevReg1Front, mIsCameraInside ? 2 : 1);
        return;
    }

    sOceanSphereState = makeFaceState(water, sOceanSphereKColor0Face, mTevReg1Front, 2);
    drawSphere(false, false);
    drawOceanSphereGlint(water, sOceanSphereKColor0Face, sOceanSphereTevReg0Face, mTevReg1Front, 2);

    if (mIsCameraInside) {
        return;
    }

    sOceanSphereState = makeFaceState(water, sOceanSphereKColor0Back, mTevReg1Back, 1);
    sOceanSphereReuse = true;
    drawSphere(false, false);
    sOceanSphereReuse = false;
    drawOceanSphereGlint(water, sOceanSphereKColor0Back, sOceanSphereTevReg0Back, mTevReg1Back, 1);
#else
    if (mIsStartPosCamera && !mIsCameraInside) {
        loadMaterialBack();

        if (mUseDisplayList) {
            GXCallDisplayList(mDispListBack, mDispListBackLen);
        } else {
            drawSphere(true, false);
        }
    }

    loadMaterialFace();

    if (mIsStartPosCamera) {
        if (mIsCameraInside) {
            GXSetCullMode(GX_CULL_FRONT);
        }

        if (mUseDisplayList) {
            GXCallDisplayList(mDispListFace, mDispListFaceLen);
        } else {
            drawSphere(false, false);
        }

        return;
    }

    GXSetCullMode(GX_CULL_FRONT);
    drawSphere(false, false);

    if (mIsCameraInside) {
        return;
    }

    GXSetCullMode(GX_CULL_BACK);
    GXSetTevColor(GX_TEVREG0, sOceanSphereTevReg0Back);
    GXSetTevColor(GX_TEVREG1, mTevReg1Back);
    GXSetTevColor(GX_TEVREG2, sOceanSphereTevReg2Back);
    GXSetTevKColor(GX_KCOLOR0, sOceanSphereKColor0Back);
    drawSphere(false, false);
#endif
}

#ifndef __3DS__
const GXColor sOceanSphereFogFace = {0xFF, 0xFF, 0xFF, 0};
const GXColor sOceanSphereFogBack = {0xFF, 0xFF, 0xFF, 0};

void OceanSphere::loadMaterialFace() const {
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_POS_XYZ, GX_F32, 0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX1, GX_DIRECT);
    GXLoadPosMtxImm(MR::getCameraViewMtx(), 0);
    GXSetCurrentMtx(0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(2);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
    GXSetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX3x4, GX_TG_TEX1, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
    mWaterTex->load(GX_TEXMAP0);
    GXSetNumIndStages(0);
    GXSetNumTevStages(4);
    GXSetTevColor(GX_TEVREG0, sOceanSphereTevReg0Face);
    GXSetTevColor(GX_TEVREG1, mTevReg1Front);
    GXSetTevColor(GX_TEVREG2, sOceanSphereTevReg2Face);
    GXSetTevKColor(GX_KCOLOR0, sOceanSphereKColor0Face);
    GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_CPREV, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE2, GX_CC_CPREV, GX_CC_A0, GX_CC_C0, GX_CC_CPREV);
    GXSetTevColorOp(GX_TEVSTAGE2, GX_TEV_COMP_R8_EQ, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    GXSetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE3, GX_CC_C1, GX_CC_C2, GX_CC_CPREV, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE3, GX_CA_A1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    GXSetZCompLoc(GX_TRUE);
    GXSetCullMode(GX_CULL_BACK);
    GXSetClipMode(GX_CLIP_ENABLE);
    GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, sOceanSphereFogFace);
}

void OceanSphere::loadMaterialBack() const {
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
    GXLoadPosMtxImm(MR::getCameraViewMtx(), 0);
    GXSetCurrentMtx(0);
    GXSetNumChans(0);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(1);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_NRM, GX_TEXMTX0, GX_FALSE, GX_PTIDENTITY);

    TPos3f pos;
    pos.set(MR::getCameraViewMtx());
    pos.zeroTrans();
    pos.scale(0.779175f);
    GXLoadTexMtxImm(pos.toMtxPtr(), GX_TEXMTX0, GX_MTX2x4);

    mWaterEnvTex->load(GX_TEXMAP0);
    GXSetNumIndStages(0);
    GXSetNumTevStages(1);
    GXSetTevColor(GX_TEVREG0, sOceanSphereTevReg0Env);
    GXSetTevColor(GX_TEVREG1, sOceanSphereTevReg1Env);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_C0, GX_CC_C1, GX_CC_TEXC, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_KONST, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GXSetZCompLoc(GX_TRUE);
    GXSetCullMode(GX_CULL_FRONT);
    GXSetClipMode(GX_CLIP_ENABLE);
    GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, sOceanSphereFogBack);
}
#endif

void OceanSphere::drawSphere(bool useEnvMap, bool useGD) const {
#ifdef __3DS__
    // The strips go to the GPU as one triangle list with the material draw() chose
    // (sOceanSphereState). With sOceanSphereReuse the strips built last are drawn again.
    if (sOceanSphereReuse) {
        drawOceanSphereStrips();
        return;
    }
    u32 used = 0;
    u32 strips = 0;
    sOceanSphereIndexCount = 0;
    auto drawStrip = [&](s32 count, auto getPoint) {
        if (strips == kOceanSphereStrips || used + count > kOceanSphereVertices) {
            return;
        }
        HostGfxVertex* vertices = &sOceanSphereVertices[used];
        for (s32 i = 0; i < count; i++) {
            const OceanSpherePoint* point = getPoint(i);
            f32 s = useEnvMap ? (point->mNormal.x * 0.5f + 0.5f) * 0.779175f : mTexOffs0X + point->mTexCoord.x;
            f32 t = useEnvMap ? (point->mNormal.y * 0.5f + 0.5f) * 0.779175f : mTexOffs0Y + point->mTexCoord.y;
            vertices[i] = WaterRender3DS::makeVertex(point->mPos, s, t,
                                                     useEnvMap ? 0.0f : mTexOffs1X + point->mTexCoord.x,
                                                     useEnvMap ? 0.0f : mTexOffs1Y + point->mTexCoord.y);
        }
        addOceanSphereStrip(used, count);
        strips++;
        used += count;
    };

    for (s32 i = 0; i < mPointCount - 1; i++) {
        s32 count = ((mPointCount - 1 - i) * 2) + 1;
        drawStrip(count, [&](s32 index) -> const OceanSpherePoint* {
            if (index == 0) return getPlanePointLeftUpper(i, i);
            s32 col = i + (index + 1) / 2;
            return (index & 1) ? getPlanePointLeftUpper(i + 1, col) : getPlanePointLeftUpper(i, col);
        });
        drawStrip(count, [&](s32 index) -> const OceanSpherePoint* {
            if (index == count - 1) return getPlanePointLeftUpper(i, i);
            s32 col = mPointCount - 1 - index / 2;
            return (index & 1) ? getPlanePointLeftUpper(col, i + 1) : getPlanePointLeftUpper(col, i);
        });
        drawStrip(count, [&](s32 index) -> const OceanSpherePoint* {
            if (index == 0) return getPlanePointRightUpper(i, i);
            s32 col = i + (index + 1) / 2;
            return (index & 1) ? getPlanePointRightUpper(i + 1, col) : getPlanePointRightUpper(i, col);
        });
        drawStrip(count, [&](s32 index) -> const OceanSpherePoint* {
            if (index == count - 1) return getPlanePointRightUpper(i, i);
            s32 col = mPointCount - 1 - index / 2;
            return (index & 1) ? getPlanePointRightUpper(col, i + 1) : getPlanePointRightUpper(col, i);
        });
        drawStrip(count, [&](s32 index) -> const OceanSpherePoint* {
            if (index == 0) return getPlanePointLeftLower(i, i);
            s32 col = i + (index + 1) / 2;
            return (index & 1) ? getPlanePointLeftLower(i + 1, col) : getPlanePointLeftLower(i, col);
        });
        drawStrip(count, [&](s32 index) -> const OceanSpherePoint* {
            if (index == count - 1) return getPlanePointLeftLower(i, i);
            s32 col = mPointCount - 1 - index / 2;
            return (index & 1) ? getPlanePointLeftLower(col, i + 1) : getPlanePointLeftLower(col, i);
        });
        drawStrip(count, [&](s32 index) -> const OceanSpherePoint* {
            if (index == 0) return getPlanePointRightLower(i, i);
            s32 col = i + (index + 1) / 2;
            return (index & 1) ? getPlanePointRightLower(i + 1, col) : getPlanePointRightLower(i, col);
        });
        drawStrip(count, [&](s32 index) -> const OceanSpherePoint* {
            if (index == count - 1) return getPlanePointRightLower(i, i);
            s32 col = mPointCount - 1 - index / 2;
            return (index & 1) ? getPlanePointRightLower(col, i + 1) : getPlanePointRightLower(col, i);
        });
    }
    sOceanSphereVertexCount = used;
    drawOceanSphereStrips(false);
#else
    const OceanSpherePoint* pPoint;

    for (s32 i = 0; i < mPointCount - 1; i++) {
        ::beginDrawPolygon(i, mPointCount, useGD);
        sendVertex(getPlanePointLeftUpper(i, i), useEnvMap, useGD);

        for (s32 j = i + 1; j < mPointCount; j++) {
            pPoint = getPlanePointLeftUpper(i, j);
            sendVertex(getPlanePointLeftUpper(i + 1, j), useEnvMap, useGD);
            sendVertex(pPoint, useEnvMap, useGD);
        }

        GXEnd();

        ::beginDrawPolygon(i, mPointCount, useGD);

        for (s32 j = 0; j < mPointCount - i - 1; j++) {
            pPoint = getPlanePointLeftUpper(mPointCount - 1 - j, i + 1);
            sendVertex(getPlanePointLeftUpper(mPointCount - 1 - j, i), useEnvMap, useGD);
            sendVertex(pPoint, useEnvMap, useGD);
        }

        sendVertex(getPlanePointLeftUpper(i, i), useEnvMap, useGD);
        GXEnd();

        ::beginDrawPolygon(i, mPointCount, useGD);
        sendVertex(getPlanePointRightUpper(i, i), useEnvMap, useGD);

        for (s32 j = i + 1; j < mPointCount; j++) {
            pPoint = getPlanePointRightUpper(i, j);
            sendVertex(getPlanePointRightUpper(i + 1, j), useEnvMap, useGD);
            sendVertex(pPoint, useEnvMap, useGD);
        }

        GXEnd();

        ::beginDrawPolygon(i, mPointCount, useGD);

        for (s32 j = 0; j < mPointCount - i - 1; j++) {
            pPoint = getPlanePointRightUpper(mPointCount - 1 - j, i + 1);
            sendVertex(getPlanePointRightUpper(mPointCount - 1 - j, i), useEnvMap, useGD);
            sendVertex(pPoint, useEnvMap, useGD);
        }

        sendVertex(getPlanePointRightUpper(i, i), useEnvMap, useGD);
        GXEnd();

        ::beginDrawPolygon(i, mPointCount, useGD);
        sendVertex(getPlanePointLeftLower(i, i), useEnvMap, useGD);

        for (s32 j = i + 1; j < mPointCount; j++) {
            pPoint = getPlanePointLeftLower(i, j);
            sendVertex(getPlanePointLeftLower(i + 1, j), useEnvMap, useGD);
            sendVertex(pPoint, useEnvMap, useGD);
        }

        GXEnd();

        ::beginDrawPolygon(i, mPointCount, useGD);

        for (s32 j = 0; j < mPointCount - i - 1; j++) {
            pPoint = getPlanePointLeftLower(mPointCount - 1 - j, i + 1);
            sendVertex(getPlanePointLeftLower(mPointCount - 1 - j, i), useEnvMap, useGD);
            sendVertex(pPoint, useEnvMap, useGD);
        }

        sendVertex(getPlanePointLeftLower(i, i), useEnvMap, useGD);
        GXEnd();

        ::beginDrawPolygon(i, mPointCount, useGD);
        sendVertex(getPlanePointRightLower(i, i), useEnvMap, useGD);

        for (s32 j = i + 1; j < mPointCount; j++) {
            pPoint = getPlanePointRightLower(i, j);
            sendVertex(getPlanePointRightLower(i + 1, j), useEnvMap, useGD);
            sendVertex(pPoint, useEnvMap, useGD);
        }

        GXEnd();

        ::beginDrawPolygon(i, mPointCount, useGD);

        for (s32 j = 0; j < mPointCount - i - 1; j++) {
            pPoint = getPlanePointRightLower(mPointCount - 1 - j, i + 1);
            sendVertex(getPlanePointRightLower(mPointCount - 1 - j, i), useEnvMap, useGD);
            sendVertex(pPoint, useEnvMap, useGD);
        }

        sendVertex(getPlanePointRightLower(i, i), useEnvMap, useGD);
        GXEnd();
    }
#endif
}

#ifndef __3DS__
void OceanSphere::sendVertex(const OceanSpherePoint* pPoint, bool useEnvMap, bool useGD) const {
    if (useGD) {
        f32 posX, posY, posZ;
        posX = pPoint->mPos.x;
        posZ = pPoint->mPos.z;
        posY = pPoint->mPos.y;
        GDWrite_f32(posX);
        GDWrite_f32(posY);
        GDWrite_f32(posZ);

        if (useEnvMap) {
            f32 normalX, normalY, normalZ;
            normalX = pPoint->mNormal.x;
            normalZ = pPoint->mNormal.z;
            normalY = pPoint->mNormal.y;
            GDWrite_f32(normalX);
            GDWrite_f32(normalY);
            GDWrite_f32(normalZ);
        } else {
            f32 u0 = pPoint->mTexCoord.x;
            f32 v0 = pPoint->mTexCoord.y;
            f32 texY0 = mTexOffs0Y + v0;
            f32 texX0 = mTexOffs0X + u0;
            GDTexCoord2f32(texX0, texY0);

            f32 u1 = pPoint->mTexCoord.x;
            f32 v1 = pPoint->mTexCoord.y;
            f32 texY1 = mTexOffs1Y + v1;
            f32 texX1 = mTexOffs1X + u1;
            GDTexCoord2f32(texX1, texY1);
        }
    } else {
        GXPosition3f32(pPoint->mPos.x, pPoint->mPos.y, pPoint->mPos.z);

        if (useEnvMap) {
            GXNormal3f32(pPoint->mNormal.x, pPoint->mNormal.y, pPoint->mNormal.z);
        } else {
            f32 v0 = pPoint->mTexCoord.y;
            f32 u0 = pPoint->mTexCoord.x;
            GXTexCoord2f32(mTexOffs0X + u0, mTexOffs0Y + v0);

            f32 v1 = pPoint->mTexCoord.y;
            f32 u1 = pPoint->mTexCoord.x;
            GXTexCoord2f32(mTexOffs1X + u1, mTexOffs1Y + v1);
        }
    }
}
#endif
