// File access for the emulated Wii disc: the game files live under sdmc:/smg/files.

#include "host.h"

#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <strings.h>
#include <sys/stat.h>

#define FILES_ROOT "sdmc:/smg/files"

const char* hostFilesRoot(void) {
    return FILES_ROOT;
}

void* hostDirOpen(const char* path) {
    return opendir(path);
}

int hostDirNext(void* dir, char* pName, unsigned long nameSize, int* pIsDir) {
    struct dirent* entry;

    while ((entry = readdir((DIR*)dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        strncpy(pName, entry->d_name, nameSize - 1);
        pName[nameSize - 1] = '\0';
        *pIsDir = entry->d_type == DT_DIR;
        return 1;
    }

    return 0;
}

void hostDirClose(void* dir) {
    closedir((DIR*)dir);
}

// Time spent opening and reading game files, for the loading profile (hostLogLoadProfile).
static unsigned long long sOpenTicks, sReadTicks, sReadBytes;
static unsigned long sOpenCount, sReadCount;

void hostFileStats(unsigned long* pOpens, unsigned long long* pOpenTicks, unsigned long* pReads, unsigned long long* pReadTicks,
                   unsigned long long* pReadBytes) {
    *pOpens = sOpenCount;
    *pOpenTicks = sOpenTicks;
    *pReads = sReadCount;
    *pReadTicks = sReadTicks;
    *pReadBytes = sReadBytes;
}

// File cache pack ------------------------------------------------------------------------------
//
// Opening a file on the SD card walks its folder, and ObjectData holds about 1800 files: an open
// took about 157 ms on the console (642 opens, 101 of the 125 seconds before Good Egg's first
// frame, CIA 0.11.5). The PC converter (tools/3ds/convert_smg_files.py) writes every game file
// and native model up to 2 MB into sdmc:/smg/cache/pack.bin, and lists every file of both trees
// in index.bin. A listed path opens at its offset in the pack, or from the card for a larger
// file; a path under those trees that is not listed does not exist, and fails with no folder walk.
// Copying files into the pack on the console was tried and dropped: its SD writes ran at about
// 45 KB/s, and the first boot took 11 minutes (CIA 0.11.7).

#define PACK_DIR "sdmc:/smg/cache"
#define PACK_PATH PACK_DIR "/pack.bin"
#define PACK_INDEX_PATH PACK_DIR "/index.bin"
#define PACK_MAGIC 0x324B5053u     // "SPK2"
#define PACK_UNPACKED 0xFFFFFFFFu  // offset of a listed file left on the card
#define PACK_SLOTS 16384           // hash table size (a power of two)

typedef struct {
    char* path;
    unsigned long offset, size;
} PackEntry;

typedef struct {
    FILE* file;            // a file of its own, or NULL for a packed one
    unsigned long offset;  // packed: where it starts in the pack
    unsigned long size;
} HostFile;

static PackEntry sPackEntries[PACK_SLOTS];
static unsigned long sPackCount;
static FILE* sPack;
static bool sPackTried;
static LightLock sPackLock;

// FAT names ignore case, so lookups do too.
static unsigned long hashPath(const char* path) {
    unsigned long h = 2166136261u;
    for (; *path != '\0'; path++) {
        h = (h ^ (unsigned char)tolower((unsigned char)*path)) * 16777619u;
    }
    return h;
}

// The slot of path: its entry, or the empty slot it would take.
static PackEntry* findPackSlot(const char* path) {
    for (unsigned long i = hashPath(path) & (PACK_SLOTS - 1);; i = (i + 1) & (PACK_SLOTS - 1)) {
        PackEntry* e = &sPackEntries[i];
        if (e->path == NULL || strcasecmp(e->path, path) == 0) {
            return e;
        }
    }
}

// Paths the index lists completely.
static bool packCovers(const char* path) {
    return strncasecmp(path, "sdmc:/smg/files/", 16) == 0 || strncasecmp(path, "sdmc:/smg/native/", 17) == 0;
}

// Reads the whole index; with no pack, or an index that does not match it, files open from the card.
static void openPack(void) {
    sPackTried = true;
    LightLock_Init(&sPackLock);
    FILE* pack = fopen(PACK_PATH, "rb");
    FILE* index = pack != NULL ? fopen(PACK_INDEX_PATH, "rb") : NULL;
    if (index == NULL) {
        if (pack != NULL) {
            fclose(pack);
        }
        hostLog("file cache: none (run the PC converter to make %s)\n", PACK_DIR);
        return;
    }
    struct stat info;
    unsigned long packSize = fstat(fileno(pack), &info) == 0 ? (unsigned long)info.st_size : 0;
    unsigned long magic = 0;
    bool ok = fread(&magic, 4, 1, index) == 1 && magic == PACK_MAGIC;
    unsigned long record[3];  // offset, size, path length
    char path[256];
    while (ok && fread(record, sizeof(record), 1, index) == 1) {
        ok = record[2] < sizeof(path) && fread(path, 1, record[2], index) == record[2] &&
             (record[0] == PACK_UNPACKED || record[0] + record[1] <= packSize) && sPackCount < PACK_SLOTS * 3 / 4;
        if (ok) {
            path[record[2]] = '\0';
            PackEntry* e = findPackSlot(path);
            if (e->path == NULL) {
                e->path = strdup(path);
                sPackCount++;
            }
            e->offset = record[0];
            e->size = record[1];
        }
    }
    fclose(index);
    if (!ok) {
        for (unsigned long i = 0; i < PACK_SLOTS; i++) {
            free(sPackEntries[i].path);
            sPackEntries[i].path = NULL;
        }
        sPackCount = 0;
        fclose(pack);
        hostLog("file cache: %s does not match %s (run the PC converter again); files open from the card\n",
                PACK_INDEX_PATH, PACK_PATH);
        return;
    }
    // Large reads go straight to the SD card; a big buffer only slows them down.
    setvbuf(pack, NULL, _IONBF, 0);
    sPack = pack;
    hostLog("file cache: %lu files listed, pack %lu KB\n", sPackCount, packSize / 1024);
}

static FILE* openUnpacked(const char* path, unsigned long* pSize) {
    FILE* file = fopen(path, "rb");
    if (file != NULL) {
        setvbuf(file, NULL, _IONBF, 0);
        struct stat info;
        *pSize = fstat(fileno(file), &info) == 0 ? (unsigned long)info.st_size : 0;
    }
    return file;
}

void* hostFileOpen(const char* path) {
    unsigned long long start = svcGetSystemTick();
    if (!sPackTried) {
        openPack();
    }
    HostFile* handle = NULL;
    FILE* file = NULL;
    unsigned long size = 0;
    PackEntry* packed = sPack != NULL ? findPackSlot(path) : NULL;
    if (packed != NULL && packed->path != NULL && packed->offset != PACK_UNPACKED) {
        handle = malloc(sizeof(HostFile));
        handle->file = NULL;
        handle->offset = packed->offset;
        handle->size = packed->size;
    } else if (packed == NULL || packed->path != NULL || !packCovers(path)) {
        file = openUnpacked(path, &size);
        if (file != NULL) {
            handle = malloc(sizeof(HostFile));
            handle->file = file;
            handle->offset = 0;
            handle->size = size;
        }
    }
    sOpenTicks += svcGetSystemTick() - start;
    sOpenCount++;
    return handle;
}

long hostFileSize(void* file) {
    return (long)((HostFile*)file)->size;
}

long hostFileRead(void* file, void* pDst, unsigned long size, unsigned long offset) {
    unsigned long long start = svcGetSystemTick();
    HostFile* f = (HostFile*)file;
    long result;
    if (f->file != NULL) {
        result = fseek(f->file, (long)offset, SEEK_SET) == 0 ? (long)fread(pDst, 1, size, f->file) : -1;
    } else {
        if (offset >= f->size) {
            size = 0;
        } else if (size > f->size - offset) {
            size = f->size - offset;
        }
        LightLock_Lock(&sPackLock);
        result = fseek(sPack, (long)(f->offset + offset), SEEK_SET) == 0 ? (long)fread(pDst, 1, size, sPack) : -1;
        LightLock_Unlock(&sPackLock);
    }
    sReadTicks += svcGetSystemTick() - start;
    sReadBytes += result > 0 ? (unsigned long)result : 0;
    sReadCount++;
    return result;
}

void hostFileClose(void* file) {
    HostFile* f = (HostFile*)file;
    if (f->file != NULL) {
        fclose(f->file);
    }
    free(f);
}

// Plain files on the SD card (the emulated Wii NAND, see rvl/nand.cpp) -------------------

// Creates every missing directory leading to path (not path itself).
static void makeParentDirs(const char* path) {
    char buf[256];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (char* p = strchr(buf, '/'); p != NULL; p = strchr(p + 1, '/')) {
        if (p == buf || p[-1] == ':') {
            continue;
        }
        *p = '\0';
        mkdir(buf, 0777);
        *p = '/';
    }
}

int hostPathType(const char* path) {
    struct stat info;
    if (stat(path, &info) != 0) {
        return 0;
    }
    return S_ISDIR(info.st_mode) ? 2 : 1;
}

void* hostFileOpenMode(const char* path, int access) {
    static const char* const kModes[4] = {"rb", "rb", "r+b", "r+b"};
    return fopen(path, kModes[access & 3]);
}

long hostFileReadNext(void* file, void* pDst, unsigned long size) {
    return (long)fread(pDst, 1, size, (FILE*)file);
}

long hostFileWriteNext(void* file, const void* pSrc, unsigned long size) {
    return (long)fwrite(pSrc, 1, size, (FILE*)file);
}

long hostFileSizeMode(void* file) {
    struct stat info;
    fflush((FILE*)file);  // a written save counts its unflushed bytes
    return fstat(fileno((FILE*)file), &info) == 0 ? (long)info.st_size : -1;
}

void hostFileCloseMode(void* file) {
    fclose((FILE*)file);
}

int hostFileCreate(const char* path) {
    makeParentDirs(path);
    FILE* file = fopen(path, "wb");
    if (file == NULL) {
        return -1;
    }
    fclose(file);
    return 0;
}

int hostDirCreate(const char* path) {
    makeParentDirs(path);
    return mkdir(path, 0777) == 0 ? 0 : -1;
}

int hostFileDelete(const char* path) {
    return remove(path) == 0 ? 0 : -1;
}

int hostFileRename(const char* from, const char* to) {
    makeParentDirs(to);
    remove(to);
    return rename(from, to) == 0 ? 0 : -1;
}

// Capture files (gx/capture.cpp): written sequentially with a large buffer ------------------

void* hostCaptureOpen(const char* path) {
    FILE* file = fopen(path, "wb");
    if (file != NULL) {
        setvbuf(file, NULL, _IOFBF, 256 * 1024);
    }
    return file;
}

void hostCaptureWrite(void* file, const void* pData, unsigned long size) {
    fwrite(pData, 1, size, (FILE*)file);
}

void hostCaptureClose(void* file) {
    fclose((FILE*)file);
}
