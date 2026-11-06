#include "Game/System/FileLoaderThread.hpp"
#include "Game/System/FileRipper.hpp"
#include "Game/Util/FileUtil.hpp"
// 3DS: headers the 3DS code of this file uses (SingletonHolder.hpp, HeapMemoryWatcher.hpp,
// MemoryUtil.hpp, JKRSolidHeap.hpp, dvd.h).
#ifdef __3DS__
#include "Game/Util/SingletonHolder.hpp"
#include "Game/System/HeapMemoryWatcher.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <revolution/dvd.h>
#endif

namespace {
#ifdef __3DS__
    // The file's size in memory: its decompressed size when the first bytes say it is compressed.
    u32 sizeInMemory(const char* pName) {
        const u32 size = (MR::getFileSize(pName, false) + 0x1f) / 0x20 * 0x20;
        DVDFileInfo fileInfo;
        if (!DVDOpen(pName, &fileInfo)) {
            return size;
        }
        u8 buffer[0x60];
        u8* head = reinterpret_cast< u8* >((reinterpret_cast< u32 >(buffer) + 0x3f) / 0x40 * 0x40);
        const s32 result = DVDReadPrio(&fileInfo, head, 0x20, 0, 2);
        DVDClose(&fileInfo);
        if (result < 0 || head[0] != 'Y' || head[1] != 'a' || (head[2] != 'y' && head[2] != 'z') || head[3] != '0') {
            return size;
        }
        return (head[4] << 24) | (head[5] << 16) | (head[6] << 8) | head[7];
    }

    // MR::getAproposHeapForSceneArchive moves to the scene heap when the file cache is 97% full and
    // knows no file's size: with the 8 MB of an Old 3DS, a 1.7 MB archive of Gateway Galaxy did not
    // fit in the 1.3 MB left (CIA 0.16.3). A file that does not fit goes to the scene heap with the
    // most room; removeIfIsEqualHeap finds it there as it finds the others.
    void moveToHeapItFits(RequestFileInfo* pInfo) {
        FileHolderFileEntry* entry = pInfo->mFileEntry;
        JKRHeap* fileCache = SingletonHolder< HeapMemoryWatcher >::get()->mFileCacheHeap;
        if (entry->mContext != nullptr || entry->mHeap != fileCache || fileCache == nullptr) {
            return;
        }
        const s32 kOverhead = 0x1000;  // the archive's objects, allocated with it
        const s32 needed = static_cast< s32 >(sizeInMemory(pInfo->mFileName)) + kOverhead;
        if (needed <= fileCache->getFreeSize()) {
            return;
        }
        JKRHeap* napa = MR::getSceneHeapNapa();
        JKRHeap* gddr = MR::getSceneHeapGDDR3();
        if (napa == nullptr || gddr == nullptr) {
            return;
        }
        JKRHeap* roomier = napa->getFreeSize() > gddr->getFreeSize() ? napa : gddr;
        if (needed <= roomier->getFreeSize()) {
            entry->mHeap = roomier;
        }
    }
#endif

    void* loadFileUsingRipper(RequestFileInfo* pInfo) {
        s32 val = 0;

        if (pInfo->mRequestType != 1) {
            val = 1;
        }

        return FileRipper::loadToMainRAM(pInfo->mFileName, (u8*)pInfo->mFileEntry->mContext, true, pInfo->mFileEntry->mHeap,
                                         (FileRipper::AllocDirection)val);
    }
};  // namespace

FileLoaderThread::FileLoaderThread(int priority, int msgCount, JKRHeap* pHeap) : OSThreadWrapper(0x8000, msgCount, priority, pHeap) {
}

void* FileLoaderThread::run() {
    OSInitFastCast();

    while (true) {
        OSMessage msg;
        OSReceiveMessage(&mQueue, &msg, OS_MESSAGE_BLOCK);
        RequestFileInfo* info = (RequestFileInfo*)msg;

        switch (info->_0) {
        case 0:
            loadToMainRAM(info);
            break;
        case 1:
            mountArchiveAndStartCreateResource(info);
            break;
        }
    }
}

void FileLoaderThread::loadToMainRAM(RequestFileInfo* pInfo) {
    pInfo->_88 = 1;
    // 3DS: the file goes to a heap where it fits (memory is tighter than on the Wii).
#ifdef __3DS__
    moveToHeapItFits(pInfo);
#endif
    void* data = ::loadFileUsingRipper(pInfo);
    pInfo->mFileEntry->setContext(data, pInfo->mFileEntry->mHeap);
    pInfo->_88 = 2;
}

void FileLoaderThread::mountArchiveAndStartCreateResource(RequestFileInfo* pInfo) {
    pInfo->_88 = 1;
    // 3DS: the file goes to a heap where it fits (memory is tighter than on the Wii).
#ifdef __3DS__
    moveToHeapItFits(pInfo);
#endif
    void* data = ::loadFileUsingRipper(pInfo);
    MR::createAndAddArchive(data, pInfo->mFileEntry->mHeap, pInfo->mFileName);
    pInfo->mFileEntry->setContext(data, pInfo->mFileEntry->mHeap);
    pInfo->_88 = 2;
}
