// Video interface: the vertical retrace interrupt and frame pacing.
// The 3DS screen refresh (VBlank, 60 Hz like an NTSC Wii) stands in for the Wii's retrace.

#include "host.h"
#include "rvl_internal.h"
#include <revolution.h>

// The SDK's NTSC 480i mode (RVL_SDK GXFrameBuf.c), which JUTException::createFB uses.
GXRenderModeObj GXNtsc480Int = {
    0, 640, 480, 480, 40, 0, 640, 480, 1, 0, 0, {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6}, {0, 0, 21, 22, 21, 0, 0}};

static volatile u32 sRetraceCount;
static OSThreadQueue sRetraceQueue;
static VIRetraceCallback sPreRetraceCallback;
static VIRetraceCallback sPostRetraceCallback;
static GXRenderModeObj sRenderMode;
static void* sNextFrameBuffer;
static void* sCurrentFrameBuffer;
static BOOL sBlack = TRUE;
static BOOL sDimmingEnabled;
static BOOL sTrapFilterEnabled;

void rvlVIRetraceInterrupt() {
    if (!hostMainLoop()) {
        hostQuit();
    }

    sRetraceCount++;

    if (sPreRetraceCallback != nullptr) {
        sPreRetraceCallback(sRetraceCount);
    }

    OSWakeupThread(&sRetraceQueue);

    if (sPostRetraceCallback != nullptr) {
        sPostRetraceCallback(sRetraceCount);
    }
}

extern "C" {

void VIInit(void) {
    OSInitThreadQueue(&sRetraceQueue);
}

void VIWaitForRetrace(void) {
    BOOL enabled = OSDisableInterrupts();
    u32 count = sRetraceCount;

    do {
        OSSleepThread(&sRetraceQueue);
    } while (count == sRetraceCount);

    OSRestoreInterrupts(enabled);
}

void VIConfigure(const GXRenderModeObj* mode) {
    if (mode != nullptr) {
        sRenderMode = *mode;
    }
}

void VIFlush(void) {
    // Render and scanout are owned by Citro3D; this commits the emulated VI buffer state.
    sCurrentFrameBuffer = sNextFrameBuffer;
    (void)sRenderMode;
    (void)sTrapFilterEnabled;
}

void VISetNextFrameBuffer(void* framebuffer) {
    sNextFrameBuffer = framebuffer;
}

void* VIGetNextFrameBuffer(void) {
    return sNextFrameBuffer;
}

void* VIGetCurrentFrameBuffer(void) {
    return sCurrentFrameBuffer;
}

void VISetBlack(BOOL black) {
    sBlack = black != FALSE;
    hostGfxSetBlack(sBlack);
}

u32 VIGetTvFormat(void) {
    return VI_NTSC;
}

u32 VIGetDTVStatus(void) {
    return 0;
}

BOOL VIEnableDimming(BOOL enable) {
    BOOL previous = sDimmingEnabled;
    sDimmingEnabled = enable != FALSE;
    return previous;
}

u32 VIGetDimmingCount(void) {
    return 0;
}

void VISetTrapFilter(VIBool enable) {
    sTrapFilterEnabled = enable != VI_FALSE;
}

u32 VIGetRetraceCount(void) {
    return sRetraceCount;
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback callback) {
    BOOL enabled = OSDisableInterrupts();
    VIRetraceCallback old = sPreRetraceCallback;
    sPreRetraceCallback = callback;
    OSRestoreInterrupts(enabled);
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback callback) {
    BOOL enabled = OSDisableInterrupts();
    VIRetraceCallback old = sPostRetraceCallback;
    sPostRetraceCallback = callback;
    OSRestoreInterrupts(enabled);
    return old;
}

}  // extern "C"
