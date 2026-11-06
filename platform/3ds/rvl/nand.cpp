// The Wii's internal memory (NAND), backed by a folder on the SD card.
//
// Absolute NAND paths map to sdmc:/smg/nand/<path>; relative ones are relative to the game's
// home directory, as on the Wii. Save files keep their Wii layout byte for byte. Calls complete
// immediately; the game makes them from its own NAND thread.

#include "host.h"
#include "rvl_internal.h"
#include <revolution.h>
#include <revolution/nand.h>
#include <string.h>

static const char kRoot[] = "sdmc:/smg/nand";
static const char kHomeDir[] = "/title/00010000/524d4745/data";  // RMGE

static const int kMaxOpenFiles = 8;
static void* sOpenFiles[kMaxOpenFiles];

static void hostPath(const char* nandPath, char* out, u32 size) {
    if (nandPath[0] == '/') {
        snprintf(out, size, "%s%s", kRoot, nandPath);
    } else {
        snprintf(out, size, "%s%s/%s", kRoot, kHomeDir, nandPath);
    }
}

static void* fileOf(const NANDFileInfo* info) {
    s32 fd = info->fileDescriptor;
    return fd >= 0 && fd < kMaxOpenFiles ? sOpenFiles[fd] : nullptr;
}

s32 NANDCreate(const char* path, u8, u8) {
    char full[160];
    hostPath(path, full, sizeof(full));
    if (hostPathType(full) != 0) {
        return NAND_RESULT_EXISTS;
    }
    return hostFileCreate(full) == 0 ? NAND_RESULT_OK : NAND_RESULT_ACCESS;
}

s32 NANDOpen(const char* path, NANDFileInfo* info, u8 access) {
    char full[160];
    hostPath(path, full, sizeof(full));
    if (hostPathType(full) != 1) {
        return NAND_RESULT_NOEXISTS;
    }

    int fd = 0;
    while (fd < kMaxOpenFiles && sOpenFiles[fd] != nullptr) {
        fd++;
    }
    if (fd == kMaxOpenFiles) {
        return NAND_RESULT_MAXFD;
    }

    void* file = hostFileOpenMode(full, access);
    if (file == nullptr) {
        return NAND_RESULT_ACCESS;
    }
    sOpenFiles[fd] = file;
    info->fileDescriptor = fd;
    info->accType = access;
    strncpy(info->origPath, path, NAND_MAX_PATH - 1);
    info->origPath[NAND_MAX_PATH - 1] = '\0';
    return NAND_RESULT_OK;
}

s32 NANDClose(NANDFileInfo* info) {
    void* file = fileOf(info);
    if (file == nullptr) {
        return NAND_RESULT_INVALID;
    }
    hostFileCloseMode(file);
    sOpenFiles[info->fileDescriptor] = nullptr;
    info->fileDescriptor = -1;
    return NAND_RESULT_OK;
}

s32 NANDRead(NANDFileInfo* info, void* buf, u32 length) {
    void* file = fileOf(info);
    if (file == nullptr) {
        return NAND_RESULT_INVALID;
    }
    long read = hostFileReadNext(file, buf, length);
    return read < 0 ? NAND_RESULT_ACCESS : static_cast< s32 >(read);
}

s32 NANDWrite(NANDFileInfo* info, const void* buf, u32 length) {
    void* file = fileOf(info);
    if (file == nullptr) {
        return NAND_RESULT_INVALID;
    }
    long written = hostFileWriteNext(file, buf, length);
    return written != static_cast< long >(length) ? NAND_RESULT_ACCESS : static_cast< s32 >(written);
}

s32 NANDGetLength(NANDFileInfo* info, u32* length) {
    void* file = fileOf(info);
    if (file == nullptr) {
        return NAND_RESULT_INVALID;
    }
    long size = hostFileSizeMode(file);
    if (size < 0) {
        return NAND_RESULT_ACCESS;
    }
    *length = static_cast< u32 >(size);
    return NAND_RESULT_OK;
}

s32 NANDDelete(const char* path) {
    char full[160];
    hostPath(path, full, sizeof(full));
    if (hostPathType(full) == 0) {
        return NAND_RESULT_NOEXISTS;
    }
    return hostFileDelete(full) == 0 ? NAND_RESULT_OK : NAND_RESULT_NOTEMPTY;
}

// Moves a file into another directory, keeping its name.
s32 NANDMove(const char* path, const char* destDir) {
    char from[160], dir[160], to[200];
    hostPath(path, from, sizeof(from));
    hostPath(destDir, dir, sizeof(dir));
    if (hostPathType(from) == 0) {
        return NAND_RESULT_NOEXISTS;
    }
    const char* name = strrchr(path, '/');
    name = name != nullptr ? name + 1 : path;
    snprintf(to, sizeof(to), "%s/%s", dir, name);
    return hostFileRename(from, to) == 0 ? NAND_RESULT_OK : NAND_RESULT_ACCESS;
}

// Free space check: the SD card always has room.
s32 NANDCheck(u32, u32, u32* answer) {
    *answer = 0;
    return NAND_RESULT_OK;
}

s32 NANDGetHomeDir(char path[NAND_MAX_PATH]) {
    strcpy(path, kHomeDir);
    return NAND_RESULT_OK;
}

static void copyBannerText(u16* dst, const u16* src) {
    for (int i = 0; i < NAND_BANNER_COMMENT_SIZE; i++) {
        dst[i] = src != nullptr ? src[i] : 0;
        if (dst[i] == 0) {
            break;
        }
    }
    dst[NAND_BANNER_COMMENT_SIZE - 1] = 0;
}

void NANDInitBanner(NANDBanner* banner, u32 flag, const u16* title, const u16* comment) {
    memset(banner, 0, 0x20 + sizeof(banner->comment));
    banner->signature = NAND_BANNER_SIGNATURE;
    banner->flag = flag;
    copyBannerText(banner->comment[0], title);
    copyBannerText(banner->comment[1], comment);
}
