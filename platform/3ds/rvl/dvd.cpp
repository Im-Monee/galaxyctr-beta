// The Wii disc file system, backed by the game files on the SD card.
//
// At startup the folder returned by hostFilesRoot() is scanned into a table laid out like
// the disc's FST: entry 0 is the root and each directory is followed by its contents, so
// entry numbers and directory iteration behave like on the Wii. Names are compared without
// case, as on the disc.
//
// Reads are performed immediately. Asynchronous reads report completion through the I/O
// interrupt, so their callbacks run from the interrupt dispatcher like on the Wii.

#include "host.h"
#include "rvl_internal.h"
#include <revolution.h>
#include <strings.h>

// Set to log every path the game looks for without finding it.
static const bool kLogMissingFiles = false;
// Set to log every path the game looks up.
static const bool kLogLookups = false;

struct FstEntry {
    char* mName;
    bool mIsDir;
    u32 mParent;
    // Directories: index of the first entry after their contents. Files: size, or -1 if unknown.
    s32 mNextOrSize;
};

static FstEntry* sEntries;
static u32 sEntryCount;
static u32 sEntryCapacity;
static u32 sCurrentDir;

static u32 addEntry(const char* pName, bool isDir, u32 parent) {
    if (sEntryCount == sEntryCapacity) {
        sEntryCapacity = sEntryCapacity == 0 ? 1024 : sEntryCapacity * 2;
        sEntries = static_cast< FstEntry* >(realloc(sEntries, sEntryCapacity * sizeof(FstEntry)));
    }

    FstEntry& entry = sEntries[sEntryCount];
    entry.mName = strdup(pName);
    entry.mIsDir = isDir;
    entry.mParent = parent;
    entry.mNextOrSize = -1;
    return sEntryCount++;
}

static int compareNames(const void* pA, const void* pB) {
    return strcasecmp(*static_cast< char* const* >(pA), *static_cast< char* const* >(pB));
}

static void buildPath(u32 entry, char* pDst, u32 size);

// Adds the contents of the directory at index dirIndex, recursively.
static void scanDirectory(u32 dirIndex) {
    char path[256];
    buildPath(dirIndex, path, sizeof(path));

    void* dir = hostDirOpen(path);
    if (dir == nullptr) {
        sEntries[dirIndex].mNextOrSize = sEntryCount;
        return;
    }

    // Collect names first, sorted, with a trailing marker for directories.
    char** names = nullptr;
    u32 count = 0;
    char name[128];
    int isDir;
    while (hostDirNext(dir, name, sizeof(name) - 1, &isDir)) {
        names = static_cast< char** >(realloc(names, (count + 1) * sizeof(char*)));
        size_t len = strlen(name);
        names[count] = static_cast< char* >(malloc(len + 2));
        memcpy(names[count], name, len);
        names[count][len] = isDir ? '/' : '\0';
        names[count][len + 1] = '\0';
        count++;
    }
    hostDirClose(dir);

    qsort(names, count, sizeof(char*), compareNames);

    for (u32 i = 0; i < count; i++) {
        size_t len = strlen(names[i]);
        bool entryIsDir = len > 0 && names[i][len - 1] == '/';
        if (entryIsDir) {
            names[i][len - 1] = '\0';
        }

        u32 index = addEntry(names[i], entryIsDir, dirIndex);
        if (entryIsDir) {
            scanDirectory(index);
        }

        free(names[i]);
    }

    free(names);
    sEntries[dirIndex].mNextOrSize = sEntryCount;
}

static void buildPath(u32 entry, char* pDst, u32 size) {
    if (entry == 0) {
        snprintf(pDst, size, "%s", hostFilesRoot());
        return;
    }

    buildPath(sEntries[entry].mParent, pDst, size);
    size_t len = strlen(pDst);
    snprintf(pDst + len, size - len, "/%s", sEntries[entry].mName);
}

static s32 findChild(u32 dir, const char* pName, size_t nameLen) {
    for (u32 i = dir + 1; i < static_cast< u32 >(sEntries[dir].mNextOrSize);) {
        if (strncasecmp(sEntries[i].mName, pName, nameLen) == 0 && sEntries[i].mName[nameLen] == '\0') {
            return i;
        }
        i = sEntries[i].mIsDir ? sEntries[i].mNextOrSize : i + 1;
    }

    return -1;
}

// Open file handles, cached by entry number: the game reads archives in many pieces.
struct OpenFile {
    u32 mEntry;
    void* mHandle;
};

static const int kOpenFileCacheSize = 8;
static OpenFile sOpenFiles[kOpenFileCacheSize];
static int sNextOpenFile;

static void* getFileHandle(u32 entry) {
    for (int i = 0; i < kOpenFileCacheSize; i++) {
        if (sOpenFiles[i].mHandle != nullptr && sOpenFiles[i].mEntry == entry) {
            return sOpenFiles[i].mHandle;
        }
    }

    char path[256];
    buildPath(entry, path, sizeof(path));
    void* handle = hostFileOpen(path);
    if (handle == nullptr) {
        return nullptr;
    }

    OpenFile& slot = sOpenFiles[sNextOpenFile];
    sNextOpenFile = (sNextOpenFile + 1) % kOpenFileCacheSize;
    if (slot.mHandle != nullptr) {
        hostFileClose(slot.mHandle);
    }

    slot.mEntry = entry;
    slot.mHandle = handle;

    if (sEntries[entry].mNextOrSize < 0) {
        sEntries[entry].mNextOrSize = hostFileSize(handle);
    }

    return handle;
}

static s32 readFile(DVDFileInfo* pInfo, void* pDst, s32 length, s32 offset) {
    void* handle = getFileHandle(pInfo->startAddr);
    if (handle == nullptr) {
        return -1;
    }

    return hostFileRead(handle, pDst, length, offset);
}

// Completed asynchronous commands, delivered by the I/O interrupt in order.
struct Completion {
    DVDCommandBlock* mBlock;
    s32 mResult;
    bool mIsFileInfo;
};

static const int kMaxCompletions = 64;
static Completion sCompletions[kMaxCompletions];
static int sCompletionHead;
static int sCompletionCount;

static void queueCompletion(DVDCommandBlock* pBlock, s32 result, bool isFileInfo) {
    if (sCompletionCount == kMaxCompletions) {
        hostFatal("Too many pending disc reads.");
    }

    Completion& completion = sCompletions[(sCompletionHead + sCompletionCount) % kMaxCompletions];
    completion.mBlock = pBlock;
    completion.mResult = result;
    completion.mIsFileInfo = isFileInfo;
    sCompletionCount++;
    hostInterruptRaise(HOST_IRQ_IO);
}

void rvlIOInterrupt() {
    while (sCompletionCount > 0) {
        Completion completion = sCompletions[sCompletionHead];
        sCompletionHead = (sCompletionHead + 1) % kMaxCompletions;
        sCompletionCount--;

        DVDCommandBlock* block = completion.mBlock;
        block->state = 0;

        if (completion.mIsFileInfo) {
            DVDFileInfo* info = reinterpret_cast< DVDFileInfo* >(block);
            if (info->callback != nullptr) {
                info->callback(completion.mResult, info);
            }
        } else if (block->callback != nullptr) {
            block->callback(completion.mResult, block);
        }
    }
}

extern "C" {

void DVDInit(void) {
    if (sEntries != nullptr) {
        return;
    }

    u64 start = hostGetTick();
    addEntry("", true, 0);
    scanDirectory(0);
    sCurrentDir = 0;

    u64 ms = (hostGetTick() - start) * 1000 / hostGetTickFrequency();
    hostLog("Disc: %lu entries in %s (%lu ms)\n", sEntryCount, hostFilesRoot(), static_cast< u32 >(ms));
    if (sEntryCount <= 1) {
        hostFatal("No game files found.\nCopy the game files to %s.", hostFilesRoot());
    }
}

// The path of a disc entry from the disc root ("/ObjectData/Mario.arc"); -1 for a bad entry.
static void buildRelPath(u32 entry, char* pDst, u32 size) {
    if (entry == 0) {
        pDst[0] = 0;
        return;
    }
    buildRelPath(sEntries[entry].mParent, pDst, size);
    size_t len = strlen(pDst);
    snprintf(pDst + len, size - len, "/%s", sEntries[entry].mName);
}

int rvlDVDEntryRelPath(s32 entry, char* pDst, u32 size) {
    if (entry <= 0 || static_cast< u32 >(entry) >= sEntryCount) {
        return -1;
    }
    buildRelPath(entry, pDst, size);
    return 0;
}

s32 DVDConvertPathToEntrynum(const char* pPath) {
    const char* pFullPath = pPath;
    if (kLogLookups) {
        hostLog("dvd: %s%c", pPath, 10);
    }
    u32 dir = *pPath == '/' ? 0 : sCurrentDir;

    while (*pPath != '\0') {
        while (*pPath == '/') {
            pPath++;
        }
        if (*pPath == '\0') {
            break;
        }

        const char* end = pPath;
        while (*end != '\0' && *end != '/') {
            end++;
        }

        size_t len = end - pPath;
        s32 next;
        if (len == 1 && pPath[0] == '.') {
            next = dir;
        } else if (len == 2 && pPath[0] == '.' && pPath[1] == '.') {
            next = sEntries[dir].mParent;
        } else {
            next = findChild(dir, pPath, len);
        }

        if (next < 0) {
            // The game probes for localized files first, so misses are normal.
            if (kLogMissingFiles) {
                hostLog("DVD: not found: %s (in \"%s\")\n", pFullPath, sEntries[dir].mName);
            }
            return -1;
        }

        dir = next;
        pPath = end;
    }

    return dir;
}

BOOL DVDFastOpen(s32 entry, DVDFileInfo* pInfo) {
    if (entry < 0 || static_cast< u32 >(entry) >= sEntryCount || sEntries[entry].mIsDir) {
        return FALSE;
    }

    if (getFileHandle(entry) == nullptr) {
        return FALSE;
    }

    pInfo->startAddr = entry;
    pInfo->length = sEntries[entry].mNextOrSize;
    pInfo->callback = nullptr;
    pInfo->cb.state = 0;
    pInfo->cb.next = pInfo->cb.prev = nullptr;
    return TRUE;
}

BOOL DVDOpen(const char* pPath, DVDFileInfo* pInfo) {
    s32 entry = DVDConvertPathToEntrynum(pPath);
    return entry >= 0 && DVDFastOpen(entry, pInfo);
}

BOOL DVDClose(DVDFileInfo* pInfo) {
    return TRUE;
}

s32 DVDReadPrio(DVDFileInfo* pInfo, void* pDst, s32 length, s32 offset, s32 prio) {
    return readFile(pInfo, pDst, length, offset);
}

BOOL DVDReadAsyncPrio(DVDFileInfo* pInfo, void* pDst, s32 length, s32 offset, DVDCallback callback, s32 prio) {
    pInfo->callback = callback;
    pInfo->cb.state = 1;
    queueCompletion(&pInfo->cb, readFile(pInfo, pDst, length, offset), true);
    return TRUE;
}

s32 DVDCancel(DVDCommandBlock* pBlock) {
    return 0;
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock* pBlock) {
    return pBlock->state;
}

// The "disc" is always present and idle.
s32 DVDGetDriveStatus(void) {
    return 0;
}

BOOL DVDCheckDiskAsync(DVDCommandBlock* pBlock, DVDCBCallback callback) {
    pBlock->callback = callback;
    pBlock->state = 1;
    queueCompletion(pBlock, 1, false);
    return TRUE;
}

BOOL DVDOpenDir(const char* pPath, DVDDir* pDir) {
    s32 entry = DVDConvertPathToEntrynum(pPath);
    if (entry < 0 || !sEntries[entry].mIsDir) {
        return FALSE;
    }

    pDir->entryNum = entry;
    pDir->location = entry + 1;
    pDir->next = sEntries[entry].mNextOrSize;
    return TRUE;
}

BOOL DVDReadDir(DVDDir* pDir, DVDDirEntry* pEntry) {
    u32 location = pDir->location;
    if (location <= pDir->entryNum || pDir->next <= location) {
        return FALSE;
    }

    pEntry->entryNum = location;
    pEntry->isDir = sEntries[location].mIsDir;
    pEntry->name = sEntries[location].mName;
    pDir->location = sEntries[location].mIsDir ? sEntries[location].mNextOrSize : location + 1;
    return TRUE;
}

BOOL DVDCloseDir(DVDDir* pDir) {
    return TRUE;
}

}  // extern "C"
