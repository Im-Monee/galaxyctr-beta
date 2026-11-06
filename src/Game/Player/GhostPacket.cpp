#include "Game/Player/GhostPacket.hpp"
#include <JSystem/JGeometry/TVec.hpp>
#include <cstdio>

GhostPacket::GhostPacket(void* pData, u32 len) {
    mDataPtr = (u8*)pData;
    mCurOffs = 0;
    _C = len;
}

void GhostPacket::read(u8* pOut, u32 len) {
    for (int i = 0; i < len; i++) {
        *pOut = mDataPtr[mCurOffs];
        pOut++;
        mCurOffs++;
    }
}

#ifdef __3DS__
// Ghost data is big-endian: multi-byte values are assembled from their bytes.
static void readBigEndian(GhostPacket* pPacket, u8* pOut, u32 len) {
    for (u32 i = 0; i < len; i++) {
        pPacket->read(&pOut[len - 1 - i], 1);
    }
}
#endif

void GhostPacket::read(u32* pOut) {
#ifdef __3DS__
    readBigEndian(this, (u8*)pOut, 4);
#else
    read((u8*)pOut, 4);
#endif
}

void GhostPacket::read(s16* pOut) {
#ifdef __3DS__
    readBigEndian(this, (u8*)pOut, 2);
#else
    read((u8*)pOut, 2);
#endif
}

void GhostPacket::read(char** pOut) {
    char* v3 = (char*)&mDataPtr[mCurOffs];
    *pOut = (char*)v3;
    s32 offs = strlen(v3) + 1;
    mCurOffs += offs;
}

void GhostPacket::read(s8* pOut) {
    read((u8*)pOut, 1);
}

void GhostPacket::read(TVec3Sc* pOut) {
    read((u8*)&pOut->x, 1);
    read((u8*)&pOut->y, 1);
    read((u8*)&pOut->z, 1);
}

void GhostPacket::read(TVec3s* pOut) {
#ifdef __3DS__
    readBigEndian(this, (u8*)&pOut->x, 2);
    readBigEndian(this, (u8*)&pOut->y, 2);
    readBigEndian(this, (u8*)&pOut->z, 2);
#else
    read((u8*)&pOut->x, 2);
    read((u8*)&pOut->y, 2);
    read((u8*)&pOut->z, 2);
#endif
}
