#include "Game/Player/MarioShadow.hpp"
#include "Game/Map/HitInfo.hpp"
#include "Game/Player/J3DModelX.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DirectDraw.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/MapUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/SchedulerUtil.hpp"
#include <JSystem/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3DGraphBase/J3DSys.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <JSystem/JUtility/JUTVideo.hpp>
#include <cstring>
#include <revolution/gd.h>
#ifdef __3DS__
#include "GXDelta3DS.hpp"
#include "GXToCitro3D.hpp"
#include "JUTTexture3DS.hpp"
#include "host.h"
#endif

void MarioShadow_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
    (void)0.5f;
    (void)-1.0f;
    (void)2.0f;
}

namespace {
    f32 cCheckOffset0 = 5.0f;
    f32 cDrawOffset0 = -5.0f;
};  // namespace


#ifdef __3DS__
namespace {
    // drawAndCaptureTex: the Wii draws the model seen from the light into the frame buffer's top
    // right corner with the alpha alone (J3DModelX mode lists 1 and 14), masks it with a circle and
    // copies the alpha to the I8 texture _300, which the shadow polygons and the ShadowProjDummy
    // materials sample. A 3DS copy keeps colour and alpha: the shadow is drawn in both alike (as an
    // I8 texture reads), with the corner saved before and put back after.
    RvlGXDelta sCaptureDeltas[2];  // 0x40 (the enlarged first pass), 0xFF
    bool sCaptureDeltasReady;
    void* sCorner;
    f32 sCornerMap[4];

    void buildCaptureDeltas() {
        for (u32 i = 0; i < 2; i++) {
            const u8 value = i == 0 ? 0x40 : 0xFF;
            RvlGXDelta& d = sCaptureDeltas[i];
            d.clear();
            d.setTevRegister(GX_TEVREG0, value, value, value, value);
            d.setTevColorIn(GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1);
            d.setTevAlphaIn(GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1);
            d.setBlend(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR, true, true);
            d.setDstAlpha(false, 0);
        }
        sCaptureDeltasReady = true;
    }

    // A rectangle of the Wii frame buffer (640x456) filled with a colour (0xRRGGBBAA) or a
    // capture (texture with its coordinate mapping), replacing colour and alpha.
    void drawFrameRect(f32 x0, f32 y0, f32 x1, f32 y1, u32 color, void* pTexture, const f32* pMap) {
        const f32 corners[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
        HostGfxVertex vertices[6] = {};
        for (u32 i = 0; i < 6; i++) {
            const f32 s = corners[i][0], t = corners[i][1];
            HostGfxVertex& v = vertices[i];
            v.x = (x0 + s * (x1 - x0)) / 320.0f - 1.0f;
            v.y = 1.0f - (y0 + t * (y1 - y0)) / 228.0f;
            v.z = 0.0f;
            v.w = 1.0f;
            v.rgba[0] = color >> 24;
            v.rgba[1] = color >> 16;
            v.rgba[2] = color >> 8;
            v.rgba[3] = color;
            if (pTexture != nullptr) {
                v.s0 = pMap[0] + pMap[1] * t;
                v.t0 = pMap[2] + pMap[3] * s;
            }
        }
        HostGfxState state = {};
        state.textures[0] = pTexture;
        state.stageCount = 1;
        state.stages[0].colorSrc[0] = state.stages[0].alphaSrc[0] = pTexture != nullptr ? 0x03 : 0x00;
        state.alphaTestFunc = GXToCitro3D::TEST_ALWAYS;
        state.depthFunc = GXToCitro3D::TEST_ALWAYS;
        state.writeMask = GXToCitro3D::WRITE_RGB | GXToCitro3D::WRITE_ALPHA;
        state.blendSrc = GXToCitro3D::FACTOR_ONE;
        state.blendDst = GXToCitro3D::FACTOR_ZERO;
        state.scissor[2] = 640;
        state.scissor[3] = 456;
        hostGfxDraw(vertices, 6, nullptr, 6, HOST_GFX_TRIANGLES, &state);
    }

    // sendVtx / sendZsortedVtx: the shadow polygons with the shadow texture, in black with the
    // texture's alpha, with DirectDraw's fragment state.
    void drawShadowPolygons(const CollisionShadow& rShadow, bool sorted, bool offset) {
        void* texture = rvl3DSGetJUTTexture(rShadow._300);
        if (texture == nullptr || rShadow._54 == 0) {
            return;
        }
        HostGfxState state = {};
        state.textures[0] = texture;
        state.stageCount = 1;
        state.stages[0].colorSrc[0] = 0x0E;  // constant black (GX_CC_ZERO)
        state.stages[0].alphaSrc[0] = 0x03;  // the texture's alpha
        state.stages[0].constant = 0;
        TVec3f shift(-rShadow._24 * 5.0f);
        const u32 kBatch = 63 * 3;
        TVec3f positions[kBatch];
        TVec2f texcoords[kBatch][3];
        u32 count = 0;
        u32 index = 0;
        for (u32 i = 0; i < rShadow._54; i++) {
            // GX_QUADS (4 vertices) is two triangles, GX_TRIANGLES one.
            static const u8 kQuad[6] = {0, 1, 2, 0, 2, 3};
            static const u8 kTriangle[3] = {0, 1, 2};
            const u8* order = rShadow._5A == 4 ? kQuad : kTriangle;
            const u32 corners = rShadow._5A == 4 ? 6 : 3;
            for (u32 k = 0; k < corners; k++) {
                const u32 j = order[k];
                TVec3f point = sorted ? rShadow.getDrawPos(i, j) : rShadow.getDrawPos(index + j);
                const TVec2f& tex = sorted ? rShadow.getDrawTx(i, j) : rShadow._64[rShadow._68[index + j]];
                if (offset) {
                    point += shift;
                }
                positions[count] = point;
                f32 u, v;
                rvl3DSMapJUTTextureCoord(rShadow._300, tex.x, tex.y, u, v);
                texcoords[count][0].set(u, v);
                count++;
            }
            index += rShadow._5A;
            if (count + 6 > kBatch || i + 1 == rShadow._54) {
                TDDraw::drawStateTriangles(positions, texcoords, nullptr, state, count);
                count = 0;
            }
        }
    }
}  // namespace
#endif

CollisionShadow::~CollisionShadow() {
}

void CollisionShadow::setMode(u32 mode) {
    if (_C == mode) {
        return;
    }

    switch (mode) {
    case 0:
        _30C = ::cCheckOffset0;
        _310 = ::cDrawOffset0;
        break;
    case 1:
        _30C = 50.0f;
        _310 = -150.0f;
        break;
    case 2:
        _30C = 18.0f;
        _310 = 0.0f;
        break;
    }

    _C = mode;
}

CollisionShadow::CollisionShadow(f32 radius, f32 length) : NameObj("投影シャドウ") {
    _2F0.zero();
    _2FC = 0;
    _2FE = 0;
    _300 = nullptr;
    _305 = false;
    _30C = 0.0f;
    _310 = 0.0f;
    _314.zero();
    _320.zero();
    _32C = nullptr;
    _330 = nullptr;
    _334 = nullptr;
    _338 = 0;
    _33A = 0;
    _33C = 0;
    _340 = nullptr;
    _344 = nullptr;
    _348 = 0;
    _308 = 0;
    _E = 0;
    _14 = radius;
    _18 = 0.0f;
    _1C = 0.0f;
    _24.zero();
    _30.zero();
    _3C.zero();
    _48.zero();
    _10 = radius;
    _20 = length;
    _54 = 0;
    _C = 255;
    setMode(0);
    initCaptureTex();
    _58 = 128;
    _5A = 3;
    _60 = new (32) TVec3f[_58 * _5A];
    _64 = new (32) TVec2f[_58 * _5A];
    _68 = new u16[_58 * _5A];
    _5C = new u8[_58];
    _6C = 0;
    _32C = new Triangle[_58];
    _330 = new Triangle[2];
    _334 = new Triangle[2];
    _338 = 0;
    _33A = 0;
    _33C = 0;
    _6E = 0;
    _306 = 0;
    _304 = 0;
    _307 = 1;

    for (u32 i = 0; i < 128; i++) {
        _70[i] = 0;
    }

    for (u32 i = 0; i < 128; i++) {
        _F0[i] = 0;
    }

    createDL();
}

void CollisionShadow::create(const TVec3f& rPosition, const TVec3f& rDirection, const TVec3f& rUp) {
    TVec3f corners[8];
    TVec3f blended;
    TVec3f vertical(rUp);
    bool success = MR::vecBlendSphere(_24, rDirection, &blended, 0.1f);
    if (MR::isNearZero(_24) || !success || _305) {
        blended = rDirection;
    }

    _305 = false;
    if (!MR::isNearZero(blended)) {
        _24 = blended;
        MR::vecKillElement(vertical, blended, &vertical);
        MR::normalizeOrZero(&vertical);
        if (!MR::isNearZero(vertical)) {
            _30 = vertical;
        }
    }

    _2F0 = rPosition;
    if (_C == 3) {
        return;
    }

    _E++;
    TVec3f side;
    side.cross(vertical, blended);
    if (MR::isNearZero(side)) {
        return;
    }

    MR::normalize(&side);
    for (u32 i = 0; i < 8; i++) {
        corners[i] = rPosition;
        if (i & 1) {
            corners[i] += vertical * _10;
        } else {
            corners[i] -= vertical * _10;
        }

        if (i & 2) {
            corners[i] += side * _10;
        } else {
            corners[i] -= side * _10;
        }

        if (i & 4) {
            corners[i] -= blended * _30C;
        } else {
            corners[i] += blended * _20;
        }
    }

    TVec3f minimum;
    TVec3f maximum;
    MR::createBoundingBox(corners, 8, &minimum, &maximum);
    _314 = minimum;
    _320 = maximum;
    if (MR::isInitializeStatePlacementSomething()) {
        return;
    }

    u32 count = MR::createAreaPolygonListArray(_32C, _58, corners, 8);
    for (u32 i = 0; i < _338; i++) {
        _32C[count] = _330[i];
        count++;
    }

    _338 = 0;
    for (u32 i = 0; i < _33A; i++) {
        _32C[count] = _334[i];
        count++;
    }

    _33A = 0;
    _33C = count;
    _54 = 0;
    if (!count) {
        _10 = _14;
        return;
    }

    u32 polygonCount = 0;
    _6E = 0;
    for (u32 i = 0; i < count; i++) {
        TVec3f normal(*_32C[i].getNormal(0));
        f32 facing = normal.dot(_24);
        if (facing > 0.0f) {
            continue;
        }

        if (normal.dot(MR::getCamZdir()) > 0.707f) {
            continue;
        }

        if (MR::isNearZero(facing, 0.15f)) {
            _70[_6E] = i;
            _6E++;
        } else {
            const char* code = MR::getFloorCodeString(&_32C[i]);
            if (code == nullptr || (strcmp(code, "PullBack") != 0 && strcmp(code, "Glass") != 0)) {
                _5C[polygonCount] = i;
                polygonCount++;
            }
        }
    }

    _54 = polygonCount;
    f32 tolerance = 2.0f + _310;
    _6C = 0;
    s32 index = 0;
    for (u32 i = 0; i < _54; i++) {
        for (u32 j = 0; j < _5A; j++) {
            const TVec3f& point = *_32C[_5C[i]].getPos(j);
            u32 k;
            for (k = 0; k < _6C; k++) {
                if (MR::abs(_60[k].x - point.x) < tolerance && MR::abs(_60[k].y - point.y) < tolerance && MR::abs(_60[k].z - point.z) < tolerance) {
                    break;
                }
            }

            if (k == _6C) {
                const TVec3f& normal = *_32C[_5C[i]].getNormal(0);
                _60[_6C] = point + normal * _310;
                _6C++;
            }

            _68[index] = k;
            index++;
        }
    }

    // TODO is this legit?
    register f32 texScale = 0.0033333334f;

    for (u32 i = 0; i < _6C; i++) {
        TVec3f relative(_60[i] - rPosition);
        f32 s = relative.dot(side);
        f32 t = relative.dot(_30);

        _64[i].x = 0.5f + s * texScale;
        _64[i].y = 0.5f + t * texScale;
    }

    if (_33C > 40) {
        _10 -= 10.0f;
    } else if (_33C > 10) {
        _10 -= 1.0f;
    } else if (_33C > 20) {
        _10 -= 4.0f;
    } else {
        _10 += 1.0f;
    }

    MR::clampBoth(&_10, 1.0f, _14);
}

void CollisionShadow::draw1() const {
#ifdef __3DS__
    // The polygons below the floor's surface (GX_GEQUAL), darkened by the shadow texture's alpha
    // where it is above 1 (setup(1, 1, 0): GX_BL_SRCALPHA, GX_BL_INVSRCALPHA).
    if (_308 & 8) {
        return;
    }

    TDDraw::setupDirect(1, 1, 0);
    TDDraw::setDirectDepthMode(true, GXToCitro3D::depthTest(GX_GEQUAL), false);
    TDDraw::setDirectAlphaTest(GXToCitro3D::alphaTest(GX_GREATER), 1);
    drawShadowPolygons(*this, false, false);
    TDDraw::closeDirect();
#else
    if (_308 & 8) {
        return;
    }

    TDDraw::setup(1, 1, 0);
    GXSetZMode(GX_TRUE, GX_GEQUAL, GX_FALSE);
    _300->load(GX_TEXMAP0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetAlphaCompare(GX_GREATER, 1, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetDstAlpha(GX_TRUE, 64);
    sendVtx();
    GXSetDstAlpha(GX_FALSE, 0);
#endif
}

void CollisionShadow::sendVtx() const {
#ifndef __3DS__  // 3DS: only the Wii draw() uses it
    if (!_54) {
        return;
    }

    u16 vertexCount = _54 * _5A;
    if (_5A == 4) {
        GXBegin(GX_QUADS, GX_VTXFMT0, vertexCount);
    } else {
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, vertexCount);
    }

    const TVec2f* pTex;
    u32 index = 0;
    for (u32 i = 0; i < _54; i++) {
        for (u32 j = 0; j < _5A; j++) {
            const TVec3f& point = getDrawPos(index);
            pTex = &_64[_68[index]];
            index++;
            GXPosition3f32(point.x, point.y, point.z);
            GXTexCoord2f32(pTex->x, pTex->y);
        }
    }
#endif
}

void CollisionShadow::sendZsortedVtx(bool offset) const {
#ifndef __3DS__  // 3DS: only the Wii draw() uses it
    u16 vertexCount = _54 * _5A;
    if (_5A == 4) {
        GXBegin(GX_QUADS, GX_VTXFMT0, vertexCount);
    } else {
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, vertexCount);
    }

    TVec3f shift(-_24 * 5.0f);
    for (u32 i = 0; i < _54; i++) {
        for (u32 j = 0; j < _5A; j++) {
            const TVec3f& point = getDrawPos(i, j);
            const TVec2f& tex = getDrawTx(i, j);
            if (offset) {
                GXPosition3f32(point.x + shift.x, point.y + shift.y, point.z + shift.z);
            } else {
                GXPosition3f32(point.x, point.y, point.z);
            }

            GXTexCoord2f32(tex.x, tex.y);
        }
    }
#endif
}

void CollisionShadow::initCaptureTex() {
    _2FC = 160;
    _2FE = 160;
    {
        MR::CurrentHeapRestorer restorer(MR::getSceneHeapGDDR3());
        _300 = new JUTTexture(_2FC, _2FE, GX_TF_I8);
        MR::zeroMemory(_300->mImage, _2FC * _2FE);
        MR::setMarioShadowTex(_300);
    }

    _300->mWrapS = GX_CLAMP;
    _300->mWrapT = GX_CLAMP;
    _300->mMinType = GX_LINEAR;
    _300->mMagType = GX_LINEAR;
    _300->init();
}

void CollisionShadow::setViewMtx(const TVec3f& rDirection) {
    TVec3f normal = rDirection;
    MR::normalize(&normal);
    TVec3f eye = _2F0 - normal * 10000.0f;

    TPos3f matrix;
    matrix.identity();
    matrix.setPositionFromLookAt(eye, _30, _2F0);
    PSMTXCopy(matrix, j3dSys.mViewMtx);
    TDDraw::setViewMtx(matrix);
    MR::setMarioShadowVec(rDirection);
}

void CollisionShadow::setUpdateFlag() {
    if (!MR::isNearZero(_24)) {
        _304 = 1;
    }
}

void CollisionShadow::calcView(J3DModelX* pModel, u32 view, J3DModelX* pReference) {
    if (!_304) {
        return;
    }

    const TVec3f& rDirection = _24;
    _304 = 0;
    _306 = 1;
    if (_C == 3) {
        Mtx matrix;
        PSMTXCopy(pReference->getBaseTRMtx(), matrix);
        if (pReference != nullptr) {
            pModel->viewCalcRefPos(view, pReference, _48, _24);
        }
    } else {
        setViewMtx(rDirection);
        if (pReference != nullptr) {
            pModel->viewCalcRef(view, pReference);
        } else {
            pModel->viewCalc3(view, nullptr);
        }
    }

    if (_C == 2) {
        doSortPolygons();
    }
}

void CollisionShadow::drawAndCaptureTex(J3DModelX* pModel, const TVec3f& rPosition) {
    if (!_306) {
        return;
    }

    if (_C == 3) {
        _340 = pModel;
        return;
    }

#ifdef __3DS__
    if (!sCaptureDeltasReady) {
        buildCaptureDeltas();
    }
    const f32 left = 608.0f - _2FC;
    sCorner = hostGfxCopyToTexture(sCorner, left, 0.0f, 608.0f, _2FE, sCornerMap);
    if (sCorner == nullptr) {
        return;  // the frame is dropped
    }
    if (!(_308 & 2)) {
        setViewMtx(_24);
        drawFrameRect(left, 0.0f, 608.0f, _2FE, 0, nullptr, nullptr);
        Mtx44 projection;
        C_MTXOrtho(projection, -150.0f, 150.0f, 150.0f, -150.0f, 10.0f, 100000.0f);
        const u16 scissor[4] = {static_cast< u16 >(left + 1), 1, static_cast< u16 >(_2FC - 2), static_cast< u16 >(_2FE - 2)};
        if (_307) {
            const f32 viewport[4] = {608.0f - (_2FC + 32), -32.0f, 64.0f + _2FC, 64.0f + _2FE};
            rvlNativeSetViewport(projection, viewport, scissor);
            pModel->setDrawView(2);
            pModel->mFlags.clear();
            pModel->mFlags._11 = true;
            pModel->mFlags._1E = true;
            pModel->setDynamicDL(reinterpret_cast< u8* >(&sCaptureDeltas[0]), sizeof(RvlGXDelta));
            pModel->directDraw(nullptr);
        }

        const f32 viewport[4] = {left, 0.0f, static_cast< f32 >(_2FC), static_cast< f32 >(_2FE)};
        rvlNativeSetViewport(projection, viewport, scissor);
        pModel->setDrawView(2);
        pModel->mFlags.clear();
        pModel->mFlags._1E = true;
        pModel->setDynamicDL(reinterpret_cast< u8* >(&sCaptureDeltas[1]), sizeof(RvlGXDelta));
        pModel->directDraw(nullptr);
        pModel->mFlags._1E = false;
        pModel->setDynamicDL(nullptr, 0);
        rvlNativeSetViewport(nullptr, nullptr, nullptr);
    }

    if (!(_308 & 4)) {
        // The circle multiplies the alpha (GX_BL_DSTALPHA, GX_BL_ZERO) by its vertex colour, 0xC0
        // at the centre down to 0 at the edge (setup(0, ...) passes the vertex colour): the
        // silhouette fades into a soft round shadow. Colour too here.
        TDDraw::setupDirect(0, 1, 2);
        TDDraw::setDirectPixelWriteMask(GXToCitro3D::WRITE_RGB | GXToCitro3D::WRITE_ALPHA);
        TDDraw::setDirectBlendState(GXToCitro3D::EQUATION_ADD, GXToCitro3D::sourceFactor(GX_BL_DSTALPHA), GXToCitro3D::FACTOR_ZERO);
        f32 y = _2FE / 2;
        f32 x = 608 - _2FC / 2;
        TVec3f center(x, y, 0.0f);
        TDDraw::fix2Dpos(&center);
        TDDraw::drawFillCircle(center, _2FC / 2, 0xC0C0C0C0, 0, 16);
        TDDraw::closeDirect();
        rvl3DSCaptureJUTTextureRegion(_300, left, 0.0f, 608.0f, _2FE);
    }

    drawFrameRect(left, 0.0f, 608.0f, _2FE, 0xFFFFFFFF, sCorner, sCornerMap);
    J3DShape::resetVcdVatCache();
    MR::setDefaultViewportAndScissor();
    TDDraw::cameraInit3D();
#else
    if (!(_308 & 2)) {
        setViewMtx(_24);
        TDDraw::setup(0, 0, 1);
        Mtx44 projection;
        C_MTXOrtho(projection, -150.0f, 150.0f, 150.0f, -150.0f, 10.0f, 100000.0f);
#ifndef __3DS__
        GXSetProjection(projection, GX_ORTHOGRAPHIC);
#endif
        if (_307) {
#ifndef __3DS__
            GXSetViewport(608 - (_2FC + 32), -32.0f, 64.0f + _2FC, 64.0f + _2FE, 0.0f, 1.0f);
#endif
            s32 left = 608 - _2FC;
#ifndef __3DS__
            GXSetScissor(left + 1, 1, _2FC - 2, _2FE - 2);
#endif
            pModel->setDrawView(2);
            pModel->mFlags.clear();
            pModel->mFlags._11 = true;
            pModel->mFlags._1E = true;
            pModel->directDraw(nullptr);
        }

#ifndef __3DS__
        GXSetViewport(608 - _2FC, 0.0f, _2FC, _2FE, 0.0f, 1.0f);
#endif
        s32 left = 608 - _2FC;
#ifndef __3DS__
        GXSetScissor(left + 1, 1, _2FC - 2, _2FE - 2);
#endif
        pModel->setDrawView(2);
        pModel->mFlags.clear();
        pModel->mFlags._1E = true;
        pModel->directDraw(nullptr);
        pModel->mFlags._1E = false;
    }

    if (!(_308 & 4)) {
        TDDraw::setup(0, 1, 2);
#ifndef __3DS__
        GXSetColorUpdate(GX_FALSE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_ZERO, GX_LO_NOOP);
        GXSetDstAlpha(GX_FALSE, 0);
#endif
        f32 y = _2FE / 2;
        f32 x = 608 - _2FC / 2;
        TVec3f center(x, y, 0.0f);
        TDDraw::fix2Dpos(&center);
        TDDraw::drawFillCircle(center, _2FC / 2, 0xFFFFFFC0, 0, 16);
        const ResTIMG* image = _300->getTexInfo();
        void* destination = const_cast< u8* >(reinterpret_cast< const u8* >(image) + image->mImageDataOffset);
#ifndef __3DS__
        GXRenderModeObj* mode = JUTVideo::getManager()->getRenderMode();
        GXSetCopyFilter(GX_FALSE, mode->sample_pattern, GX_FALSE, mode->vfilter);
#endif
        JUTTexture::captureDolTexture(destination, _2FC, _2FE, 608 - _2FC, 0, false, GX_CTF_A8);
#ifndef __3DS__
        GXSetCopyFilter(GX_FALSE, mode->sample_pattern, GX_TRUE, mode->vfilter);
#endif
#ifndef __3DS__
        GXInvalidateTexAll();
#endif
    }

#ifndef __3DS__
    GXSetAlphaUpdate(GX_FALSE);
    GXSetColorUpdate(GX_TRUE);
#endif
    J3DShape::resetVcdVatCache();
    MR::setDefaultViewportAndScissor();
    TDDraw::cameraInit3D();
#ifndef __3DS__
    GXSetDstAlpha(GX_FALSE, 0);
#endif
#endif
}

void CollisionShadow::clearAlphaBuffer() {
    TVec2f position;
    TVec2f size(static_cast< f32 >(_2FC), static_cast< f32 >(_2FE));
    position.x = 608.0f - _2FC;
    position.y = 0.0f;
    MR::clearAlphaBuffer(0, position, size);
}

void CollisionShadow::drawVolumeBox(const TVec3f& rPosition, const TVec3f& rDepth) const {
    TVec3f up(_3C);
    TVec3f side;
    side.cross(_24, up);
    MR::normalizeOrZero(&side);
#ifdef __3DS__
    // The Wii adds 255 to the alpha with the box's front faces in front of the scene (GX_LEQUAL)
    // and takes it away with its back faces: the scene inside the box keeps 255. In the stencil.
    bool cullAll;
    TDDraw::setDirectPixelWriteMask(0);
    TDDraw::setDirectDepthMode(true, GXToCitro3D::depthTest(GX_LEQUAL), false);
    TDDraw::setDirectCullMode(GXToCitro3D::cullMode(GX_CULL_BACK, cullAll));
    TDDraw::setDirectStencil(true, GXToCitro3D::TEST_ALWAYS, 1, 0xFF, 0xFF, GXToCitro3D::STENCIL_KEEP, GXToCitro3D::STENCIL_KEEP,
                             GXToCitro3D::STENCIL_REPLACE);
    TDDraw::drawFillBox3D(rPosition, rDepth * 2.0f, side * _1C, up * _18, 0xFFFFFF01);
    TDDraw::setDirectCullMode(GXToCitro3D::cullMode(GX_CULL_FRONT, cullAll));
    TDDraw::setDirectStencil(true, GXToCitro3D::TEST_ALWAYS, 0, 0xFF, 0xFF, GXToCitro3D::STENCIL_KEEP, GXToCitro3D::STENCIL_KEEP,
                             GXToCitro3D::STENCIL_ZERO);
    TDDraw::drawFillBox3D(rPosition, rDepth * 2.0f, side * _1C, up * _18, 0xFFFFFF01);
    TDDraw::setDirectStencil(false, 0, 0, 0, 0, 0, 0, 0);
    TDDraw::setDirectCullMode(GXToCitro3D::CULL_NONE);
    TDDraw::setDirectPixelWriteMask(GXToCitro3D::WRITE_RGB);
    TDDraw::setDirectDepthMode(false, GXToCitro3D::TEST_ALWAYS, false);
#else
    TDDraw::setup(0, 1, 0);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetColorUpdate(GX_FALSE);
    GXSetDstAlpha(GX_FALSE, 0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXColorS10 color = {255, 255, 255, 255};
    GXSetTevColorS10(GX_TEVREG0, color);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_NOOP);
    GXSetCullMode(GX_CULL_BACK);
    TDDraw::drawFillBox3D(rPosition, rDepth * 2.0f, side * _1C, up * _18, 0xFFFFFF01);
    GXSetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE, GX_BL_ONE, GX_LO_NOOP);
    GXSetCullMode(GX_CULL_FRONT);
    TDDraw::drawFillBox3D(rPosition, rDepth * 2.0f, side * _1C, up * _18, 0xFFFFFF01);
#endif
}

void CollisionShadow::draw() const {
#ifdef __3DS__
    switch (_C) {
    case 0:
        draw1();
        break;
    case 1:
        draw2();
        break;
    case 2:
        draw3();
        break;
    case 3:
        TDDraw::setupDirect(0, 0, 0);
        drawVolume();
        TDDraw::closeDirect();
        break;
    }
    return;
#else
    MR::loadViewMtx();
    MR::loadProjectionMtx();
    switch (_C) {
    case 0:
        draw1();
        break;
    case 1:
        draw2();
        break;
    case 2:
        draw3();
        break;
    case 3:
        drawVolume();
        break;
    }

    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_FALSE);
#endif
}

void CollisionShadow::draw2() const {
#ifdef __3DS__
    // The box marks the floor inside it, then the polygons blacken the marked pixels where the
    // shadow texture's alpha is above 1 (GX_BL_DSTALPHA, GX_BL_INVDSTALPHA with a 0/255 mask),
    // clearing the mark (destination alpha 0).
    if (_308 & 8) {
        return;
    }

    TVec3f position(_2F0 - _24 * _30C);
    TDDraw::setupDirect(1, 0, 0);
    drawVolumeBox(position, _24 * _20);
    TDDraw::setDirectAlphaTest(GXToCitro3D::alphaTest(GX_GREATER), 1);
    TDDraw::setDirectStencil(true, GXToCitro3D::TEST_NOTEQUAL, 0, 0xFF, 0xFF, GXToCitro3D::STENCIL_KEEP, GXToCitro3D::STENCIL_KEEP,
                             GXToCitro3D::STENCIL_ZERO);
    drawShadowPolygons(*this, false, false);
    TDDraw::closeDirect();
#else
    if (_308 & 8) {
        return;
    }

    TVec3f position(_2F0 - _24 * _30C);
    drawVolumeBox(position, _24 * _20);
    TDDraw::setup(1, 1, 0);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetColorUpdate(GX_TRUE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_INVDSTALPHA, GX_LO_NOOP);
    GXSetAlphaCompare(GX_GREATER, 1, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetDstAlpha(GX_TRUE, 0);
    _300->load(GX_TEXMAP0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    sendVtx();
    GXSetDstAlpha(GX_FALSE, 0);
#endif
}

void CollisionShadow::draw3() const {
#ifdef __3DS__
    // The Wii's sorted form refines draw2's per polygon in the destination alpha: each marked pixel
    // ends up with the polygon's texture alpha, then black is blended by it (GX_BL_DSTALPHA,
    // GX_BL_INVDSTALPHA). The 3DS blends by the texture's alpha directly (setup(1, 1, 0)), once per
    // pixel through the stencil mark, with the sorted, offset polygons.
    if (_308 & 8) {
        return;
    }

    TVec3f position(_2F0 - _24 * _30C);
    TDDraw::setupDirect(1, 1, 0);
    drawVolumeBox(position, _24 * _20);
    TDDraw::setDirectAlphaTest(GXToCitro3D::alphaTest(GX_GREATER), 1);
    TDDraw::setDirectStencil(true, GXToCitro3D::TEST_NOTEQUAL, 0, 0xFF, 0xFF, GXToCitro3D::STENCIL_KEEP, GXToCitro3D::STENCIL_KEEP,
                             GXToCitro3D::STENCIL_ZERO);
    drawShadowPolygons(*this, true, true);
    TDDraw::closeDirect();
#else
    if (_308 & 8) {
        return;
    }

    TVec3f position(_2F0 - _24 * _30C);
    drawVolumeBox(position, _24 * _20);
    TDDraw::setup(1, 1, 0);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetColorUpdate(GX_FALSE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_NOOP);
    GXSetAlphaCompare(GX_EQUAL, 0, GX_AOP_AND, GX_EQUAL, 0);
    GXSetDstAlpha(GX_TRUE, 0);
    _300->load(GX_TEXMAP0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    sendZsortedVtx(true);
    if (_6E) {
        GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, _6E * 3);
        for (u32 i = 0; i < _6E; i++) {
            for (u32 j = 0; j < 3; j++) {
                const TVec3f& point = *_32C[_70[i]].getPos(j);
                GXPosition3f32(point.x, point.y, point.z);
                GXTexCoord2f32(-1.0f, -1.0f);
            }
        }
    }

    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetColorUpdate(GX_TRUE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_INVDSTALPHA, GX_LO_NOOP);
    GXSetAlphaCompare(GX_NEQUAL, 0, GX_AOP_AND, GX_NEQUAL, 0);
    GXSetMisc(GX_MT_XF_FLUSH, 8);
    for (u32 i = 0; i < _54; i++) {
        GXSetColorUpdate(GX_FALSE);
        GXSetDstAlpha(GX_FALSE, 0);
        if (_5A == 4) {
            GXBegin(GX_QUADS, GX_VTXFMT0, _5A);
        } else {
            GXBegin(GX_TRIANGLES, GX_VTXFMT0, _5A);
        }

        for (u32 j = 0; j < _5A; j++) {
            const TVec3f& point = getDrawPos(i, j);
            const TVec2f& tex = getDrawTx(i, j);
            GXPosition3f32(point.x, point.y, point.z);
            GXTexCoord2f32(tex.x, tex.y);
        }

        GXSetColorUpdate(GX_TRUE);
        GXSetDstAlpha(GX_TRUE, 0);
        if (_5A == 4) {
            GXBegin(GX_QUADS, GX_VTXFMT0, _5A);
        } else {
            GXBegin(GX_TRIANGLES, GX_VTXFMT0, _5A);
        }

        for (u32 j = 0; j < _5A; j++) {
            const TVec3f& point = getDrawPos(i, j);
            const TVec2f& tex = getDrawTx(i, j);
            GXPosition3f32(point.x, point.y, point.z);
            GXTexCoord2f32(tex.x, tex.y);
        }
    }

    GXSetMisc(GX_MT_XF_FLUSH, 0);
    GXSetDstAlpha(GX_FALSE, 0);
#endif
}

void CollisionShadow::createDL() {
#ifdef __3DS__
    return;  // no display lists on 3DS (the volume draws natively)
#else
    u8 buffer[512] ATTRIBUTE_ALIGN(32);
    GDLObj displayList;
    MR::ProhibitSchedulerAndInterrupts guard(false);
    GDInitGDLObj(&displayList, buffer, sizeof(buffer));
    GDSetCurrent(&displayList);
    GDSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GDSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    GDSetTevDirect(GX_TEVSTAGE0);
    GDSetGenMode(0, 1, 1);
    GDSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
    GDSetTevAlphaCalcAndSwap(GX_TEVSTAGE0, GX_CA_A0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV,
                             GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXColor color = {0, 0, 0, 4};
    GDSetTevColor(GX_TEVREG0, color);
    GDPadCurr32();
    u32 size = GDGetGDLObjOffset(&displayList);
    _348 = size;
    size = (size + 31) & ~31;
    _344 = new (32) u8[size];
    MR::copyMemory(_344, buffer, size);
    DCStoreRange(_344, size);
#endif
}

void CollisionShadow::drawVolume() const {
    if (_308 & 8) {
        return;
    }

    _340->setDrawView(2);
    TVec3f position(_2F0 - _24 * _30C);
    drawVolumeBox(position, _24 * _20);
    _340->mFlags.clear();
    _340->mFlags._15 = true;
    _340->directDraw(nullptr);
#ifndef __3DS__
    GXSetDstAlpha(GX_FALSE, 0);
#endif
}

void CollisionShadow::doSortPolygons() {
    f32 distances[128];
    f32 inverseVertexCount = 0.33333334f;
    for (u32 i = 0; i < _54; i++) {
        TVec3f center(getDrawPos(0));
        center += getDrawPos(1);
        center += getDrawPos(2);
        center.scale(inverseVertexCount);
        TVec3f relative(center);
        relative.sub(_2F0);
        distances[i] = relative.dot(_24);
    }

    MR::sortSmall(_54, distances, _F0);
}

const TVec3f& CollisionShadow::getDrawPos(u32 index) const {
    return _60[_68[index]];
}

const TVec3f& CollisionShadow::getDrawPos(u32 polygon, u32 vertex) const {
    return _60[_68[vertex + _5A * _F0[polygon]]];
}

const TVec2f& CollisionShadow::getDrawTx(u32 polygon, u32 vertex) const {
    return _64[_68[vertex + _5A * _F0[polygon]]];
}
