#include "Game/MapObj/GCaptureRibbon.hpp"
#include "Game/LiveActor/ActorLightCtrl.hpp"
#include "Game/Util/DirectDraw.hpp"
#include "Game/Util/JMapInfo.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include <JSystem/JGeometry/TVec.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <revolution/gx/GXEnum.h>
#include <revolution/gx/GXGeometry.h>
#include <revolution/gx/GXPixel.h>
#include <revolution/gx/GXVert.h>
#include <revolution/types.h>

GCaptureRibbon::GCaptureRibbon(const char* pName) : LiveActor(pName), mTexture(), _990(), _994() {
}

void GCaptureRibbon::draw() const {
    if (_990 >= 2) {
#ifdef __3DS__
        TDDraw::setupDirect(1, 2, 0);
        TDDraw::setDirectDepthMode(true, 7, false);
        const f32 width = calcLineWidth();
        const TVec3f* axes[2] = {_690, _390};

        for (u32 ribbon = 0; ribbon < 2; ribbon++) {
            for (s32 firstSegment = 0; firstSegment < _990 - 1; firstSegment += 32) {
                const s32 segmentCount = (_990 - 1 - firstSegment) < 32 ? (_990 - 1 - firstSegment) : 32;
                TVec3f positions[192];
                TVec2f texcoords[192];
                u32 out = 0;

                for (s32 segment = firstSegment; segment < firstSegment + segmentCount; segment++) {
                    const f32 v0 = static_cast<f32>(segment) / (_990 - 1.0f);
                    const f32 v1 = static_cast<f32>(segment + 1) / (_990 - 1.0f);
                    const TVec3f side0 = axes[ribbon][segment] * width;
                    const TVec3f side1 = axes[ribbon][segment + 1] * width;
                    const TVec3f a = _90[segment] + side0;
                    const TVec3f b = _90[segment] - side0;
                    const TVec3f c = _90[segment + 1] + side1;
                    const TVec3f d = _90[segment + 1] - side1;

                    positions[out] = a;
                    texcoords[out++].set(0.0f, v0);
                    positions[out] = b;
                    texcoords[out++].set(1.0f, v0);
                    positions[out] = c;
                    texcoords[out++].set(0.0f, v1);
                    positions[out] = c;
                    texcoords[out++].set(0.0f, v1);
                    positions[out] = b;
                    texcoords[out++].set(1.0f, v0);
                    positions[out] = d;
                    texcoords[out++].set(1.0f, v1);
                }

                TDDraw::drawTexturedTriangles(positions, texcoords, mTexture, out);
            }
        }
        TDDraw::closeDirect();
#else
        TDDraw::cameraInit3D();
        TDDraw::setup(1, 2, 0);
        f32 width = calcLineWidth();
        mTexture->load(GX_TEXMAP0);
        GXSetZMode(true, GX_LEQUAL, false);

        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 2 * _990);
        {
            for (s32 i = 0; i < _990; i++) {
                f32 v2 = i / (_990 - 1.0f);

                TVec3f v1 = _690[i] * width;

                GXPosition3f32(v1.x + _90[i].x, v1.y + _90[i].y, v1.z + _90[i].z);
                GXTexCoord2f32(0.0f, v2);
                GXPosition3f32(_90[i].x - v1.x, _90[i].y - v1.y, _90[i].z - v1.z);
                GXTexCoord2f32(1.0f, v2);
            }
        }
        GXEnd();

        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 2 * _990);
        {
            for (s32 j = 0; j < _990; j++) {
                f32 v2 = j / (_990 - 1.0f);

                TVec3f v1 = _390[j] * width;

                GXPosition3f32(v1.x + _90[j].x, v1.y + _90[j].y, v1.z + _90[j].z);
                GXTexCoord2f32(0.0f, v2);
                GXPosition3f32(_90[j].x - v1.x, _90[j].y - v1.y, _90[j].z - v1.z);
                GXTexCoord2f32(1.0f, v2);
            }
        }
        GXEnd();
#endif
    }
}

void GCaptureRibbon::reset() {
    _990 = 0;
    _994 = 0;
}

void GCaptureRibbon::lengthen(const TVec3f& a1, const TVec3f& a2) {
    _90[0].set(a1);
    _90[1].set(a2);
    _990 = 2;
    updateAxis();
}

void GCaptureRibbon::shorten(const TVec3f& a1, const TVec3f& a2) {
    _90[0].set(a1);
    _90[1].set(a2);
    _990 = 2;
    updateAxis();
}

void GCaptureRibbon::updateAxis() {
    TQuat4f quat;
    if (_990 >= 2) {
        TVec3f v1 = _90[1] - _90[0];
        MR::normalizeOrZero(&v1);

        if (MR::isNearZero(v1)) {
            _390[0] = TVec3f(1, 0, 0);
            _690[0] = TVec3f(0, 1, 0);
            v1 = TVec3f(0, 0, 1);
        } else {
            MR::makeAxisVerticalZX(&_390[0], v1);
            _690[0].cross(v1, _390[0]);
        }

        for (s32 i = 1; i < _990 - 1; i++) {
            TVec3f v2 = _90[i + 1] - _90[i];
            if (MR::isNearZero(v2)) {
                _390[i] = _390[i - 1];
                _690[i] = _690[i - 1];
            } else {
                MR::normalize(&v2);
                if (v1.dot(v2) < 0.0f) {
                    v2 = -v2;
                }
                quat.setRotate(v1, v2);
                quat.transform(_390[i - 1], _390[i]);
                quat.transform(_690[i - 1], _690[i]);
                v1.set(v2);
            }
        }
        _390[_990 - 1] = _390[_990 - 2];
        _690[_990 - 1] = _690[_990 - 2];
    }
}

f32 GCaptureRibbon::calcLineWidth() const {
    f32 normalize = MR::normalize(_90[0].distance(_90[_990 - 1]), 500.0f, 2000.0f);
    return (20.0f * normalize) + (90.0f * (1.0f - normalize));
}

GCaptureRibbon::~GCaptureRibbon() {
}

void GCaptureRibbon::init(const JMapInfoIter& rIter) {
    mTexture = new JUTTexture(MR::loadTexFromArc("GCaptureRibbon.arc", "GCaptureRibbon.bti"), 0);
    MR::invalidateClipping(this);
    makeActorAppeared();
}
