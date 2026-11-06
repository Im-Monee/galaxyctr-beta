// ##############################################################################################
// # The 3DS renderer (citro3d)                                                                 #
// #                                                                                            #
// # Everything the game draws reaches the GPU here, through host.h. The game world             #
// # (platform/3ds/gx) has already turned the Wii's draws into the 3DS GPU's terms: vertices,   #
// # combiner stages and pixel state. Two kinds of draws arrive: vertices already in clip       #
// # space, made on the CPU (layouts, particles, effects: hostGfxDraw), and the native models,  #
// # whose vertices stay in model space and are transformed, lit and texture-mapped by the      #
// # vertex shader (shaders/gx.v.pica) from the uniforms of HostGfxTransform (hostGfxDrawArena, #
// # hostGfxDrawNativeIndexed).                                                                 #
// #                                                                                            #
// # The render target is the 400x240 top screen on its side (240x400): the Wii's 640x456 frame #
// # buffer coordinates are turned onto it in the shader and in the frame buffer reads below.   #
// # This file also paces the frames (at most 30 a second, fewer when the game runs behind),    #
// # starts the GPU while the frame is still being drawn, makes the Wii's EFB copies and reads, #
// # and keeps the diagnostics of a GPU stall.                                                  #
// ##############################################################################################
#include "host.h"
#include "showcase.h"

#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_shbin.h"

#define DISPLAY_TRANSFER_FLAGS                                                                                         \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                                  \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |                    \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// ##############################################################################################
// # Vertex arena and index buffers                                                             #
// #                                                                                            #
// # All vertices live in one array in linear memory, split into windows of 65536 vertices      #
// # because indices are 16 bits: a draw binds one window. Persistent vertices (models uploaded #
// # once, gx/native.cpp) stay until the arena is full, then all go at the next frame's start   #
// # and are uploaded again; transient ones last one frame. The sizes shrink in the 96 MB of an #
// # Old 3DS.                                                                                   #
// ##############################################################################################
// Vertex arena in linear memory (I-4): windows of 65536 vertices, the reach of 16-bit indices.
// The first windows keep vertices across frames (draw.cpp caches decoded display lists there),
// the last one holds the current frame's other vertices.
#define ARENA_WINDOW 65536
#define ARENA_PERSISTENT_WINDOWS 2
#define ARENA_WINDOWS (ARENA_PERSISTENT_WINDOWS + 1)
// The current frame's vertices take the start of the last window (about 1700 a frame in Good
// Egg); persistent vertices go on after them, to the end of the arena.
#define ARENA_TRANSIENT 16384
#define ARENA_TRANSIENT_FIRST (ARENA_PERSISTENT_WINDOWS * ARENA_WINDOW)
#define ARENA_PERSISTENT_END (ARENA_WINDOWS * ARENA_WINDOW)
// With 96 MB the last window is cut after 20000 persistent vertices. Without them the castle's
// garden filled the arena after 129000 vertices, and emptying it stopped the game for 11.6 s while
// its models uploaded again (CIA 0.16.0, 268 MHz). The smaller index buffers below pay for them.
#define ARENA_LOW_MEMORY_END (ARENA_TRANSIENT_FIRST + ARENA_TRANSIENT + 20000)
static u32 sArenaPersistentEnd;
// Indices of one frame, and indices kept across frames with the persistent vertices.
#define INDEX_BUFFER_SIZE (512 * 1024)
#define PERSISTENT_INDEX_SIZE (768 * 1024)
// With 96 MB: the garden's peak was 245682 persistent indices, and about 1200 others a frame.
#define INDEX_BUFFER_LOW_SIZE (256 * 1024)
#define PERSISTENT_INDEX_LOW_SIZE (400 * 1024)
static u32 sIndexBufferSize, sPersistentIndexSize;
// Internal primitive: a draw of persistent indices (never merged).
#define PRIM_CACHED_INDICES 3

// Command buffer space (in words) kept free for one draw's state changes and the frame end.
#define CMDBUF_RESERVE_WORDS 4096
#define NATIVE_DRAW_BUDGET 2048

// ##############################################################################################
// # Renderer state                                                                             #
// #                                                                                            #
// # What the renderer keeps between draws and frames: the target and the shader, the arena's   #
// # use and the parts of it the CPU wrote since the last cache writeback, counters for the     #
// # log, and the GPU's frame counts (a texture is freed only once the GPU has finished every   #
// # frame that reads it).                                                                      #
// ##############################################################################################
static C3D_RenderTarget* sTarget;
static C3D_RenderTarget* sOutputTarget;
static DVLB_s* sShaderDvlb;
static shaderProgram_s sShader;
static HostGfxModelVertex* sArena;
static u32 sPersistentUsed, sTransientUsed;
static u32 sPersistentDirtyLo = ~0u, sPersistentDirtyHi;  // vertices to write back before the GPU reads them
static u32 sTransientFlushed;
static bool sArenaResetPending;
// Since the last log: persistent arena resets because the vertices or the indices were full,
// and the most persistent vertices and indices in use before a reset.
static unsigned long sArenaFullVertices, sArenaFullIndices, sArenaPeakVertices, sArenaPeakIndices;
// Draws dropped since the last log, by reason.
enum { DROP_SKIPPED_FRAME, DROP_INDEX_BUFFER, DROP_COMMAND_BUFFER, DROP_NATIVE_BUDGET, DROP_TRANSIENT_VERTICES, DROP_REASONS };
static unsigned long sDropReasons[DROP_REASONS];
static unsigned long long sGpuWaitTicks;  // time C3D_FrameBegin waited for the GPU (hostGfxTakeGpuWait)
// GPU time of each finished frame from C3D_FrameEnd on (C3D_GetDrawingTime; kickGpu starts it
// earlier), read when the next frame begins.
static float sGpuDrawMsSum, sGpuDrawMsMax;
static unsigned long sGpuDrawSamples;
static unsigned long long sLastKickTick;  // last start of the GPU during the frame (kickGpu)
static unsigned long sGpuKicks;
static unsigned long long sLastFrameEndTick;
static float sFrameMs = 16.7f;  // hostGfxFrameMs
static float sLoopMs = 16.7f;   // the last loop, at most 100 ms
static unsigned long sGpuBusyFrames;      // frames skipped because the previous Citro3D queue is still running
static unsigned long sArenaEpoch;  // bumped when the persistent windows are emptied
static int sBoundWindow = -1;
static u16* sIndices;
static u32 sIndexCount, sIndicesFlushed;
static u16* sPersistentIndices;
static u32 sPersistentIndexUsed;
static u32 sPersistentIndexDirtyLo = ~0u, sPersistentIndexDirtyHi;
static bool sInFrame;
// EFB reads (hostGfxPeekEfb, hostGfxReadRgb565) since the last frame log.
static unsigned long sEfbReads;
static unsigned long sGpuFrameBeginCount, sGpuFrameEndCount;
// Frames the GPU has finished (all frames ended before the last successful C3D_FrameBegin).
static unsigned long sCompletedFrames;
static u32 sGpuLastP3dTotal, sGpuLastPpfTotal;
static bool sGpuPerfBaseline;

// ##############################################################################################
// # GPU stall reports                                                                          #
// #                                                                                            #
// # When the GPU never finishes a frame, every later frame waits for it: after 30 frames that  #
// # could not begin, reportGpuStall writes the stalled frame's command buffer, its draws and   #
// # the GPU's registers to the SD card (sdmc:/smg/c3d_stall*.bin), to find the draw that hung  #
// # it. With STALL_DRAW_DIAG the frame is also split into small groups of draws, which costs   #
// # time, so it is off in normal builds.                                                       #
// ##############################################################################################
// What the frame being recorded did besides plain draws, kept for the last submitted frame so a
// GPU that never finishes it can be traced back to the operation that stalled it.
typedef struct {
    unsigned long frame;
    u32 draws, indices, transient;
    u32 splits, copies, peeks, textures;
    u32 copyIn, copyInDim, copyOut, copyOutDim, copySize;
    u32 cmdWords;
} FrameOps;
static FrameOps sFrameOps, sSubmittedOps;
static u32* sFrameCmdBase;
static bool sStallReported;

// Until the first GPU stall, a frame's command list is ended every DIAG_GROUP draws, so the
// stalled queue entry names a small group of draws; each draw records where its commands start
// and what its vertices hold, and the report lists the draws of the stalled list.
#define DIAG_GROUP 3  // with the queue's room, the first ~66 draws of a frame
// Off in normal builds: it costs CPU per draw and GX queue entries. 1 to trace a GPU stall.
#define STALL_DRAW_DIAG 0
#define DIAG_RECORDS 2048
typedef struct {
    u32 cmd;                  // (virtual) address of the draw's first command word
    u16 count, window;
    u16 minIndex, maxIndex;
    u8 cpu, primitive;        // cpu: pretransformed (clip-space) vertices
    const u16* indices;
    u16 bad;                  // vertices with a NaN or infinite component
    float maxAbs;             // model draws: largest |x|, |y|, |z|
    float minW, maxRatio;     // pretransformed draws: smallest w, largest |x| / w or |y| / w
} DrawRecord;
static DrawRecord sDrawRecords[DIAG_RECORDS], sSubmittedRecords[DIAG_RECORDS];
static u32 sDrawRecordCount, sSubmittedRecordCount, sDrawsSinceSplit;

// Citro3D's context starts with its GX command queue (citro3d source/internal.h, C3D_Context).
// The queue advances one entry per GPU completion event; an entry whose event never comes
// stalls every later frame, so the stalled entry is the one at lastEntry.
extern gxCmdQueue_s __C3D_Context;
#define GX_QUEUE_ENTRIES 128  // entries of that queue (hostGfxInit; citro3d makes 32)

// Saves the command buffer of the frame the GPU never finished, the GX queue entries and the GPU
// status and PPF transfer registers. (Reading the P3D registers through GSP blocked the program.)
static void reportGpuStall(void) {
    if (sStallReported) {
        return;
    }
    sStallReported = true;
    const FrameOps* o = &sSubmittedOps;
    hostLog("gfx: GPU stall in frame %lu: draws %lu indices %lu transient %lu splits %lu copies %lu peeks %lu new textures %lu, %lu command words at %p phys %08lx\n",
            o->frame, (unsigned long)o->draws, (unsigned long)o->indices, (unsigned long)o->transient, (unsigned long)o->splits,
            (unsigned long)o->copies, (unsigned long)o->peeks, (unsigned long)o->textures, (unsigned long)o->cmdWords, sFrameCmdBase,
            (unsigned long)osConvertVirtToPhys(sFrameCmdBase));
    if (o->copies != 0) {
        hostLog("gfx: GPU stall first copy in %08lx dim %08lx out %08lx dim %08lx size %lu\n", (unsigned long)o->copyIn,
                (unsigned long)o->copyInDim, (unsigned long)o->copyOut, (unsigned long)o->copyOutDim, (unsigned long)o->copySize);
    }

    FILE* file = sFrameCmdBase != NULL ? fopen("sdmc:/smg/c3d_stall.bin", "wb") : NULL;
    if (file != NULL) {
        // CMD2, physical address of the buffer, used words, then the raw PICA command stream.
        u32 header[3] = {0x32444D43, osConvertVirtToPhys(sFrameCmdBase), o->cmdWords};
        fwrite(header, sizeof(u32), 3, file);
        fwrite(sFrameCmdBase, sizeof(u32), o->cmdWords, file);
        fclose(file);
        hostLog("gfx: GPU stall command buffer saved to sdmc:/smg/c3d_stall.bin\n");
    }

    const gxCmdQueue_s* queue = &__C3D_Context;
    if (queue->lastEntry < queue->numEntries && (queue->entries[queue->lastEntry].data[0] & 0xFF) == 0x01) {
        // The stalled command list: its draws, by the address of their first command.
        u32 start = queue->entries[queue->lastEntry].data[1], end = start + queue->entries[queue->lastEntry].data[2];
        u32 listed = 0;
        for (u32 i = 0; i < sSubmittedRecordCount; i++) {
            const DrawRecord* r = &sSubmittedRecords[i];
            if (r->cmd < start || r->cmd >= end) {
                continue;
            }
            if (listed++ >= 80) {
                continue;
            }
            // %g would take printf's deep floating-point path: magnitudes as integers (w in 1/1000).
            hostLog("gfx: GPU stall draw %lu: %s prim %u count %u window %u index %u..%u bad %u max|xyz| %lu min w*1000 %ld max ratio %lu\n",
                    (unsigned long)i, r->cpu ? "cpu" : "model", r->primitive, r->count, r->window, r->minIndex,
                    r->maxIndex, r->bad, (unsigned long)(r->maxAbs < 4.0e9f ? r->maxAbs : 4.0e9f),
                    (long)(r->minW > 2.0e6f ? 2.0e9f : r->minW < -2.0e6f ? -2.0e9f : r->minW * 1000.0f),
                    (unsigned long)(r->maxRatio < 4.0e9f ? r->maxRatio : 4.0e9f));
        }
        // The stalled list's draws: indices and vertices, to replay the vertex shader off the console
        // (the uniforms are in c3d_stall.bin). Persistent data is intact; a frame's transient data
        // may already hold the next frame's.
        FILE* draws = fopen("sdmc:/smg/c3d_stall_draw.bin", "wb");
        if (draws != NULL) {
            for (u32 i = 0; i < sSubmittedRecordCount; i++) {
                const DrawRecord* r = &sSubmittedRecords[i];
                if (r->cmd < start || r->cmd >= end || r->minIndex > r->maxIndex) {
                    continue;
                }
                // DRW2, draw number, command address, index count, window, first and last vertex,
                // primitive, cpu, vertex size; then the indices and the vertices first..last.
                u32 header[10] = {0x32575244, i, r->cmd, r->count, r->window, r->minIndex, r->maxIndex, r->primitive, r->cpu,
                                  sizeof(HostGfxModelVertex)};
                fwrite(header, sizeof(u32), 10, draws);
                fwrite(r->indices, sizeof(u16), r->count, draws);
                fwrite(&sArena[r->window * ARENA_WINDOW + r->minIndex], sizeof(HostGfxModelVertex), r->maxIndex - r->minIndex + 1u, draws);
            }
            fclose(draws);
            hostLog("gfx: GPU stall draws saved to sdmc:/smg/c3d_stall_draw.bin\n");
        }
        hostLog("gfx: GPU stall list %08lx..%08lx holds %lu of the frame's %lu recorded draws\n", (unsigned long)start, (unsigned long)end,
                (unsigned long)listed, (unsigned long)sSubmittedRecordCount);
    }
    hostLog("gfx: GPU stall queue entries %u max %u submitted %u completed %u\n", queue->numEntries, queue->maxEntries,
            queue->curEntry, queue->lastEntry);
    for (u32 i = 0; i < queue->numEntries && i < queue->maxEntries; i++) {
        const u32* d = queue->entries[i].data;
        hostLog("gfx: GPU stall queue[%lu]%s %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n", (unsigned long)i,
                i == queue->lastEntry ? " <" : "", (unsigned long)d[0], (unsigned long)d[1], (unsigned long)d[2],
                (unsigned long)d[3], (unsigned long)d[4], (unsigned long)d[5], (unsigned long)d[6], (unsigned long)d[7]);
    }

    static const struct { u32 addr; u8 words; const char* name; } kRegs[] = {
        {0x400000, 16, "ext 0x00"},  // memory fills, then status at 0x34
        {0x400C00, 12, "ppf"},       // display transfer / texture copy
    };
    for (u32 r = 0; r < sizeof(kRegs) / sizeof(kRegs[0]); r++) {
        u32 v[16] = {0};
        Result result = GSPGPU_ReadHWRegs(kRegs[r].addr, v, kRegs[r].words * 4);
        hostLog("gfx: GPU stall regs %s (0x%06lx) result %08lx: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
                kRegs[r].name, (unsigned long)kRegs[r].addr, (unsigned long)result, (unsigned long)v[0], (unsigned long)v[1],
                (unsigned long)v[2], (unsigned long)v[3], (unsigned long)v[4], (unsigned long)v[5], (unsigned long)v[6],
                (unsigned long)v[7], (unsigned long)v[8], (unsigned long)v[9], (unsigned long)v[10], (unsigned long)v[11],
                (unsigned long)v[12], (unsigned long)v[13], (unsigned long)v[14], (unsigned long)v[15]);
    }
}

// Read libctru's hardware performance log at sparse diagnostic points. Citro3D queues
// frames asynchronously; P3D/PPF progress and the pending top-screen swap separate a
// completed render from a completed display transfer/presentation.
static void logGpuProgress(const char* point) {
    GSPGPU_PerfLog perf = {};
    Result result = GSPGPU_GetPerfLog(&perf);
    if (R_FAILED(result)) {
        hostLog("gfx: GPU %s perf log read failed 0x%08lx\n", point, (unsigned long)result);
        return;
    }

    u32 p3d = perf.entries[GSPGPU_EVENT_P3D].totalDurationUs;
    u32 ppf = perf.entries[GSPGPU_EVENT_PPF].totalDurationUs;
    u32 p3dDelta = sGpuPerfBaseline ? p3d - sGpuLastP3dTotal : 0;
    u32 ppfDelta = sGpuPerfBaseline ? ppf - sGpuLastPpfTotal : 0;
    // Avoid printf's floating-point dtoa path in this recurring diagnostic. On 3DS that
    // path has a deep stack frame; a busy GPU can reach it repeatedly on the shallow game stack.
    unsigned long drawUs = (unsigned long)(C3D_GetDrawingTime() * 1000.0f);
    unsigned long cpuUs = (unsigned long)(C3D_GetProcessingTime() * 1000.0f);
    hostLog("gfx: GPU %s P3D last=%lu total=%lu (+%lu us), PPF last=%lu total=%lu (+%lu us), draw %lu us, CPU %lu us, top pending %d, C3D vblank %lu/%lu\n",
            point, (unsigned long)perf.entries[GSPGPU_EVENT_P3D].lastDurationUs, (unsigned long)p3d,
            (unsigned long)p3dDelta, (unsigned long)perf.entries[GSPGPU_EVENT_PPF].lastDurationUs,
            (unsigned long)ppf, (unsigned long)ppfDelta, drawUs, cpuUs,
            gspIsPresentPending(GSP_SCREEN_TOP), (unsigned long)C3D_FrameCounter(0), (unsigned long)C3D_FrameCounter(1));
    sGpuLastP3dTotal = p3d;
    sGpuLastPpfTotal = ppf;
    sGpuPerfBaseline = true;
}

// ##############################################################################################
// # Frame pacing state                                                                         #
// #                                                                                            #
// # The game updates at every retrace (60 Hz); the port draws at most every second one, and    #
// # fewer while the game runs behind the clock, so that its updates keep the game's speed.     #
// # planNextFrame decides at each frame's end whether the next loop draws.                     #
// ##############################################################################################
static u32 sFrameNumber;
static u32 sClearColor = 0x000000FF;
static bool sBlackScreen;
static unsigned long sLastFrameVBlank = ~0ul;  // vertical blank of the last submitted frame
static unsigned long sLastBeginVBlank = ~0ul;  // vertical blank the last drawn frame began in
static bool sSkipFrame;                        // the current frame is dropped
// The port draws at most 30 frames a second: a frame is drawn only HOST_GFX_FRAME_VBLANKS vertical
// blanks or more after the last drawn one began. The game's update still runs every retrace (60 Hz),
// so it keeps its speed; the frames in between are not drawn (GameSystem::frameLoop).
#define HOST_GFX_FRAME_VBLANKS 2
// The game runs one update per loop, and a loop over a retrace slows the game down: the console's
// game logic alone costs 12-18 ms of the 16.7. While the game runs behind the retrace, frames are
// drawn only every HOST_GFX_LATE_FRAME_VBLANKS (20 a second), so that the cheap undrawn loops catch
// up on the clock (CIA 0.14.5: 17-50 ms loops, the game at 35-100% of its speed).
#define HOST_GFX_LATE_FRAME_VBLANKS 3
// At 268 MHz (96 MB) a drawn loop costs about 110 ms and an undrawn one about 30 in the castle's
// garden: drawn every loop, the game ran at 12-14% of its speed (CIA 0.16.1).
#define HOST_GFX_LATE_FRAME_LOW_VBLANKS 8
// A loop that begins later than this after the vertical blank did not wait for it: it is behind.
// (Waking up after the retrace can include the audio threads' work.)
#define HOST_GFX_LATE_MS 4.0f
static bool sNextFrameDrawn = true;  // hostGfxNextFrameDrawn
static bool sLoopLate;               // the current loop began behind the retrace
// hostPerfDetail: lowered while the game runs under ~88% of its speed, raised back above ~96%.
#define HOST_PERF_DETAIL_MIN 0.4f
static float sPerfDetail = 1.0f;
static u32 sNativeDrawsThisFrame;
static u32 sDrawCallsThisFrame;
static u32 sDroppedDrawsThisFrame;
static bool sNativeBudgetReported;
static void flushPendingDraw(void);
static char sShotPending[64];                  // screenshot to save once the frame is rendered
static char sShotNextFrame[64];                // screenshot of the next frame (hostGfxRequestShot)
static char sShotRequested[64];                // screenshot of the frame being drawn
// Last state sent to the GPU this frame: unchanged parts are not sent again, which keeps the
// command buffer small (citro3d re-emits every setting it is handed).
static HostGfxState sLastState;
static bool sLastStateValid;
// Stencil: the ambient one (hostGfxSetStencilAmbient) and the one the GPU has now.
typedef struct {
    unsigned char enable, func, ref, mask, writeMask, fail, zfail, zpass;
} StencilState;
static StencilState sStencilAmbient, sStencilApplied;
static bool sStencilAppliedValid;
static C3D_Tex sBlankTexture;  // bound to texture units a draw does not use
// Fog LUTs already built, by their GX fog parameters: materials with different fog alternate
// within a frame, and building one costs 129 exponentials and a conversion.
#define FOG_CACHE_SIZE 8
static struct {
    bool valid;
    u32 a, b, shift, c;
    C3D_FogLut lut;
} sFogCache[FOG_CACHE_SIZE];
static u32 sFogCacheNext;
static unsigned long sFogLutBuilds;
// applyFog runs on the renderer thread, whose stack is shared with game code and is relatively
// shallow. Keep the LUT scratch buffers out of its stack frame (even the no-fog path).
static float sFogLutValues[129];
static float sFogLutData[256];

// ##############################################################################################
// # Shader uniforms                                                                            #
// #                                                                                            #
// # The locations of the vertex shader's uniforms, and the transform last sent: a draw sends   #
// # only the vectors that changed (uploadTransform).                                           #
// ##############################################################################################
// Shader uniforms (shaders/gx.v.pica) and what was last sent of them.
enum { MODE_NONE, MODE_PRETRANSFORMED, MODE_MODEL };
static int sMode;  // kind of the draws the shader is set up for
static bool sTransformValid;
static unsigned long sTransformVersion;
static HostGfxTransform sUploaded;
static s8 sLocClip, sLocPalette, sLocTexMtx, sLocTexPost, sLocTexFinal, sLocMaterial, sLocAmbient, sLocMaskMat, sLocMaskAmb,
    sLocMaskLit, sLocSrtg, sLocLights;
static s8 sLocPretransformed, sLocLit, sLocHasLights, sLocNormalize[3], sLocDivide[3], sLocNrmPalette, sLocLightLoop;
static s8 sLocVtxMtx[3], sLocNrmCofactor;

// ##############################################################################################
// # Initialization                                                                             #
// #                                                                                            #
// # citro3d with a larger command buffer and GX queue, the combiner buffer's constant 0.5, the #
// # render target on the top screen, the shader and its vertex layout (HostGfxModelVertex,     #
// # host.h), the arena and the index buffers, and the blank texture for unused units.          #
// ##############################################################################################
void hostGfxInit(void) {
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 8);
    // citro3d's GX queue holds 32 entries a frame (C3D_Init) and panics when full: the GPU
    // starts during the frame (kickGpu) and EFB copies each take entries. Its array is malloc'd
    // (C3D_Fini frees it); the queue is empty and idle here.
    gxCmdEntry_s* entries = malloc(GX_QUEUE_ENTRIES * sizeof(gxCmdEntry_s));
    if (entries != NULL) {
        free(__C3D_Context.entries);
        __C3D_Context.entries = entries;
        __C3D_Context.maxEntries = GX_QUEUE_ENTRIES;
    }
    // The combiner buffer is never updated, so every stage reads its initial colour as
    // GPU_PREVIOUS_BUFFER: 0.5, a second constant the material combiners use (mat.py half).
    C3D_TexEnvBufUpdate(C3D_Both, 0);
    C3D_TexEnvBufColor(0x80808080);
#if SMG_DIAG_CLEAR_ONLY && SMG_DIAG_FIRST_DRAW_ONLY
    sClearColor = 0xFF00FFFF;  // visible magenta: proves the top-screen clear/transfer path
    hostLog("gfx: diagnostic first clear is magenta\n");
#endif
    Result perfResult = GSPGPU_SetPerfLogMode(true);
    hostLog("gfx: GPU performance log enable result 0x%08lx\n", (unsigned long)perfResult);
    if (R_SUCCEEDED(perfResult)) {
        logGpuProgress("baseline");
    }

    sOutputTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(sOutputTarget, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    sTarget = sOutputTarget;

    sShaderDvlb = DVLB_ParseFile((u32*)gx_shbin, gx_shbin_size);
    shaderProgramInit(&sShader);
    shaderProgramSetVsh(&sShader, &sShaderDvlb->DVLE[0]);
    C3D_BindProgram(&sShader);

    // Locations are register numbers minus 0x10: float c0-c95 from 0, integer i0-i3 from 0x60,
    // boolean b0-b15 from 0x68.
    shaderInstance_s* vsh = sShader.vertexShader;
    sLocClip = shaderInstanceGetUniformLocation(vsh, "clip");
    sLocPalette = shaderInstanceGetUniformLocation(vsh, "palette");
    sLocTexMtx = shaderInstanceGetUniformLocation(vsh, "texmtx");
    sLocTexPost = shaderInstanceGetUniformLocation(vsh, "texpost");
    sLocTexFinal = shaderInstanceGetUniformLocation(vsh, "texfinal");
    sLocMaterial = shaderInstanceGetUniformLocation(vsh, "material");
    sLocAmbient = shaderInstanceGetUniformLocation(vsh, "ambient");
    sLocMaskMat = shaderInstanceGetUniformLocation(vsh, "maskmat");
    sLocMaskAmb = shaderInstanceGetUniformLocation(vsh, "maskamb");
    sLocMaskLit = shaderInstanceGetUniformLocation(vsh, "masklit");
    sLocSrtg = shaderInstanceGetUniformLocation(vsh, "srtg");
    sLocLights = shaderInstanceGetUniformLocation(vsh, "lights");
    sLocPretransformed = shaderInstanceGetUniformLocation(vsh, "pretransformed");
    sLocLit = shaderInstanceGetUniformLocation(vsh, "lit");
    sLocHasLights = shaderInstanceGetUniformLocation(vsh, "haslights");
    sLocNormalize[0] = shaderInstanceGetUniformLocation(vsh, "normalize0");
    sLocNormalize[1] = shaderInstanceGetUniformLocation(vsh, "normalize1");
    sLocNormalize[2] = shaderInstanceGetUniformLocation(vsh, "normalize2");
    sLocDivide[0] = shaderInstanceGetUniformLocation(vsh, "divide0");
    sLocDivide[1] = shaderInstanceGetUniformLocation(vsh, "divide1");
    sLocDivide[2] = shaderInstanceGetUniformLocation(vsh, "divide2");
    sLocNrmPalette = shaderInstanceGetUniformLocation(vsh, "nrmpalette");
    sLocNrmCofactor = shaderInstanceGetUniformLocation(vsh, "nrmcofactor");
    sLocVtxMtx[0] = shaderInstanceGetUniformLocation(vsh, "vtxmtx0");
    sLocVtxMtx[1] = shaderInstanceGetUniformLocation(vsh, "vtxmtx1");
    sLocVtxMtx[2] = shaderInstanceGetUniformLocation(vsh, "vtxmtx2");
    sLocLightLoop = shaderInstanceGetUniformLocation(vsh, "lightloop");
    hostLog("gfx: shader uniforms clip %d palette %d lights %d, bools %d..%d, loop %d\n", sLocClip, sLocPalette, sLocLights,
            sLocPretransformed - 0x68, sLocDivide[2] - 0x68, sLocLightLoop - 0x60);

    C3D_AttrInfo* attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);          // position
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 3);          // normal
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4);  // colour
    AttrInfo_AddLoader(attr, 3, GPU_UNSIGNED_BYTE, 4);  // position matrix row
    AttrInfo_AddLoader(attr, 4, GPU_FLOAT, 3);          // texture coordinate generation inputs
    AttrInfo_AddLoader(attr, 5, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attr, 6, GPU_FLOAT, 3);

    sArenaPersistentEnd = hostLowMemory() ? ARENA_LOW_MEMORY_END : ARENA_PERSISTENT_END;
    sArena = linearAlloc(sArenaPersistentEnd * sizeof(HostGfxModelVertex));
    sIndexBufferSize = hostLowMemory() ? INDEX_BUFFER_LOW_SIZE : INDEX_BUFFER_SIZE;
    sPersistentIndexSize = hostLowMemory() ? PERSISTENT_INDEX_LOW_SIZE : PERSISTENT_INDEX_SIZE;
    sIndices = linearAlloc(sIndexBufferSize * sizeof(u16));
    sPersistentIndices = linearAlloc(sPersistentIndexSize * sizeof(u16));
    if (sArena == NULL || sIndices == NULL || sPersistentIndices == NULL) {
        hostFatal("Could not allocate the vertex buffers.");
    }
    C3D_TexInit(&sBlankTexture, 8, 8, GPU_RGBA8);
    memset(sBlankTexture.data, 0xFF, 8 * 8 * 4);
    GSPGPU_FlushDataCache(sBlankTexture.data, 8 * 8 * 4);
}

// ##############################################################################################
// # Textures                                                                                   #
// #                                                                                            #
// # Decoded RGBA8 pixels become a texture in the smallest GPU format that holds them as they   #
// # are, written in the GPU's 8x8 tiles and upside down (the GPU stores the bottom row first). #
// # Textures the converter already wrote in the GPU's layout (ETC1, ETC1A4, LA4) are copied as #
// # they are. A texture released while the GPU may still read it waits in sPendingDeletes      #
// # until the next frame begins.                                                               #
// ##############################################################################################
// Textures released during a frame are deleted once the GPU is done with it.
#define MAX_PENDING_DELETES 4096
static C3D_Tex* sPendingDeletes[MAX_PENDING_DELETES];
static int sPendingDeleteCount;
static bool sPendingDeleteOverflowReported;

// Position of texel (x, y) inside an 8x8 tile: x and y bits interleaved (Morton order).
static inline u32 mortonOffset(u32 x, u32 y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

static GPU_TEXTURE_WRAP_PARAM wrapMode(int wrap) {
    switch (wrap) {
    case 1:
        return GPU_REPEAT;
    case 2:
        return GPU_MIRRORED_REPEAT;
    default:
        return GPU_CLAMP_TO_EDGE;
    }
}

// The smallest GPU format that holds the decoded 0xRRGGBBAA texels as they are: grey images as
// L8 or LA8 (the Wii's I4, I8, IA4, IA8), opaque colour as RGB565 and colour with on/off alpha
// as RGBA5551 (the Wii's CMPR, RGB565 and most RGB5A3 hold no more), anything else as RGBA8.
// The decoded RGBA8 of every texture filled the linear heap in Good Egg (CIA 0.10.14).
static GPU_TEXCOLOR textureFormat(const unsigned long* pPixels, unsigned count) {
    bool grey = true, opaque = true, binaryAlpha = true;
    for (unsigned i = 0; i < count; i++) {
        u32 p = pPixels[i];
        u32 r = p >> 24, g = (p >> 16) & 0xFF, b = (p >> 8) & 0xFF, a = p & 0xFF;
        grey = grey && r == g && g == b;
        opaque = opaque && a == 0xFF;
        binaryAlpha = binaryAlpha && (a == 0 || a == 0xFF);
    }
    if (grey) {
        return opaque ? GPU_L8 : GPU_LA8;
    }
    return opaque ? GPU_RGB565 : binaryAlpha ? GPU_RGBA5551 : GPU_RGBA8;
}

void* hostGfxTextureCreate(const unsigned long* pPixels, unsigned width, unsigned height, unsigned potWidth, unsigned potHeight,
                           int wrapS, int wrapT, int linear, int minLinear) {
    GPU_TEXCOLOR format = textureFormat(pPixels, width * height);
    C3D_Tex* tex = malloc(sizeof(C3D_Tex));
    if (tex == NULL || !C3D_TexInit(tex, potWidth, potHeight, format)) {
        free(tex);
        return NULL;
    }
    sFrameOps.textures++;

    // Texels are stored in 8x8 tiles, Morton order inside a tile (RGBA8 as 0xRRGGBBAA
    // little-endian words, the 16-bit formats as little-endian halfwords, LA8 with the alpha in
    // the low byte). The GPU samples the first row in memory at t = 1 (images are stored
    // bottom-up), while the Wii samples its top row at t = 0, so image row y goes to memory row
    // (potHeight - 1 - y). Seen on the strap screen, a single large texture that was shown upside
    // down and cut.
    memset(tex->data, 0, tex->size);
    u32 tilesPerRow = potWidth / 8;
    for (u32 y = 0; y < height; y++) {
        u32 row = potHeight - 1 - y;
        for (u32 x = 0; x < width; x++) {
            u32 tile = (row / 8) * tilesPerRow + (x / 8);
            u32 index = tile * 64 + mortonOffset(x & 7, row & 7);
            u32 p = pPixels[y * width + x];
            u32 r = p >> 24, g = (p >> 16) & 0xFF, b = (p >> 8) & 0xFF, a = p & 0xFF;
            switch (format) {
            case GPU_L8:
                ((u8*)tex->data)[index] = (u8)r;
                break;
            case GPU_LA8:
                ((u16*)tex->data)[index] = (u16)((r << 8) | a);
                break;
            case GPU_RGB565:
                ((u16*)tex->data)[index] = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                break;
            case GPU_RGBA5551:
                ((u16*)tex->data)[index] = (u16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7));
                break;
            default:
                ((u32*)tex->data)[index] = p;
                break;
            }
        }
    }
    GSPGPU_FlushDataCache(tex->data, tex->size);

    C3D_TexSetWrap(tex, wrapMode(wrapS), wrapMode(wrapT));
    C3D_TexSetFilter(tex, linear ? GPU_LINEAR : GPU_NEAREST, minLinear ? GPU_LINEAR : GPU_NEAREST);
    return tex;
}

void* hostGfxTextureCreateNative(const void* pData, int nativeFormat, unsigned width, unsigned height, int wrapS, int wrapT, int linear,
                                  int minLinear) {
    static const GPU_TEXCOLOR kFormats[] = {GPU_ETC1, GPU_ETC1A4, GPU_LA4};
    C3D_Tex* tex = malloc(sizeof(C3D_Tex));
    if (tex == NULL || !C3D_TexInit(tex, width, height, kFormats[nativeFormat])) {
        free(tex);
        return NULL;
    }
    sFrameOps.textures++;
    memcpy(tex->data, pData, tex->size);
    GSPGPU_FlushDataCache(tex->data, tex->size);
    C3D_TexSetWrap(tex, wrapMode(wrapS), wrapMode(wrapT));
    C3D_TexSetFilter(tex, linear ? GPU_LINEAR : GPU_NEAREST, minLinear ? GPU_LINEAR : GPU_NEAREST);
    return tex;
}

void hostGfxTextureDestroy(void* texture) {
    if (sPendingDeleteCount < MAX_PENDING_DELETES) {
        sPendingDeletes[sPendingDeleteCount++] = texture;
    } else if (!sPendingDeleteOverflowReported) {
        hostLog("gfx: deferred texture deletion queue full; texture handle leaked\n");
        sPendingDeleteOverflowReported = true;
    }
}

void hostGfxTextureDestroyNow(void* texture) {
    C3D_TexDelete((C3D_Tex*)texture);
    free(texture);
}

// ##############################################################################################
// # Frame pacing                                                                               #
// #                                                                                            #
// # Whether the next loop draws, and the detail level (hostPerfDetail) the rest of the port    #
// # lowers its work by while the game runs behind: actors clip nearer, fewer particles. With a #
// # drawn and an undrawn loop together over 41 ms, one loop in three is drawn, until they are  #
// # under 37 ms again (DETAILS.md, section 10).                                                #
// ##############################################################################################
static void beginFrame(void);

int hostGfxFrameDropped(void) {
    beginFrame();
    return sSkipFrame;
}

float hostGfxFrameMs(void) {
    return sFrameMs;
}

int hostGfxNextFrameDrawn(void) {
    return sNextFrameDrawn;
}

float hostPerfDetail(void) {
    return sPerfDetail;
}

// At the end of a frame: whether the next loop draws, and the detail level. The next loop begins
// in the next vertical blank, or in this one when the game is behind (the retrace it would wait
// for has passed).
static unsigned long sPacingLoops, sPacingDrawn, sPacingLate;
static unsigned long sPacingVBlank = ~0ul;
// The game's speed before its smoothness: with a drawn loop and an undrawn one together over about
// 41 ms (under 80% of its speed; the observatory: 67 ms, at 42-61%), one loop in three is drawn,
// until the two are under 37 ms. They are measured whichever is in use. Not with 96 MB, which has
// its own limit (HOST_GFX_LATE_FRAME_LOW_VBLANKS).
#define HOST_GFX_THIRD_ENTER_MS 41.0f
#define HOST_GFX_THIRD_LEAVE_MS 37.0f
static float sDrawnLoopMs = 16.7f, sUndrawnLoopMs = 16.7f;
static unsigned long sUndrawnLoops;
static bool sDrawOneInThree;

void hostGfxLogPacing(void) {
    const unsigned long vblank = hostVBlankCount();
    const unsigned long vblanks = sPacingVBlank == ~0ul ? 0 : vblank - sPacingVBlank;
    hostLog("pacing: %lu loops in %lu retraces (game at %lu%% speed), %lu drawn, %lu late, detail %lu%%, one in three %d (%lu + %lu ms)\n",
            sPacingLoops, vblanks, vblanks != 0 ? sPacingLoops * 100 / vblanks : 0, sPacingDrawn, sPacingLate,
            (unsigned long)(sPerfDetail * 100.0f + 0.5f), sDrawOneInThree, (unsigned long)sDrawnLoopMs, (unsigned long)sUndrawnLoopMs);
    sPacingVBlank = vblank;
    sPacingLoops = sPacingDrawn = sPacingLate = 0;
}

int hostGfxFallingBehind(void) {
    return sDrawOneInThree;
}

static void planNextFrame(void) {
    sPacingLoops++;
    sPacingDrawn += !sSkipFrame;
    sPacingLate += sLoopLate;

    const unsigned long next = hostVBlankCount() + (sLoopLate ? 0 : 1);
    const unsigned long lateVBlanks = hostLowMemory() ? HOST_GFX_LATE_FRAME_LOW_VBLANKS : HOST_GFX_LATE_FRAME_VBLANKS;
    const unsigned long since = sLastBeginVBlank == ~0ul ? lateVBlanks : next - sLastBeginVBlank;
    sNextFrameDrawn = since >= lateVBlanks || (since >= HOST_GFX_FRAME_VBLANKS && !sLoopLate);

    if (sSkipFrame) {
        sUndrawnLoopMs += (sLoopMs - sUndrawnLoopMs) * 0.125f;
        sUndrawnLoops++;
    } else {
        sDrawnLoopMs += (sLoopMs - sDrawnLoopMs) * 0.125f;
        sUndrawnLoops = 0;
    }
    const float twoLoopsMs = sDrawnLoopMs + sUndrawnLoopMs;
    sDrawOneInThree = !hostLowMemory() && (sDrawOneInThree ? twoLoopsMs >= HOST_GFX_THIRD_LEAVE_MS : twoLoopsMs > HOST_GFX_THIRD_ENTER_MS);
    if (sDrawOneInThree) {
        sNextFrameDrawn = sUndrawnLoops >= 2;
    }

    if (sFrameMs > 19.0f) {
        sPerfDetail -= 0.01f;
    } else if (sFrameMs < 17.4f) {
        sPerfDetail += 0.005f;
    }
    sPerfDetail = sPerfDetail < HOST_PERF_DETAIL_MIN ? HOST_PERF_DETAIL_MIN : sPerfDetail > 1.0f ? 1.0f : sPerfDetail;
}

// ##############################################################################################
// # Frame ids                                                                                  #
// #                                                                                            #
// # For texture lifetimes (rvl/jut_texture.cpp): the frame being recorded, and the last one    #
// # the GPU has finished.                                                                      #
// ##############################################################################################
unsigned long hostGfxFrameId(void) {
    return sGpuFrameEndCount + 1;
}

unsigned long hostGfxCompletedFrameId(void) {
    return sCompletedFrames;
}

unsigned long hostGfxLinearFree(void) {
    return (unsigned long)linearSpaceFree();
}

// Called when C3D_FrameBegin succeeds, that is when the GPU has finished every submitted frame:
// textures released since the previous call can no longer be read by it.
static void deletePendingTextures(void) {
    for (int i = 0; i < sPendingDeleteCount; i++) {
        C3D_TexDelete(sPendingDeletes[i]);
        free(sPendingDeletes[i]);
    }
    sPendingDeleteCount = 0;
}

// ##############################################################################################
// # Screenshots                                                                                #
// #                                                                                            #
// # debug.txt shot= and the capture tool: the render target and single textures, as they are   #
// # in memory.                                                                                 #
// ##############################################################################################
// Saves the render target of the last frame (240x400 RGBA8 in the GPU's 8x8 tiled layout) for
// the debug.txt "shot" option. The game's screen is read from the render target rather than
// the display frame buffer, which emulators with a hardware renderer do not keep up to date.
static void saveShot(const char* path) {
    C3D_RenderTarget* target = sTarget;
    u32 size = C3D_CalcColorBufSize(target->frameBuf.width, target->frameBuf.height, GPU_RB_RGBA8);
    GSPGPU_InvalidateDataCache(target->frameBuf.colorBuf, size);
    FILE* file = fopen(path, "wb");
    if (file != NULL) {
        u32 header[4] = {0x48534D53, 0x31544F, target->frameBuf.width, target->frameBuf.height};  // "SMSHOT1"
        fwrite(header, 1, sizeof(header), file);
        fwrite(target->frameBuf.colorBuf, 1, size, file);
        fclose(file);
    }
}

void hostGfxSaveTexture(void* texture, const char* path) {
    C3D_Tex* tex = (C3D_Tex*)texture;
    if (tex == NULL || tex->data == NULL) {
        return;
    }
    GSPGPU_InvalidateDataCache(tex->data, tex->size);
    FILE* file = fopen(path, "wb");
    if (file != NULL) {
        u32 header[4] = {0x58455454, (u32)tex->fmt, tex->width, tex->height};  // "TTEX"
        fwrite(header, 1, sizeof(header), file);
        fwrite(tex->data, 1, tex->size, file);
        fclose(file);
    }
    hostLog("gfx: texture %ux%u saved to %s\n", tex->width, tex->height, path);
}

// ##############################################################################################
// # Beginning a frame                                                                          #
// #                                                                                            #
// # Each frame begins lazily, at its first draw or query. A frame is dropped (its draws thrown #
// # away) when it is not due (pacing), or when the GPU is still busy after                     #
// # GPU_BUSY_TIMEOUT_MS. A frame that begins frees the textures released before it, empties    #
// # the persistent arena if it was full, clears the target and forgets the state last sent:    #
// # the first draw sends it in full.                                                           #
// ##############################################################################################
#define GPU_BUSY_TIMEOUT_MS 2000

static void beginFrame(void) {
    if (sInFrame) {
        return;
    }

    // The game paces itself on the retrace (VIWaitForRetrace), where other game threads run.
    // After a slow frame it renders frames back to back to catch up; the GPU takes one frame
    // per vertical blank, so the extra ones are dropped instead of waited for here.
    const unsigned long vblank = hostVBlankCount();
    const float sinceVBlankMs = (float)(svcGetSystemTick() - hostVBlankTick()) * (1000.0f / SYSCLOCK_ARM11);
    sLoopLate = sinceVBlankMs > HOST_GFX_LATE_MS;
    // A frame planned as undrawn has no view calculation (GameScene::calcAnim): it is not drawn.
    sSkipFrame = hostDebugTurbo() || !sNextFrameDrawn || vblank == sLastFrameVBlank ||
                 (sLastBeginVBlank != ~0ul && vblank - sLastBeginVBlank < HOST_GFX_FRAME_VBLANKS);
#if SMG_SHOWCASE
    // Submit an initial clear even while the title scene is still loading.
    if (sGpuFrameEndCount == 0) {
        sSkipFrame = false;
    }
#endif
    if (sShotNextFrame[0] != '\0' && sNextFrameDrawn) {
        // A frame to screenshot is always drawn.
        memcpy(sShotRequested, sShotNextFrame, sizeof(sShotRequested));
        sShotNextFrame[0] = '\0';
        sSkipFrame = hostDebugTurbo();
    }
    sInFrame = true;
    if (sSkipFrame) {
        return;
    }

    sLastBeginVBlank = vblank;
    sTarget = sOutputTarget;
    unsigned long long waitStart = hostGetTick();
    hostDebugTrace("Citro3D FrameBegin", sFrameNumber, hostVBlankCount(), 0);
    // Wait for the GPU to finish the previous frame (about 38 ms in Good Egg) rather than drop
    // this one: a dropped frame threw away the game's whole update and draw (about 100 ms of CPU,
    // every other frame in CIA 0.10.15). Only a GPU stuck for GPU_BUSY_TIMEOUT_MS drops it.
    bool begun = C3D_FrameBegin(C3D_FRAME_NONBLOCK);
    while (!begun && hostGetTick() - waitStart < hostGetTickFrequency() / 1000 * GPU_BUSY_TIMEOUT_MS) {
        svcSleepThread(250000);
        begun = C3D_FrameBegin(C3D_FRAME_NONBLOCK);
    }
    if (!begun) {
        sGpuWaitTicks += hostGetTick() - waitStart;
        sGpuBusyFrames++;
        sSkipFrame = true;
        hostDebugTrace("Citro3D queue busy", sFrameNumber, hostVBlankCount(), sGpuBusyFrames);
        if (sGpuBusyFrames == 30) {
            reportGpuStall();
        }
        if (sGpuBusyFrames == 1 || sGpuBusyFrames % 60 == 0) {
            hostLog("gfx: skipping frame, Citro3D queue still busy (%lu dropped), begin successes %lu, ends %lu\n",
                    sGpuBusyFrames, sGpuFrameBeginCount, sGpuFrameEndCount);
            logGpuProgress("frame begin busy");
        }
        return;
    }
    sGpuWaitTicks += hostGetTick() - waitStart;
    sLastKickTick = hostGetTick();
    if (sGpuFrameEndCount != 0) {
        float gpuMs = C3D_GetDrawingTime();
        sGpuDrawMsSum += gpuMs;
        sGpuDrawMsMax = gpuMs > sGpuDrawMsMax ? gpuMs : sGpuDrawMsMax;
        sGpuDrawSamples++;
    }
    hostDebugTrace("Citro3D FrameBegin returned", sFrameNumber, (unsigned long)(hostGetTick() - waitStart), 0);
    sGpuFrameBeginCount++;
    sCompletedFrames = sGpuFrameEndCount;
    deletePendingTextures();
    memset(&sFrameOps, 0, sizeof(sFrameOps));
    sDrawRecordCount = 0;
    sDrawsSinceSplit = 0;
    sFrameCmdBase = gpuCmdBuf;
    if (sGpuFrameBeginCount <= 4) {
        hostLog("gfx: Citro3D FrameBegin success #%lu frame %lu vblank %lu\n", sGpuFrameBeginCount,
                sFrameNumber, hostVBlankCount());
        logGpuProgress("frame begin success");
    }
    if (sGpuBusyFrames != 0) {
        hostLog("gfx: Citro3D queue resumed after %lu dropped frames\n", sGpuBusyFrames);
        sGpuBusyFrames = 0;
        logGpuProgress("queue resumed");
    }
    if (sShotPending[0] != '\0') {
        saveShot(sShotPending);
        sShotPending[0] = '\0';
    }
    C3D_RenderTargetClear(sTarget, C3D_CLEAR_ALL, sBlackScreen ? 0x000000FF : sClearColor, 0);
    C3D_FrameDrawOn(sTarget);
    C3D_SetViewport(0, 0, 240, 400);
    sTransientUsed = sTransientFlushed = 0;
    sIndexCount = sIndicesFlushed = 0;
    sNativeDrawsThisFrame = 0;
    sDrawCallsThisFrame = 0;
    sDroppedDrawsThisFrame = 0;
    sNativeBudgetReported = false;
    sBoundWindow = -1;
    if (sArenaResetPending) {
        // The GPU has finished the previous frame: the cached vertices can go.
        sArenaPeakVertices = sPersistentUsed > sArenaPeakVertices ? sPersistentUsed : sArenaPeakVertices;
        sArenaPeakIndices = sPersistentIndexUsed > sArenaPeakIndices ? sPersistentIndexUsed : sArenaPeakIndices;
        sArenaResetPending = false;
        sPersistentUsed = 0;
        sPersistentIndexUsed = 0;
        sArenaEpoch++;
    }
    sLastStateValid = false;
    sStencilAppliedValid = false;
    sMode = MODE_NONE;  // uniforms are sent again in full
    sTransformValid = false;
}

// ##############################################################################################
// # Texture combiners                                                                          #
// #                                                                                            #
// # The draw's combiner stages, sent only when they differ from the last draw's. GX's          #
// # destination alpha (the alpha written to the frame buffer is a constant) becomes one more   #
// # stage that replaces the alpha with it.                                                     #
// ##############################################################################################
static u32 effectiveStageCount(const HostGfxState* pState) {
    u32 count = pState->stageCount;
    if (pState->dstAlphaEnable && count < HOST_GFX_STAGES) {
        count++;
    }
    return count;
}

static HostGfxStage effectiveStage(const HostGfxState* pState, u32 index) {
    HostGfxStage stage = {};
    if (index < pState->stageCount) {
        stage = pState->stages[index];
    } else {
        stage.colorSrc[0] = GPU_PREVIOUS;
        stage.colorSrc[1] = stage.colorSrc[2] = GPU_PREVIOUS;
        stage.colorFunc = GPU_REPLACE;
        stage.alphaSrc[0] = GPU_CONSTANT;
        stage.alphaSrc[1] = stage.alphaSrc[2] = GPU_CONSTANT;
        stage.alphaFunc = GPU_REPLACE;
    }

    if (pState->dstAlphaEnable && index == effectiveStageCount(pState) - 1) {
        stage.alphaSrc[0] = stage.alphaSrc[1] = stage.alphaSrc[2] = GPU_CONSTANT;
        stage.alphaOp[0] = stage.alphaOp[1] = stage.alphaOp[2] = 0;
        stage.alphaFunc = GPU_REPLACE;
        stage.alphaScale = GPU_TEVSCALE_1;
        stage.constant = (stage.constant & 0x00FFFFFF) | ((u32)pState->dstAlpha << 24);
    }
    return stage;
}

void hostGfxClearFrame(unsigned long clearColor) {
    beginFrame();
    if (sSkipFrame) {
        return;
    }
    C3D_RenderTargetClear(sTarget, C3D_CLEAR_ALL, clearColor, 0);
}

static void applyStages(const HostGfxState* pState) {
    // Most draws repeat the last draw's combiners: nothing to compare stage by stage.
    const bool sameStages = sLastStateValid && pState->stageCount == sLastState.stageCount &&
                            pState->dstAlphaEnable == sLastState.dstAlphaEnable &&
                            (!pState->dstAlphaEnable || pState->dstAlpha == sLastState.dstAlpha) &&
                            memcmp(pState->stages, sLastState.stages, pState->stageCount * sizeof(HostGfxStage)) == 0;
    u32 count = sameStages ? 0 : effectiveStageCount(pState);
    u32 previousCount = sLastStateValid ? effectiveStageCount(&sLastState) : 0;
    for (int i = 0; i < HOST_GFX_STAGES && !sameStages; i++) {
        bool used = (u32)i < count;
        bool wasUsed = sLastStateValid && (u32)i < previousCount;
        HostGfxStage stage = {0};
        HostGfxStage previous = {0};
        if (used) {
            stage = effectiveStage(pState, i);
        }
        if (wasUsed) {
            previous = effectiveStage(&sLastState, i);
        }
        if (sLastStateValid && used == wasUsed && (!used || memcmp(&stage, &previous, sizeof(HostGfxStage)) == 0)) {
            continue;
        }

        C3D_TexEnv* env = C3D_GetTexEnv(i);
        C3D_TexEnvInit(env);
        if (!used) {
            continue;  // passes the previous stage through
        }
        C3D_TexEnvSrc(env, C3D_RGB, stage.colorSrc[0], stage.colorSrc[1], stage.colorSrc[2]);
        C3D_TexEnvSrc(env, C3D_Alpha, stage.alphaSrc[0], stage.alphaSrc[1], stage.alphaSrc[2]);
        C3D_TexEnvOpRgb(env, stage.colorOp[0], stage.colorOp[1], stage.colorOp[2]);
        C3D_TexEnvOpAlpha(env, stage.alphaOp[0], stage.alphaOp[1], stage.alphaOp[2]);
        C3D_TexEnvFunc(env, C3D_RGB, stage.colorFunc);
        C3D_TexEnvFunc(env, C3D_Alpha, stage.alphaFunc);
        C3D_TexEnvScale(env, C3D_RGB, stage.colorScale);
        C3D_TexEnvScale(env, C3D_Alpha, stage.alphaScale);
        C3D_TexEnvColor(env, stage.constant);
    }

    // Stages that save their result in the combiner buffer (a material reading a stage's input
    // after its first combiner, mat.py); the buffer starts each draw at 0.5 (hostGfxInit).
    static unsigned char sBufferRgb, sBufferAlpha;
    if (pState->bufferUpdateRgb != sBufferRgb || pState->bufferUpdateAlpha != sBufferAlpha) {
        sBufferRgb = pState->bufferUpdateRgb;
        sBufferAlpha = pState->bufferUpdateAlpha;
        C3D_TexEnvBufUpdate(C3D_RGB, sBufferRgb);
        C3D_TexEnvBufUpdate(C3D_Alpha, sBufferAlpha);
    }
}

// ##############################################################################################
// # Fog                                                                                        #
// #                                                                                            #
// # The Wii's fog registers (BP 0xEE-0xF2) carry the parameters of a curve of the depth; the   #
// # PICA takes a table of 128 steps from far to near (C3D_FogLut). The table is computed from  #
// # the Wii's formula, for each of its curves, and the last few are cached: materials with     #
// # different fog alternate within a frame.                                                    #
// ##############################################################################################
static float decodeFogFloat(unsigned int value) {
    u32 bits = ((value & 0x7FF) << 12) | (((value >> 11) & 0xFF) << 23) | (((value >> 19) & 1) << 31);
    float decoded;
    memcpy(&decoded, &bits, sizeof(decoded));
    return decoded;
}

static void applyFog(const HostGfxState* pState) {
    u32 fogMode = (pState->fogParamC >> 21) & 7;
    bool orthographic = (pState->fogParamC >> 20) & 1;
    u32 shift = pState->fogParamShift & 0x1F;

    // GX fog curves (fsel): 2 linear, 4 exp, 5 exp2, 6 reverse exp, 7 reverse exp2, perspective
    // or orthographic. 0, 1 and 3 are no fog.
    if (fogMode < 2 || fogMode == 3 || (!orthographic && shift == 0)) {
        C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        C3D_FogLutBind(NULL);
        return;
    }

    u32 color = ((pState->fogColor >> 16) & 0xFF) | (pState->fogColor & 0xFF00) | ((pState->fogColor & 0xFF) << 16);
    for (u32 i = 0; i < FOG_CACHE_SIZE; i++) {
        if (sFogCache[i].valid && sFogCache[i].a == pState->fogParamA && sFogCache[i].b == pState->fogParamB &&
            sFogCache[i].shift == pState->fogParamShift && sFogCache[i].c == pState->fogParamC) {
            C3D_FogLutBind(&sFogCache[i].lut);
            C3D_FogColor(color);
            C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
            return;
        }
    }

    float a = orthographic ? decodeFogFloat(pState->fogParamA) : ldexpf(decodeFogFloat(pState->fogParamA), shift);
    float b = orthographic ? 0.0f : ldexpf((float)(pState->fogParamB & 0xFFFFFF) / 8388638.0f, (int)shift - 1);
    float c = decodeFogFloat(pState->fogParamC);
    if (!isfinite(a) || !isfinite(b) || !isfinite(c)) {
        C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        C3D_FogLutBind(NULL);
        return;
    }

    for (u32 i = 0; i <= 128; i++) {
        // Citro3D's fog LUT input runs from far (0) to near (1); Wii perspective depth does
        // the reverse. GXPixel.c encodes f = A / (B - z) - C, the amount of fog to apply.
        // Orthographic fog is linear in depth: ze = A z (the Wii hardware, as Dolphin's).
        float picaDepth = (float)i / 128.0f;
        float wiiDepth = 1.0f - picaDepth;
        float fog;
        if (orthographic) {
            fog = a * wiiDepth - c;
        } else {
            float denominator = b - wiiDepth;
            fog = denominator == 0.0f ? (a > 0.0f ? 1.0f : 0.0f) : a / denominator - c;
        }
        if (!isfinite(fog)) {
            fog = fog > 0.0f ? 1.0f : 0.0f;
        }
        if (fog < 0.0f) {
            fog = 0.0f;
        } else if (fog > 1.0f) {
            fog = 1.0f;
        }
        switch (fogMode) {
        case 4: fog = 1.0f - exp2f(-8.0f * fog); break;
        case 5: fog = 1.0f - exp2f(-8.0f * fog * fog); break;
        case 6: fog = exp2f(-8.0f * (1.0f - fog)); break;
        case 7: fog = exp2f(-8.0f * (1.0f - fog) * (1.0f - fog)); break;
        default: break;
        }
        sFogLutValues[i] = 1.0f - fog;  // PICA's fog LUT stores the unfogged fraction.
    }
    for (u32 i = 0; i < 128; i++) {
        sFogLutData[i] = sFogLutValues[i];
        sFogLutData[i + 128] = sFogLutValues[i + 1] - sFogLutValues[i];
    }
    // Citro3D copies a bound LUT into the command list, so a slot can be rebuilt at once.
    u32 slot = sFogCacheNext;
    sFogCacheNext = (sFogCacheNext + 1) % FOG_CACHE_SIZE;
    sFogCache[slot].valid = true;
    sFogCache[slot].a = pState->fogParamA;
    sFogCache[slot].b = pState->fogParamB;
    sFogCache[slot].shift = pState->fogParamShift;
    sFogCache[slot].c = pState->fogParamC;
    FogLut_FromArray(&sFogCache[slot].lut, sFogLutData);
    C3D_FogLutBind(&sFogCache[slot].lut);
    sFogLutBuilds++;
    C3D_FogColor(color);
    C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
}

// ##############################################################################################
// # Issuing draws, and merging them                                                            #
// #                                                                                            #
// # A triangle list waits as the pending draw; the next one with the same state, transform and #
// # window extends it, and anything else sends it first (flushPendingDraw). Most of the game's #
// # small CPU-made draws thus reach the GPU as one draw call.                                  #
// ##############################################################################################
// Draws are indexed triangle lists into the vertex arena; consecutive triangle lists with the
// same state, transform and arena window are sent as one draw call. The pending draw is
// issued when anything changes.
static bool sPendingDraw;

unsigned long long hostGfxTakeGpuWait(void) {
    unsigned long long ticks = sGpuWaitTicks;
    sGpuWaitTicks = 0;
    return ticks;
}

void hostGfxTakeGpuTime(float* pAverage, float* pMax) {
    *pAverage = sGpuDrawSamples != 0 ? sGpuDrawMsSum / (float)sGpuDrawSamples : 0.0f;
    *pMax = sGpuDrawMsMax;
    sGpuDrawMsSum = sGpuDrawMsMax = 0.0f;
    sGpuDrawSamples = 0;
}
static u32 sPendingFirstIndex, sPendingIndexCount;
static unsigned long sDiagnosticDrawInputCount;

static u32 diagnosticFloatBits(const float* value) {
    u32 bits;
    memcpy(&bits, value, sizeof(bits));
    return bits;
}

static void drawElements(GPU_Primitive_t primitive, u32 count, const u16* indices) {
    if (SMG_DIAG_FIRST_DRAW_ONLY && sFrameNumber == 1 && sDiagnosticDrawInputCount++ == 0) {
        C3D_BufInfo* info = C3D_GetBufInfo();
        u32 indexPhys = osConvertVirtToPhys(indices);
        u32 vertexPhys = sBoundWindow >= 0 && sBoundWindow < ARENA_WINDOWS
                             ? osConvertVirtToPhys(&sArena[sBoundWindow * ARENA_WINDOW])
                             : 0;
        hostLog("gfx: first draw inputs primitive %d count %lu window %d index %p phys %08lx base %08lx offset %08lx vertex %p phys %08lx stride %lu\n",
                (int)primitive, (unsigned long)count, sBoundWindow, indices, (unsigned long)indexPhys,
                info ? (unsigned long)info->base_paddr : 0, info ? (unsigned long)info->buffers[0].offset : 0,
                sBoundWindow >= 0 && sBoundWindow < ARENA_WINDOWS ? &sArena[sBoundWindow * ARENA_WINDOW] : NULL,
                (unsigned long)vertexPhys, (unsigned long)sizeof(HostGfxModelVertex));
        u32 n = count < 12 ? count : 12;
        for (u32 i = 0; i < n; i++) {
            u16 index = indices[i];
            if (sBoundWindow < 0 || sBoundWindow >= ARENA_WINDOWS) {
                hostLog("gfx: first draw index %lu = %u (no bound arena window)\n", (unsigned long)i, index);
                continue;
            }
            const HostGfxModelVertex* v = &sArena[sBoundWindow * ARENA_WINDOW + index];
            hostLog("gfx: first draw index %lu = %u vertex %08lx %08lx %08lx normal %08lx %08lx %08lx rgba %02x%02x%02x%02x matrix %u texrow %u,%u,%u tex0 %08lx %08lx %08lx\n",
                    (unsigned long)i, index, (unsigned long)diagnosticFloatBits(&v->x), (unsigned long)diagnosticFloatBits(&v->y),
                    (unsigned long)diagnosticFloatBits(&v->z), (unsigned long)diagnosticFloatBits(&v->nx),
                    (unsigned long)diagnosticFloatBits(&v->ny), (unsigned long)diagnosticFloatBits(&v->nz), v->rgba[0],
                    v->rgba[1], v->rgba[2], v->rgba[3], v->matrix, v->texRow[0], v->texRow[1], v->texRow[2],
                    (unsigned long)diagnosticFloatBits(&v->tex[0][0]), (unsigned long)diagnosticFloatBits(&v->tex[0][1]),
                    (unsigned long)diagnosticFloatBits(&v->tex[0][2]));
        }
    }
    if (STALL_DRAW_DIAG && !sStallReported && sDrawRecordCount < DIAG_RECORDS && sBoundWindow >= 0 && sBoundWindow < ARENA_WINDOWS) {
        DrawRecord* r = &sDrawRecords[sDrawRecordCount++];
        memset(r, 0, sizeof(*r));
        r->cmd = (u32)(gpuCmdBuf + gpuCmdBufOffset);  // virtual, as the GX queue entries hold it
        r->count = (u16)(count < 0xFFFF ? count : 0xFFFF);
        r->window = (u16)sBoundWindow;
        r->indices = indices;
        r->cpu = sMode == MODE_PRETRANSFORMED;
        r->primitive = (u8)(primitive >> 8);
        r->minIndex = 0xFFFF;
        r->minW = 1.0e30f;
        for (u32 i = 0; i < count; i++) {
            u16 index = indices[i];
            r->minIndex = index < r->minIndex ? index : r->minIndex;
            r->maxIndex = index > r->maxIndex ? index : r->maxIndex;
        }
        for (u32 index = r->minIndex; index <= r->maxIndex && r->minIndex <= r->maxIndex; index++) {
            const HostGfxModelVertex* v = &sArena[sBoundWindow * ARENA_WINDOW + index];
            if (!isfinite(v->x) || !isfinite(v->y) || !isfinite(v->z) || (sMode == MODE_PRETRANSFORMED && !isfinite(v->nx))) {
                r->bad++;
                continue;
            }
            if (sMode == MODE_PRETRANSFORMED) {
                float w = v->nx;
                r->minW = w < r->minW ? w : r->minW;
                float a = fabsf(v->x) > fabsf(v->y) ? fabsf(v->x) : fabsf(v->y);
                float ratio = w > 0.0f ? a / w : 4.0e9f;
                r->maxRatio = ratio > r->maxRatio ? ratio : r->maxRatio;
            } else {
                float a = fabsf(v->x) > fabsf(v->y) ? fabsf(v->x) : fabsf(v->y);
                a = fabsf(v->z) > a ? fabsf(v->z) : a;
                r->maxAbs = a > r->maxAbs ? a : r->maxAbs;
            }
        }
        sDrawsSinceSplit++;
    }
    C3D_DrawElements(primitive, count, C3D_UNSIGNED_SHORT, indices);
}

static void flushPendingDraw(void) {
    if (sPendingDraw) {
        if (!sBlackScreen) {
            drawElements(GPU_TRIANGLES, sPendingIndexCount, &sIndices[sPendingFirstIndex]);
        }
        sPendingDraw = false;
    }
}

void hostGfxSetBlack(int black) {
    sBlackScreen = black != 0;
}

void hostGfxSetStencilAmbient(int enable, int func, int ref, int mask, int writeMask, int fail, int zfail, int zpass) {
    StencilState s = {0};
    if (enable) {
        s.enable = 1;
        s.func = (unsigned char)func;
        s.ref = (unsigned char)ref;
        s.mask = (unsigned char)mask;
        s.writeMask = (unsigned char)writeMask;
        s.fail = (unsigned char)fail;
        s.zfail = (unsigned char)zfail;
        s.zpass = (unsigned char)zpass;
    }
    if (memcmp(&s, &sStencilAmbient, sizeof(s)) != 0) {
        flushPendingDraw();  // a merged draw keeps the stencil it began with
        sStencilAmbient = s;
    }
}

// ##############################################################################################
// # Vertex arena                                                                               #
// #                                                                                            #
// # Allocating arena vertices and indices. Persistent allocations never straddle a window and  #
// # skip the transient part; when the arena or the persistent indices are full, the request    #
// # fails and the arena is emptied at the next frame's start (the caller uploads again then).  #
// # Transient vertices fail while the frame is dropped: the GPU may still read the last        #
// # frame's.                                                                                   #
// ##############################################################################################
static void bindWindow(int window) {
    if (window == sBoundWindow) {
        return;
    }
    flushPendingDraw();
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, &sArena[window * ARENA_WINDOW], sizeof(HostGfxModelVertex), 7, 0x6543210);
    sBoundWindow = window;
}

unsigned long hostGfxArenaAlloc(unsigned long count, int persistent, HostGfxModelVertex** ppOut) {
    beginFrame();
    if (count == 0 || count > ARENA_WINDOW) {
        return HOST_GFX_NO_ARENA;
    }
    unsigned long first;
    if (persistent) {
        // Bump allocation that never straddles a window; when full, the cache is emptied at
        // the start of the next drawn frame (the GPU has finished with it then).
        u32 used = sPersistentUsed;
        if (used % ARENA_WINDOW + count > ARENA_WINDOW) {
            used = (used / ARENA_WINDOW + 1) * ARENA_WINDOW;
        }
        if (used >= ARENA_TRANSIENT_FIRST && used < ARENA_TRANSIENT_FIRST + ARENA_TRANSIENT) {
            used = ARENA_TRANSIENT_FIRST + ARENA_TRANSIENT;
        }
        if (used + count > sArenaPersistentEnd) {
            if (!sArenaResetPending) {
                sArenaFullVertices++;
            }
            sArenaResetPending = true;
            return HOST_GFX_NO_ARENA;
        }
        first = used;
        sPersistentUsed = used + count;
        if (first < sPersistentDirtyLo) {
            sPersistentDirtyLo = first;
        }
        if (first + count > sPersistentDirtyHi) {
            sPersistentDirtyHi = first + count;
        }
    } else {
        // This frame's vertices: nothing while the frame is dropped (the GPU may still read the
        // previous frame's).
        u32 limit = ARENA_TRANSIENT;
        if (sSkipFrame || sTransientUsed + count > limit) {
            if (!sSkipFrame) {
                static int sReports;
                if (sReports < 3) {
                    sReports++;
                    hostLog("gfx: transient vertices full (frame %lu)\n", sFrameNumber);
                }
            }
            return HOST_GFX_NO_ARENA;
        }
        first = ARENA_PERSISTENT_WINDOWS * ARENA_WINDOW + sTransientUsed;
        sTransientUsed += count;
    }
    *ppOut = &sArena[first];
    return first;
}

unsigned short* hostGfxIndexAlloc(unsigned long count) {
    if (sPersistentIndexUsed + count > sPersistentIndexSize) {
        if (!sArenaResetPending) {
            sArenaFullIndices++;
        }
        sArenaResetPending = true;
        return NULL;
    }
    u32 first = sPersistentIndexUsed;
    sPersistentIndexUsed += count;
    if (first < sPersistentIndexDirtyLo) {
        sPersistentIndexDirtyLo = first;
    }
    if (first + count > sPersistentIndexDirtyHi) {
        sPersistentIndexDirtyHi = first + count;
    }
    return &sPersistentIndices[first];
}

unsigned short* hostGfxIndexAllocTransient(unsigned long count) {
    // A queued draw may still reference the tail of sIndices. Submit it before appending data.
    flushPendingDraw();
    if (count == 0 || sIndexCount + count > sIndexBufferSize) {
        return NULL;
    }
    u16* first = &sIndices[sIndexCount];
    sIndexCount += count;
    return first;
}

HostGfxModelVertex* hostGfxArenaVertex(unsigned long index) {
    return &sArena[index];
}

void hostGfxArenaReset(void) {
    sArenaResetPending = true;
}

unsigned long hostGfxArenaEpoch(void) {
    // The frame's start may empty the persistent arena (a new epoch): an upload checked against
    // the epoch before it would be drawn from vertices this frame then overwrites.
    beginFrame();
    return sArenaEpoch;
}

// ##############################################################################################
// # Cache writeback                                                                            #
// #                                                                                            #
// # The GPU reads memory, not the CPU's cache: everything written since the last writeback is  #
// # flushed before the GPU can read it (before each early start of the GPU, copies and the     #
// # frame end).                                                                                #
// ##############################################################################################
// Writes the CPU's data cache back for everything the GPU reads that changed since the last
// time: new arena vertices and this frame's indices.
static void flushWrittenData(void) {
    if (sPersistentDirtyHi > sPersistentDirtyLo) {
        GSPGPU_FlushDataCache(&sArena[sPersistentDirtyLo], (sPersistentDirtyHi - sPersistentDirtyLo) * sizeof(HostGfxModelVertex));
    }
    sPersistentDirtyLo = ~0u;
    sPersistentDirtyHi = 0;
    if (sPersistentIndexDirtyHi > sPersistentIndexDirtyLo) {
        GSPGPU_FlushDataCache(&sPersistentIndices[sPersistentIndexDirtyLo],
                              (sPersistentIndexDirtyHi - sPersistentIndexDirtyLo) * sizeof(u16));
    }
    sPersistentIndexDirtyLo = ~0u;
    sPersistentIndexDirtyHi = 0;
    if (sTransientUsed > sTransientFlushed) {
        GSPGPU_FlushDataCache(&sArena[ARENA_PERSISTENT_WINDOWS * ARENA_WINDOW + sTransientFlushed],
                              (sTransientUsed - sTransientFlushed) * sizeof(HostGfxModelVertex));
        sTransientFlushed = sTransientUsed;
    }
    if (sIndexCount > sIndicesFlushed) {
        GSPGPU_FlushDataCache(&sIndices[sIndicesFlushed], (sIndexCount - sIndicesFlushed) * sizeof(u16));
        sIndicesFlushed = sIndexCount;
    }
}

// ##############################################################################################
// # Starting the GPU during the frame                                                          #
// #                                                                                            #
// # So that the GPU draws while the CPU is still recording the frame (details below). After    #
// # each split the GPU state is sent again in full.                                            #
// ##############################################################################################
// citro3d runs a frame's command lists only from C3D_FrameEnd: the GPU (about 31 ms a frame in
// Good Egg) started after the whole draw phase, and the next frame waited for it in
// C3D_FrameBegin. Every GPU_KICK_MS of drawing the commands so far are sent and the queue runs,
// so the GPU draws while the CPU still fills the frame. Once the queue runs, a list starts
// before C3D_FrameEnd flushes the linear heap: every split sends its list flushed
// (GX_CMDLIST_FLUSH), after the vertices and indices (flushWrittenData).
#define GPU_KICK_MS 2
// Kicks stop with this many queue entries still free for the rest of the frame (EFB copies,
// the final list, the EFB read copy, the screen transfer).
#define GPU_KICK_RESERVE 48

// After a split the shader configuration and the state are sent again in full.
static void forgetStateAfterSplit(void) {
    sLastStateValid = false;
    sStencilAppliedValid = false;
    sMode = MODE_NONE;
    sTransformValid = false;
    sBoundWindow = -1;
}

static void kickGpu(void) {
    flushWrittenData();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    gxCmdQueueRun(&__C3D_Context);
    sFrameOps.splits++;
    sGpuKicks++;
    sLastKickTick = hostGetTick();
    forgetStateAfterSplit();
}

// ##############################################################################################
// # Frame buffer reads                                                                         #
// #                                                                                            #
// # Some game objects read the Wii's frame buffer (the EFB): a pixel's colour or depth. They   #
// # read the copy of the last finished frame, made at each frame's end while reads keep        #
// # coming.                                                                                    #
// ##############################################################################################
// EFB reads (BrightObj, StarPointerPeekZ, TalkPeekZ, OdhConverter) come in the middle of the
// draw phase, when the GPU may have drawn part of the frame. They read the last finished frame,
// as before the GPU started early: a copy of the target's colour and depth the GPU makes at the
// end of each frame (while reads keep coming), done before the next C3D_FrameBegin returns.
#define EFB_COPY_FRAMES 120  // frames the copy goes on after the last read
static void* sEfbCopy[2];    // colour, depth
static u32 sEfbCopySize[2];
static unsigned long sEfbCopyFrame = ~0ul, sLastEfbReadFrame = ~0ul;
static unsigned long sDepthKeptFrame = ~0ul;  // hostGfxKeepDepthForReads: this frame's depth is copied

static void copyEfbForReads(void) {
    // Two copies and the screen transfer still to come in the queue.
    if (sLastEfbReadFrame == ~0ul || sFrameNumber - sLastEfbReadFrame > EFB_COPY_FRAMES ||
        __C3D_Context.numEntries + 4 > __C3D_Context.maxEntries) {
        return;
    }
    void* sources[2] = {sTarget->frameBuf.colorBuf, sTarget->frameBuf.depthBuf};
    u32 sizes[2] = {C3D_CalcColorBufSize(sTarget->frameBuf.width, sTarget->frameBuf.height, sTarget->frameBuf.colorFmt),
                    C3D_CalcDepthBufSize(sTarget->frameBuf.width, sTarget->frameBuf.height, sTarget->frameBuf.depthFmt)};
    for (int i = 0; i < 2; i++) {
        if (sEfbCopy[i] == NULL) {
            sEfbCopy[i] = linearAlloc(sizes[i]);
            sEfbCopySize[i] = sizes[i];
        }
        if (sEfbCopy[i] == NULL || sources[i] == NULL || sEfbCopySize[i] != sizes[i]) {
            return;
        }
    }
    for (int i = 0; i < 2; i++) {
        if (i == 1 && sDepthKeptFrame == sFrameNumber) {
            continue;
        }
        GX_TextureCopy((u32*)sources[i], GX_BUFFER_DIM(sizes[i] / 16, 0), (u32*)sEfbCopy[i], GX_BUFFER_DIM(sizes[i] / 16, 0),
                       sizes[i], 8);
    }
    sEfbCopyFrame = sFrameNumber;
}

// The buffer an EFB read takes (colour or depth): the copy of the last frame, else the target.
static const u32* efbReadSource(int depth) {
    sLastEfbReadFrame = sFrameNumber;
    if (sEfbCopyFrame + 1 == sFrameNumber && sEfbCopy[depth ? 1 : 0] != NULL) {
        return (const u32*)sEfbCopy[depth ? 1 : 0];
    }
    return (const u32*)(depth ? sTarget->frameBuf.depthBuf : sTarget->frameBuf.colorBuf);
}

// ##############################################################################################
// # Draws: uniforms and state                                                                  #
// #                                                                                            #
// # Statistics of the native submissions for the log, and the upload of a model draw's         #
// # transform: only the uniform vectors that changed are sent.                                 #
// ##############################################################################################
static unsigned long sSubmitCalls, sSubmitVectors, sSubmitBinds;
static unsigned long long sSubmitStateTicks, sSubmitDrawTicks;

void hostGfxTakeSubmitStats(unsigned long* pCalls, unsigned long long* pStateTicks, unsigned long long* pDrawTicks,
                            unsigned long* pVectors, unsigned long* pBinds) {
    *pCalls = sSubmitCalls;
    *pStateTicks = sSubmitStateTicks;
    *pDrawTicks = sSubmitDrawTicks;
    *pVectors = sSubmitVectors;
    *pBinds = sSubmitBinds;
    sSubmitCalls = sSubmitVectors = sSubmitBinds = 0;
    sSubmitStateTicks = sSubmitDrawTicks = 0;
}

static void setVec(s8 base, int count, float (*values)[4], float (*uploaded)[4], bool all) {
    for (int i = 0; i < count; i++) {
        if (all || memcmp(values[i], uploaded[i], sizeof(values[i])) != 0) {
            sSubmitVectors++;
            C3D_FVUnifSet(GPU_VERTEX_SHADER, base + i, values[i][0], values[i][1], values[i][2], values[i][3]);
            memcpy(uploaded[i], values[i], sizeof(values[i]));
        }
    }
}

// Sends the uniforms of a model draw that differ from the last ones sent.
static void uploadTransform(const HostGfxTransform* pTransform) {
    HostGfxTransform* t = (HostGfxTransform*)pTransform;
    HostGfxTransform* u = &sUploaded;
    bool all = !sTransformValid;
    setVec(sLocClip, 4, t->clip, u->clip, all);
    // Rows past the ones the vertices use are left as they are: a native draw uses 3 to 30.
    const int paletteRows = t->paletteRows != 0 && t->paletteRows < 30 ? t->paletteRows : 30;
    setVec(sLocPalette, paletteRows, t->palette, u->palette, all);
    setVec(sLocTexMtx, 9, t->texmtx, u->texmtx, all);
    setVec(sLocTexPost, 9, t->texpost, u->texpost, all);
    setVec(sLocTexFinal, 6, t->texfinal, u->texfinal, all);
    setVec(sLocMaterial, 1, &t->material, &u->material, all);
    setVec(sLocAmbient, 1, &t->ambient, &u->ambient, all);
    setVec(sLocMaskMat, 1, &t->maskmat, &u->maskmat, all);
    setVec(sLocMaskAmb, 1, &t->maskamb, &u->maskamb, all);
    setVec(sLocMaskLit, 1, &t->masklit, &u->masklit, all);
    setVec(sLocSrtg, 1, &t->srtg, &u->srtg, all);
    setVec(sLocLights, t->lightCount * 5, t->lights, u->lights, all);
    if (all || t->lit != u->lit) {
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, sLocLit, t->lit);
    }
    if (all || t->lightCount != u->lightCount) {
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, sLocHasLights, t->lightCount != 0);
        if (t->lightCount != 0) {
            C3D_IVUnifSet(GPU_VERTEX_SHADER, sLocLightLoop, t->lightCount - 1, 0, 5, 0);
        }
    }
    for (int i = 0; i < 3; i++) {
        if (all || t->normalize[i] != u->normalize[i]) {
            C3D_BoolUnifSet(GPU_VERTEX_SHADER, sLocNormalize[i], t->normalize[i]);
        }
        if (all || t->divide[i] != u->divide[i]) {
            C3D_BoolUnifSet(GPU_VERTEX_SHADER, sLocDivide[i], t->divide[i]);
        }
        if (all || t->vtxmtx[i] != u->vtxmtx[i]) {
            C3D_BoolUnifSet(GPU_VERTEX_SHADER, sLocVtxMtx[i], t->vtxmtx[i]);
        }
    }
    if (all || t->nrmpalette != u->nrmpalette) {
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, sLocNrmPalette, t->nrmpalette);
    }
    u->nrmpalette = t->nrmpalette;
    if (all || t->nrmcofactor != u->nrmcofactor) {
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, sLocNrmCofactor, t->nrmcofactor);
    }
    u->nrmcofactor = t->nrmcofactor;
    u->lit = t->lit;
    u->lightCount = t->lightCount;
    memcpy(u->normalize, t->normalize, sizeof(u->normalize));
    memcpy(u->divide, t->divide, sizeof(u->divide));
    memcpy(u->vtxmtx, t->vtxmtx, sizeof(u->vtxmtx));
    sTransformValid = true;
}

// ##############################################################################################
// # Setting up a draw                                                                          #
// #                                                                                            #
// # Drops the draw when the frame is dropped or a buffer is full; merges it into the pending   #
// # one when it can; otherwise starts the GPU early if it is time, binds the window, the       #
// # shader mode, the transform and the textures, then the combiners, fog, depth, alpha test,   #
// # culling, stencil, scissor and blending: each only when it changed since the last draw.     #
// ##############################################################################################
// Sets up the GPU for a draw of indexCount indices: returns false to drop it, and sets *pMerge
// when the indices extend the pending triangle list.
static bool beginDraw(unsigned long indexCount, int primitive, int window, const HostGfxState* pState, int mode,
                      const HostGfxTransform* pTransform, unsigned long version, bool* pMerge) {
    *pMerge = false;
#if SMG_DIAG_CLEAR_ONLY
    if (sFrameNumber == 0) {
        static bool sReportedClearOnly;
        if (!sReportedClearOnly) {
            hostLog("gfx: diagnostic clear-only first frame; GPU draws suppressed\n");
            sReportedClearOnly = true;
        }
        return false;
    }
#endif
    #if SMG_DIAG_FIRST_DRAW_ONLY
    if (sFrameNumber > 1) {
        return false;
    }
    if (sFrameNumber == 1) {
        static bool sDiagnosticDrawAllowed;
        if (sDiagnosticDrawAllowed) {
            return false;
        }
        sDiagnosticDrawAllowed = true;
        hostLog("gfx: diagnostic allowing one GPU draw on frame 1; other draws suppressed\n");
    }
#endif
    if (sSkipFrame) {
        sDroppedDrawsThisFrame++;
        sDropReasons[DROP_SKIPPED_FRAME]++;
        return false;
    }
    u32 indexLimit = sIndexBufferSize;
    if (sIndexCount + indexCount > indexLimit) {
        static int sReports;
        if (sReports < 3) {
            sReports++;
            hostLog("gfx: index buffer full, dropping draws (frame %lu)\n", sFrameNumber);
        }
        sDroppedDrawsThisFrame++;
        sDropReasons[DROP_INDEX_BUFFER]++;
        return false;
    }

    bool sameTransform = mode == sMode && (mode == MODE_PRETRANSFORMED || (sTransformValid && version == sTransformVersion));
    if (sPendingDraw && primitive == HOST_GFX_TRIANGLES && hostDebugMerge() && sLastStateValid && sameTransform && window == sBoundWindow &&
        sPendingFirstIndex + sPendingIndexCount == sIndexCount && memcmp(pState, &sLastState, sizeof(HostGfxState)) == 0) {
        *pMerge = true;
        return true;
    }
    flushPendingDraw();

    // Past this point a draw could overflow the command buffer (libctru stops the program):
    // drop the rest of the frame. C3D_GetCmdBufUsage() is only updated when a frame is
    // submitted, so the live write position is checked instead.
    if (gpuCmdBufOffset + CMDBUF_RESERVE_WORDS > gpuCmdBufSize) {
        static int sReports;
        if (sReports < 3) {
            sReports++;
            hostLog("gfx: command buffer full, dropping draws (frame %lu)\n", sFrameNumber);
        }
        sDroppedDrawsThisFrame++;
        sDropReasons[DROP_COMMAND_BUFFER]++;
        return false;
    }

    // GPU stall diagnosis: end the command list every DIAG_GROUP draws while the queue has room.
    if (STALL_DRAW_DIAG && !sStallReported && sDrawsSinceSplit >= DIAG_GROUP && __C3D_Context.numEntries + 8 < __C3D_Context.maxEntries) {
        flushWrittenData();
        C3D_FrameSplit(GX_CMDLIST_FLUSH);
        sFrameOps.splits++;
        sDrawsSinceSplit = 0;
        forgetStateAfterSplit();
    }
    if (hostDebugGpuKick() && hostGetTick() - sLastKickTick >= hostGetTickFrequency() / 1000 * GPU_KICK_MS &&
        __C3D_Context.numEntries + GPU_KICK_RESERVE < __C3D_Context.maxEntries) {
        kickGpu();
    }

    bindWindow(window);
    if (mode != sMode) {
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, sLocPretransformed, mode == MODE_PRETRANSFORMED);
        sMode = mode;
    }
    if (mode == MODE_MODEL && (!sTransformValid || version != sTransformVersion)) {
        uploadTransform(pTransform);
        sTransformVersion = version;
    }

    // A projective coordinate on unit 0 (GX's s/q, t/q): the shader sends q as texcoord0w and
    // unit 0, the only one able to, divides per pixel in projection mode. A copy of the texture
    // carries that mode, so the shared one keeps 2D for other draws.
    const bool projection = mode == MODE_MODEL && pTransform != NULL && pTransform->divide[0];
    static C3D_Tex sProjectionTexture;
    static bool sBoundProjection;
    for (int unit = 0; unit < 3; unit++) {
        // Unused units get a placeholder: a stale binding may point at a deleted texture.
        bool modeChanged = unit == 0 && (projection || sBoundProjection);
        if (!sLastStateValid || pState->textures[unit] != sLastState.textures[unit] || modeChanged) {
            C3D_Tex* tex = (C3D_Tex*)pState->textures[unit];
            if (tex == NULL) {
                tex = &sBlankTexture;
            }
            if (unit == 0 && projection) {
                sProjectionTexture = *tex;
                sProjectionTexture.param = (tex->param & ~GPU_TEXTURE_MODE(7)) | GPU_TEXTURE_MODE(GPU_TEX_PROJECTION);
                tex = &sProjectionTexture;
            }
            C3D_TexBind(unit, tex);
            sSubmitBinds++;
            if (unit == 0) {
                sBoundProjection = projection;
            }
        }
    }

    applyStages(pState);

    const HostGfxState* last = sLastStateValid ? &sLastState : NULL;
    if (!last || memcmp(&pState->fogParamA, &last->fogParamA, 5 * sizeof(pState->fogParamA)) != 0) {
        applyFog(pState);
    }
    if (!last || pState->depthTest != last->depthTest || pState->depthFunc != last->depthFunc || pState->writeMask != last->writeMask) {
        C3D_DepthTest(pState->depthTest, pState->depthFunc, pState->writeMask);
    }
    if (!last || pState->alphaTestFunc != last->alphaTestFunc || pState->alphaRef != last->alphaRef) {
        C3D_AlphaTest(pState->alphaTestFunc != GPU_ALWAYS, pState->alphaTestFunc, pState->alphaRef);
    }
    if (!last || pState->cull != last->cull) {
        C3D_CullFace(pState->cull);
    }
    StencilState stencil;
    if (pState->stencilEnable) {
        stencil.enable = 1;
        stencil.func = pState->stencilFunc;
        stencil.ref = pState->stencilRef;
        stencil.mask = pState->stencilMask;
        stencil.writeMask = pState->stencilWriteMask;
        stencil.fail = pState->stencilFail;
        stencil.zfail = pState->stencilZFail;
        stencil.zpass = pState->stencilZPass;
    } else {
        stencil = sStencilAmbient;
    }
    if (!stencil.enable) {
        memset(&stencil, 0, sizeof(stencil));
    }
    if (!sStencilAppliedValid || memcmp(&stencil, &sStencilApplied, sizeof(stencil)) != 0) {
        C3D_StencilTest(stencil.enable != 0, (GPU_TESTFUNC)stencil.func, stencil.ref, stencil.mask, stencil.writeMask);
        C3D_StencilOp((GPU_STENCILOP)stencil.fail, (GPU_STENCILOP)stencil.zfail, (GPU_STENCILOP)stencil.zpass);
        sStencilApplied = stencil;
        sStencilAppliedValid = true;
    }
    if (!last || memcmp(pState->scissor, last->scissor, sizeof(pState->scissor)) != 0) {
        // The render target is the 400x240 screen on its side: frame buffer x runs along target
        // y (reversed), frame buffer y along target x (reversed).
        const unsigned short* r = pState->scissor;
        if (r[0] == 0 && r[1] == 0 && r[2] >= 640 && r[3] >= 456) {
            C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        } else {
            u32 width = 240;
            u32 height = 400;
            u32 left = width - (r[3] * width + 455) / 456, right = width - (r[1] * width) / 456;
            u32 top = height - (r[2] * height + 639) / 640, bottom = height - (r[0] * height) / 640;
            C3D_SetScissor(GPU_SCISSOR_NORMAL, left, top, right, bottom);
        }
    }
    if (!last || pState->logicOpEnable != last->logicOpEnable ||
        (pState->logicOpEnable ? pState->logicOp != last->logicOp
                               : pState->blendEquation != last->blendEquation || pState->blendSrc != last->blendSrc ||
                                     pState->blendDst != last->blendDst)) {
        if (pState->logicOpEnable) {
            C3D_ColorLogicOp(pState->logicOp);
        } else {
            C3D_AlphaBlend(pState->blendEquation, pState->blendEquation, pState->blendSrc, pState->blendDst,
                           pState->blendSrc, pState->blendDst);
        }
    }
    sLastState = *pState;
    sLastStateValid = true;
    return true;
}

// ##############################################################################################
// # Drawing arena vertices                                                                     #
// #                                                                                            #
// # The three entry points of model draws (host.h): arena vertices with indices written into   #
// # this frame's buffer, persistent indices drawn in place, and native draws, which have their #
// # own budget per frame so a broken model cannot fill the command buffer.                     #
// ##############################################################################################
static void drawArena(unsigned long first, const unsigned short* pIndices, unsigned long count, int primitive, const HostGfxState* pState,
                      int mode, const HostGfxTransform* pTransform, unsigned long version) {
    beginFrame();
    int window = (int)(first / ARENA_WINDOW);
    bool merge;
    if (!beginDraw(count, primitive, window, pState, mode, pTransform, version, &merge)) {
        return;
    }
    sDrawCallsThisFrame++;

    // Indices relative to the window.
    u16 base = (u16)(first % ARENA_WINDOW);
    u16* out = &sIndices[sIndexCount];
    if (pIndices == NULL) {
        for (unsigned long i = 0; i < count; i++) {
            out[i] = base + i;
        }
    } else {
        for (unsigned long i = 0; i < count; i++) {
            out[i] = base + pIndices[i];
        }
    }

    if (merge) {
        sPendingIndexCount += count;
    } else if (primitive == HOST_GFX_TRIANGLES) {
        sPendingDraw = true;
        sPendingFirstIndex = sIndexCount;
        sPendingIndexCount = count;
    } else {
        // Strips and fans go to the GPU as they are (same winding rules as GX).
        drawElements(primitive == HOST_GFX_STRIP ? GPU_TRIANGLE_STRIP : GPU_TRIANGLE_FAN, count, out);
    }
    sIndexCount += count;
}

void hostGfxDrawArena(unsigned long first, const unsigned short* pIndices, unsigned long count, int primitive, const HostGfxState* pState,
                      const HostGfxTransform* pTransform, unsigned long transformVersion) {
    if (hostDebugGxSkip() == 1 || hostDebugGxSkip() == 2) {
        return;
    }
    drawArena(first, pIndices, count, primitive, pState, MODE_MODEL, pTransform, transformVersion);
}

void hostGfxDrawIndexed(unsigned long first, const unsigned short* pIndices, unsigned long count, const HostGfxState* pState,
                        const HostGfxTransform* pTransform, unsigned long transformVersion) {
    if (hostDebugGxSkip() == 1 || hostDebugGxSkip() == 2) {
        return;
    }
    beginFrame();
    bool merge;
    if (!beginDraw(0, PRIM_CACHED_INDICES, (int)(first / ARENA_WINDOW), pState, MODE_MODEL, pTransform, transformVersion, &merge)) {
        return;
    }
    sDrawCallsThisFrame++;
    drawElements(GPU_TRIANGLES, count, pIndices);
}

static bool sProfileTicks;

void hostGfxSetProfile(int on) {
    sProfileTicks = on != 0;
}

int hostGfxDrawNativeIndexed(unsigned long first, const unsigned short* pIndices, unsigned long count, const HostGfxState* pState,
                             const HostGfxTransform* pTransform, unsigned long transformVersion) {
    if (hostDebugGxSkip() == 1 || hostDebugGxSkip() == 2) {
        return false;
    }
    beginFrame();
    if (sSkipFrame || sNativeDrawsThisFrame >= NATIVE_DRAW_BUDGET) {
        sDroppedDrawsThisFrame++;
        sDropReasons[sSkipFrame ? DROP_SKIPPED_FRAME : DROP_NATIVE_BUDGET]++;
        if (!sNativeBudgetReported && sNativeDrawsThisFrame >= NATIVE_DRAW_BUDGET) {
            hostLog("gfx: native draw budget reached (%lu), dropping remaining native draws (frame %lu)\n",
                    (unsigned long)NATIVE_DRAW_BUDGET, sFrameNumber);
            sNativeBudgetReported = true;
        }
        return false;
    }
    bool merge;
    const u64 start = sProfileTicks ? svcGetSystemTick() : 0;
    if (!beginDraw(0, PRIM_CACHED_INDICES, (int)(first / ARENA_WINDOW), pState, MODE_MODEL, pTransform, transformVersion, &merge)) {
        return false;
    }
    const u64 drawn = sProfileTicks ? svcGetSystemTick() : 0;
    sDrawCallsThisFrame++;
    drawElements(GPU_TRIANGLES, count, pIndices);
    sNativeDrawsThisFrame++;
    sSubmitCalls++;
    if (sProfileTicks) {
        sSubmitStateTicks += drawn - start;
        sSubmitDrawTicks += svcGetSystemTick() - drawn;
    }
    return true;
}

// ##############################################################################################
// # Drawing clip-space vertices                                                                #
// #                                                                                            #
// # CPU-made vertices are copied into this frame's arena, turned onto the sideways target,     #
// # with w in the normal's x for the shader's pretransformed mode. A draw wholly inside the    #
// # guard band goes as it is. Otherwise it still goes as it is (the PICA clips the near plane  #
// # itself) unless it lies wholly outside one plane, and then it is dropped. With debug.txt    #
// # cpuclip=1, or when a vertex is NaN or infinite, it is clipped triangle by triangle on the  #
// # CPU (clip.c), and the triangles with such vertices are dropped.                            #
// ##############################################################################################
// The vertices' place in the arena, or HOST_GFX_NO_ARENA.
static unsigned long drawPretransformed(const HostGfxVertex* pVertices, unsigned long count, const unsigned short* pIndices,
                                        unsigned long indexCount, int primitive, const HostGfxState* pState) {
    HostGfxModelVertex* out;
    unsigned long first = hostGfxArenaAlloc(count, 0, &out);
    if (first == HOST_GFX_NO_ARENA) {
        sDroppedDrawsThisFrame++;
        sDropReasons[DROP_TRANSIENT_VERTICES]++;
        return first;
    }
    // Clip-space positions: x, y, z in the position, w in the normal's x. The render target is the
    // 400x240 screen on its side, so the Wii's clip x runs along target y and its clip y along
    // target -x (as the model path's view-to-clip matrix, draw.cpp buildNativeViewToClip).
    for (unsigned long i = 0; i < count; i++) {
        const HostGfxVertex* v = &pVertices[i];
        HostGfxModelVertex* o = &out[i];
        o->x = v->y;
        o->y = -v->x;
        o->z = v->z;
        o->nx = v->w;
        memcpy(o->rgba, v->rgba, 4);
        o->tex[0][0] = v->s0;
        o->tex[0][1] = v->t0;
        o->tex[1][0] = v->s1;
        o->tex[1][1] = v->t1;
        o->tex[2][0] = v->s2;
        o->tex[2][1] = v->t2;
    }
    drawArena(first, pIndices, indexCount, primitive, pState, MODE_PRETRANSFORMED, NULL, 0);
    return first;
}

static void lerpVertex(void* pOut, const void* pA, const void* pB, float t) {
    const HostGfxVertex* a = (const HostGfxVertex*)pA;
    const HostGfxVertex* b = (const HostGfxVertex*)pB;
    HostGfxVertex* o = (HostGfxVertex*)pOut;
    const float* fa = &a->x;
    const float* fb = &b->x;
    float* fo = &o->x;
    for (int i = 0; i < 4; i++) {
        fo[i] = fa[i] + (fb[i] - fa[i]) * t;
    }
    fa = &a->s0;
    fb = &b->s0;
    fo = &o->s0;
    for (int i = 0; i < 6; i++) {
        fo[i] = fa[i] + (fb[i] - fa[i]) * t;
    }
    for (int i = 0; i < 4; i++) {
        o->rgba[i] = (unsigned char)(a->rgba[i] + (b->rgba[i] - a->rgba[i]) * t + 0.5f);
    }
}

// Vertices of the triangles clipped by hostGfxDraw, drawn as one triangle list.
#define CLIP_OUT_VERTICES 1536
static HostGfxVertex sClipOut[CLIP_OUT_VERTICES];
static unsigned long sClippedDraws, sClippedTriangles, sInvalidTriangles, sInvalidReports;

static unsigned long sDrawnAgainFirst = HOST_GFX_NO_ARENA;  // hostGfxDrawAgain

int hostGfxDrawAgain(const unsigned short* pIndices, unsigned long indexCount, int primitive, const HostGfxState* pState) {
    if (sDrawnAgainFirst == HOST_GFX_NO_ARENA) {
        return 0;
    }
    drawArena(sDrawnAgainFirst, pIndices, indexCount, primitive, pState, MODE_PRETRANSFORMED, NULL, 0);
    return 1;
}

void hostGfxDraw(const HostGfxVertex* pVertices, unsigned long count, const unsigned short* pIndices, unsigned long indexCount,
                 int primitive, const HostGfxState* pState) {
    bool inside = true;
    for (unsigned long i = 0; i < count && inside; i++) {
        inside = hostClipOutcode(&pVertices[i].x) == 0;
    }
    sDrawnAgainFirst = HOST_GFX_NO_ARENA;
    if (inside) {
        sDrawnAgainFirst = drawPretransformed(pVertices, count, pIndices, indexCount, primitive, pState);
        return;
    }
    if (!hostDebugCpuClip()) {
        // As native draws (CIA 0.12.10): the PICA clips the near plane itself, so a draw reaching
        // past the near plane or the guard band goes as it is, unless it lies wholly outside one.
        unsigned all = ~0u, any = 0;
        for (unsigned long i = 0; i < count; i++) {
            unsigned code = hostClipOutcode(&pVertices[i].x);
            all &= code;
            any |= code;
        }
        if ((any & HOST_CLIP_INVALID_BIT) == 0) {
            if (all == 0) {
                sDrawnAgainFirst = drawPretransformed(pVertices, count, pIndices, indexCount, primitive, pState);
            }
            return;
        }
    }

    // Some vertex lies behind the near plane or far outside the screen: the primitive becomes a
    // triangle list, clipped triangle by triangle (strips keep the GPU's alternating winding).
    sClippedDraws++;
    unsigned long triangles = primitive == HOST_GFX_TRIANGLES ? indexCount / 3 : indexCount >= 3 ? indexCount - 2 : 0;
    unsigned long used = 0;
    for (unsigned long k = 0; k < triangles; k++) {
        unsigned long i0, i1, i2;
        if (primitive == HOST_GFX_TRIANGLES) {
            i0 = 3 * k, i1 = 3 * k + 1, i2 = 3 * k + 2;
        } else if (primitive == HOST_GFX_STRIP) {
            i0 = (k & 1) ? k + 1 : k, i1 = (k & 1) ? k : k + 1, i2 = k + 2;
        } else {
            i0 = 0, i1 = k + 1, i2 = k + 2;
        }
        if (pIndices != NULL) {
            i0 = pIndices[i0], i1 = pIndices[i1], i2 = pIndices[i2];
        }
        if (i0 >= count || i1 >= count || i2 >= count) {
            continue;
        }
        HostGfxVertex polygon[HOST_CLIP_MAX_VERTICES], scratch[HOST_CLIP_MAX_VERTICES];
        polygon[0] = pVertices[i0];
        polygon[1] = pVertices[i1];
        polygon[2] = pVertices[i2];
        unsigned c0 = hostClipOutcode(&polygon[0].x), c1 = hostClipOutcode(&polygon[1].x), c2 = hostClipOutcode(&polygon[2].x);
        if ((c0 | c1 | c2) & HOST_CLIP_INVALID_BIT) {
            // A NaN or infinite vertex: the triangle has no shape to draw.
            if (sInvalidReports < 8) {
                sInvalidReports++;
                hostLog("gfx: draw with NaN or infinite vertices (caller %p, frame %lu), its triangles dropped\n",
                        __builtin_return_address(0), (unsigned long)sFrameNumber);
            }
            sInvalidTriangles++;
            continue;
        }
        if (c0 & c1 & c2) {
            continue;  // wholly outside one plane
        }
        int n = 3;
        if (c0 | c1 | c2) {
            n = hostClipPolygon((unsigned char*)polygon, 3, sizeof(HostGfxVertex), lerpVertex, (unsigned char*)scratch);
            sClippedTriangles++;
        }
        if (used + 3 * (unsigned long)(n - 2) > CLIP_OUT_VERTICES) {
            drawPretransformed(sClipOut, used, NULL, used, HOST_GFX_TRIANGLES, pState);
            used = 0;
        }
        for (int j = 1; j + 1 < n; j++) {
            sClipOut[used++] = polygon[0];
            sClipOut[used++] = polygon[j];
            sClipOut[used++] = polygon[j + 1];
        }
    }
    if (used != 0) {
        drawPretransformed(sClipOut, used, NULL, used, HOST_GFX_TRIANGLES, pState);
    }
}

// ##############################################################################################
// # Ending a frame                                                                             #
// #                                                                                            #
// # Times the loop for the pacing, sends the pending draw and the rest of the frame, makes the #
// # copy EFB reads use, and ends the frame (citro3d then shows it on the top screen). Every    #
// # 300 frames the memory, arena and dropped-draw counters go to the log.                      #
// ##############################################################################################
// Saves the top screen as raw 240x400 BGR8 (the framebuffer's own layout) to sdmc:/smg.
static void dumpFrame(const char* path) {
    u16 width, height;
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &width, &height);
    FILE* file = fopen(path, "wb");
    if (file != NULL) {
        fwrite(fb, 1, width * height * 3, file);
        fclose(file);
    }
}

void hostGfxEndFrame(unsigned long clearColor) {
    hostDebugTrace("hostGfxEndFrame enter", sFrameNumber, hostVBlankCount(), sIndexCount);
    const unsigned long long frameEndTick = svcGetSystemTick();
    if (sLastFrameEndTick != 0) {
        float ms = (float)(frameEndTick - sLastFrameEndTick) * (1000.0f / SYSCLOCK_ARM11);
        sLoopMs = ms < 100.0f ? ms : 100.0f;
        sFrameMs += (sLoopMs - sFrameMs) * 0.125f;
    }
    sLastFrameEndTick = frameEndTick;
    beginFrame();
    planNextFrame();
    flushPendingDraw();
    memset(&sStencilAmbient, 0, sizeof(sStencilAmbient));  // a frame starts without one
    sClearColor = clearColor;
    static unsigned long sSkipped;
    if (sSkipFrame) {
        sInFrame = false;
        sSkipped++;
        if (sShotRequested[0] != '\0') {
            hostLog("gfx: %s not saved, the frame was dropped\n", sShotRequested);
            sShotRequested[0] = '\0';
        }
        return;
    }

    sLastFrameVBlank = hostVBlankCount();
    hostDebugTrace("hostGfxEndFrame before GPU end", sFrameNumber, sIndexCount, sTransientUsed);
#if SMG_SHOWCASE
    if (sDrawCallsThisFrame == 0) {
        C3D_RenderTargetClear(sTarget, C3D_CLEAR_ALL, 0x000000FF, 0);
    }
#endif
    flushWrittenData();
    sInFrame = false;
    hostDebugTrace("Citro3D FrameEnd", sFrameNumber, sIndexCount, sTransientUsed);
    bool logFirstGpuFrames = sGpuFrameEndCount < 4;
    if (logFirstGpuFrames || sGpuFrameEndCount % 30 == 29) {
        hostLog("gfx: Citro3D FrameEnd submit #%lu frame %lu target %ux%u fmt %d screen %d side %d transfer 0x%08lx indices %lu transient %lu\n",
                sGpuFrameEndCount + 1, sFrameNumber, sTarget->frameBuf.width, sTarget->frameBuf.height,
                sTarget->frameBuf.colorFmt, sTarget->screen, sTarget->side, (unsigned long)sTarget->transferFlags,
                sIndexCount, sTransientUsed);
        hostLog("gfx: frame draws=%lu native=%lu dropped=%lu\n", (unsigned long)sDrawCallsThisFrame,
                (unsigned long)sNativeDrawsThisFrame, (unsigned long)sDroppedDrawsThisFrame);
    }
    if (sGpuFrameEndCount == ((SMG_DIAG_CLEAR_ONLY && SMG_DIAG_FIRST_DRAW_ONLY) ? 1 : 0)) {
        u32* commandBuffer = NULL;
        u32 commandBufferSize = 0, commandBufferOffset = 0;
        GPUCMD_GetBuffer(&commandBuffer, &commandBufferSize, &commandBufferOffset);
        FILE* file = fopen("sdmc:/smg/c3d_first_frame.bin", "wb");
        int saved = 0;
        if (file != NULL) {
            // CMD1, allocated capacity in words, used words, then the raw PICA command stream.
            u32 header[3] = {0x31444D43, commandBufferSize, commandBufferOffset};
            size_t headerWords = fwrite(header, sizeof(u32), 3, file);
            size_t commandWords = commandBuffer != NULL
                                      ? fwrite(commandBuffer, sizeof(u32), commandBufferOffset, file)
                                      : 0;
            saved = headerWords == 3 && commandWords == commandBufferOffset;
            fclose(file);
        }
        hostLog("gfx: first PICA command buffer %lu/%lu words, saved %d to sdmc:/smg/c3d_first_frame.bin\n",
                (unsigned long)commandBufferOffset, (unsigned long)commandBufferSize, saved);
    }
    sFrameOps.frame = sFrameNumber;
    sFrameOps.draws = sDrawCallsThisFrame;
    sFrameOps.indices = sIndexCount;
    sFrameOps.transient = sTransientUsed;
    sFrameOps.cmdWords = (u32)(gpuCmdBuf + gpuCmdBufOffset - sFrameCmdBase);
    sSubmittedOps = sFrameOps;
    if (!sStallReported) {
        memcpy(sSubmittedRecords, sDrawRecords, sDrawRecordCount * sizeof(DrawRecord));
        sSubmittedRecordCount = sDrawRecordCount;
    }
    // The rest of the frame as a flushed list (the queue may be running), then the copy EFB reads use.
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    copyEfbForReads();
    C3D_FrameEnd(0);
    sGpuFrameEndCount++;
    hostDebugTrace("Citro3D FrameEnd returned", sFrameNumber, hostVBlankCount(), 0);
    if (logFirstGpuFrames) {
        hostLog("gfx: Citro3D FrameEnd returned #%lu frame %lu vblank %lu\n", sGpuFrameEndCount, sFrameNumber,
                hostVBlankCount());
        logGpuProgress("FrameEnd returned");
    }

    sFrameNumber++;
    // Lowest free linear memory since the start, for the memory budget (Old 3DS).
    static unsigned long sLinearMinFree = 0xFFFFFFFFul;
    const unsigned long linearFree = (unsigned long)linearSpaceFree();
    if (linearFree < sLinearMinFree) {
        sLinearMinFree = linearFree;
    }
    if (sFrameNumber % 300 == 0) {
        hostLog("gfx: %lu frames shown, %lu dropped, vblank %lu, free %lu KB heap, %lu KB linear (lowest %lu KB), main stack up to %lu of 256 KB\n",
                sFrameNumber, sSkipped, hostVBlankCount(), hostGetFreeMemory() / 1024, linearFree / 1024, sLinearMinFree / 1024,
                hostMainStackUsed() / 1024);
        hostLog("gfx: %lu fog LUTs built, %lu EFB reads, GPU started %lu times during frames\n", sFogLutBuilds, sEfbReads,
                sGpuKicks);
        sFogLutBuilds = 0;
        sEfbReads = 0;
        sGpuKicks = 0;
        u32 peakVertices = sPersistentUsed > sArenaPeakVertices ? sPersistentUsed : sArenaPeakVertices;
        u32 peakIndices = sPersistentIndexUsed > sArenaPeakIndices ? sPersistentIndexUsed : sArenaPeakIndices;
        hostLog("gfx: persistent arena emptied %lu times when full (%lu for vertices, %lu for indices), arena position up to %lu of %lu "
                "and %lu of %lu indices in use\n",
                sArenaFullVertices + sArenaFullIndices, sArenaFullVertices, sArenaFullIndices, (unsigned long)peakVertices,
                (unsigned long)sArenaPersistentEnd, (unsigned long)peakIndices, (unsigned long)sPersistentIndexSize);
        hostLog("gfx: draws dropped: skipped frame %lu, index buffer %lu, command buffer %lu, native budget %lu, transient vertices %lu\n",
                sDropReasons[DROP_SKIPPED_FRAME], sDropReasons[DROP_INDEX_BUFFER], sDropReasons[DROP_COMMAND_BUFFER],
                sDropReasons[DROP_NATIVE_BUDGET], sDropReasons[DROP_TRANSIENT_VERTICES]);
        sArenaFullVertices = sArenaFullIndices = sArenaPeakVertices = sArenaPeakIndices = 0;
        memset(sDropReasons, 0, sizeof(sDropReasons));
        if (sClippedDraws != 0) {
            hostLog("gfx: %lu clip-space draws clipped before the GPU (%lu triangles, %lu dropped for NaN or infinite vertices)\n",
                    sClippedDraws, sClippedTriangles, sInvalidTriangles);
            sClippedDraws = sClippedTriangles = sInvalidTriangles = 0;
        }
        dumpFrame("sdmc:/smg/frame.bin");
    }
    unsigned long shot = hostDebugShotDue();
    if (sShotRequested[0] != '\0') {
        snprintf(sShotPending, sizeof(sShotPending), "%s", sShotRequested);
        sShotRequested[0] = '\0';
    } else if (hostDebugTevShotDue()) {
        snprintf(sShotPending, sizeof(sShotPending), "sdmc:/smg/shot_tev.bin");
    } else if (shot != 0) {
        snprintf(sShotPending, sizeof(sShotPending), "sdmc:/smg/shot_%lu.bin", shot);
    } else if ((shot = hostDebugShotFrameDue()) != 0) {
        snprintf(sShotPending, sizeof(sShotPending), "sdmc:/smg/shot_f%lu.bin", shot);
    }
}

// ##############################################################################################
// # Depth for the next frame's reads                                                           #
// #                                                                                            #
// # A game object that clears the depth buffer in the middle of the frame (MR::clearZBuffer)   #
// # would leave the frame end's copy without the depth the reads need: the depth is copied     #
// # before the clear.                                                                          #
// ##############################################################################################
void hostGfxKeepDepthForReads(void) {
    beginFrame();
    if (sSkipFrame || sLastEfbReadFrame == ~0ul || sFrameNumber - sLastEfbReadFrame > EFB_COPY_FRAMES ||
        __C3D_Context.numEntries + 6 > __C3D_Context.maxEntries) {
        return;
    }
    const u32 size = C3D_CalcDepthBufSize(sTarget->frameBuf.width, sTarget->frameBuf.height, sTarget->frameBuf.depthFmt);
    if (sEfbCopy[1] == NULL) {
        sEfbCopy[1] = linearAlloc(size);
        sEfbCopySize[1] = size;
    }
    if (sEfbCopy[1] == NULL || sEfbCopySize[1] != size || sTarget->frameBuf.depthBuf == NULL) {
        return;
    }
    flushPendingDraw();
    flushWrittenData();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    sFrameOps.splits++;
    GX_TextureCopy((u32*)sTarget->frameBuf.depthBuf, GX_BUFFER_DIM(size / 16, 0), (u32*)sEfbCopy[1], GX_BUFFER_DIM(size / 16, 0), size, 8);
    forgetStateAfterSplit();
    sDepthKeptFrame = sFrameNumber;
}

// ##############################################################################################
// # Reading one frame buffer pixel                                                             #
// #                                                                                            #
// # A Wii frame buffer pixel (640x456) maps to a texel of the sideways target; only its cache  #
// # line is read. The depth buffer runs the other way from the Wii's, so the value is turned   #
// # back.                                                                                      #
// ##############################################################################################
int hostGfxPeekEfb(unsigned short x, unsigned short y, int depth, unsigned long* pValue) {
    if (pValue == NULL || x >= 640 || y >= 456) {
        return 0;
    }

    beginFrame();
    if (sSkipFrame) {
        *pValue = depth ? 0 : ((sClearColor & 0xFF) << 24) | ((sClearColor >> 8) & 0x00FFFFFF);
        return 1;
    }

    flushPendingDraw();
    flushWrittenData();
    sFrameOps.peeks++;
    sEfbReads++;
    // The last finished frame (efbReadSource). (A frame split and C3D_FrameSync here waited for
    // the next vblank, up to 16 ms, for nothing: before CIA 0.12.2.)

    u32 targetWidth = 240;
    u32 targetHeight = 400;
    u32 column = targetWidth - 1 - (u32)y * targetWidth / 456;
    u32 row = (u32)x * targetHeight / 640;
    u32 stride = sTarget->frameBuf.width;
    u32 offset = ((row / 8) * (stride / 8) + column / 8) * 64 + mortonOffset(column & 7, row & 7);

    const u32* source = efbReadSource(depth);
    // Only the cache line of the texel is read. Through the kernel: the GSP service is a request to
    // another process. A homebrew launcher's title may not have this call.
    if (envIsHomebrew()) {
        GSPGPU_InvalidateDataCache((void*)((u32)&source[offset] & ~31u), 32);
    } else {
        svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)&source[offset] & ~31u, 32);
    }
    if (depth) {
        u32 depthValue = source[offset] & 0x00FFFFFF;
        *pValue = 0x00FFFFFF - depthValue;
    } else {
        u32 rgba = source[offset];
        *pValue = ((rgba & 0xFF) << 24) | ((rgba >> 8) & 0x00FFFFFF);
    }
    return 1;
}

// ##############################################################################################
// # EFB copies                                                                                 #
// #                                                                                            #
// # The Wii copies part of the frame buffer into a texture (bloom, water, captures); here the  #
// # GPU copies whole tiles of the render target into a texture, queued after the drawing so    #
// # far.                                                                                       #
// ##############################################################################################
// The render target (240x400, the screen on its side) is RGBA8 in 8x8 tiles of 256 bytes,
// stored tile row by tile row along its 400-pixel side: frame buffer x goes along target
// rows (y = x * 400 / 640), frame buffer y along target columns, reversed (x = 240 - y * 240
// / 456). A copy takes whole tiles: the tile rectangle covering the copy goes into the top
// left of a power-of-two texture, with GX_TextureCopy queued after the drawing so far.
void* hostGfxCopyToTexture(void* reuse, float x0, float y0, float x1, float y1, float* coeffs) {
    beginFrame();
    if (coeffs == NULL || sSkipFrame) {
        return NULL;
    }
    // The copy, the edge copies (up to 2 * log2 of the sides) and its list take queue entries, and
    // the frame end four more: with no room the texture keeps its last image.
    if (__C3D_Context.numEntries + 24 > __C3D_Context.maxEntries) {
        return reuse;
    }

    const u32 targetWidth = 240;
    const u32 targetHeight = 400;
    x0 = x0 < 0.0f ? 0.0f : x0;
    y0 = y0 < 0.0f ? 0.0f : y0;
    x1 = x1 > 640.0f ? 640.0f : x1;
    y1 = y1 > 456.0f ? 456.0f : y1;
    if (x1 <= x0 || y1 <= y0) {
        return NULL;
    }
    const u32 sourceTileWidth = sTarget->frameBuf.width / 8;
    const float kx = (float)targetHeight / 640.0f, ky = (float)targetWidth / 456.0f;
    int colLo = (int)(targetWidth - y1 * ky), colHi = (int)(targetWidth - y0 * ky + 0.999f);
    int rowLo = (int)(x0 * kx), rowHi = (int)(x1 * kx + 0.999f);
    colLo = colLo < 0 ? 0 : colLo & ~7;
    rowLo = rowLo < 0 ? 0 : rowLo & ~7;
    colHi = colHi > (int)targetWidth ? (int)targetWidth : (colHi + 7) & ~7;
    rowHi = rowHi > (int)targetHeight ? (int)targetHeight : (rowHi + 7) & ~7;
    if (colHi <= colLo || rowHi <= rowLo) {
        return NULL;
    }
    u32 width = colHi - colLo, height = rowHi - rowLo;
    u32 potW = 8, potH = 8;
    while (potW < width) {
        potW <<= 1;
    }
    while (potH < height) {
        potH <<= 1;
    }

    C3D_Tex* tex = (C3D_Tex*)reuse;
    if (tex == NULL || tex->width != potW || tex->height != potH) {
        if (tex != NULL) {
            hostGfxTextureDestroy(tex);
        }
        tex = malloc(sizeof(C3D_Tex));
        if (tex == NULL || !C3D_TexInit(tex, potW, potH, GPU_RGBA8)) {
            free(tex);
            return NULL;
        }
        C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    }

    // Everything drawn so far must reach the render target before the copy.
    flushPendingDraw();
    flushWrittenData();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);  // flushed: the queue may be running (kickGpu)
    sFrameOps.splits++;
    u32 tiles = width / 8;
    u32 lineBytes = tiles * 256;
    u8* src = (u8*)sTarget->frameBuf.colorBuf + ((rowLo / 8) * sourceTileWidth + colLo / 8) * 256;
    if (sFrameOps.copies++ == 0) {
        sFrameOps.copyIn = osConvertVirtToPhys(src);
        sFrameOps.copyInDim = GX_BUFFER_DIM(lineBytes / 16, (sourceTileWidth - tiles) * 256 / 16);
        sFrameOps.copyOut = osConvertVirtToPhys(tex->data);
        sFrameOps.copyOutDim = GX_BUFFER_DIM(lineBytes / 16, (potW / 8 - tiles) * 256 / 16);
        sFrameOps.copySize = lineBytes * (height / 8);
    }
    GX_TextureCopy((u32*)src, GX_BUFFER_DIM(lineBytes / 16, (sourceTileWidth - tiles) * 256 / 16), (u32*)tex->data,
                   GX_BUFFER_DIM(lineBytes / 16, (potW / 8 - tiles) * 256 / 16), lineBytes * (height / 8), 8);
    // The texture is larger than the image (power-of-two sides) and its rest is never written:
    // memory of other textures, different every frame (a capture's texture is new each time).
    // Filtering, the bloom's blur taps and water's projected coordinates read past the image's
    // edge there: stale light flickered along the screen's border (Festival) and a black zig-zag
    // crossed the water (0.14.1). GX clamps at the image's edge, so the edge texels are repeated
    // over the rest: texture columns from width, then memory rows from height (the other two
    // edges are the texture's own and clamp). Each copy doubles the repeated part; the queue runs
    // them after the image's copy, in order.
    const u32 texTiles = potW / 8;
    if (width < potW) {
        GX_TextureCopy((u32*)(src + (tiles - 1) * 256), GX_BUFFER_DIM(256 / 16, (sourceTileWidth - 1) * 256 / 16),
                       (u32*)((u8*)tex->data + tiles * 256), GX_BUFFER_DIM(256 / 16, (texTiles - 1) * 256 / 16),
                       256 * (height / 8), 8);
        for (u32 done = 1; tiles + done < texTiles;) {
            const u32 n = done < texTiles - tiles - done ? done : texTiles - tiles - done;
            GX_TextureCopy((u32*)((u8*)tex->data + tiles * 256), GX_BUFFER_DIM(n * 256 / 16, (texTiles - n) * 256 / 16),
                           (u32*)((u8*)tex->data + (tiles + done) * 256), GX_BUFFER_DIM(n * 256 / 16, (texTiles - n) * 256 / 16),
                           n * 256 * (height / 8), 8);
            done += n;
        }
    }
    if (height < potH) {
        const u32 rows = height / 8, texRows = potH / 8, rowBytes = texTiles * 256;
        u8* image = (u8*)tex->data;
        GX_TextureCopy((u32*)(image + (rows - 1) * rowBytes), GX_BUFFER_DIM(rowBytes / 16, 0), (u32*)(image + rows * rowBytes),
                       GX_BUFFER_DIM(rowBytes / 16, 0), rowBytes, 8);
        for (u32 done = 1; rows + done < texRows;) {
            const u32 n = done < texRows - rows - done ? done : texRows - rows - done;
            GX_TextureCopy((u32*)(image + rows * rowBytes), GX_BUFFER_DIM(n * rowBytes / 16, 0),
                           (u32*)(image + (rows + done) * rowBytes), GX_BUFFER_DIM(n * rowBytes / 16, 0), n * rowBytes, 8);
            done += n;
        }
    }

    // The split restarts the GPU state: the shader program is configured again, which resets its
    // boolean uniforms (pretransformed, lit...), so the mode and the transform are sent again.
    forgetStateAfterSplit();

    // Texel (column n, memory row m) is sampled at u = n / potW, v = 1 - m / potH.
    coeffs[0] = (targetWidth - y0 * ky - colLo) / potW;
    coeffs[1] = -(y1 - y0) * ky / potW;
    coeffs[2] = 1.0f - (x0 * kx - rowLo) / potH;
    coeffs[3] = -(x1 - x0) * kx / potH;
    return tex;
}

int hostGfxReadRgb565(unsigned x, unsigned y, unsigned width, unsigned height, unsigned short* pixels) {
    if (pixels == NULL || width == 0 || height == 0 || x + width > 640 || y + height > 456) {
        return 0;
    }

    beginFrame();
    if (sSkipFrame) {
        memset(pixels, 0, width * height * sizeof(*pixels));
        return 1;
    }

    flushPendingDraw();
    flushWrittenData();
    sFrameOps.peeks++;
    sEfbReads++;

    const u32 targetWidth = 240;
    const u32 targetHeight = 400;
    const u32 stride = sTarget->frameBuf.width;
    const u32 size = C3D_CalcColorBufSize(sTarget->frameBuf.width, sTarget->frameBuf.height, sTarget->frameBuf.colorFmt);
    // The last finished frame, as hostGfxPeekEfb.
    const u32* source = efbReadSource(0);
    GSPGPU_InvalidateDataCache((void*)source, size);

    for (unsigned row = 0; row < height; row++) {
        for (unsigned column = 0; column < width; column++) {
            const u32 fbX = x + column;
            const u32 fbY = y + row;
            const u32 targetColumn = targetWidth - 1 - fbY * targetWidth / 456;
            const u32 targetRow = fbX * targetHeight / 640;
            const u32 offset = ((targetRow / 8) * (stride / 8) + targetColumn / 8) * 64 +
                               mortonOffset(targetColumn & 7, targetRow & 7);
            const u32 rgba = source[offset];
            const u32 red = rgba & 0xFF;
            const u32 green = (rgba >> 8) & 0xFF;
            const u32 blue = (rgba >> 16) & 0xFF;
            pixels[row * width + column] = (unsigned short)(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
        }
    }
    return 1;
}

void hostGfxRequestShot(const char* path) {
    snprintf(sShotNextFrame, sizeof(sShotNextFrame), "%s", path);
}
