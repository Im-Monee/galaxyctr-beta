// Stand-in for the Wii's audio DSP.
//
// JAudio2 boots a DSP program and talks to it through two mailboxes. The 3DS has no such DSP,
// so this answers the protocol and does the DSP program's mixing on the CPU:
//
// - booting a task immediately runs its init callback;
// - DSPSendCommands2 (JAudio2/dsptask.cpp) sends the number of mails, then the command mails.
//   Once a whole command has arrived, the DSP replies 0xF355xxxx, where xxxx is the top half
//   of the first command mail, and calls the task's request callback. Frame sync commands
//   reply 0xF355FF00 instead ("frame done");
// - the 2-channel frame sync (DsyncFrame2ch: 0x82, subframes, mixer level; left buffer;
//   right buffer) mixes the 64 channels of JASDsp::CH_BUF (layout in JASDSPInterface.cpp)
//   into the two buffers, subframes * 80 samples each, which JASDriver::readDspBuffer then
//   interleaves for the audio interface (ai.cpp).
//
// Replies are delivered immediately, because the game spins waiting for them without
// re-enabling interrupts.
//
// Mixing (first version): AFC ADPCM (9-byte blocks, coefficients of JASDsp::DSPADPCM_FILTER)
// and big-endian PCM16 waves, linear resampling by the channel pitch (4096 = one source sample
// per output sample), loops, end and forced stop (mIsFinished), volume ramps per subframe on
// the front left/right buses. Reverb sends, filters and oscillator channels are silent.

#include <revolution/dsp.h>

#include "JSystem/JAudio2/JASDspInterface.hpp"
#include "host.h"
#include "rvl_internal.h"

#include <math.h>

// Start of the dummy ARAM in memory, sent by DsetVARAM (command 0x8E): wave addresses are
// relative to it.
static u32 sVaramBase;

// First byte of the frame sync command sent by DsyncFrame2ch and DsyncFrame4ch.
static const u32 kFrameSyncCommands[] = {0x82};

// Mixer ---------------------------------------------------------------------------------

static const int kChannels = 64;
static const int kSubframeSamples = 80;
static const int kMaxSamples = 10 * kSubframeSamples;

// JASDsp::DSPADPCM_FILTER as s16 pairs (the table is big-endian bytes).
static const s16 kAfcCoefs[16][2] = {
    {0, 0},          {2048, 0},     {0, 2048},     {1024, 1024},   {4096, -2048}, {3584, -1536},
    {3072, -1024},   {4608, -2560}, {4200, -2248}, {4800, -2300},  {5120, -3072}, {2048, -2048},
    {1024, -1024},   {-1024, 1024}, {-1024, 0},    {-2048, 0},
};

// Bus ids written by TChannel::setBusConnect; the others (reverb sends...) are not mixed yet.
static const u16 kBusFrontLeft = 0x0D00, kBusFrontRight = 0x0D60, kBusBackLeft = 0x0DC0, kBusBackRight = 0x0E20;

enum Format { FORMAT_AFC4, FORMAT_PCM16, FORMAT_OTHER, FORMAT_OSC };

// Oscillator channels (JASDsp::TChannel::setOscInfo: no wave, _100 = the waveform, numbered as
// in the Zelda-family DSP programs Dolphin emulates: 0 square, 1 saw, 3 square at 25%). One
// cycle is 64 source samples: JASBank plays them at 16736 Hz for the base key, 261.5 Hz, middle
// C. The other waveforms come from tables inside the DSP program and stay silent.
static bool isOscillator(u16 waveform) {
    return waveform == 0 || waveform == 1 || waveform == 3;
}

static s16 oscillatorSample(u16 waveform, u32 index) {
    u32 phase = index & 63;
    switch (waveform) {
    case 0:
        return phase < 32 ? 0x4000 : -0x4000;
    case 3:
        return phase < 16 ? 0x4000 : -0x4000;
    default:
        return static_cast< s16 >((static_cast< s32 >(phase) - 32) * 0x200);
    }
}

// What the DSP program keeps per channel.
struct Voice {
    Format format;
    s16 hist1, hist2;   // ADPCM history
    s16 block[16];      // decoded samples of the current ADPCM block
    u32 blockIndex;     // which block is in `block` (~0: none)
    s16 s0, s1;         // resampler: current and next source sample
    u32 frac;           // position between s0 and s1, 16.16
    bool ended;
};

static Voice sVoices[kChannels];
static u32 sReportedBuses;

static s16 clamp16(s32 v) {
    return v < -32768 ? -32768 : v > 32767 ? 32767 : static_cast< s16 >(v);
}

static void decodeAfcBlock(const u8* src, Voice& v) {
    s32 scale = 1 << (src[0] >> 4);
    const s16* coef = kAfcCoefs[src[0] & 15];
    for (int i = 0; i < 16; i++) {
        s32 nibble = (i & 1) ? (src[1 + i / 2] & 15) : (src[1 + i / 2] >> 4);
        if (nibble >= 8) {
            nibble -= 16;
        }
        s32 sample = ((nibble * scale) << 11) + coef[0] * v.hist1 + coef[1] * v.hist2;
        v.hist2 = v.hist1;
        v.hist1 = clamp16(sample >> 11);
        v.block[i] = v.hist1;
    }
}

// The playback position lives in the channel, as the DSP program keeps it (the same fields as
// the Zelda-family DSP programs Dolphin emulates): _68 current sample, _70 current ARAM address,
// _74 samples left before the loop end, minus the block size _64. Streams (JASAramStream) read
// _70 and _74 to know which ring buffer blocks were played, and move _74, _110 (loop start) and
// _114 (loop end) to run the ring; without them the title music paused itself after one frame.
static void setCurrentAddress(JASDsp::TChannel& ch, const Voice& v) {
    u32 position = static_cast< u32 >(ch._68);
    ch._70 = static_cast< int >(ch._118 + (v.format == FORMAT_PCM16 ? position * 2 : (position >> 4) * 9));
}

// Next source sample of a channel; loops, or marks the end (then returns silence).
static inline s16 readSample(JASDsp::TChannel& ch, Voice& v) {
    if (v.ended) {
        return 0;
    }
    if (v.format == FORMAT_OSC) {
        return oscillatorSample(ch._100, static_cast< u32 >(ch._68++));
    }
    if (ch._74 + static_cast< int >(ch._64) <= 0) {
        if (ch._102 == 0) {
            v.ended = true;
            return 0;
        }
        ch._68 = static_cast< int >(ch._110);
        ch._74 = static_cast< int >(ch._114 - ch._110) - ch._64;
        v.hist1 = ch._104;
        v.hist2 = ch._106;
        // Most loop starts are inside a block: the stored history is the one at the start of
        // that block (checked against the decoded waves), so the block is decoded again with it.
        v.blockIndex = ~0u;
    }

    const u8* base = reinterpret_cast< const u8* >(sVaramBase + ch._118);
    u32 index = static_cast< u32 >(ch._68++);
    ch._74--;
    if (v.format == FORMAT_PCM16) {
        const u8* p = base + index * 2;
        return static_cast< s16 >((p[0] << 8) | p[1]);
    }

    u32 block = index >> 4;
    if (block != v.blockIndex) {
        decodeAfcBlock(base + block * 9, v);
        v.blockIndex = block;
    }
    return v.block[index & 15];
}

static void startVoice(JASDsp::TChannel& ch, Voice& v) {
    v.format = FORMAT_OTHER;
    if (ch._118 != 0 && ch._64 == 16 && ch._100 == 9) {
        v.format = FORMAT_AFC4;
    } else if (ch._118 != 0 && ch._64 == 1 && ch._100 == 16) {
        v.format = FORMAT_PCM16;
    } else if (ch._118 == 0 && isOscillator(ch._100)) {
        v.format = FORMAT_OSC;
    } else {
        static int sReports;
        if (sReports < 4) {
            sReports++;
            hostLog("dsp: channel format not mixed yet (block %u samples, %u bytes, address %08lX)\n", ch._64, ch._100,
                    static_cast< unsigned long >(ch._118));
        }
    }
    // ADPCM: setWaveInfo already moved the address and loop points past the skipped samples;
    // PCM: it only stored the start sample.
    if (v.format != FORMAT_PCM16) {
        ch._68 = 0;
    }
    ch._74 = static_cast< int >(ch._114) - ch._68 - ch._64;
    setCurrentAddress(ch, v);
    v.hist1 = v.hist2 = 0;
    v.blockIndex = ~0u;
    v.ended = false;
    v.frac = 0;
    if (v.format != FORMAT_OTHER) {
        v.s0 = readSample(ch, v);
        v.s1 = readSample(ch, v);
    }

    // The first channel starts (audio diagnosis): wave, loop, pitch, volumes and the peak of
    // the first source samples (decoded on a copy of the voice).
    static int sStartReports;
    if (sStartReports < 40) {
        sStartReports++;
        s32 peak = 0;
        if (v.format != FORMAT_OTHER) {
            Voice probe = v;
            JASDsp::TChannel copy = ch;
            for (int i = 0; i < 512 && !probe.ended; i++) {
                s32 s = readSample(copy, probe);
                peak = s < 0 ? (-s > peak ? -s : peak) : (s > peak ? s : peak);
            }
        }
        u16* m = &ch._10[0][0];
        hostLog("dsp: start fmt %d (%u/%u) addr %08lX pos %ld loop %u %lu-%lu pitch %04X auto %u vol %04X pan %04X "
                "bus %04X:%04X %04X:%04X %04X:%04X %04X:%04X peak %ld filter %04X\n",
                v.format, ch._64, ch._100, static_cast< unsigned long >(ch._118), static_cast< long >(ch._68), ch._102,
                static_cast< unsigned long >(ch._110), static_cast< unsigned long >(ch._114), ch.mPitch, ch._58, ch._56, ch._50,
                m[0], m[1], m[4], m[5], m[8], m[9], m[12], m[13], static_cast< long >(peak), ch.mFilterMode);
    }
}

static void mixChannel(JASDsp::TChannel& ch, Voice& v, s32* left, s32* right, int samples, u32 level) {
    if (ch._8 != 0) {
        ch._8 = 0;
        startVoice(ch, v);
    }
    if (ch.mForcedStop != 0 || (v.format == FORMAT_OTHER && ch._118 != 0)) {
        ch.mIsFinished = 1;
        return;
    }
    if (ch.mPauseFlag != 0 || v.format == FORMAT_OTHER) {
        return;
    }

    // Volumes (1.15, ramped from the current to the target value over this frame).
    s32 startL = 0, startR = 0, endL = 0, endR = 0;
    if (ch._58 != 0) {
        // Auto mixer (JASChannel::updateAutoMixer, sounds whose mix config is 0xFFFF): volume
        // _56 (current _54) and pan in the top byte of _50 (0 left .. 127 right), gains in 1.15.
        static s16 sPanCos[128];
        if (sPanCos[0] == 0) {
            for (int p = 0; p < 128; p++) {
                sPanCos[p] = static_cast< s16 >(cosf(static_cast< f32 >(p) / 127.0f * 1.5707963f) * 32767.0f);
            }
        }
        u32 pan = (ch._50 >> 8) & 0x7F;
        s32 gainL = sPanCos[pan], gainR = sPanCos[127 - pan];
        s32 target = static_cast< s16 >(ch._56), current = static_cast< s16 >(ch._54);
        ch._54 = ch._56;
        startL = (current * gainL) >> 15, endL = (target * gainL) >> 15;
        startR = (current * gainR) >> 15, endR = (target * gainR) >> 15;
    }
    for (int bus = 0; bus < 6 && ch._58 == 0; bus++) {
        u16* mixer = &ch._10[0][0] + bus * 4;
        u16 id = mixer[0];
        s32 target = static_cast< s16 >(mixer[1]), current = static_cast< s16 >(mixer[2]);
        mixer[2] = mixer[1];
        if (id == kBusFrontLeft || id == kBusBackLeft) {
            startL += current, endL += target;
        } else if (id == kBusFrontRight || id == kBusBackRight) {
            startR += current, endR += target;
        } else if (id != 0 && target != 0 && !(sReportedBuses & (1u << (bus + 8)))) {
            sReportedBuses |= 1u << (bus + 8);
            hostLog("dsp: bus %04X (mixer %d) not mixed yet\n", id, bus);
        }
    }
    u32 step = static_cast< u32 >(ch.mPitch) << 4;
    // The ramps advance by addition, in 16.8: the ARM11 has no divide instruction, and a division
    // per sample of each voice was most of the mixing's cost at 268 MHz.
    s32 volL = startL << 8, volR = startR << 8;
    const s32 rampL = ((endL - startL) << 8) / samples, rampR = ((endR - startR) << 8) / samples;
    s32 s0 = v.s0, s1 = v.s1;
    u32 frac = v.frac;
    for (int i = 0; i < samples; i++) {
        s32 sample = s0 + (((s1 - s0) * static_cast< s32 >(frac >> 1)) >> 15);
        left[i] += (sample * (volL >> 8)) >> 15;
        right[i] += (sample * (volR >> 8)) >> 15;
        volL += rampL;
        volR += rampR;
        frac += step;
        while (frac >= 0x10000) {
            frac -= 0x10000;
            s0 = s1;
            s1 = readSample(ch, v);
        }
        if (v.ended && s0 == 0 && s1 == 0) {
            break;
        }
    }
    v.s0 = static_cast< s16 >(s0);
    v.s1 = static_cast< s16 >(s1);
    v.frac = frac;
    // The streams read the position between subframes only.
    setCurrentAddress(ch, v);
    if (v.ended) {
        ch.mIsFinished = 1;
    }
    (void)level;
}

// The frame being mixed. Like the DSP program, subframes are mixed one at a time: the first when
// the frame sync arrives, each next one once the game has updated the channels for it (the
// release halt of the last channel group, sent at the end of JASDSPChannel::updateAll), each
// followed by its "subframe done" mail. Streams rely on it: JASAramStream prepares its loop
// jump when it is less than 400 samples away, which a whole 560-sample frame could skip (the
// file select music then stopped at its loop point).
static u32 sFrameLeft, sFrameRight, sFrameLevel, sFrameSubframes, sFrameNext;

// CPU time of the mixing since the last "frames mixed" line.
static unsigned long long sMixTicks;

namespace {
    struct MixTimer {
        MixTimer() : mStart(hostGetTick()) {}
        ~MixTimer() { sMixTicks += hostGetTick() - mStart; }
        unsigned long long mStart;
    };
};  // namespace

static void mixSubframe(u32 index) {
    MixTimer timer;
    const int samples = kSubframeSamples;
    // Mixer level (DSP_MIXERLEVEL = 4096 * the DSP level, dspproc.cpp): 0x1000 is unity gain (the game
    // sends about 0x3999; read as 0x4000 = 1 the output was four times too quiet).
    u32 level = sFrameLevel;

    static s32 sLeft[kSubframeSamples], sRight[kSubframeSamples];
    for (int i = 0; i < samples; i++) {
        sLeft[i] = sRight[i] = 0;
    }
    int active = 0;
    if (JASDsp::CH_BUF != nullptr && sVaramBase != 0) {
        for (int c = 0; c < kChannels; c++) {
            JASDsp::TChannel& ch = JASDsp::CH_BUF[c];
            if (ch.mIsActive != 0 && ch.mIsFinished == 0) {
                active++;
                mixChannel(ch, sVoices[c], sLeft, sRight, samples, level);
            }
        }
    }

    // About every 5 seconds: frames mixed and the most channels playing at once.
    static u32 sFrames, sMostActive;
    if (active > static_cast< int >(sMostActive)) {
        sMostActive = active;
    }
    if (index == 0 && ++sFrames % 300 == 1) {
        hostLog("dsp: %lu ms of mixing\n", static_cast< unsigned long >(sMixTicks * 1000 / hostGetTickFrequency()));
        sMixTicks = 0;
        hostLog("dsp: %lu frames mixed, up to %lu channels, %lu subframes, level %04lX\n", static_cast< unsigned long >(sFrames),
                static_cast< unsigned long >(sMostActive), static_cast< unsigned long >(sFrameSubframes),
                static_cast< unsigned long >(level));
        sMostActive = 0;
    }

    s16* outL = reinterpret_cast< s16* >(sFrameLeft) + index * kSubframeSamples;
    s16* outR = reinterpret_cast< s16* >(sFrameRight) + index * kSubframeSamples;
    for (int i = 0; i < samples; i++) {
        outL[i] = clamp16(static_cast< s32 >((static_cast< s64 >(sLeft[i]) * level) >> 12));
        outR[i] = clamp16(static_cast< s32 >((static_cast< s64 >(sRight[i]) * level) >> 12));
    }
}

// Mails of the command being received.
static u32 sMails[8];
static u32 sMailCount;

static DSPTaskInfo* sTask;
static u32 sReply;
static bool sReplyPending;
static u32 sMailsLeft;
static bool sExpectCount = true;
static u32 sCommand;
static bool sHaveCommand;

static void reply(u32 mail) {
    sReply = mail;
    sReplyPending = true;

    if (sTask != nullptr && sTask->req_cb != nullptr) {
        sTask->req_cb(sTask);
    }
}

// With the 96 MB of an Old 3DS the mixing, 18% of the CPU in Gateway Galaxy (CIA 0.16.4), runs on
// the second core, as it runs on the Wii's DSP: the game hears "subframe done" by interrupt once
// it is over, and JASAudioThread touches the channels only after that.
static void* sMixSemaphore;
static volatile u32 sMixIndex;
static bool sMixReady, sMixBusy;
static unsigned long long sMixStartTick;
// Where the second core is: 0 not started, 1 waiting, 2 mixing, 3 mixed. CIA 0.16.8 stayed silent
// with no subframe ever mixed: rvlDSPWatchdog gives the work back to the first core.
static volatile u32 sMixStage;
static volatile bool sMixAbandoned;

static void mixWorker(void*) {
    sMixStage = 1;
    while (true) {
        hostSemaphoreWait(sMixSemaphore);
        if (sMixAbandoned) {
            return;
        }
        sMixStage = 2;
        mixSubframe(sMixIndex);
        sMixStage = 3;
        if (sMixAbandoned) {
            return;
        }
        hostInterruptRaise(HOST_IRQ_DSP);
    }
}

static bool mixOnSecondCore() {
    static bool sTried;
    if (!sTried) {
        sTried = true;
        if (hostLowMemory()) {
            sMixSemaphore = hostSemaphoreCreate();
            sMixReady = hostCore1ThreadCreate(mixWorker, nullptr, 0x8000) != 0;
            hostLog("dsp: mixing on the %s core\n", sMixReady ? "second" : "first");
        }
    }
    return sMixReady;
}

static void startMix(u32 index) {
    sMixIndex = index;
    sMixBusy = true;
    sMixStartTick = hostGetTick();
    hostSemaphoreSignal(sMixSemaphore);
}

void rvlDSPInterrupt() {
    if (!sMixBusy) {
        return;
    }
    sMixBusy = false;
    reply(0xF355FF00);
}

// With every audio interrupt: a subframe the second core has not finished in 200 ms is mixed here,
// and so is all the rest.
void rvlDSPWatchdog() {
    if (!sMixBusy || hostGetTick() - sMixStartTick < hostGetTickFrequency() / 5) {
        return;
    }
    hostLog("dsp: second core not answering (stage %lu), mixing on the first core\n", static_cast< unsigned long >(sMixStage));
    sMixAbandoned = true;
    sMixReady = false;
    sMixBusy = false;
    mixSubframe(sMixIndex);
    reply(0xF355FF00);
}

extern "C" {

void DSPInit(void) {
}

void __DSP_boot_task(DSPTaskInfo* pTask) {
    sTask = pTask;
    if (pTask->init_cb != nullptr) {
        pTask->init_cb(pTask);
    }
}

// Bit 31 set means "a mail is waiting". The boot handshake waits for a first mail.
u32 DSPCheckMailFromDSP(void) {
    return sReplyPending || sTask != nullptr ? 0x80000000 | sReply : 0;
}

u32 DSPReadMailFromDSP(void) {
    sReplyPending = false;
    return sReply;
}

// Zero means the DSP has read the previous mail.
u32 DSPCheckMailToDSP(void) {
    return 0;
}

void DSPSendMailToDSP(u32 mail) {
    if (sExpectCount) {
        sMailsLeft = mail == 0 ? 1 : mail;
        sExpectCount = false;
        sHaveCommand = false;
        sMailCount = 0;
        return;
    }

    if (!sHaveCommand) {
        sCommand = mail;
        sHaveCommand = true;
    }
    if (sMailCount < sizeof(sMails) / sizeof(sMails[0])) {
        sMails[sMailCount++] = mail;
    }

    if (--sMailsLeft == 0) {
        sExpectCount = true;

        u32 opcode = sCommand >> 24;
        bool frameSync = false;
        for (u32 i = 0; i < sizeof(kFrameSyncCommands) / sizeof(kFrameSyncCommands[0]); i++) {
            frameSync |= opcode == kFrameSyncCommands[i];
        }
        bool mixed = false;
        if (opcode == 0x82 && sMailCount >= 3) {
            sFrameLevel = sMails[0] & 0xFFFF;
            sFrameSubframes = (sMails[0] >> 16) & 0xFF;
            if (sFrameSubframes * kSubframeSamples > kMaxSamples) {
                sFrameSubframes = kMaxSamples / kSubframeSamples;
            }
            sFrameLeft = sMails[1];
            sFrameRight = sMails[2];
            sFrameNext = 0;
            if (sFrameSubframes != 0) {
                if (mixOnSecondCore()) {
                    startMix(sFrameNext++);
                } else {
                    mixSubframe(sFrameNext++);
                    mixed = true;
                }
            }
        }
        if (opcode == 0x8E && sMailCount >= 2) {
            sVaramBase = sMails[1];
            hostLog("dsp: ARAM at %08lX\n", static_cast< unsigned long >(sVaramBase));
        }

        if (opcode < 0x80) {
            // Release halt (DSPReleaseHalt2: group, active channel map): sent after each group of
            // 16 channels is updated; no reply is expected. After the last group (3) the next
            // subframe is mixed with the updated channels.
            if ((sCommand >> 16) == 3 && sFrameNext < sFrameSubframes) {
                if (mixOnSecondCore()) {
                    startMix(sFrameNext++);
                } else {
                    mixSubframe(sFrameNext++);
                    reply(0xF355FF00);
                }
            }
        } else if (frameSync) {
            // One "subframe done" per subframe: JASAudioThread counts them down (updateDSP after
            // each, finishDSPFrame, which starts the next frame, after the last); this is the
            // first one, the others follow the updates.
            if (mixed || sFrameSubframes == 0) {
                reply(0xF355FF00);
            }
        } else {
            reply(0xF3550000 | (sCommand >> 16));
        }
    }
}

void DSPAssertInt(void) {
}

}  // extern "C"
