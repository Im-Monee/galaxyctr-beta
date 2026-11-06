#pragma once

#include "Inline.hpp"
#include "JSystem/JParticle/JPATexture.hpp"
#include <revolution.h>

class JKRHeap;
class JPAResource;
// 3DS: the particles' textures are kept as JUTTextures for the Citro3D draw.
#ifdef __3DS__
class JUTTexture;
#endif
struct ResTIMG;

class JPAResourceManager {
public:
    JPAResourceManager(void const*, JKRHeap*);
    JPAResource* getResource(u16) const;
    bool checkUserIndexDuplication(u16) const;
    const ResTIMG* swapTexture(ResTIMG const*, char const*);
    void registRes(JPAResource*);
    void registTex(JPATexture*);
    u32 getResUserWork(u16) const;
#ifdef __3DS__
    JUTTexture* getJUTTexture(u16 idx) const {
        return idx < mTexNum ? mpTexArr[idx]->getJUTTexture() : nullptr;
    }
    // The textures load() bound last to GX_TEXMAP0 (base particles), GX_TEXMAP1 (child
    // particles), GX_TEXMAP2 (indirect texture) and GX_TEXMAP3 (second texture): GX keeps them
    // as state, and the particle draws sample them.
    static JUTTexture* sLoadedTexture[4];
#endif

    void load(u16 idx, GXTexMapID texMapID) NO_INLINE {
        CALL_INLINE_FUNC(load, idx, texMapID);
    }

    inline void INLINE_FUNC_DECL(load, u16 idx, GXTexMapID texMapID) {
        // 3DS: remembers the texture loaded on each map, which the particles' Citro3D draw binds
        // (particle.cpp).
#ifdef __3DS__
        if (texMapID < 4) {
            sLoadedTexture[texMapID] = getJUTTexture(idx);
        }
        if (idx >= mTexNum) {
            return;
        }
#endif
        mpTexArr[idx]->load(texMapID);
    }

public:
    /* 0x00 */ JKRHeap* mpHeap;
    /* 0x04 */ JPAResource** mpResArr;
    /* 0x08 */ JPATexture** mpTexArr;
    /* 0x0C */ u16 mResMax;
    /* 0x0E */ u16 mResNum;
    /* 0x10 */ u16 mTexMax;
    /* 0x12 */ u16 mTexNum;
};
