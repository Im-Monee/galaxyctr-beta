#include "Game/System/HeapMemoryWatcher.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <revolution/os.h>
#include <revolution/wpad.h>
// 3DS: host.h, the port's host interface.
#ifdef __3DS__
#include "host.h"
#endif

JKRExpHeap* HeapMemoryWatcher::sRootHeapGDDR3;

namespace {
    JKRExpHeap* createExpHeap(u32 size, JKRHeap* pHeap, bool a3) NO_INLINE {
        JKRExpHeap* heap;

        if (a3) {
            void* data = new (pHeap, -4) u8[size];
            heap = JKRExpHeap::create(data, size, pHeap, true);
        } else {
            heap = JKRExpHeap::create(size, pHeap, true);
        }

        if (MR::isEqualCurrentHeap(heap)) {
            JKRHeap::sRootHeap->becomeCurrentHeap();
        }

        return heap;
    }

    JKRSolidHeap* createSolidHeap(u32 size, JKRHeap* pHeap) {
        JKRSolidHeap* heap = JKRSolidHeap::create(size, pHeap, true);

        if (MR::isEqualCurrentHeap(heap)) {
            JKRHeap::sRootHeap->becomeCurrentHeap();
        }

        return heap;
    }

    void destroyHeapAndSetNULL(JKRHeap** pHeap) {
        if (*pHeap != nullptr) {
            JKRHeap::destroy(*pHeap);
            *pHeap = nullptr;
        }
    }
};  // namespace

JKRHeap* HeapMemoryWatcher::getHeapNapa(const JKRHeap* pHeap) {
    if (pHeap == mStationedHeapNapa || pHeap == mStationedHeapGDDR) {
        return mStationedHeapNapa;
    }

    if (pHeap == mSceneHeapNapa || pHeap == mSceneHeapGDDR) {
        return mSceneHeapNapa;
    }

    return nullptr;
}

JKRHeap* HeapMemoryWatcher::getHeapGDDR3(const JKRHeap* pHeap) {
    if (pHeap == mStationedHeapNapa || pHeap == mStationedHeapGDDR) {
        return mStationedHeapGDDR;
    }

    if (pHeap == mSceneHeapNapa || pHeap == mSceneHeapGDDR) {
        return mSceneHeapGDDR;
    }

    return nullptr;
}

void HeapMemoryWatcher::createFileCacheHeapOnGameHeap(u32 size) {
    mFileCacheHeap = ::createSolidHeap(size, mGameHeapGDDR);
}

void HeapMemoryWatcher::createSceneHeapOnGameHeap() {
    mSceneHeapNapa = ::createSolidHeap(-1, mGameHeapNapa);
    mSceneHeapGDDR = ::createSolidHeap(-1, mGameHeapGDDR);
#ifdef __3DS__
    // 3DS port: heap sizes in KB, to compare the memory budget with the Wii's.
    OSReport("Heaps: stationed napa %u gddr %u, game napa %u gddr %u, file cache %u, scene napa %u gddr %u\n",
             static_cast< u32 >(mStationedHeapNapa->getEnd() - mStationedHeapNapa->getStart()) / 1024,
             static_cast< u32 >(mStationedHeapGDDR->getEnd() - mStationedHeapGDDR->getStart()) / 1024,
             static_cast< u32 >(mGameHeapNapa->getEnd() - mGameHeapNapa->getStart()) / 1024,
             static_cast< u32 >(mGameHeapGDDR->getEnd() - mGameHeapGDDR->getStart()) / 1024,
             static_cast< u32 >(mFileCacheHeap->getEnd() - mFileCacheHeap->getStart()) / 1024,
             static_cast< u32 >(mSceneHeapNapa->getEnd() - mSceneHeapNapa->getStart()) / 1024,
             static_cast< u32 >(mSceneHeapGDDR->getEnd() - mSceneHeapGDDR->getStart()) / 1024);
    OSReport("Heaps: free in stationed napa %d gddr %d, root gddr %d\n", mStationedHeapNapa->getTotalFreeSize() / 1024,
             mStationedHeapGDDR->getTotalFreeSize() / 1024, sRootHeapGDDR3->getTotalFreeSize() / 1024);
#endif
}

void HeapMemoryWatcher::adjustStationedHeaps() {
    MR::adjustHeapSize(mStationedHeapNapa, 0);
    MR::adjustHeapSize(mStationedHeapGDDR, 0);
}

void HeapMemoryWatcher::setCurrentHeapToStationedHeap() {
    MR::becomeCurrentHeap(mStationedHeapNapa);
}

void HeapMemoryWatcher::setCurrentHeapToGameHeap() {
    MR::becomeCurrentHeap(JKRHeap::sSystemHeap);
}

void HeapMemoryWatcher::setCurrentHeapToSceneHeap() {
    MR::becomeCurrentHeap(mSceneHeapNapa);
}

#ifdef __3DS__
namespace {
    // 3DS port: what the scene used of its heaps (solid heaps only grow: this is their peak) and
    // of the file cache, in KB, to size the memory budget (Old 3DS).
    void reportSceneHeaps(JKRHeap* pSceneHeapNapa, JKRHeap* pSceneHeapGDDR, JKRHeap* pFileCacheHeap) {
        if (pSceneHeapNapa == nullptr || pSceneHeapGDDR == nullptr) {
            return;
        }
        // The file cache may already be gone (GameSystemSceneController::tryDestroyFileCacheHeap).
        u32 cacheUsed = 0, cacheSize = 0;
        if (pFileCacheHeap != nullptr) {
            cacheSize = static_cast< u32 >(pFileCacheHeap->getEnd() - pFileCacheHeap->getStart());
            cacheUsed = cacheSize - pFileCacheHeap->getTotalFreeSize();
        }
        OSReport("Heaps used by the scene: napa %u of %u, gddr %u of %u, file cache %u of %u\n",
                 static_cast< u32 >(pSceneHeapNapa->getEnd() - pSceneHeapNapa->getStart() - pSceneHeapNapa->getTotalFreeSize()) / 1024,
                 static_cast< u32 >(pSceneHeapNapa->getEnd() - pSceneHeapNapa->getStart()) / 1024,
                 static_cast< u32 >(pSceneHeapGDDR->getEnd() - pSceneHeapGDDR->getStart() - pSceneHeapGDDR->getTotalFreeSize()) / 1024,
                 static_cast< u32 >(pSceneHeapGDDR->getEnd() - pSceneHeapGDDR->getStart()) / 1024, cacheUsed / 1024, cacheSize / 1024);
    }
}  // namespace
#endif

void HeapMemoryWatcher::destroySceneHeap() {
#ifdef __3DS__
    reportSceneHeaps(mSceneHeapNapa, mSceneHeapGDDR, mFileCacheHeap);
    // The scene's models go with it: their cached vertices leave the GPU arena now, rather than
    // filling it until it empties in the middle of the next scene and every model there uploads
    // again (about 4 s entering the Comet Observatory in CIA 0.14.7).
    hostGfxArenaReset();
#endif
    ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mSceneHeapNapa));
    ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mSceneHeapGDDR));
}

void HeapMemoryWatcher::destroyGameHeap() {
    // 3DS: what the scene used of its heaps, to the log.
#ifdef __3DS__
    reportSceneHeaps(mSceneHeapNapa, mSceneHeapGDDR, mFileCacheHeap);
#endif
    if (mSceneHeapNapa != nullptr) {
        ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mSceneHeapNapa));
    }

    if (mSceneHeapGDDR != nullptr) {
        ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mSceneHeapGDDR));
    }

    if (mFileCacheHeap != nullptr) {
        ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mFileCacheHeap));
    }

    ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mGameHeapNapa));
    ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mGameHeapGDDR));
    createGameHeap();
}

void HeapMemoryWatcher::createRootHeap() {
    JKRExpHeap* pHeap;
    void* newHi;
    u32 arenaHi, arenaLo;

    JKRExpHeap::createRoot(1, true);
    arenaLo = reinterpret_cast< u32 >(OSGetMEM2ArenaLo());
    arenaHi = reinterpret_cast< u32 >(OSGetMEM2ArenaHi());
    newHi = reinterpret_cast< void* >(arenaLo + 0xE00000);
    OSSetMEM2ArenaHi(newHi);
    JKRHeap::setAltAramStartAdr(arenaLo);
    pHeap = JKRExpHeap::create(newHi, arenaHi - reinterpret_cast< u32 >(newHi), JKRHeap::sRootHeap, true);

    if (MR::isEqualCurrentHeap(pHeap)) {
        JKRHeap::sRootHeap->becomeCurrentHeap();
    }

    HeapMemoryWatcher::sRootHeapGDDR3 = pHeap;
}

void HeapMemoryWatcher::createHeaps() {
    MR::CurrentHeapRestorer heapRestorer = MR::CurrentHeapRestorer(JKRHeap::sRootHeap);
    ::createExpHeap(0x40000, JKRHeap::sRootHeap, false)->becomeSystemHeap();
    mAudSystemHeap = ::createSolidHeap(0x1E0000, JKRHeap::sRootHeap);
    mStationedHeapNapa = ::createExpHeap(0x900000, JKRHeap::sRootHeap, false);
    JKRHeap* pRootHeapGDDR = HeapMemoryWatcher::sRootHeapGDDR3;
    u32 wpadHeapSize = OSRoundUp32B(WPADGetWorkMemorySize()) + 0xD0;
    mWPadHeap = ::createExpHeap(wpadHeapSize, pRootHeapGDDR, false);
    mHomeButtonLayoutHeap = ::createExpHeap(0x80000, HeapMemoryWatcher::sRootHeapGDDR3, false);
    mStationedHeapGDDR = ::createExpHeap(0x1400000, HeapMemoryWatcher::sRootHeapGDDR3, false);
    createGameHeap();
}

void HeapMemoryWatcher::createGameHeap() {
    mGameHeapNapa = ::createExpHeap(-1, JKRHeap::sRootHeap, false);
    mGameHeapGDDR = ::createExpHeap(-1, HeapMemoryWatcher::sRootHeapGDDR3, false);
}

HeapMemoryWatcher::HeapMemoryWatcher()
    : mStationedHeapNapa(nullptr), mStationedHeapGDDR(nullptr), mGameHeapNapa(nullptr), mGameHeapGDDR(nullptr), mFileCacheHeap(nullptr),
      mSceneHeapNapa(nullptr), mSceneHeapGDDR(nullptr), mWPadHeap(nullptr), mHomeButtonLayoutHeap(nullptr), mAudSystemHeap(nullptr) {
    JKRHeap::setErrorHandler(HeapMemoryWatcher::memoryErrorCallback);
    createHeaps();
}

#ifdef __3DS__
void HeapMemoryWatcher::memoryErrorCallback(void* pHeap, u32 size, int align) {
    // 3DS port: say which heap ran out before stopping.
    JKRHeap* pJKRHeap = static_cast< JKRHeap* >(pHeap);
    OSReport("Out of memory: %u bytes (align %d) from heap %p (%u KB, %d bytes free, largest %d)\n", size, align, pHeap,
             static_cast< u32 >(pJKRHeap->getEnd() - pJKRHeap->getStart()) / 1024, pJKRHeap->getTotalFreeSize(), pJKRHeap->getFreeSize());
    OSPanic(__FILE__, 537, "");
}
#else
void HeapMemoryWatcher::memoryErrorCallback(void*, u32, int) {
    OSPanic(__FILE__, 537, "");
}
#endif

void HeapMemoryWatcher::checkRestMemory() {
}
