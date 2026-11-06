#include "Game/Util/DirectDraw.hpp"
#include <JSystem/JUtility/JUTTexture.hpp>

#ifdef __3DS__
#include "Game/Util/ScreenUtil.hpp"
#include "host.h"
#include <cstring>
#include <revolution/dvd.h>

// The movies converted for the 3DS (tools/3ds/builder/movie.py): THP's layout, little-endian,
// with H.264 pictures for the New 3DS video decoder (platform/3ds/host/movie.c) and the THP's
// DSP-ADPCM audio. The game's player (THPSimplePlayerWrapper) reads them as on the Wii; the
// pictures go through these functions instead of the JPEG decoder and the YUV textures.
namespace {
    const u32 kMovieVersion = 0x3D50001;
    u16 sWidth, sHeight, sPaddedWidth, sPaddedHeight;

    u16 powerOfTwo(u32 value) {
        u32 out = 8;
        while (out < value) {
            out <<= 1;
        }
        return static_cast< u16 >(out);
    }
}  // namespace

extern "C" u32 THPNativeMovieVersion() {
    return kMovieVersion;
}

// The movie can play: a New 3DS and a converted file.
extern "C" BOOL THPNativeIsMoviePlayable(const char* pPath) {
    if (!hostMovieSupported()) {
        return FALSE;
    }
    DVDFileInfo info;
    if (!DVDOpen(pPath, &info)) {
        return FALSE;
    }
    u32 head[2] ATTRIBUTE_ALIGN(32) = {0, 0};
    const s32 read = DVDReadPrio(&info, head, sizeof(head), 0, 2);
    DVDClose(&info);
    return read == static_cast< s32 >(sizeof(head)) && std::memcmp(head, "THP", 4) == 0 && head[1] == kMovieVersion;
}

// The video info's type word holds the encoded size (width << 16 | height).
extern "C" BOOL THPNativeMovieBegin(u32 videoType) {
    sWidth = static_cast< u16 >(videoType >> 16);
    sHeight = static_cast< u16 >(videoType & 0xFFFF);
    sPaddedWidth = powerOfTwo(sWidth);
    sPaddedHeight = powerOfTwo(sHeight);
    return hostMovieBegin(sWidth, sHeight, sPaddedWidth, sPaddedHeight) != 0;
}

extern "C" void THPNativeMovieDecode(const u8* pData, u32 size) {
    if (size != 0) {
        hostMovieDecode(pData, size);
    }
}

// Without sound output the game's mix callback never takes the movie's audio, which would fill
// its buffers and stop the movie: the player then leaves the audio out.
extern "C" BOOL THPNativeAudioActive() {
    return hostAudioActive() != 0;
}

extern "C" void THPNativeMovieEnd() {
    hostMovieEnd();
}

// THPGXYuv2RgbDraw: the last decoded picture over the polygon (frame buffer pixels, as the
// Wii's THPGXYuv2RgbSetup projection), the texture's encoded part only.
extern "C" void THPNativeDraw(const u8*, const u8*, const u8*, s16 x, s16 y, s16 textureWidth, s16 textureHeight, s16 polygonWidth,
                               s16 polygonHeight) {
    void* texture = hostMovieTexture();
    if (texture == nullptr || textureWidth <= 0 || textureHeight <= 0 || polygonWidth <= 0 || polygonHeight <= 0) {
        return;
    }
    TDDraw::setupDirect(1, 0, 3);
    Mtx44 projectionMtx;
    C_MTXOrtho(projectionMtx, 0.0f, MR::getFrameBufferHeight(), 0.0f, MR::getFrameBufferWidth(), -1.0f, 1.0f);
    TDDraw::setDirectProjectionMtx(projectionMtx);
    Mtx viewMtx;
    PSMTXIdentity(viewMtx);
    TDDraw::setDirectViewMtx(viewMtx);
    TDDraw::setDirectDepthMode(false, 5, false);
    TDDraw::setDirectCullMode(0);
    TDDraw::drawHostTexture(TVec2f(x, y), TVec2f(polygonWidth, polygonHeight), texture, static_cast< f32 >(sWidth) / sPaddedWidth,
                            static_cast< f32 >(sHeight) / sPaddedHeight);
    TDDraw::closeDirect();
}
#else
extern "C" void THPNativeDraw(const u8*, const u8*, const u8*, s16, s16, s16, s16, s16, s16) {
}
#endif
