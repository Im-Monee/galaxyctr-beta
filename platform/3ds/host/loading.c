// ##############################################################################################
// # The loading screen                                                                         #
// #                                                                                            #
// # While the port loads, before the first scene (the title screen), the top screen shows the  #
// # game's saving window with the words "GalaxyCTR is loading..." and plays the game's saving  #
// # sounds (the owner's request, 25 Nov 2026). The frames and sounds are made by the builder   #
// # from the user's disc (tools/beta/builder_loading.py, sdmc:/smg/loading); without them the  #
// # port starts as before, with nothing on the top screen.                                     #
// #                                                                                            #
// # A thread of its own draws it, so the star keeps turning while the game is busy loading.    #
// # It writes the top screen's framebuffer itself: until it lets go, the renderer's frames do  #
// # not go to the top screen (gfx.c, hostLoadingOwnsTop). It goes as the game saves:           #
// #   appear   the window's appearing animation                                                #
// #   wait     the window and the turning star, in a loop; SE_SY_LV_SAVING from step 20        #
// #   end      when the title starts, the first scene after "Logo" (startScene in              #
// #            GameSystemSceneController.cpp calls hostLoadingFinish): the star goes, the      #
// #            window's closing animation, then SE_SY_SAVE_SUCCESS once the window is gone     #
// #            (SaveDataHandleSequence)                                                        #
// # hostLoadingFinish waits for the window to be gone and a few frames more, so the title does  #
// # not start under it.                                                                        #
// ##############################################################################################
#include "host.h"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOADING_FOLDER "sdmc:/smg/loading/"
#define LOADING_VERSION 1
#define LOADING_SAVING_START 20     // trySave: SE_SY_LV_SAVING from this step on
#define LOADING_GAP_FRAMES 4        // black frames between the window and the first scene
#define LOADING_SOUND_CHANNEL 23    // the game's audio uses channel 0 (audio.c)
#define LOADING_SOUND_RATE 32000.0f
#define LOADING_TOP_WIDTH 400
#define LOADING_TOP_HEIGHT 240

static Thread sThread;
static bool sStarted;               // hostLoadingStart ran (sGone is set up)
static volatile bool sOwnsTop;      // the top screen is the loading screen's
static volatile bool sFinishAsked;  // the first scene is starting
static LightEvent sGone;            // the window has closed (or the screen never started)

// frames.bin, read whole into memory at the start (about 570 KB): read frame by frame from the
// SD card, the star stopped whenever the loading game kept the card busy (owner, 25 Nov 2026).
static unsigned char* sFile;
static unsigned long sFileSize;
static unsigned sAppear, sWait, sEnd;
static unsigned sLeft, sTop, sWidth, sHeight;
static unsigned long* sFrameOffsets;
static unsigned long* sFrameSizes;
static unsigned short* sPixels;  // the frame being decoded, RGB565, box-sized
static int sDecoded = -1;        // the frame sPixels holds

// The sounds, in linear memory (ndsp reads them there).
static short* sSaving;
static unsigned long sSavingSamples;
static short* sSuccess;
static unsigned long sSuccessSamples;
static bool sSound;
static ndspWaveBuf sSavingBuf, sSuccessBuf;

static void* readWhole(const char* name, unsigned long* pSize, bool linear) {
    FILE* file = fopen(name, "rb");
    if (file == NULL) {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    void* data = size > 0 ? (linear ? linearAlloc(size) : malloc(size)) : NULL;
    if (data != NULL && fread(data, 1, size, file) != (size_t)size) {
        if (linear) {
            linearFree(data);
        } else {
            free(data);
        }
        data = NULL;
    }
    fclose(file);
    *pSize = data != NULL ? (unsigned long)size : 0;
    return data;
}

static unsigned readU16(const unsigned char* p) {
    return p[0] | (p[1] << 8);
}

static bool openFrames(void) {
    sFile = readWhole(LOADING_FOLDER "frames.bin", &sFileSize, false);
    if (sFile == NULL) {
        return false;
    }
    const unsigned char* head = sFile;
    if (sFileSize < 20 || memcmp(head, "GCLS", 4) != 0 || readU16(head + 4) != LOADING_VERSION) {
        hostLog("loading: frames.bin is missing or from another version, no loading screen\n");
        return false;
    }
    sAppear = readU16(head + 6);
    sWait = readU16(head + 8);
    sEnd = readU16(head + 10);
    sLeft = readU16(head + 12);
    sTop = readU16(head + 14);
    sWidth = readU16(head + 16);
    sHeight = readU16(head + 18);
    const unsigned count = sAppear + sWait + sEnd;
    if (sWait == 0 || count == 0 || sLeft + sWidth > LOADING_TOP_WIDTH || sTop + sHeight > LOADING_TOP_HEIGHT) {
        return false;
    }
    sFrameOffsets = malloc(count * sizeof(unsigned long));
    sFrameSizes = malloc(count * sizeof(unsigned long));
    sPixels = malloc(sWidth * sHeight * 2);
    if (sFrameOffsets == NULL || sFrameSizes == NULL || sPixels == NULL) {
        return false;
    }
    unsigned long offset = 20 + count * 4;
    if (offset > sFileSize) {
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        const unsigned char* size = sFile + 20 + i * 4;
        sFrameSizes[i] = size[0] | (size[1] << 8) | (size[2] << 16) | ((unsigned long)size[3] << 24);
        sFrameOffsets[i] = offset;
        offset += sFrameSizes[i];
    }
    return offset <= sFileSize;  // every frame inside the file
}

// Frame index of the file into sPixels. Each part (appear, wait, end) starts from black, the
// frames after its first from the frame before (builder_loading._compress).
static void decodeFrame(unsigned index) {
    if ((int)index == sDecoded) {
        return;
    }
    const bool partStart = index == 0 || index == sAppear || index == sAppear + sWait;
    if (!partStart && (int)index != sDecoded + 1) {
        // Out of order (never, but safe): decode from the part's start.
        unsigned start = index >= sAppear + sWait ? sAppear + sWait : index >= sAppear ? sAppear : 0;
        sDecoded = -1;
        for (unsigned i = start; i < index; i++) {
            decodeFrame(i);
        }
    }
    if (partStart) {
        memset(sPixels, 0, sWidth * sHeight * 2);
    }
    const unsigned long size = sFrameSizes[index];
    const unsigned char* sBlob = sFile + sFrameOffsets[index];
    const unsigned long pixels = sWidth * sHeight;
    unsigned long at = 0, p = 0;
    while (p + 2 <= size && at < pixels) {
        unsigned count = readU16(sBlob + p);
        p += 2;
        if (count & 0x8000) {
            at += count & 0x7FFF;  // kept from the frame before
            continue;
        }
        for (unsigned i = 0; i < count && p + 2 <= size && at < pixels; i++, p += 2) {
            sPixels[at++] = (unsigned short)readU16(sBlob + p);
        }
    }
    sDecoded = (int)index;
}

// sPixels onto the top screen's back framebuffer (BGR8, columns from the bottom of the screen),
// black around it, and shown at the next retrace.
static void showFrame(bool blank) {
    // The framebuffer being shown until the next retrace must not be written.
    while (gspIsPresentPending(GFX_TOP)) {
        svcSleepThread(1000000);
    }
    u16 fbWidth, fbHeight;
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbWidth, &fbHeight);
    const unsigned long bytes = (unsigned long)fbWidth * fbHeight * 3;
    memset(fb, 0, bytes);
    if (!blank) {
        for (unsigned y = 0; y < sHeight; y++) {
            const unsigned short* row = sPixels + y * sWidth;
            const unsigned screenY = sTop + y;
            for (unsigned x = 0; x < sWidth; x++) {
                const unsigned short c = row[x];
                u8* px = fb + ((sLeft + x) * fbWidth + (fbWidth - 1 - screenY)) * 3;
                px[0] = (u8)((c & 0x1F) << 3);
                px[1] = (u8)(((c >> 5) & 0x3F) << 2);
                px[2] = (u8)((c >> 11) << 3);
            }
        }
    }
    GSPGPU_FlushDataCache(fb, bytes);
    gfxScreenSwapBuffers(GFX_TOP, false);
}

static void startSound(ndspWaveBuf* buf, short* data, unsigned long samples, bool looping) {
    if (!sSound || data == NULL) {
        return;
    }
    ndspChnWaveBufClear(LOADING_SOUND_CHANNEL);
    memset(buf, 0, sizeof(*buf));
    buf->data_vaddr = data;
    buf->nsamples = samples;
    buf->looping = looping;
    ndspChnWaveBufAdd(LOADING_SOUND_CHANNEL, buf);
}

static void openSound(void) {
    unsigned long bytes;
    sSaving = readWhole(LOADING_FOLDER "saving.pcm", &bytes, true);
    sSavingSamples = bytes / 2;
    sSuccess = readWhole(LOADING_FOLDER "success.pcm", &bytes, true);
    sSuccessSamples = bytes / 2;
    if (sSaving == NULL || sSuccess == NULL) {
        return;
    }
    DSP_FlushDataCache(sSaving, sSavingSamples * 2);
    DSP_FlushDataCache(sSuccess, sSuccessSamples * 2);
    if (R_FAILED(ndspInit())) {
        return;  // no DSP firmware: the window shows without its sounds
    }
    sSound = true;
    ndspChnReset(LOADING_SOUND_CHANNEL);
    ndspChnSetInterp(LOADING_SOUND_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(LOADING_SOUND_CHANNEL, LOADING_SOUND_RATE);
    ndspChnSetFormat(LOADING_SOUND_CHANNEL, NDSP_FORMAT_MONO_PCM16);
    float mix[12] = {1.0f, 1.0f};
    ndspChnSetMix(LOADING_SOUND_CHANNEL, mix);
}

static void closeAll(void) {
    if (sSound) {
        ndspChnWaveBufClear(LOADING_SOUND_CHANNEL);
        ndspChnReset(LOADING_SOUND_CHANNEL);
        ndspExit();
        sSound = false;
    }
    if (sSaving != NULL) {
        linearFree(sSaving);
    }
    if (sSuccess != NULL) {
        linearFree(sSuccess);
    }
    sSaving = sSuccess = NULL;
    free(sFile);
    sFile = NULL;
    free(sFrameOffsets);
    free(sFrameSizes);
    free(sPixels);
    sFrameOffsets = sFrameSizes = NULL;
    sPixels = NULL;
}

// Frames of 60 a second since start, by the system clock (the port's retrace count starts later).
static unsigned long framesSince(u64 start) {
    return (unsigned long)((svcGetSystemTick() - start) * 60ULL / SYSCLOCK_ARM11);
}

static void waitForFrame(u64 start, unsigned long frame) {
    while (framesSince(start) < frame) {
        svcSleepThread(2000000);
    }
}

static void loadingThread(void* arg) {
    (void)arg;
    u64 start = svcGetSystemTick();
    unsigned long frame = 0;
    bool saving = false;
    // appear, then wait until the first scene starts (at least one whole appear)
    while (frame < sAppear || !sFinishAsked) {
        waitForFrame(start, frame);
        const unsigned long now = framesSince(start);
        frame = now > frame ? now : frame;  // a late thread skips frames rather than lag
        if (!saving && frame >= LOADING_SAVING_START) {
            startSound(&sSavingBuf, sSaving, sSavingSamples, true);
            saving = true;
        }
        const unsigned index = frame < sAppear ? (unsigned)frame : sAppear + (unsigned)((frame - sAppear) % sWait);
        decodeFrame(index);
        showFrame(false);
        frame++;
    }
    // end: the saving sound stops, the window closes, then the success sound
    if (sSound) {
        ndspChnWaveBufClear(LOADING_SOUND_CHANNEL);
    }
    start = svcGetSystemTick();
    for (frame = 0; frame < sEnd; frame++) {
        waitForFrame(start, frame);
        decodeFrame(sAppear + sWait + (unsigned)frame);
        showFrame(false);
    }
    for (unsigned i = 0; i < LOADING_GAP_FRAMES; i++) {
        waitForFrame(start, sEnd + i);
        showFrame(true);
    }
    startSound(&sSuccessBuf, sSuccess, sSuccessSamples, false);
    sOwnsTop = false;
    LightEvent_Signal(&sGone);
    // The success sound plays out, then everything is let go.
    if (sSound) {
        const u64 until = svcGetSystemTick() + (u64)(sSuccessSamples / LOADING_SOUND_RATE * SYSCLOCK_ARM11) + SYSCLOCK_ARM11 / 10;
        while (svcGetSystemTick() < until) {
            svcSleepThread(10000000);
        }
    }
    closeAll();
}

void hostLoadingStart(void) {
    LightEvent_Init(&sGone, RESET_STICKY);
    sStarted = true;
    if (!openFrames()) {
        closeAll();
        LightEvent_Signal(&sGone);
        return;
    }
    openSound();
    sOwnsTop = true;
    // On a New 3DS's third core when it can, so it takes no time from the loading game.
    sThread = threadCreate(loadingThread, NULL, 0x4000, 0x30, 2, true);
    const bool thirdCore = sThread != NULL;
    if (sThread == NULL) {
        sThread = threadCreate(loadingThread, NULL, 0x4000, 0x30, -2, true);
    }
    if (sThread == NULL) {
        sOwnsTop = false;
        closeAll();
        LightEvent_Signal(&sGone);
        return;
    }
    hostLog("loading: loading screen on (%u + %u + %u frames, %ux%u, %lu KB in memory, %s)%s\n", sAppear, sWait, sEnd,
            sWidth, sHeight, sFileSize >> 10, thirdCore ? "third core" : "the game's core", sSound ? "" : ", no sound");
}

int hostLoadingOwnsTop(void) {
    return sOwnsTop ? 1 : 0;
}

void hostLoadingFinish(void) {
    if (!sStarted || sFinishAsked) {
        return;
    }
    sFinishAsked = true;
    // The window closes before the scene draws (at most about two seconds).
    LightEvent_WaitTimeout(&sGone, 2000000000LL);
}
