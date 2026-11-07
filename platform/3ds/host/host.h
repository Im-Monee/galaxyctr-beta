#pragma once

// ##############################################################################################
// # The host interface                                                                         #
// #                                                                                            #
// # The only interface between the game world (the decompiled game and the Wii SDK             #
// # replacements of platform/3ds/rvl, compiled with the Wii's ABI) and the host world (this    #
// # folder, compiled against libctru and citro3d): DETAILS.md, section 4. Keep it to plain C   #
// # types: no enums, no libctru types, so both worlds read the same layout.                    #
// ##############################################################################################

#ifdef __cplusplus
extern "C" {
#endif

// ##############################################################################################
// # Log and fatal errors                                                                       #
// #                                                                                            #
// # hostLog writes to the bottom screen and to sdmc:/smg/log.txt (main.c); hostFatal stops the #
// # game with a message. Wii SDK functions the port does not implement report themselves once  #
// # (hostStubHit).                                                                             #
// ##############################################################################################
// Logging. Messages go to the bottom-screen console and to sdmc:/smg/log.txt.
void hostLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void hostVLog(const char* fmt, __builtin_va_list args);

// Stops the game, shows the message and waits for START to exit.
void hostFatal(const char* fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

// Called by every unimplemented Wii SDK function the first time it runs.
void hostStubHit(const char* name);

// ##############################################################################################
// # Main loop and controls                                                                     #
// #                                                                                            #
// # hostMainLoop runs once per game frame (main.c): it reads the buttons, the circle pad, the  #
// # C-Stick and the touch screen into HostInput, which rvl/kpad.cpp turns into the Wii Remote  #
// # and Nunchuk the game expects.                                                              #
// ##############################################################################################
// Returns true while the app should keep running (false once the user quits).
int hostMainLoop(void);

// Controls, read once per frame by hostMainLoop. Buttons use the 3DS bit values (libctru
// KEY_*); the circle pad is about -156..156; the touch screen is 320x240.
#define HOST_KEY_A (1u << 0)
#define HOST_KEY_B (1u << 1)
#define HOST_KEY_SELECT (1u << 2)
#define HOST_KEY_START (1u << 3)
#define HOST_KEY_DRIGHT (1u << 4)
#define HOST_KEY_DLEFT (1u << 5)
#define HOST_KEY_DUP (1u << 6)
#define HOST_KEY_DDOWN (1u << 7)
#define HOST_KEY_R (1u << 8)
#define HOST_KEY_L (1u << 9)
#define HOST_KEY_X (1u << 10)
#define HOST_KEY_Y (1u << 11)
#define HOST_KEY_ZL (1u << 14)
#define HOST_KEY_ZR (1u << 15)
typedef struct {
    unsigned long held;
    short stickX, stickY;
    short cstickX, cstickY;  // New 3DS C-Stick (0 when absent)
    int touching;
    int touchX, touchY;
} HostInput;
void hostInputGet(HostInput* pInput);

// ##############################################################################################
// # Debug options                                                                              #
// #                                                                                            #
// # Options read from sdmc:/smg/debug.txt (debug.c) for development builds: a start stage,     #
// # timed screenshots and captures, A/B switches that turn parts of the renderer off to time   #
// # them. Showcase and beta builds ignore the file (showcase.h).                               #
// ##############################################################################################
// Debug options from sdmc:/smg/debug.txt (debug.c).
void hostDebugInit(void);
// True while the "turbo" retraces run (retraces come as fast as possible, nothing is drawn).
int hostDebugTurbo(void);
// Called by the renderer after each frame (turbo only starts after the first one).
void hostDebugFrameDrawn(void);
// GX frame whose draws are logged, or 0.
unsigned long hostDebugLogFrame(void);
// Buttons held by the "press" option at this moment.
unsigned long hostDebugPressedKeys(void);
// GX frame to capture to sdmc:/smg/capture.bin ("capture" option), or 0.
unsigned long hostDebugCaptureFrame(void);
// Retrace of a "shot" screenshot due now (the caller saves the top screen), or 0.
unsigned long hostDebugShotDue(void);
// "TEV approximated" was logged (debug.txt capture=tev / shot=tev wait for it).
void hostDebugTevSeen(void);
// True once, at the frame boundary after the first "TEV approximated" (capture=tev).
int hostDebugCaptureNow(void);
// True once after the first "TEV approximated" (shot=tev: saved to shot_tev.bin).
int hostDebugTevShotDue(void);
// Saves the render target of the frame being drawn to this file once it is rendered
// (same format as the "shot" option; used by the GX capture).
void hostGfxRequestShot(const char* path);
// Copies what is drawn so far in the Wii frame buffer rectangle [x0, x1) x [y0, y1) (640x456
// pixels) into a texture, in drawing order (EFB copies). `reuse` is the texture of an
// earlier copy to the same place, or NULL. Fills `coeffs` so that a coordinate (s, t)
// over the copied rectangle samples the texture at u = c0 + c1 t, v = c2 + c3 s (the
// render target is the screen on its side). Returns NULL when nothing was copied (a frame
// the renderer skips, no room in the GPU queue or in memory); `reuse` then stays valid.
void* hostGfxCaptureToTexture(void* reuse, float x0, float y0, float x1, float y1, float* coeffs);
// As hostGfxCaptureToTexture, but when nothing was copied it returns `reuse` (its last image),
// for callers that store the result over their texture.
void* hostGfxCopyToTexture(void* reuse, float x0, float y0, float x1, float y1, float* coeffs);
int hostGfxReadRgb565(unsigned x, unsigned y, unsigned width, unsigned height, unsigned short* pixels);
// Copies the depth buffer as it is now for the next frame's EFB depth reads (the frame's end would
// copy it after MR::clearZBuffer has reset it). Only while reads keep coming.
void hostGfxKeepDepthForReads(void);
// Stall watchdog, called by the vblank thread: logs once when no frame was drawn for 5 s.
void hostDebugCheckStall(void);
// Bounded in-memory trace, dumped by the stall watchdog (no SD writes on the hot path).
void hostDebugTrace(const char* event, unsigned long a, unsigned long b, unsigned long c);
// "profile" option: time the GX emulation stages.
int hostDebugProfile(void);
void hostGfxSetProfile(int on);  // the draw timers, for the logs that report them
// debug.txt "vcache=0": decoded vertices are not kept between frames (A/B checks).
int hostDebugVertexCache(void);
int hostDebugEnvSkip(void);
int hostDebugNative(void);  // native=1: J3D models drawn from the .n3d files (block N)
// debug.txt "lcache=0": display lists are not drawn from the display list cache.
int hostDebugListCache(void);
int hostDebugGpuPath(void);  // gpu=0 (or gpu=f<frame>): draws sent without the GPU path, for A/B checks
unsigned long hostDebugDrawDistance(void);  // drawdist=<units>: 3D draws beyond are dropped (0: off)
int hostDebugAudio(void);                // audio=0: no sound output (on by default)
int hostDebugSky(void);                  // sky=0: sky actors not drawn (drawn by default)
int hostDebugJ3dSkip(void);              // j3dskip=1: J3D model packets not drawn (A/B timing)
int hostDebugGxSkip(void);               // gxskip=1: nothing sent to the GPU, 2: draws not prepared, 3: culled only, 4: + setups (A/B timing)
int hostDebugAir(void);                  // air=0: air actors (atmosphere) are not drawn
unsigned long hostDebugAudioDump(void);  // audiodump=<seconds>: output written to sdmc:/smg/audio.wav
int hostDebugCpuClip(void);  // cpuclip=1: native draws crossing the guard band or the near plane are clipped on the CPU
int hostDebugGpuKick(void);  // gpukick=0: the GPU starts on a frame only at its end (C3D_FrameEnd)
int hostDebugFarClip(void);  // farclip=<m>: actors clip past this many metres (default the Wii's; 96 MB: 300; 0: the Wii distances)
int hostDebugMerge(void);  // merge=0: every draw is its own draw call
// debug.txt "shot=f350": the frame number when that frame is due, else 0.
unsigned long hostDebugShotFrameDue(void);
// Stage and scenario to start after the boot screens ("stage" option); 0 when not set.
int hostDebugStartStage(const char** pName, long* pScenario);
// Non-zero when the galaxy's opening camera is skipped (debug.txt "stage=" without "opening=1").
int hostDebugSkipOpeningCamera(void);
// debug.txt demo= (or a showcase build's SMG_START_DEMO): story demos to play at the debug start
// stage, or NULL.
const char* hostDebugStartDemo(void);

// ##############################################################################################
// # Audio                                                                                      #
// #                                                                                            #
// # The game's mixed sound (rvl/ai.cpp) played through the 3DS DSP (audio.c).                  #
// ##############################################################################################
// Audio output (audio.c): stereo PCM16 at the given rate; 0 when there is no sound.
int hostAudioStart(unsigned long rate);
// Non-zero once the output plays (the game's audio callbacks run).
int hostAudioActive(void);
// Non-zero when the output can take another buffer.
int hostAudioSlotFree(void);
// Copies and plays one buffer of the Wii AI layout (interleaved right, left).
void hostAudioQueue(const short* pSamples, unsigned long frames);
// Stops the output and its ndsp thread (at quit).
void hostAudioShutdown(void);

// ##############################################################################################
// # Movies                                                                                     #
// #                                                                                            #
// # The converted THP movies through the New 3DS hardware video decoder (movie.c), for the     #
// # game's player (THPSimplePlayerWrapper).                                                    #
// ##############################################################################################
// Movies (movie.c): H.264 pictures through the New 3DS video decoder into a texture.
// Non-zero on a New 3DS (the decoder's console).
int hostMovieSupported(void);
// Starts the decoder for pictures of width x height inside paddedWidth x paddedHeight (the
// encoded size, powers of two); 0 when it cannot.
int hostMovieBegin(unsigned width, unsigned height, unsigned paddedWidth, unsigned paddedHeight);
// Decodes the NAL units of one picture (00 00 01 start codes); 1 when a picture was stored.
int hostMovieDecode(const unsigned char* data, unsigned long size);
// The texture of the last stored picture (a hostGfx texture handle), or null.
void* hostMovieTexture(void);
// Stops the decoder and frees its buffers and textures.
void hostMovieEnd(void);

// ##############################################################################################
// # Quitting, and the game world's entry points                                                #
// #                                                                                            #
// # Functions of the game world that the host calls at startup (main.c) and at quit.           #
// ##############################################################################################
// Closes the log and exits the app.
void hostQuit(void) __attribute__((noreturn));

// Implemented by the game world (platform/3ds/rvl): sets up the emulated Wii OS.
void rvlInit(void);
// Reads the data files the Wii keeps inside main.dol from sdmc:/smg/port/ (platform/3ds/rvl/port_data.cpp).
void rvlLoadPortData(void);
// Called by hostQuit so registered Wii shutdown callbacks run before app teardown.
void rvlShutdown(void);

// ##############################################################################################
// # Memory                                                                                     #
// #                                                                                            #
// # The application's heaps (system.c). The linear heap is memory the GPU and the DSP can      #
// # read: textures, vertex buffers, audio output, the movie decoder.                           #
// ##############################################################################################
// Memory. hostGetFreeMemory returns the bytes still available to the application.
unsigned long hostGetFreeMemory(void);
// Deepest use of the main (game) thread's stack so far, in bytes.
unsigned long hostMainStackUsed(void);
// True in the 96 MB of an Old 3DS (the New models have 124 MB): smaller arenas, file cache and textures.
int hostLowMemory(void);
void* hostAllocAligned(unsigned long size, unsigned long alignment);
// True if [address, address + size) is memory the game may read (program or heap).
int hostIsValidRange(unsigned long address, unsigned long size);
// Bounds of the heap the game memory lives in.
void hostGetHeapRange(unsigned long* pStart, unsigned long* pEnd);
// Memory the GPU can read (never freed by its users here).
void* hostLinearAlloc(unsigned long size);

// ##############################################################################################
// # Clock, Mii data                                                                            #
// #                                                                                            #
// # Time in the Wii's terms (system.c), and the console's own Mii database and Mii parts for   #
// # the game's Mii faces (rvl/rfl.cpp).                                                        #
// ##############################################################################################
// Wall clock in milliseconds since 2000-01-01 (the Wii's epoch), and its conversion to a date.
long long hostGetMillisecondsSince2000(void);
void hostGetCalendarTime(long long msecSince2000, int* pYear, int* pMonth, int* pDay, int* pHour, int* pMinute,
                         int* pSecond, int* pWeekDay, int* pYearDay);
// Reads Mii Maker's database (CFL_DB.dat in the shared extdata 0xF000000B) into pData, up to
// capacity bytes. Returns non-zero when it was read; *pSize takes the bytes read.
int hostMiiDatabase(void* pData, unsigned long capacity, unsigned long* pSize);
// Returns the system Mii component archive (CFL_Res.dat), kept alive for the process.
int hostCflResource(const void** data, unsigned long* size);

// ##############################################################################################
// # Threads                                                                                    #
// #                                                                                            #
// # Host threads and semaphores (thread.c).                                                    #
// ##############################################################################################
// Threads. Game threads all run on the same core and priority; the game-side scheduler
// guarantees that only one of them runs at a time, like on the single-core Wii.
typedef void (*HostThreadEntry)(void* arg);
int hostThreadCreate(HostThreadEntry entry, void* arg, unsigned long stackSize);
void hostThreadExit(void) __attribute__((noreturn));
// A thread on the second core, which must never touch game state; returns 0 if the system refuses.
int hostCore1ThreadCreate(HostThreadEntry entry, void* arg, unsigned long stackSize);
void* hostSemaphoreCreate(void);
void hostSemaphoreWait(void* sem);
void hostSemaphoreSignal(void* sem);

// ##############################################################################################
// # Interrupts                                                                                 #
// #                                                                                            #
// # The Wii's hardware interrupts as bits (thread.c): raised by host threads and callbacks,    #
// # handled by the game world where it enables interrupts (rvl/os_thread.cpp).                 #
// ##############################################################################################
// Interrupt sources, delivered to the game as a bitmask of pending interrupts.
#define HOST_IRQ_TIMER 1u
#define HOST_IRQ_VBLANK 2u
#define HOST_IRQ_IO 4u
#define HOST_IRQ_AUDIO 8u  // an audio output slot is free (audio.c)
#define HOST_IRQ_DSP 16u   // the second core finished mixing a subframe (rvl/dsp.cpp)

void hostInterruptInit(void);
// Stops the timer and vblank threads (at quit: their stacks live on the heap exit frees).
void hostInterruptShutdown(void);
void hostInterruptRaise(unsigned bits);
unsigned hostInterruptTakePending(void);
int hostInterruptPending(void);
// Blocks until at least one interrupt is pending (used when no game thread can run).
void hostInterruptWait(void);
// Raises HOST_IRQ_TIMER once the system tick reaches the deadline (0 disables the timer).
void hostInterruptSetTimer(unsigned long long deadlineTick);
// Number of top-screen vertical blanks so far.
unsigned long hostVBlankCount(void);
// System tick of the last top-screen vertical blank.
unsigned long long hostVBlankTick(void);

// ##############################################################################################
// # System language                                                                            #
// #                                                                                            #
// # The console's language, for the game's language choice.                                    #
// ##############################################################################################
// The 3DS system language, as one of these values (other languages are returned as-is).
#define HOST_LANGUAGE_JAPANESE 0
#define HOST_LANGUAGE_ENGLISH 1
#define HOST_LANGUAGE_FRENCH 2
#define HOST_LANGUAGE_GERMAN 3
#define HOST_LANGUAGE_ITALIAN 4
#define HOST_LANGUAGE_SPANISH 5
int hostGetSystemLanguage(void);

// ##############################################################################################
// # Game files (the emulated disc)                                                             #
// #                                                                                            #
// # The converted game files under sdmc:/smg/files and the file cache pack (fs.c), read by the #
// # Wii disc functions of rvl/dvd.cpp.                                                         #
// ##############################################################################################
// Files of the emulated Wii disc. hostFilesRoot is the folder that holds the disc's files.
const char* hostFilesRoot(void);
void* hostDirOpen(const char* path);
// Returns 0 when the directory has no more entries.
int hostDirNext(void* dir, char* pName, unsigned long nameSize, int* pIsDir);
void hostDirClose(void* dir);
void* hostFileOpen(const char* path);
long hostFileSize(void* file);
long hostFileRead(void* file, void* pDst, unsigned long size, unsigned long offset);
// Game file opens and reads so far: counts, time in system ticks, bytes read.
void hostFileStats(unsigned long* pOpens, unsigned long long* pOpenTicks, unsigned long* pReads, unsigned long long* pReadTicks,
                   unsigned long long* pReadBytes);
// Logs where the time since start went (file opens and reads, log writes).
void hostLogLoadProfile(void);
void hostFileClose(void* file);

// ##############################################################################################
// # Plain files on the SD card                                                                 #
// #                                                                                            #
// # Files by full path (fs.c): the emulated Wii NAND with the saves (rvl/nand.cpp), the port's #
// # data files, and debug captures.                                                            #
// ##############################################################################################
// Plain files on the SD card, by full path (used for the emulated Wii NAND).
// hostPathType: 0 missing, 1 file, 2 directory. Access: 1 read, 2 write, 3 read/write.
int hostPathType(const char* path);
void* hostFileOpenMode(const char* path, int access);
long hostFileReadNext(void* file, void* pDst, unsigned long size);
long hostFileWriteNext(void* file, const void* pSrc, unsigned long size);
// Size and close of a file from hostFileOpenMode (not hostFileSize/hostFileClose, which take disc files).
long hostFileSizeMode(void* file);
void hostFileCloseMode(void* file);
// These create missing parent directories; they return 0 on success.
int hostFileCreate(const char* path);
int hostDirCreate(const char* path);
int hostFileDelete(const char* path);
int hostFileRename(const char* from, const char* to);

// Sequential binary output for frame captures (fs.c).
void* hostCaptureOpen(const char* path);
void hostCaptureWrite(void* file, const void* pData, unsigned long size);
void hostCaptureClose(void* file);

// ##############################################################################################
// # Rendering: vertices and pixel state                                                        #
// #                                                                                            #
// # The renderer's drawing interface (gfx.c). The values are the 3DS GPU's own, already        #
// # translated from the Wii's by the game world (gx/, GXToCitro3D.hpp): the host side only     #
// # hands them to citro3d.                                                                     #
// ##############################################################################################
// Rendering on the top screen (gfx.c). Positions are in clip space; colour is RGBA. Texture
// unit n samples with (sn, tn).
typedef struct {
    float x, y, z, w;
    unsigned char rgba[4];
    float s0, t0, s1, t1, s2, t2;
} HostGfxVertex;

// One texture combiner stage. Sources, operands, functions and scales use the 3DS GPU's
// values (libctru GPU_TEVSRC, GPU_TEVOP_*, GPU_COMBINEFUNC, GPU_TEVSCALE).
#define HOST_GFX_STAGES 6
typedef struct {
    unsigned char colorSrc[3], colorOp[3], colorFunc, colorScale;
    unsigned char alphaSrc[3], alphaOp[3], alphaFunc, alphaScale;
    unsigned long constant;  // 0xAABBGGRR
} HostGfxStage;

// Pixel state of a draw, also in the 3DS GPU's values (GPU_TESTFUNC, GPU_BLENDFACTOR,
// GPU_BLENDEQUATION, GPU_WRITEMASK, GPU_CULLMODE).
typedef struct {
    void* textures[3];  // from hostGfxTextureCreate, or NULL
    unsigned char stageCount;
    unsigned char blendEquation, blendSrc, blendDst;
    unsigned char alphaTestFunc, alphaRef;
    unsigned char depthTest, depthFunc;
    unsigned char writeMask;
    unsigned char cull;
    HostGfxStage stages[HOST_GFX_STAGES];
    // Visible rectangle in Wii frame buffer pixels (640x456): x0, y0 inclusive, x1, y1 exclusive.
    // Appended so that captures keep the existing state layout above.
    unsigned short scissor[4];
    unsigned char dstAlphaEnable, dstAlpha;
    // Raw Wii BP fog registers (0xEE, 0xEF, 0xF0, 0xF1, 0xF2).
    unsigned int fogParamA, fogParamB, fogParamShift, fogParamC, fogColor;
    // Citro3D colour logic operation (GPU_LOGICOP); appended to preserve the capture layout.
    unsigned char logicOpEnable, logicOp;
    // Stencil test (GPU_TESTFUNC, GPU_STENCILOP). With stencilEnable 0 the draw takes the ambient
    // stencil (hostGfxSetStencilAmbient), off unless a caller set one.
    unsigned char stencilEnable, stencilFunc, stencilRef, stencilMask, stencilWriteMask;
    unsigned char stencilFail, stencilZFail, stencilZPass;
    // Combiner buffer updates (C3D_TexEnvBufUpdate masks: bit n, the buffer takes stage n's
    // result, n 0-3) of the colour and of the alpha. With 0 the buffer keeps its initial
    // colour, 0.5, which material combiners read as a constant (mat.py half).
    unsigned char bufferUpdateRgb, bufferUpdateAlpha;
} HostGfxState;

// The stencil of draws that set none: the Wii's "destination alpha 0" windows, where every draw
// resets the alpha the shadow volumes count in, become draws that reset the stencil (zpass ZERO).
// enable 0 turns it off. Taken by the following draws; reset at each frame start.
void hostGfxSetStencilAmbient(int enable, int func, int ref, int mask, int writeMask, int fail, int zfail, int zpass);

// ##############################################################################################
// # Textures                                                                                   #
// #                                                                                            #
// # Textures in linear memory, made from decoded RGBA8 pixels or from texels the converter     #
// # already wrote in the GPU's layout. A texture can be freed only after the GPU has finished  #
// # every frame that reads it: hostGfxTextureDestroy waits for that, the frame ids tell when.  #
// ##############################################################################################
// Creates a texture from RGBA8 pixels (0xRRGGBBAA, row by row) of width x height, placed in
// a texture of potWidth x potHeight (powers of two). Wrap: 0 clamp, 1 repeat, 2 mirror. Filters:
// 1 linear, 0 nearest, for magnification and minification (the base level of GX's min filter).
void* hostGfxTextureCreate(const unsigned long* pPixels, unsigned width, unsigned height, unsigned potWidth, unsigned potHeight,
                           int wrapS, int wrapT, int linear, int minLinear);
void hostGfxTextureDestroy(void* texture);
// Writes a texture as it is in memory (16-byte header "TTEX", format, width, height, then the
// GPU's tiled data) to the SD card. For one-off checks of what the GPU drew into a capture.
void hostGfxSaveTexture(void* texture, const char* path);
// Frees a texture at once. Only for textures no draw of the current frame uses: the GPU has
// finished the earlier frames when a frame starts.
void hostGfxTextureDestroyNow(void* texture);
// Frame ids for texture lifetimes: the frame being recorded now, and the last frame the GPU has
// finished. A texture last used in a frame up to the finished one can be destroyed at once.
unsigned long hostGfxFrameId(void);
unsigned long hostGfxCompletedFrameId(void);
// Free linear memory (textures, vertex and command buffers), in bytes.
unsigned long hostGfxLinearFree(void);
// A texture from texels already in the GPU's layout (the converter's texreduce.py): power-of-two
// size, nativeFormat 0 ETC1, 1 ETC1A4, 2 LA4.
void* hostGfxTextureCreateNative(const void* pData, int nativeFormat, unsigned width, unsigned height, int wrapS, int wrapT, int linear,
                                 int minLinear);

// ##############################################################################################
// # Drawing                                                                                    #
// #                                                                                            #
// # Draws of vertices already in clip space (made on the CPU: 2D layouts, effects, particles), #
// # and the Wii frame buffer reads (EFB peeks).                                                #
// ##############################################################################################
void hostGfxInit(void);
// Draws a triangle list.
#define HOST_GFX_TRIANGLES 0
#define HOST_GFX_STRIP 1
#define HOST_GFX_FAN 2
// Draws clip-space vertices; with pIndices, a triangle list of indexCount indices into them,
// else the first indexCount vertices as primitive. Positions are the Wii's clip space (x right and
// y up on the 640x456 screen, visible z in [-w, 0]); the turn to the 3DS target is made here.
void hostGfxDraw(const HostGfxVertex* pVertices, unsigned long count, const unsigned short* pIndices, unsigned long indexCount,
                 int primitive, const HostGfxState* pState);
// The vertices of the last hostGfxDraw again, with other indices or material: no copy of them.
// Zero when that draw left none to use (clipped, none left this frame): call hostGfxDraw then.
int hostGfxDrawAgain(const unsigned short* pIndices, unsigned long indexCount, int primitive, const HostGfxState* pState);
// Reads one emulated Wii framebuffer pixel. Returns nonzero when the coordinate is valid.
int hostGfxPeekEfb(unsigned short x, unsigned short y, int depth, unsigned long* pValue);

// ##############################################################################################
// # GPU-transformed models                                                                     #
// #                                                                                            #
// # The native models (gx/native.cpp): their vertices stay in model space in the vertex arena, #
// # and the vertex shader (shaders/gx.v.pica) transforms, lights and texture-maps them with    #
// # the matrices and lights of HostGfxTransform, as the Wii's XF unit does.                    #
// ##############################################################################################
// Draws whose vertices the GPU transforms, lights and texture-maps (shaders/gx.v.pica): the
// vertex carries model-space data, the transform the GX settings as shader uniforms.
typedef struct {
    float x, y, z;         // model-space position
    float nx, ny, nz;      // view-space normal (normalized)
    unsigned char rgba[4]; // vertex colour of the lighting channel
    unsigned char matrix;  // position matrix row (0..29)
    unsigned char texRow[3];  // with vtxmtx: the XF row of each unit's own texture matrix
    float tex[3][3];       // input of each unit's texture coordinate generation (w is 1)
} HostGfxModelVertex;

typedef struct {
    float clip[4][4];
    float palette[30][4];
    float texmtx[9][4];
    float texpost[9][4];
    float texfinal[6][4];
    float material[4], ambient[4], maskmat[4], maskamb[4], masklit[4];
    float srtg[4];         // 1 where the unit's coordinates are the lit colour's red and green (GX_TG_SRTG)
    float lights[30][4];
    unsigned char lit, lightCount;
    unsigned char normalize[3], divide[3];
    unsigned char nrmpalette;  // normals are in model space, transformed with the position matrix
    unsigned char nrmcofactor; // with nrmpalette: transform normals by the position matrix's inverse transpose
    unsigned char vtxmtx[3];   // the unit's matrix is each vertex's palette rows texRow (texmtx: see draw.cpp)
    unsigned char paletteRows; // palette rows the vertices use (0: all 30); the others are not sent
} HostGfxTransform;

// Model vertices live in the vertex arena (linear memory the GPU reads directly). Persistent
// allocations stay until the arena is emptied, which bumps hostGfxArenaEpoch (at a frame start,
// after an allocation failed for lack of room); the others last for the current frame. Returns
// the first vertex's arena index and sets *ppOut, or HOST_GFX_NO_ARENA (no room, or the frame
// is dropped). The caller fills the vertices before drawing them in this frame.
#define HOST_GFX_NO_ARENA 0xFFFFFFFFul
unsigned long hostGfxArenaAlloc(unsigned long count, int persistent, HostGfxModelVertex** ppOut);
unsigned long hostGfxArenaEpoch(void);
// Empties the persistent arena at the next drawn frame's start (a scene ends: its models go).
void hostGfxArenaReset(void);
// The vertex at an arena index returned by hostGfxArenaAlloc (valid while the allocation is).
HostGfxModelVertex* hostGfxArenaVertex(unsigned long index);

// ##############################################################################################
// # Clipping before the GPU                                                                    #
// #                                                                                            #
// # The PICA clips only against the near and far planes (clip.c).                              #
// ##############################################################################################
// Clipping before the GPU (clip.c): the PICA clips only against near and far, and a triangle
// crossing the camera plane far off to the side stalls it. Polygons are clipped against the near
// plane and a lateral guard band of HOST_CLIP_GUARD screens.
#define HOST_CLIP_GUARD 8.0f
#define HOST_CLIP_PLANES 5
// Room a clipped triangle needs: 3 vertices plus one per plane.
#define HOST_CLIP_MAX_VERTICES (3 + HOST_CLIP_PLANES)
// Bit n set when clip-space position c (x, y, z, w) is outside plane n; 0 when inside them all.
// HOST_CLIP_INVALID (every plane bit too) when a component is NaN or infinite: a triangle with
// such a vertex must be dropped, not clipped.
#define HOST_CLIP_INVALID_BIT 0x80000000u
#define HOST_CLIP_INVALID (HOST_CLIP_INVALID_BIT | ((1u << HOST_CLIP_PLANES) - 1))
// Tests take HOST_CLIP_INVALID_BIT: HOST_CLIP_INVALID also holds the plane bits, which any
// vertex outside a plane has (testing it dropped every triangle reaching outside the guard band
// or behind the near plane instead of clipping it, until CIA 0.12.9).
unsigned hostClipOutcode(const float* c);
// Writes into out the vertex at parameter t (0..1) from a to b, clip position included.
typedef void (*HostClipLerp)(void* out, const void* a, const void* b, float t);
// Clips the convex polygon of count vertices in pPolygon, each size bytes and starting with its
// clip-space x, y, z, w floats; both buffers hold HOST_CLIP_MAX_VERTICES vertices. Returns the
// vertex count of the clipped polygon, left in pPolygon (0: nothing remains).
int hostClipPolygon(unsigned char* pPolygon, int count, unsigned size, HostClipLerp lerp, unsigned char* pScratch);

// ##############################################################################################
// # Drawing models                                                                             #
// #                                                                                            #
// # Draws of arena vertices with their transform; the indices of persistent models stay in     #
// # their own buffer like their vertices.                                                      #
// ##############################################################################################
// Draws count vertices of an arena allocation starting at first: first + pIndices[i], or the
// count vertices in order without indices. Draws passing the same transformVersion must pass
// the same transform (it is then not compared again).
void hostGfxDrawArena(unsigned long first, const unsigned short* pIndices, unsigned long count, int primitive, const HostGfxState* pState,
                      const HostGfxTransform* pTransform, unsigned long transformVersion);

// Persistent triangle list indices (emptied with the persistent vertices), relative to the
// arena window (65536 vertices) their vertices are in; NULL when there is no room.
unsigned short* hostGfxIndexAlloc(unsigned long count);
// Triangle indices for vertices in the current frame's transient arena window.
unsigned short* hostGfxIndexAllocTransient(unsigned long count);
// Draws a triangle list of persistent indices whose vertices are in the window of arena index first.
void hostGfxDrawIndexed(unsigned long first, const unsigned short* pIndices, unsigned long count, const HostGfxState* pState,
                        const HostGfxTransform* pTransform, unsigned long transformVersion);
// Native packets use a separate per-frame budget so a pathological model cannot fill the
// Citro3D command buffer before the frame is submitted.
int hostGfxDrawNativeIndexed(unsigned long first, const unsigned short* pIndices, unsigned long count, const HostGfxState* pState,
                             const HostGfxTransform* pTransform, unsigned long transformVersion);
// ##############################################################################################
// # Frames and pacing                                                                          #
// #                                                                                            #
// # Frame start and end, and the pacing that decides which frames are drawn when the game runs #
// # behind the clock (gfx.c, DETAILS.md section 7).                                            #
// ##############################################################################################
// Ends the frame; the next one starts cleared to clearColor (0xRRGGBBAA).
void hostGfxClearFrame(unsigned long clearColor);
void hostGfxEndFrame(unsigned long clearColor);
// Begins the frame now and returns non-zero when it is dropped (begun in the vertical blank the
// last frame ended in, as the game catches up after a slow one): its draws would be thrown away.
int hostGfxFrameDropped(void);
// Implements the Wii VI black-screen control over the Citro3D render target.
void hostGfxSetBlack(int black);
// Time the CPU waited for the GPU at frame starts since the last call (ticks): a GPU-bound frame waits here.
unsigned long long hostGfxTakeGpuWait(void);
// Average and longest GPU time (ms) of the frames finished since the last call.
void hostGfxTakeGpuTime(float* pAverage, float* pMax);
// Time between frame ends (ms), averaged over the last frames (about 8); load stalls count 100 ms at most.
float hostGfxFrameMs(void);
// Whether the next frame will be drawn, decided when the current one ends (hostGfxEndFrame): the
// game skips its view calculation for a frame that will not be drawn.
int hostGfxNextFrameDrawn(void);
// How much of the Wii's detail the port keeps while the game runs behind the clock, from
// HOST_PERF_DETAIL_MIN to 1 (actor clipping distances, particle emission, early LOD).
float hostPerfDetail(void);
// Logs the loops and frames drawn since the last call, the late loops and the detail level.
void hostGfxLogPacing(void);
int hostGfxFallingBehind(void);  // the game is drawing one loop in three: draws worth less go first
// Native draw submissions since the last call: calls, ticks setting up state and uniforms, ticks
// in C3D_DrawElements, uniform vectors sent and texture binds.
void hostGfxTakeSubmitStats(unsigned long* pCalls, unsigned long long* pStateTicks, unsigned long long* pDrawTicks,
                            unsigned long* pVectors, unsigned long* pBinds);

// ##############################################################################################
// # Ticks                                                                                      #
// #                                                                                            #
// # The system tick, for timing.                                                               #
// ##############################################################################################
// Free-running tick counter (268 MHz on 3DS) and its frequency.
unsigned long long hostGetTick(void);
unsigned long long hostGetTickFrequency(void);

#ifdef __cplusplus
}
#endif