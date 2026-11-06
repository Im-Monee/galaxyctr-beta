// Audio output through the 3DS DSP (ndsp), for the emulated Wii audio interface (rvl/ai.cpp).
//
// One stereo PCM16 channel plays the game's buffers from a few slots in linear memory. When
// the DSP has finished a slot, HOST_IRQ_AUDIO is raised: the game side then queues the next
// buffer (hostAudioQueue) and lets the game prepare the one after, like the Wii's AI DMA
// interrupt. The ndsp callback only checks the slots; game memory is read on the game side.
//
// ndsp needs the DSP firmware, sdmc:/3ds/dspfirm.cdc (dumped on the console with DSP1).
// Without it the game runs silently.

#include "host.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

// ##############################################################################################
// # Output slots                                                                               #
// #                                                                                            #
// # Three buffers in linear memory, where the DSP can read them. The game's mixed audio        #
// # arrives one buffer at a time (the Wii AI's DMA, rvl/ai.cpp): each goes into a free slot,   #
// # and the DSP plays the slots in order.                                                      #
// ##############################################################################################
#define SLOTS 3
#define SLOT_FRAMES 2048  // stereo frames per slot (the game sends 560 at 32 kHz)

static bool sReady;
static s16* sSlotData[SLOTS];
static ndspWaveBuf sSlots[SLOTS];
static unsigned long sQueued, sPeak;

// Debug option audiodump=<seconds>: the output, as sdmc:/smg/audio.wav.
static FILE* sDump;
static unsigned long sDumpFrames, sDumpLimit, sDumpRate;

static void writeWavHeader(FILE* file, unsigned long frames, unsigned long rate) {
    unsigned long bytes = frames * 4;
    u32 header[11] = {
        0x46464952, 36 + bytes, 0x45564157, 0x20746D66, 16, 0x00020001, rate, rate * 4, 0x00100004, 0x61746164, bytes,
    };
    fseek(file, 0, SEEK_SET);
    fwrite(header, sizeof(header), 1, file);
}

// ##############################################################################################
// # The DSP's callback                                                                         #
// #                                                                                            #
// # ndsp calls frameCallback on its own thread after every audio frame. It reads only the      #
// # slots' status: when one is free it raises HOST_IRQ_AUDIO, and the game side (ai.cpp, at    #
// # its next interrupt point) sends the next buffer. Game memory is never read here.           #
// ##############################################################################################
static bool slotFree(int i) {
    return sSlots[i].status == NDSP_WBUF_FREE || sSlots[i].status == NDSP_WBUF_DONE;
}

static void frameCallback(void* data) {
    (void)data;
    for (int i = 0; i < SLOTS; i++) {
        if (slotFree(i)) {
            hostInterruptRaise(HOST_IRQ_AUDIO);
            return;
        }
    }
}

// ##############################################################################################
// # Starting and stopping the output                                                           #
// #                                                                                            #
// # hostAudioStart starts ndsp (which needs the console's DSP firmware), allocates the slots   #
// # once and sets one stereo PCM16 channel to the game's rate (32 kHz). With the debug option  #
// # audiodump the output is also written to sdmc:/smg/audio.wav.                               #
// ##############################################################################################
int hostAudioActive(void) {
    return sReady ? 1 : 0;
}

int hostAudioStart(unsigned long rate) {
    if (!sReady) {
        Result rc = ndspInit();
        if (R_FAILED(rc)) {
            hostLog("audio: ndspInit failed 0x%08lX (sdmc:/3ds/dspfirm.cdc missing?), no sound\n", (unsigned long)rc);
            return 0;
        }
        for (int i = 0; i < SLOTS; i++) {
            sSlotData[i] = linearAlloc(SLOT_FRAMES * 4);
            if (sSlotData[i] == NULL) {
                hostLog("audio: no linear memory for the output, no sound\n");
                ndspExit();
                return 0;
            }
            memset(&sSlots[i], 0, sizeof(sSlots[i]));
            sSlots[i].data_vaddr = sSlotData[i];
            sSlots[i].status = NDSP_WBUF_FREE;
        }
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspSetCallback(frameCallback, NULL);
        sReady = true;
    }

    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, (float)rate);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = {1.0f, 1.0f};
    ndspChnSetMix(0, mix);
    for (int i = 0; i < SLOTS; i++) {
        sSlots[i].status = NDSP_WBUF_FREE;
    }

    sDumpLimit = hostDebugAudioDump() * rate;
    sDumpRate = rate;
    if (sDumpLimit != 0 && sDump == NULL) {
        sDump = fopen("sdmc:/smg/audio.wav", "wb");
        if (sDump != NULL) {
            writeWavHeader(sDump, 0, rate);
        }
    }
    hostLog("audio: output started, %lu Hz\n", rate);
    return 1;
}

void hostAudioShutdown(void) {
    if (sReady) {
        ndspSetCallback(NULL, NULL);
        ndspExit();  // joins the ndsp thread
        sReady = false;
    }
}

// ##############################################################################################
// # Queueing the game's buffers                                                                #
// #                                                                                            #
// # A buffer of the game into the first free slot: its samples swapped from the Wii's          #
// # right-left order to ndsp's left-right, the cache flushed so the DSP reads them, then       #
// # queued. The loudest sample of the last few seconds goes to the log (peak 32767 or 32768    #
// # means the mix was clipped).                                                                #
// ##############################################################################################
int hostAudioSlotFree(void) {
    if (!sReady) {
        return 0;
    }
    for (int i = 0; i < SLOTS; i++) {
        if (slotFree(i)) {
            return 1;
        }
    }
    return 0;
}

// Wii AI buffers are interleaved right, left (the DSP's left block is copied second, see
// JASDriver::readDspBuffer); ndsp wants left, right.
void hostAudioQueue(const short* pSamples, unsigned long frames) {
    int slot = -1;
    for (int i = 0; i < SLOTS && slot < 0; i++) {
        if (slotFree(i)) {
            slot = i;
        }
    }
    if (slot < 0) {
        return;
    }
    if (frames > SLOT_FRAMES) {
        frames = SLOT_FRAMES;
    }

    s16* out = sSlotData[slot];
    for (unsigned long i = 0; i < frames; i++) {
        s16 right = pSamples[i * 2], left = pSamples[i * 2 + 1];
        out[i * 2] = left;
        out[i * 2 + 1] = right;
        unsigned long l = left < 0 ? -left : left, r = right < 0 ? -right : right;
        if (l > sPeak) {
            sPeak = l;
        }
        if (r > sPeak) {
            sPeak = r;
        }
    }
    DSP_FlushDataCache(out, frames * 4);
    sSlots[slot].nsamples = frames;
    ndspChnWaveBufAdd(0, &sSlots[slot]);

    if (sDump != NULL) {
        fwrite(out, 4, frames, sDump);
        sDumpFrames += frames;
        if (sDumpFrames >= sDumpLimit) {
            writeWavHeader(sDump, sDumpFrames, sDumpRate);
            fclose(sDump);
            sDump = NULL;
            hostLog("audio: %lu frames written to sdmc:/smg/audio.wav\n", sDumpFrames);
        }
    }

    // About every 5 seconds at 32 kHz.
    if (++sQueued % 300 == 0) {
        hostLog("audio: %lu buffers queued, peak %lu\n", sQueued, sPeak);
        sPeak = 0;
    }
}
