// Core Wii OS functions: memory arenas, logging, time, interrupts and cache control.
// Threads, mutexes and message queues live in os_thread.cpp.

#include "host.h"
#include "rvl_internal.h"
#include <revolution.h>
#include <revolution/os/OSBootInfo.h>

// Sizes of the two Wii memory arenas. MEM1 holds the root heap; the first 14 MB of MEM2
// stand in for the audio RAM and the rest becomes the "GDDR3" heap.
// On the Wii the game's code and data take the first 6.7 MB of MEM1 (its arena is about
// 17 MB); here they live elsewhere, so MEM1 is 20 MB and the rest goes to MEM2.
static const u32 kMem1ArenaSize = 20 * 1024 * 1024;
// With the 96 MB of an Old 3DS, the size of the Wii's arena: the game's scene heaps are made for it.
static const u32 kMem1ArenaLowSize = 17 * 1024 * 1024;
static const u32 kMem2ArenaMinSize = 0xE00000 + 24 * 1024 * 1024;
// The Wii's MEM2 arena for this game (0x90000000-0x933E0000), plus headroom: the port's
// stationed data fill the GDDR3 heaps more than the Wii's apparently do (with the Wii's size,
// the scene heap left for a stage was 1.8 MB, too small for the bloom textures of
// EggStarGalaxy), so MEM2 grows by up to 5 MB when the 3DS has the memory.
static const u32 kMem2ArenaWiiSize = 0x33E0000;
static const u32 kMem2ArenaMaxSize = kMem2ArenaWiiSize + 5 * 1024 * 1024;

// Memory the host keeps for itself from the application heap. Textures and GPU buffers come
// from the separate linear heap, so this only covers small allocations (about 1.7 MB are in
// use with the title scene loaded). With 4 MB, MEM2 ended up 320 KB smaller than the Wii's,
// and stages ran out of scene heap.
static const u32 kHostReserve = 3 * 1024 * 1024;
// About 1.5 MB stayed in use through the stages of the console logs.
static const u32 kHostLowReserve = 2560 * 1024;

// Emulated Wii low memory (0x80000000-0x80003FFF). Only the boot info is filled in.
u8 rvlLowMemory[0x4000] ATTRIBUTE_ALIGN(32);

static void initLowMemory() {
    OSBootInfo* info = reinterpret_cast< OSBootInfo* >(rvlLowMemory);
    memcpy(info->DVDDiskID.gameName, "RMGE", 4);
    memcpy(info->DVDDiskID.company, "01", 2);
    info->memorySize = 0x01800000;
}

static void* sArenaLo;
static void* sArenaHi;
static void* sMem2ArenaLo;
static void* sMem2ArenaHi;

static void initArenas() {
    if (sArenaLo != nullptr) {
        return;
    }

    initLowMemory();

    u32 freeBytes = hostGetFreeMemory();
    const u32 mem1Size = hostLowMemory() ? kMem1ArenaLowSize : kMem1ArenaSize;
    const u32 reserve = hostLowMemory() ? kHostLowReserve : kHostReserve;
    u32 mem2Size = kMem2ArenaMaxSize;
    if (freeBytes < mem1Size + mem2Size + reserve) {
        mem2Size = freeBytes > mem1Size + reserve ? freeBytes - mem1Size - reserve : 0;
        mem2Size &= ~0xFFFFu;
    }

    hostLog("Memory: %lu KB free, MEM1 %lu KB, MEM2 %lu KB\n", freeBytes / 1024, mem1Size / 1024, mem2Size / 1024);
    if (mem2Size < kMem2ArenaMinSize) {
        hostFatal("Not enough memory for the game (%lu KB free).", freeBytes / 1024);
    }

    u8* mem1 = static_cast< u8* >(hostAllocAligned(mem1Size, 32));
    u8* mem2 = static_cast< u8* >(hostAllocAligned(mem2Size, 32));
    if (mem1 == nullptr || mem2 == nullptr) {
        hostFatal("Could not allocate the game arenas.");
    }

    sArenaLo = mem1;
    sArenaHi = mem1 + mem1Size;
    sMem2ArenaLo = mem2;
    sMem2ArenaHi = mem2 + mem2Size;
}

extern "C" {

void* OSGetArenaLo(void) {
    initArenas();
    return sArenaLo;
}

void* OSGetArenaHi(void) {
    initArenas();
    return sArenaHi;
}

void OSSetArenaLo(void* pAddr) {
    sArenaLo = pAddr;
}

void OSSetArenaHi(void* pAddr) {
    sArenaHi = pAddr;
}

void* OSGetMEM2ArenaLo(void) {
    initArenas();
    return sMem2ArenaLo;
}

void* OSGetMEM2ArenaHi(void) {
    initArenas();
    return sMem2ArenaHi;
}

void OSSetMEM2ArenaLo(void* pAddr) {
    sMem2ArenaLo = pAddr;
}

void OSSetMEM2ArenaHi(void* pAddr) {
    sMem2ArenaHi = pAddr;
}

// The game only uses OSInitAlloc to give the OS heap a range; the SDK heap itself is unused.
void* OSInitAlloc(void* pArenaStart, void* pArenaEnd, int maxHeaps) {
    return pArenaStart;
}

void OSReport(const char* pFormat, ...) {
    va_list args;
    va_start(args, pFormat);
    hostVLog(pFormat, args);
    va_end(args);
}

void OSVReport(const char* pFormat, va_list args) {
    hostVLog(pFormat, args);
}

void OSPanic(const char* pFile, int line, const char* pFormat, ...) {
    va_list args;
    va_start(args, pFormat);
    hostLog("OSPanic at %s:%d\n", pFile, line);
    hostVLog(pFormat, args);
    va_end(args);
    hostFatal("The game stopped (OSPanic).");
}

void OSRegisterVersion(const char* pVersion) {
}

// Time is reported in Wii timer ticks (OS_TIMER_CLOCK) so that game timing stays unchanged.
// Like on the Wii, OSGetTime counts from 2000-01-01: the wall clock is read once at startup
// and the high-resolution system tick provides the elapsed time from then on.
static bool sTimeInitialized;
static OSTime sBaseTime;
static u64 sBaseTick;

static void initTime() {
    if (!sTimeInitialized) {
        sTimeInitialized = true;
        sBaseTime = static_cast< OSTime >(hostGetMillisecondsSince2000()) * (OS_TIMER_CLOCK / 1000);
        sBaseTick = hostGetTick();
    }
}

OSTime OSGetTime(void) {
    initTime();
    u64 tick = hostGetTick() - sBaseTick;
    u64 freq = hostGetTickFrequency();
    return sBaseTime + static_cast< OSTime >((tick / freq) * OS_TIMER_CLOCK + (tick % freq) * OS_TIMER_CLOCK / freq);
}

OSTick OSGetTick(void) {
    return static_cast< OSTick >(OSGetTime());
}

OSTime __OSGetSystemTime(void) {
    return OSGetTime();
}

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* pTime) {
    s64 msec = ticks / (OS_TIMER_CLOCK / 1000);
    hostGetCalendarTime(msec, &pTime->year, &pTime->mon, &pTime->mday, &pTime->hour, &pTime->min, &pTime->sec, &pTime->wday, &pTime->yday);
    pTime->msec = static_cast< int >(msec % 1000);
    pTime->usec = 0;
}

// Only one game thread runs at a time (see os_thread.cpp), so disabling interrupts only
// has to postpone interrupt handlers: they are delivered once interrupts are enabled again.
static BOOL sInterruptsEnabled = TRUE;

BOOL OSDisableInterrupts(void) {
    BOOL prev = sInterruptsEnabled;
    sInterruptsEnabled = FALSE;
    return prev;
}

BOOL OSEnableInterrupts(void) {
    BOOL prev = sInterruptsEnabled;
    sInterruptsEnabled = TRUE;
    if (!prev) {
        rvlInterruptsEnabled();
    }
    return prev;
}

// For loops that wait for an interrupt handler to change a variable (the Wii delivers
// interrupts at any instruction; here only at the points above): delivers pending
// interrupts if interrupts are enabled.
extern "C" void rvlPollInterrupts(void) {
    if (sInterruptsEnabled) {
        rvlInterruptsEnabled();
    }
}

BOOL rvlInterruptLevel() {
    return sInterruptsEnabled;
}

void rvlSetInterruptLevel(BOOL level) {
    sInterruptsEnabled = level;
}

BOOL OSRestoreInterrupts(BOOL level) {
    BOOL prev = sInterruptsEnabled;
    sInterruptsEnabled = level;
    if (level && !prev) {
        rvlInterruptsEnabled();
    }
    return prev;
}

// The CPU caches are coherent from the game's point of view; the GPU layer flushes what it needs
// (the native renderer uploads its own vertices: nothing the game writes is read by the GPU).
void DCFlushRange(void* pAddr, u32 size) {
}

void DCStoreRange(void* pAddr, u32 size) {
}

void DCStoreRangeNoSync(void* pAddr, u32 size) {
}

void DCInvalidateRange(void* pAddr, u32 size) {
}

void DCZeroRange(void* pAddr, u32 size) {
    memset(pAddr, 0, size);
}

void LCEnable(void) {
}

void LCDisable(void) {
}

u32 PPCMfmsr(void) {
    return 0;
}

void PPCMtmsr(u32 value) {
}

void PPCSync(void) {
}

void OSProtectRange(u32 channel, void* pAddr, u32 size, u32 control) {
}

BOOL OSGetResetButtonState(void) {
    return 0;
}

u32 OSGetStackPointer(void) {
    return reinterpret_cast< u32 >(__builtin_frame_address(0));
}

void OSFillFPUContext(OSContext* pContext) {
}

}  // extern "C"

u64 rvlOSTimeToHostTick(OSTime time) {
    initTime();
    if (time <= sBaseTime) {
        return sBaseTick;
    }

    u64 delta = static_cast< u64 >(time - sBaseTime);
    u64 freq = hostGetTickFrequency();
    return sBaseTick + (delta / OS_TIMER_CLOCK) * freq + (delta % OS_TIMER_CLOCK) * freq / OS_TIMER_CLOCK;
}
