// The Wii audio interface (AI): streams the game's stereo buffers to the speakers.
//
// On the Wii, AIInitDMA sets the buffer the DMA plays next; when a buffer starts playing the
// AI interrupt calls the game's callback, which sets the following one (JASDriver::updateDac).
// Here the 3DS output (host/audio.c) raises HOST_IRQ_AUDIO when it can take a buffer: the
// current one is queued, then the callback runs, as many times as there are free slots.

#include <revolution/ai.h>
#include <revolution/os.h>

#include "host.h"
#include "rvl_internal.h"

static AIDCallback sCallback;
static u32 sStart, sLength;
static u32 sRate = 32000;
static bool sRunning, sOutput;

extern "C" {

void AIInit(u8*) {
}

AIDCallback AIRegisterDMACallback(AIDCallback callback) {
    BOOL enabled = OSDisableInterrupts();
    AIDCallback old = sCallback;
    sCallback = callback;
    OSRestoreInterrupts(enabled);
    return old;
}

void AIInitDMA(u32 start, u32 length) {
    sStart = start;
    sLength = length;
}

u32 AIGetDMAStartAddr(void) {
    return sStart;
}

u32 AIGetDMALength(void) {
    return sLength;
}

// 0: 32 kHz, 1: 48 kHz (JASDriver::initAI).
void AISetDSPSampleRate(u32 rate) {
    sRate = rate == 0 ? 32000 : 48000;
}

// Sound output is off unless debug.txt has audio=1: with it, sounds start playing, and a scene
// change waits for them to stop (JAUSoundMgr::stopSync), which can still hang while the mixer
// and the streams are incomplete (block J).
void AIStartDMA(void) {
    sOutput = hostDebugAudio() && hostAudioStart(sRate) != 0;
    sRunning = true;
    if (sOutput) {
        hostInterruptRaise(HOST_IRQ_AUDIO);
    }
}

void AIStopDMA(void) {
    sRunning = false;
}

}  // extern "C"

void rvlAIInterrupt() {
    while (sRunning && sOutput && hostAudioSlotFree()) {
        if (sStart != 0 && sLength != 0) {
            hostAudioQueue(reinterpret_cast< const short* >(sStart), sLength / 4);
        }
        if (sCallback == nullptr) {
            break;
        }
        sCallback();
    }
}
