// Debug options, read at startup from sdmc:/smg/debug.txt (one "name=value" per line):
//
//   turbo=1300            after the first frame and until retrace 1300: skip idle time (the next
//                         retrace comes as soon as every game thread waits) and draw nothing
//   press=1290:A,1600:A   hold a 3DS button for a few retraces starting at these retraces
//                         (A B X Y L R START SELECT UP DOWN LEFT RIGHT)
//   logframe=1510         log every draw of this GX frame
//   capture=1510          write this GX frame to sdmc:/smg/capture.bin;
//   capture=tev           or the first frame drawing a scene (1000 draws or more)
//   shot=950,1250         save the top screen of the first frame shown at or after these
//                         retraces to sdmc:/smg/shot_<retrace>.bin (16-byte header, tiled RGBA8);
//   shot=tev              or the first frame drawing a scene, to shot_tev.bin
//   shot=f350             or the 350th frame drawn, to shot_f350.bin
//   lcache=0              draw display lists command by command (no display list cache)
//   profile=1             time the GX emulation stages (logged with every "GX frame" line)
//   drawdist=20000        drop 3D draws farther than this from the camera (depth-writing ones only)
//   audio=0               no sound output (on by default)
//   j3dskip=1             A/B timing: J3D model packets are not drawn at all (native renderer budget)
//   gxskip=1              A/B timing: no draws sent to the GPU (2: draws not even prepared,
//                         3: only culled, 4: culled and their setups built)
//   sky=0                 sky actors not drawn (the clear colour shows instead, as in Wii stages
//                         without one; the Wii sky models cost most of a frame)
//   air=0                 air actors (atmosphere) are not drawn
//   audiodump=10          write the first 10 seconds of sound output to sdmc:/smg/audio.wav
//   vcache=0              decode every draw again each frame (no vertex cache, for A/B checks)
//   native=1              native material diagnostics (the .n3d path is always used on 3DS)
//   native=2              diagnostic: native materials use only their first texture
//   native=3              diagnostic: native=2 with alpha testing disabled
//   native=4              diagnostic: native=2 with the alpha comparison reversed
//   envskip=0             per-vertex texture matrices outside the shader palette applied on the CPU every
//                         frame (default 1: a fixed model-space mapping, decoded once and cached)
//   stage=EggStarGalaxy:1 when the boot screens end, start this stage and scenario instead of
//                         the title (3DS-only hook in GameSequenceProgress::endScene); the
//                         galaxy's opening camera is skipped (GameScene::isValidScenarioOpeningCamera)
//   opening=1             with stage=: play the opening camera all the same
//   demo=grandstar1       with stage=AstroGalaxy:4: play the story's demos after the first Grand
//                         Star (the flight around the observatory, then Rosalina), as on the way
//                         back from Gateway Galaxy (StorySequenceExecutor::moveGalaxy)
//   farclip=300           actors (planets included) clip past this many metres from the camera;
//                         the Wii keeps "far max" actors out to the camera's 8 km (0: Wii distances)
//
// Without the file every option is off.

#include "host.h"
#include "showcase.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <3ds.h>

#define MAX_PRESSES 32
#define MAX_SHOTS 16
#define PRESS_LENGTH 6

static unsigned long sTurbo;
// Turbo starts once the game has drawn its first frame: during start-up, objects register
// retrace callbacks before they are fully set up, and early retraces would hit that window.
static volatile int sTurboArmed;
static unsigned long sLogFrame;
static unsigned long sCaptureFrame;
static unsigned long sCaptureDrawn;  // "capture=f420": the 420th frame drawn
static int sProfile = SMG_SHOWCASE_PROFILE;
static unsigned long sDrawDistance;  // "drawdist=20000": 3D draws farther away are dropped
static int sAudio = 1;           // "audio=0": no sound output
static int sJ3dSkip;  // "j3dskip=1"
static int sGxSkip;  // "gxskip=1/2": A/B timing of the GX stages
static int sSky = 1;             // "sky=0": sky actors not drawn
static int sAir = 1;             // "air=0": air actors are not drawn
static unsigned long sAudioDump;  // "audiodump=10": the first 10 seconds of sound to sdmc:/smg/audio.wav
static int sVertexCache = 1;
static int sEnvSkip = 1;
static int sNative = 1;
static int sListCache = 1;
static int sGpuPath = 1;
static int sMerge = 1;
static int sGpuKick = 1;
static int sCpuClip;
static int sFarClip = -1;  // the default: see hostDebugFarClip
static unsigned long sGpuOffFrame;  // "gpu=f350": the CPU path from the 350th frame drawn
static unsigned long sShotFrame, sFramesDrawn;
static int sPressCount;
static unsigned long sPressAt[MAX_PRESSES];
static unsigned long sPressKey[MAX_PRESSES];
static int sShotCount;
static int sCaptureOnTev, sShotOnTev;  // "tev" values of capture= and shot=
static int sTevSeen, sTevShotDue, sTevCaptureDue;
static int sShotsTaken;
static unsigned long sShotAt[MAX_SHOTS];
static char sStage[64];
static int sOpening;  // "opening=1": the opening camera of a debug stage is played
static int sStageDirect;  // a stage started by stage= or by a showcase build
static long sStageScenario = 1;
static char sStartDemo[32];  // demo=

static unsigned long keyByName(const char* name) {
    static const struct {
        const char* name;
        unsigned long key;
    } kKeys[] = {
        {"A", HOST_KEY_A},         {"B", HOST_KEY_B},           {"X", HOST_KEY_X},         {"Y", HOST_KEY_Y},
        {"L", HOST_KEY_L},         {"R", HOST_KEY_R},           {"START", HOST_KEY_START}, {"SELECT", HOST_KEY_SELECT},
        {"UP", HOST_KEY_DUP},      {"DOWN", HOST_KEY_DDOWN},    {"LEFT", HOST_KEY_DLEFT},  {"RIGHT", HOST_KEY_DRIGHT},
    };
    for (unsigned i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++) {
        if (strcasecmp(name, kKeys[i].name) == 0) {
            return kKeys[i].key;
        }
    }
    return 0;
}

static void parsePresses(char* list) {
    for (char* entry = strtok(list, ","); entry != NULL && sPressCount < MAX_PRESSES; entry = strtok(NULL, ",")) {
        char* colon = strchr(entry, ':');
        if (colon == NULL) {
            continue;
        }
        *colon = '\0';
        unsigned long key = keyByName(colon + 1);
        if (key != 0) {
            sPressAt[sPressCount] = strtoul(entry, NULL, 10);
            sPressKey[sPressCount] = key;
            sPressCount++;
        }
    }
}

static void parseStage(char* value) {
    char* colon = strchr(value, ':');
    if (colon != NULL) {
        *colon = '\0';
        sStageScenario = strtol(colon + 1, NULL, 10);
    }
    snprintf(sStage, sizeof(sStage), "%s", value);
}

void hostDebugInit(void) {
    // A showcase build's start stage (showcase.h); debug.txt can still override it.
    static char sBuiltIn[] = SMG_START_STAGE;
    if (sBuiltIn[0] != '\0') {
        parseStage(sBuiltIn);
        sStageDirect = 1;
    }
    snprintf(sStartDemo, sizeof(sStartDemo), "%s", SMG_START_DEMO);
#if SMG_SHOWCASE
    hostLog("debug: showcase build ignores sdmc:/smg/debug.txt\n");
    return;
#endif

    FILE* file = fopen("sdmc:/smg/debug.txt", "r");
    if (file == NULL) {
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        char* eq = strchr(line, '=');
        if (eq == NULL || line[0] == '#') {
            continue;
        }
        *eq = '\0';
        char* value = eq + 1;
        if (strcmp(line, "turbo") == 0) {
            sTurbo = strtoul(value, NULL, 10);
        } else if (strcmp(line, "logframe") == 0) {
            sLogFrame = strtoul(value, NULL, 10);
        } else if (strcmp(line, "capture") == 0) {
            sCaptureOnTev = strcmp(value, "tev") == 0;
            if (value[0] == 'f') {
                sCaptureDrawn = strtoul(value + 1, NULL, 10);
            } else {
                sCaptureFrame = strtoul(value, NULL, 10);
            }
        } else if (strcmp(line, "press") == 0) {
            parsePresses(value);
        } else if (strcmp(line, "lcache") == 0) {
            sListCache = (int)strtol(value, NULL, 10);
        } else if (strcmp(line, "gpu") == 0) {
            if (value[0] == 'f') {
                sGpuOffFrame = strtoul(value + 1, NULL, 10);
            } else {
                sGpuPath = (int)strtol(value, NULL, 10);
            }
        } else if (strcmp(line, "cpuclip") == 0) {
            sCpuClip = (int)strtol(value, NULL, 10);
        } else if (strcmp(line, "gpukick") == 0) {
            sGpuKick = (int)strtol(value, NULL, 10);
        } else if (strcmp(line, "farclip") == 0) {
            sFarClip = (int)strtol(value, NULL, 10);
        } else if (strcmp(line, "merge") == 0) {
            sMerge = (int)strtol(value, NULL, 10);
        } else if (strcmp(line, "native") == 0) {
            sNative = (int)strtol(value, NULL, 10);
        } else if (strcmp(line, "water") == 0) {
        } else if (strcmp(line, "envskip") == 0) {
            sEnvSkip = (int)strtol(value, NULL, 10);
        } else if (strcmp(line, "vcache") == 0) {
            sVertexCache = (int)strtol(value, NULL, 10);
        } else if (strcmp(line, "drawdist") == 0) {
            sDrawDistance = strtoul(value, NULL, 10);
        } else if (strcmp(line, "air") == 0) {
            sAir = (int)strtol(value, NULL, 10);
        } else if (strcmp(line, "j3dskip") == 0) {
            sJ3dSkip = (int)strtol(value, NULL, 10);
        } else if (strcmp(line, "gxskip") == 0) {
            sGxSkip = (int)strtol(value, NULL, 10);
        } else if (strcmp(line, "sky") == 0) {
            sSky = (int)strtol(value, NULL, 10);
        } else if (strcmp(line, "audio") == 0) {
            sAudio = (int)strtol(value, NULL, 10);
        } else if (strcmp(line, "audiodump") == 0) {
            sAudioDump = strtoul(value, NULL, 10);
        } else if (strcmp(line, "profile") == 0) {
            sProfile = (int)strtol(value, NULL, 10);
        } else if (strcmp(line, "stage") == 0) {
            parseStage(value);
            sStageDirect = 1;
        } else if (strcmp(line, "demo") == 0) {
            snprintf(sStartDemo, sizeof(sStartDemo), "%s", value);
        } else if (strcmp(line, "opening") == 0) {
            sOpening = (int)strtol(value, NULL, 10);
        } else if (strcmp(line, "shot") == 0 && strcmp(value, "tev") == 0) {
            sShotOnTev = 1;
        } else if (strcmp(line, "shot") == 0) {
            for (char* entry = strtok(value, ","); entry != NULL && sShotCount < MAX_SHOTS; entry = strtok(NULL, ",")) {
                if (entry[0] == 'f') {
                    sShotFrame = strtoul(entry + 1, NULL, 10);  // "f350": the 350th frame drawn
                } else {
                    sShotAt[sShotCount++] = strtoul(entry, NULL, 10);
                }
            }
        }
    }
    fclose(file);

    hostLog("debug: turbo %lu, %d presses, log frame %lu, stage '%s' %ld\n", sTurbo, sPressCount, sLogFrame, sStage, sStageScenario);
}

// Retraces generated by skipping idle time (thread.c), for the turbo log line.
volatile unsigned long gHostDebugIdleSkips;

// Retrace of the last frame drawn, for the stall watchdog.
static volatile unsigned long sLastFrameVBlank;
extern int rvlGXDebugGetFramePhase(void);

// Keep recent progress in RAM and write it only if the watchdog detects a stall. This avoids
// changing the timing of every GX command with per-command SD logging.
#define DEBUG_TRACE_CAPACITY 256
typedef struct {
    volatile unsigned long sequence;
    const char* event;
    unsigned long a, b, c;
} DebugTraceEntry;
static DebugTraceEntry sDebugTrace[DEBUG_TRACE_CAPACITY];
static volatile unsigned long sDebugTraceSequence;

void hostDebugTrace(const char* event, unsigned long a, unsigned long b, unsigned long c) {
    unsigned long sequence = sDebugTraceSequence + 1;
    DebugTraceEntry* entry = &sDebugTrace[sequence % DEBUG_TRACE_CAPACITY];
    entry->sequence = 0;
    entry->event = event;
    entry->a = a;
    entry->b = b;
    entry->c = c;
    __sync_synchronize();
    entry->sequence = sequence;
    sDebugTraceSequence = sequence;
}

static void dumpDebugTrace(void) {
    unsigned long last = sDebugTraceSequence;
    unsigned long first = last > DEBUG_TRACE_CAPACITY ? last - DEBUG_TRACE_CAPACITY + 1 : 1;
    for (unsigned long sequence = first; sequence <= last; sequence++) {
        DebugTraceEntry* entry = &sDebugTrace[sequence % DEBUG_TRACE_CAPACITY];
        unsigned long before = entry->sequence;
        if (before == sequence) {
            __sync_synchronize();
            const char* event = entry->event;
            unsigned long a = entry->a, b = entry->b, c = entry->c;
            __sync_synchronize();
            if (entry->sequence == before) {
                hostLog("trace #%lu %s %lu %lu %lu\n", sequence, event != NULL ? event : "?", a, b, c);
            }
        }
    }
}

// Called once when the game has drawn nothing for STALL_VBLANKS retraces. Kept out of line so
// that a debugger can stop on it: "break hostDebugStalled" then "thread apply all bt" shows
// what every game thread is doing (Azahar's GDB stub cannot interrupt a running game).
__attribute__((noinline)) void hostDebugStalled(unsigned long vblanks) {
    static const char* const kPhases[] = {"between frames", "begin render", "draw", "end render", "update", "calc animation",
                                         "screen preserver", "end frame", "wait for retrace", "endGX setup", "endGX console draw",
                                         "endGX assertion flush", "endGX GXFlush", "endGX returned", "framebuffer copy", "combination ratio"};
    int phase = rvlGXDebugGetFramePhase();
    const char* name = phase >= 0 && phase < (int)(sizeof(kPhases) / sizeof(kPhases[0])) ? kPhases[phase] : "unknown";
    hostLog("debug: no frame drawn for %lu retraces (retrace %lu), game phase %d (%s)\n", vblanks, hostVBlankCount(), phase, name);
    extern void rvlDebugLogThreads(void);
    rvlDebugLogThreads();
    dumpDebugTrace();
}

#define STALL_VBLANKS 300

void hostDebugCheckStall(void) {
    static unsigned long sReportedFor = ~0ul;
    unsigned long last = sLastFrameVBlank;
    if (sTurboArmed && hostVBlankCount() - last > STALL_VBLANKS && sReportedFor != last) {
        sReportedFor = last;
        hostDebugStalled(hostVBlankCount() - last);
    }
}

void hostDebugFrameDrawn(void) {
    static unsigned long sFrames;
    sTurboArmed = 1;
    sLastFrameVBlank = hostVBlankCount();
    sFrames++;
    sFramesDrawn = sFrames;
    if (sTurbo != 0 && sFrames % 100 == 0) {
        hostLog("debug: frame %lu, retrace %lu, idle skips %lu\n", sFrames, hostVBlankCount(), gHostDebugIdleSkips);
    }
}

int hostDebugTurbo(void) {
    return sTurboArmed && hostVBlankCount() < sTurbo;
}

unsigned long hostDebugLogFrame(void) {
    return sLogFrame;
}

unsigned long hostDebugCaptureFrame(void) {
    return sCaptureFrame;
}

int hostDebugVertexCache(void) {
    return sVertexCache;
}

int hostDebugEnvSkip(void) {
    return sEnvSkip;
}

int hostDebugCpuClip(void) {
    return sCpuClip;
}

int hostDebugGpuKick(void) {
    return sGpuKick;
}

int hostDebugFarClip(void) {
    // The Wii's distances (0), but with the 96 MB of an Old 3DS.
    return sFarClip >= 0 ? sFarClip : hostLowMemory() ? 300 : 0;
}

int hostDebugMerge(void) {
    return sMerge;
}

int hostDebugGpuPath(void) {
    if (sGpuOffFrame != 0 && sFramesDrawn + 1 >= sGpuOffFrame) {
        return 0;
    }
    return sGpuPath;
}

int hostDebugListCache(void) {
    return sListCache;
}

unsigned long hostDebugShotFrameDue(void) {
    if (sShotFrame != 0 && sFramesDrawn >= sShotFrame) {
        unsigned long frame = sShotFrame;
        sShotFrame = 0;
        return frame;
    }
    return 0;
}

int hostDebugProfile(void) {
    return sProfile;
}

unsigned long hostDebugDrawDistance(void) {
    return sDrawDistance;
}

int hostDebugJ3dSkip(void) {
    return sJ3dSkip;
}

int hostDebugGxSkip(void) {
    return sGxSkip;
}

int hostDebugSky(void) {
    return sSky;
}

int hostDebugAir(void) {
    return sAir;
}

int hostDebugAudio(void) {
    return sAudio;
}

unsigned long hostDebugAudioDump(void) {
    return sAudioDump;
}

int hostDebugStartStage(const char** pName, long* pScenario) {
    if (sStage[0] == '\0') {
        return 0;
    }
    *pName = sStage;
    *pScenario = sStageScenario;
    return 1;
}

const char* hostDebugStartDemo(void) {
    return sStartDemo[0] != '\0' ? sStartDemo : NULL;
}

int hostDebugSkipOpeningCamera(void) {
    return sStageDirect && !sOpening;
}

// The shot and capture are taken at the first frame that draws a scene (owner's request).
#define TEV_WAIT_MS 0

static unsigned long long sTevTick;

void hostDebugTevSeen(void) {
    if (!sTevSeen) {
        sTevSeen = 1;
        hostLog("debug: first scene frame, free %lu KB heap, %lu KB linear\n", hostGetFreeMemory() / 1024,
                (unsigned long)linearSpaceFree() / 1024);
        hostLogLoadProfile();
        sTevTick = hostGetTick();
        sTevShotDue = sShotOnTev;
        sTevCaptureDue = sCaptureOnTev;
    }
}

static int tevWaitOver(void) {
    return hostGetTick() - sTevTick >= hostGetTickFrequency() / 1000 * TEV_WAIT_MS;
}

int hostDebugCaptureNow(void) {
    if (sCaptureDrawn != 0 && sFramesDrawn + 1 >= sCaptureDrawn) {
        sCaptureDrawn = 0;
        return 1;
    }
    if (!sTevCaptureDue || !tevWaitOver()) {
        return 0;
    }
    sTevCaptureDue = 0;
    return 1;
}

int hostDebugTevShotDue(void) {
    if (!sTevShotDue || !tevWaitOver()) {
        return 0;
    }
    sTevShotDue = 0;
    return 1;
}

unsigned long hostDebugShotDue(void) {
    // Shots are listed in increasing order and taken one per shown frame.
    if (sShotsTaken < sShotCount && hostVBlankCount() >= sShotAt[sShotsTaken]) {
        return sShotAt[sShotsTaken++];
    }
    return 0;
}

unsigned long hostDebugPressedKeys(void) {
    unsigned long now = hostVBlankCount();
    unsigned long keys = 0;
    for (int i = 0; i < sPressCount; i++) {
        if (now >= sPressAt[i] && now < sPressAt[i] + PRESS_LENGTH) {
            keys |= sPressKey[i];
        }
    }
    return keys;
}

int hostDebugNative(void) {
    return sNative;
}
