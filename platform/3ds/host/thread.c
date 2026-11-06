// Host threads, semaphores and the interrupt sources that drive the emulated Wii OS.
//
// Game threads run on real 3DS threads, but the scheduler in rvl/os_thread.cpp only lets
// one of them run at a time. The threads here never touch game state: they only raise
// interrupt bits that the game-side dispatcher handles.

#include "host.h"

#include <3ds.h>
#include <stdlib.h>

// All game threads share one priority and core; the scheduler decides who runs.
#define GAME_THREAD_PRIORITY 0x30
// Interrupt sources must be able to run while a game thread is busy.
#define IRQ_THREAD_PRIORITY 0x2F

// ##############################################################################################
// # Game threads                                                                               #
// #                                                                                            #
// # Every thread of the game (the Wii OS threads of rvl/os_thread.cpp) is a real 3DS thread on #
// # the first core, all at the same priority: the scheduler in os_thread.cpp lets only one of  #
// # them run at a time, as on the Wii's single CPU. The trampoline frees the start record and  #
// # ends the thread when its entry function returns.                                           #
// ##############################################################################################
typedef struct {
    HostThreadEntry entry;
    void* arg;
} ThreadStart;

static void threadTrampoline(void* param) {
    ThreadStart start = *(ThreadStart*)param;
    free(param);
    start.entry(start.arg);
    threadExit(0);
}

int hostThreadCreate(HostThreadEntry entry, void* arg, unsigned long stackSize) {
    ThreadStart* start = malloc(sizeof(ThreadStart));
    start->entry = entry;
    start->arg = arg;

    // libctru needs a reasonable minimum: Wii threads sometimes ask for tiny stacks.
    if (stackSize < 0x10000) {
        stackSize = 0x10000;
    }

    Thread thread = threadCreate(threadTrampoline, start, stackSize, GAME_THREAD_PRIORITY, 0, true);
    if (thread == NULL) {
        free(start);
        return 0;
    }

    return 1;
}

// ##############################################################################################
// # A thread on the second core                                                                #
// #                                                                                            #
// # Used only with the 96 MB of an Old 3DS, where rvl/dsp.cpp mixes the audio on the second    #
// # core (the port now refuses an Old 3DS at startup, main.c). The system shares that core     #
// # with the application for a limited part of its time: the limit is asked for first, and the #
// # thread is refused without it. Such a thread must never touch game state.                   #
// ##############################################################################################
int hostCore1ThreadCreate(HostThreadEntry entry, void* arg, unsigned long stackSize) {
    ThreadStart* start = malloc(sizeof(ThreadStart));
    start->entry = entry;
    start->arg = arg;
    // The application's share of the core the system also runs on. The rsf's MaxCpu 0x9E caps it at
    // 0x1E = 30%: asking for more is refused (0xD8E05BF4, CIA 0.16.7).
    Result limit = APT_SetAppCpuTimeLimit(30);
    if (R_FAILED(limit)) {
        hostLog("second core: time limit refused (0x%08lX)\n", (unsigned long)limit);
        free(start);
        return 0;
    }
    u32 granted = 0;
    APT_GetAppCpuTimeLimit(&granted);
    hostLog("second core: time limit %lu%%\n", (unsigned long)granted);
    Thread thread = threadCreate(threadTrampoline, start, stackSize, IRQ_THREAD_PRIORITY, 1, true);
    if (thread == NULL) {
        hostLog("second core: thread refused\n");
        free(start);
        return 0;
    }
    return 1;
}

void hostThreadExit(void) {
    threadExit(0);
}

// ##############################################################################################
// # Semaphores                                                                                 #
// #                                                                                            #
// # Counting semaphores for the host threads (libctru's LightSemaphore): every signal is       #
// # counted, none is lost when several arrive before the waiting thread wakes.                 #
// ##############################################################################################
void* hostSemaphoreCreate(void) {
    LightSemaphore* sem = malloc(sizeof(LightSemaphore));
    LightSemaphore_Init(sem, 0, 0x7FFF);
    return sem;
}

void hostSemaphoreWait(void* sem) {
    LightSemaphore_Acquire((LightSemaphore*)sem, 1);
}

void hostSemaphoreSignal(void* sem) {
    LightSemaphore_Release((LightSemaphore*)sem, 1);
}

// ##############################################################################################
// # Interrupts                                                                                 #
// #                                                                                            #
// # The Wii's hardware interrupts (timer, retrace, disc, audio, DSP: HOST_IRQ_* in host.h)     #
// # become bits in sPendingIrqs. Host threads and callbacks raise them at any time; the game   #
// # side takes them all at once (hostInterruptTakePending) and runs their handlers only where  #
// # the game has interrupts enabled (os_thread.cpp, dispatchInterrupts). When every game       #
// # thread waits, hostInterruptWait sleeps until a bit is raised.                              #
// ##############################################################################################
static volatile u32 sPendingIrqs;
static LightEvent sIrqEvent;
static LightEvent sDeadlineChanged;
static volatile u64 sTimerDeadline;
static volatile bool sIrqThreadsRunning;
static Thread sTimerThread, sVBlankThread;

void hostInterruptRaise(unsigned bits) {
    __atomic_fetch_or(&sPendingIrqs, bits, __ATOMIC_SEQ_CST);
    LightEvent_Signal(&sIrqEvent);
}

unsigned hostInterruptTakePending(void) {
    return __atomic_exchange_n(&sPendingIrqs, 0, __ATOMIC_SEQ_CST);
}

int hostInterruptPending(void) {
    return __atomic_load_n(&sPendingIrqs, __ATOMIC_RELAXED) != 0;
}

static volatile unsigned long sVBlankCount;
static volatile unsigned long long sVBlankTick;  // system tick of the last vertical blank

void hostInterruptWait(void) {
    // Debug fast-forward: when every game thread is waiting, the next retrace comes at once
    // instead of at the next screen refresh, so idle time disappears.
    if (!hostInterruptPending() && hostDebugTurbo()) {
        extern volatile unsigned long gHostDebugIdleSkips;
        gHostDebugIdleSkips++;
        sVBlankCount++;
        hostInterruptRaise(HOST_IRQ_VBLANK);
        return;
    }

    while (!hostInterruptPending()) {
        LightEvent_Wait(&sIrqEvent);
        LightEvent_Clear(&sIrqEvent);
    }
}

// ##############################################################################################
// # The timer and retrace threads                                                              #
// #                                                                                            #
// # Two host threads, at a priority above the game's so they run while it is busy. The timer   #
// # thread raises HOST_IRQ_TIMER at the deadline os_thread.cpp sets for the next OS alarm. The #
// # retrace thread waits for each vertical blank of the screens, counts it and raises          #
// # HOST_IRQ_VBLANK (the Wii's VI interrupt, rvl/vi.cpp); it also lets debug.c notice a game   #
// # that has stopped presenting frames.                                                        #
// ##############################################################################################
void hostInterruptSetTimer(unsigned long long deadlineTick) {
    sTimerDeadline = deadlineTick;
    LightEvent_Signal(&sDeadlineChanged);
}

static void timerThread(void* arg) {
    while (sIrqThreadsRunning) {
        u64 deadline = sTimerDeadline;

        if (deadline == 0) {
            LightEvent_Wait(&sDeadlineChanged);
            continue;
        }

        u64 now = svcGetSystemTick();
        if (now >= deadline) {
            sTimerDeadline = 0;
            hostInterruptRaise(HOST_IRQ_TIMER);
            continue;
        }

        // Ticks to nanoseconds: the system tick runs at SYSCLOCK_ARM11 Hz.
        s64 waitNs = (s64)((deadline - now) * 1000000000ULL / SYSCLOCK_ARM11);
        LightEvent_WaitTimeout(&sDeadlineChanged, waitNs);
    }
}

unsigned long hostVBlankCount(void) {
    return sVBlankCount;
}

unsigned long long hostVBlankTick(void) {
    return sVBlankTick;
}

static void vblankThread(void* arg) {
    while (sIrqThreadsRunning) {
        gspWaitForVBlank();
        sVBlankTick = svcGetSystemTick();
        sVBlankCount++;
        hostInterruptRaise(HOST_IRQ_VBLANK);
        hostDebugCheckStall();
    }
}

void hostInterruptInit(void) {
    LightEvent_Init(&sIrqEvent, RESET_STICKY);
    LightEvent_Init(&sDeadlineChanged, RESET_ONESHOT);
    sIrqThreadsRunning = true;
    sTimerThread = threadCreate(timerThread, NULL, 0x2000, IRQ_THREAD_PRIORITY, 0, false);
    sVBlankThread = threadCreate(vblankThread, NULL, 0x2000, IRQ_THREAD_PRIORITY, 0, false);
}

void hostInterruptShutdown(void) {
    sIrqThreadsRunning = false;
    LightEvent_Signal(&sDeadlineChanged);
    Thread threads[2] = {sTimerThread, sVBlankThread};
    for (int i = 0; i < 2; i++) {
        if (threads[i] != NULL) {
            threadJoin(threads[i], U64_MAX);  // the vblank thread returns at the next vblank
            threadFree(threads[i]);
        }
    }
    sTimerThread = sVBlankThread = NULL;
}
