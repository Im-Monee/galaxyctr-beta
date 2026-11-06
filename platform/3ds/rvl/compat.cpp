// Typed compatibility entry points for Wii services without a 3DS hardware counterpart.
// Keeping the Wii ABI signatures here prevents generated int(void) placeholders from
// corrupting arguments or making unsupported operations look successful.

#include "host.h"
#include "rvl_internal.h"

#include <revolution.h>
#include <revolution/base/PPCArch.h>
#include <revolution/nwc24/NWC24Config.h>
#include <revolution/nwc24/NWC24Manage.h>
#include <revolution/nwc24/NWC24MsgCommit.h>
#include <revolution/nwc24/NWC24MsgObj.h>
#include <revolution/os/OSError.h>
#include <revolution/os/OSReset.h>
#include <revolution/os/OSResetSW.h>
#include <revolution/rso.h>
#include <revolution/thp.h>
#include <revolution/vf.h>
#include <string.h>

u32 __OSFpscrEnableBits;

namespace {

OSShutdownFunctionInfo* sShutdownHead;
OSErrorHandler sErrorHandlers[OS_ERROR_MAX];
OSPowerCallback sPowerCallback;
bool sShutdownCallbacksRun;

void runShutdownCallbacks() {
    if (sShutdownCallbacksRun) {
        return;
    }
    sShutdownCallbacksRun = true;
    for (OSShutdownFunctionInfo* entry = sShutdownHead; entry != nullptr; entry = entry->next) {
        if (entry->func != nullptr && !entry->func(TRUE, 0)) {
            break;
        }
    }
}

}  // namespace

extern "C" void rvlShutdown(void) {
    runShutdownCallbacks();
}

extern "C" {

void OSRegisterShutdownFunction(OSShutdownFunctionInfo* info) {
    if (info == nullptr || info->func == nullptr) {
        return;
    }
    for (OSShutdownFunctionInfo* it = sShutdownHead; it != nullptr; it = it->next) {
        if (it == info) {
            return;
        }
    }

    OSShutdownFunctionInfo* previous = nullptr;
    OSShutdownFunctionInfo* next = sShutdownHead;
    while (next != nullptr && next->priority <= info->priority) {
        previous = next;
        next = next->next;
    }
    info->prev = previous;
    info->next = next;
    if (previous != nullptr) {
        previous->next = info;
    } else {
        sShutdownHead = info;
    }
    if (next != nullptr) {
        next->prev = info;
    }
}

void OSRebootSystem(void) {
    hostQuit();
}

void OSShutdownSystem(void) {
    hostQuit();
}

void OSRestart(u32 resetCode) {
    (void)resetCode;
    hostQuit();
}

void OSReturnToMenu(void) {
    hostQuit();
}

OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) {
    if (error >= OS_ERROR_MAX) {
        return nullptr;
    }
    OSErrorHandler previous = sErrorHandlers[error];
    sErrorHandlers[error] = handler;
    return previous;
}

OSPowerCallback OSSetPowerCallback(OSPowerCallback callback) {
    OSPowerCallback previous = sPowerCallback;
    sPowerCallback = callback;
    return previous;
}

// These PowerPC-only registers do not exist on ARM11. Reporting HID2 as clear forces
// callers onto their portable path; writes are intentionally discarded.
u32 PPCMfhid2(void) {
    return 0;
}

void PPCMthid2(u32 value) {
    (void)value;
}

void PPCMtwpar(u32 value) {
    (void)value;
}

void VFInitEx(void* work, u32 size) {
    (void)work;
    (void)size;
}

NWC24Err NWC24OpenLib(void* work) {
    (void)work;
    return NWC24_ERR_DISABLED;
}

NWC24Err NWC24CloseLib(void) {
    return NWC24_ERR_LIB_NOT_OPENED;
}

s32 NWC24GetErrorCode(void) {
    return NWC24_ERR_DISABLED;
}

NWC24Err NWC24GetMyUserId(NWC24UserId* userId) {
    if (userId != nullptr) {
        *userId = 0;
    }
    return NWC24_ERR_DISABLED;
}

NWC24Err NWC24InitMsgObj(NWC24MsgObj* message, NWC24MsgType type) {
    (void)type;
    if (message != nullptr) {
        memset(message, 0, sizeof(*message));
    }
    return NWC24_ERR_DISABLED;
}

NWC24Err NWC24SetMsgToId(NWC24MsgObj* message, NWC24UserId userId) {
    (void)message;
    (void)userId;
    return NWC24_ERR_DISABLED;
}

NWC24Err NWC24SetMsgText(NWC24MsgObj* message, const char* text, u32 length, NWC24Charset charset, NWC24Encoding encoding) {
    (void)message;
    (void)text;
    (void)length;
    (void)charset;
    (void)encoding;
    return NWC24_ERR_DISABLED;
}

NWC24Err NWC24SetMsgAttached(NWC24MsgObj* message, const char* data, u32 size, NWC24MIMEType type) {
    (void)message;
    (void)data;
    (void)size;
    (void)type;
    return NWC24_ERR_DISABLED;
}

NWC24Err NWC24SetMsgLedPattern(NWC24MsgObj* message, u16 pattern) {
    (void)message;
    (void)pattern;
    return NWC24_ERR_DISABLED;
}

NWC24Err NWC24GetMsgSize(const NWC24MsgObj* message, u32* size) {
    (void)message;
    if (size != nullptr) {
        *size = 0;
    }
    return NWC24_ERR_DISABLED;
}

NWC24Err NWC24SetMsgAltName(NWC24MsgObj* message, const u16* name, u32 length) {
    (void)message;
    (void)name;
    (void)length;
    return NWC24_ERR_DISABLED;
}

NWC24Err NWC24SetMsgMBNoReply(NWC24MsgObj* message, BOOL enable) {
    (void)message;
    (void)enable;
    return NWC24_ERR_DISABLED;
}

NWC24Err NWC24CommitMsg(NWC24MsgObj* message) {
    (void)message;
    return NWC24_ERR_DISABLED;
}

BOOL RSOListInit(void* object) {
    (void)object;
    return FALSE;
}

BOOL RSOLinkList(void* object, void* bss) {
    (void)object;
    (void)bss;
    return FALSE;
}

BOOL RSOIsImportSymbolResolvedAll(const RSOObjectHeader* object) {
    (void)object;
    return FALSE;
}

int RSOGetJumpCodeSize(const RSOObjectHeader* object) {
    (void)object;
    return 0;
}

void RSOMakeJumpCode(const RSOObjectHeader* object, void* buffer) {
    (void)object;
    (void)buffer;
}

void* RSOFindExportSymbolAddr(const RSOObjectHeader* object, const char* name) {
    (void)object;
    (void)name;
    return nullptr;
}

int RSOLinkJump(RSOObjectHeader* importObject, const RSOObjectHeader* exportObject, void* buffer) {
    (void)importObject;
    (void)exportObject;
    (void)buffer;
    return -1;
}

// The movies' pictures go to the New 3DS video decoder (THPDrawNative.cpp), not the THP decoder.
BOOL THPInit(void) {
    return TRUE;
}

s32 THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work) {
    (void)file;
    (void)tileY;
    (void)tileU;
    (void)tileV;
    (void)work;
    return 0;
}

}  // extern "C"
