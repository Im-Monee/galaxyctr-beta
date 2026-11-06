#include "Game/System/ArchiveHolder.hpp"
#include "Game/Util.hpp"
#include <cstring>
// 3DS: headers the 3DS code of this file uses (revolution.h).
#ifdef __3DS__
#include <revolution.h>
#endif

ArchiveHolderArchiveEntry::ArchiveHolderArchiveEntry(void* pData, JKRHeap* pHeap, const char* pArchiveName)
    : mHeap(pHeap), mArchive(nullptr), mArchiveName(nullptr) {
    // 3DS: the first bytes of every mounted archive go to the log, to tell a converted file from an
    // original.
#ifdef __3DS__
    const u8* header = static_cast< const u8* >(pData);
    if (header != nullptr) {
        OSReport("Archive mount: %s header %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                 pArchiveName, header[0], header[1], header[2], header[3], header[4], header[5], header[6], header[7],
                 header[8], header[9], header[10], header[11], header[12], header[13], header[14], header[15]);
    } else {
        OSReport("Archive mount: %s has null data\n", pArchiveName);
    }
#endif
    JKRMemArchive* archive = new (pHeap, 0) JKRMemArchive();
    archive->mountFixed(pData, JKR_MEM_BREAK_FLAG_0);
    mArchive = archive;

    s32 len = strlen(pArchiveName) + 1;
    mArchiveName = new (pHeap, 0) char[len];
    MR::copyString(mArchiveName, pArchiveName, len);
}

ArchiveHolderArchiveEntry::~ArchiveHolderArchiveEntry() {
    mArchive->unmount();
    delete mArchiveName;
}

ArchiveHolder::ArchiveHolder() {
    mEntries.init(0x180);
    OSInitMutex(&mMutex);
}

ArchiveHolderArchiveEntry* ArchiveHolder::createAndAdd(void* pData, JKRHeap* pHeap, const char* pArchiveName) {
    ArchiveHolderArchiveEntry* entry = new (pHeap, 0) ArchiveHolderArchiveEntry(pData, pHeap, pArchiveName);
    OSMutex* mutex = &mMutex;
    OSLockMutex(mutex);

    mEntries.push_back(entry);

    OSUnlockMutex(mutex);
    return entry;
}

JKRMemArchive* ArchiveHolder::getArchive(const char* pArchiveName) const {
    ArchiveHolderArchiveEntry* entry = findEntry(pArchiveName);
    return (entry != nullptr) ? entry->mArchive : nullptr;
}

void ArchiveHolder::getArchiveAndHeap(const char* pArchiveName, JKRArchive** pArchive, JKRHeap** pHeap) const {
    ArchiveHolderArchiveEntry* entry = findEntry(pArchiveName);

    if (entry != nullptr) {
        *pArchive = entry->mArchive;
        *pHeap = entry->mHeap;
    }
}

void ArchiveHolder::removeIfIsEqualHeap(JKRHeap* pHeap) {
    if (pHeap == nullptr) {
        return;
    }

    for (ArchiveHolderArchiveEntry** i = mEntries.begin(); i != mEntries.end();) {
        if ((*i)->mHeap == pHeap || MR::getHeapNapa((*i)->mHeap) == pHeap || MR::getHeapGDDR3((*i)->mHeap) == pHeap) {
            delete *i;
            mEntries.erase(i);
        } else {
            i++;
        }
    }
}

ArchiveHolderArchiveEntry* ArchiveHolder::findEntry(const char* pArchiveName) const {
    for (ArchiveHolderArchiveEntry* const* i = mEntries.begin(); i != mEntries.end(); i++) {
        if (MR::isEqualStringCase((*i)->mArchiveName, pArchiveName)) {
            return *i;
        }
    }

    return nullptr;
}
