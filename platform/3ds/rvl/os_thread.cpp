// ##############################################################################################
// # Wii OS threads, alarms and interrupt delivery                                              #
// #                                                                                            #
// # On top of 3DS threads. The scheduling logic is Nintendo's (adapted from                    #
// # src/RVL_SDK/os/OSThread.c): run queues per priority, wait queues, suspension and priority  #
// # inheritance through mutexes. Only the context switch is different. Each OSThread runs on   #
// # its own 3DS thread, and switching hands a "baton" to the next thread's semaphore while the #
// # previous one waits on its own. So exactly one game thread runs at any time, like on the    #
// # Wii's single core.                                                                         #
// #                                                                                            #
// # Interrupts (alarms, the video retrace, I/O completion, audio) are raised by host threads   #
// # and handled here whenever the running thread re-enables interrupts, or when no thread is   #
// # ready to run. Handlers therefore run with the same guarantees as on the Wii.               #
// ##############################################################################################

#include "host.h"
#include "rvl_internal.h"
#include "SysTimer3DS.hpp"
#include <revolution.h>

#define EnqueueTail(queue, thread, link)                                                                                                             \
    do {                                                                                                                                             \
        OSThread* __prev = (queue)->tail;                                                                                                            \
        if (__prev == NULL)                                                                                                                          \
            (queue)->head = (thread);                                                                                                                \
        else                                                                                                                                         \
            __prev->link.next = (thread);                                                                                                            \
        (thread)->link.prev = __prev;                                                                                                                \
        (thread)->link.next = NULL;                                                                                                                  \
        (queue)->tail = (thread);                                                                                                                    \
    } while (0)

#define EnqueuePrio(queue, thread, link)                                                                                                             \
    do {                                                                                                                                             \
        OSThread* __prev;                                                                                                                            \
        OSThread* __next;                                                                                                                            \
        for (__next = (queue)->head; __next && __next->priority <= thread->priority; __next = __next->link.next)                                     \
            ;                                                                                                                                        \
        if (__next == NULL)                                                                                                                          \
            EnqueueTail(queue, thread, link);                                                                                                        \
        else {                                                                                                                                       \
            (thread)->link.next = __next;                                                                                                            \
            __prev = __next->link.prev;                                                                                                              \
            __next->link.prev = (thread);                                                                                                            \
            (thread)->link.prev = __prev;                                                                                                            \
            if (__prev == NULL)                                                                                                                      \
                (queue)->head = (thread);                                                                                                            \
            else                                                                                                                                     \
                __prev->link.next = (thread);                                                                                                        \
        }                                                                                                                                            \
    } while (0)

#define DequeueItem(queue, thread, link)                                                                                                             \
    do {                                                                                                                                             \
        OSThread* __next = (thread)->link.next;                                                                                                      \
        OSThread* __prev = (thread)->link.prev;                                                                                                      \
        if (__next == NULL)                                                                                                                          \
            (queue)->tail = __prev;                                                                                                                  \
        else                                                                                                                                         \
            __next->link.prev = __prev;                                                                                                              \
        if (__prev == NULL)                                                                                                                          \
            (queue)->head = __next;                                                                                                                  \
        else                                                                                                                                         \
            __prev->link.next = __next;                                                                                                              \
    } while (0)

#define DequeueHead(queue, thread, link)                                                                                                             \
    do {                                                                                                                                             \
        OSThread* __next;                                                                                                                            \
        (thread) = (queue)->head;                                                                                                                    \
        __next = (thread)->link.next;                                                                                                                \
        if (__next == NULL)                                                                                                                          \
            (queue)->tail = NULL;                                                                                                                    \
        else                                                                                                                                         \
            __next->link.prev = NULL;                                                                                                                \
        (queue)->head = __next;                                                                                                                      \
    } while (0)

#define IsSuspended(suspendCount) (0 < (suspendCount))

extern "C" void __OSUnlockAllMutex(OSThread* thread);

// ##############################################################################################
// # The host side of a game thread                                                             #
// #                                                                                            #
// # Each OSThread has a HostThread: its 3DS thread and the semaphore it waits on for the       #
// # baton. A thread cancelled while it waits ends its 3DS thread when it wakes.                #
// ##############################################################################################
struct HostThread {
    OSThread* mThread;
    void* mSemaphore;
    void* (*mFunc)(void*);
    void* mParam;
    u32 mStackSize;
    bool mStarted;
};

static const int kMaxThreads = 64;
static HostThread sHostThreads[kMaxThreads];

// The host thread this code is running on.
static __thread HostThread* tSelf;

static HostThread* findHostThread(OSThread* thread) {
    for (int i = 0; i < kMaxThreads; i++) {
        if (sHostThreads[i].mThread == thread) {
            return &sHostThreads[i];
        }
    }

    return nullptr;
}

static HostThread* allocHostThread(OSThread* thread) {
    HostThread* host = findHostThread(thread);
    if (host == nullptr) {
        host = findHostThread(nullptr);
        if (host == nullptr) {
            hostFatal("Too many threads.");
        }
    }

    host->mThread = thread;
    if (host->mSemaphore == nullptr) {
        host->mSemaphore = hostSemaphoreCreate();
    }

    host->mStarted = false;
    return host;
}

static bool isDead(OSThread* thread) {
    return thread->state == 0 || thread->state == OS_THREAD_STATE_MORIBUND;
}

// Called by a host thread that just gave away the baton: waits until it gets it back.
// A thread that was terminated meanwhile ends its host thread instead.
static void waitForBaton() {
    HostThread* self = tSelf;

    if (!isDead(self->mThread) || self->mThread == OSGetCurrentThread()) {
        hostSemaphoreWait(self->mSemaphore);
    }

    if (isDead(self->mThread) && self->mThread != OSGetCurrentThread()) {
        self->mThread = nullptr;
        hostThreadExit();
    }
}

static void threadMain(void* param) {
    HostThread* self = static_cast< HostThread* >(param);
    tSelf = self;
    // A new thread starts with interrupts enabled, whatever the thread that switched to it had.
    rvlSetInterruptLevel(TRUE);
    void* result = self->mFunc(self->mParam);
    OSExitThread(result);
}

// ##############################################################################################
// # The scheduler                                                                              #
// #                                                                                            #
// # Adapted from OSThread.c: the same queues, priorities and states. SelectThread picks the    #
// # next thread; switching to it signals its semaphore and waits on our own (the "context      #
// # switch"). With no thread ready, the CPU waits for an interrupt and dispatches it.          #
// ##############################################################################################
static volatile u32 RunQueueBits;
static OSThreadQueue RunQueue[32];
static volatile BOOL RunQueueHint;
static volatile s32 Reschedule;
static OSThread DefaultThread;
static bool sIdle;

extern "C" {

OSThread* __OSCurrentThread;
OSThreadQueue __OSActiveThreadQueue;

}

static void SetRun(OSThread* thread) {
    thread->queue = &RunQueue[thread->priority];
    EnqueueTail(thread->queue, thread, link);
    RunQueueBits |= 1 << (31 - thread->priority);
    RunQueueHint = TRUE;
}

static void UnsetRun(OSThread* thread) {
    OSThreadQueue* queue = thread->queue;
    DequeueItem(queue, thread, link);

    if (queue->head == 0) {
        RunQueueBits &= ~(1 << (31 - thread->priority));
    }

    thread->queue = 0;
}

extern "C" OSPriority __OSGetEffectivePriority(OSThread* thread) {
    OSPriority priority = thread->base;

    for (OSMutex* mutex = thread->queueMutex.head; mutex; mutex = mutex->link.next) {
        OSThread* blocked = mutex->queue.head;
        if (blocked != 0 && blocked->priority < priority) {
            priority = blocked->priority;
        }
    }

    return priority;
}

static OSThread* SetEffectivePriority(OSThread* thread, OSPriority priority) {
    switch (thread->state) {
    case OS_THREAD_STATE_READY:
        UnsetRun(thread);
        thread->priority = priority;
        SetRun(thread);
        break;
    case OS_THREAD_STATE_WAITING:
        DequeueItem(thread->queue, thread, link);
        thread->priority = priority;
        EnqueuePrio(thread->queue, thread, link);
        if (thread->mutex != 0) {
            return thread->mutex->thread;
        }
        break;
    case OS_THREAD_STATE_RUNNING:
        RunQueueHint = TRUE;
        thread->priority = priority;
        break;
    }

    return NULL;
}

static void UpdatePriority(OSThread* thread) {
    do {
        if (IsSuspended(thread->suspend)) {
            break;
        }

        OSPriority priority = __OSGetEffectivePriority(thread);
        if (thread->priority == priority) {
            break;
        }

        thread = SetEffectivePriority(thread, priority);
    } while (thread);
}

extern "C" void __OSPromoteThread(OSThread* thread, OSPriority priority) {
    do {
        if (thread->suspend > 0 || thread->priority <= priority) {
            break;
        }

        thread = SetEffectivePriority(thread, priority);
    } while (thread);
}

static void dispatchInterrupts();

static OSThread* SelectThread(BOOL yield) {
    if (Reschedule > 0 || sIdle) {
        return 0;
    }

    OSThread* currentThread = OSGetCurrentThread();
    if (currentThread != 0 && currentThread->state == OS_THREAD_STATE_RUNNING) {
        if (!yield) {
            OSPriority priority = __cntlzw(RunQueueBits);
            if (currentThread->priority <= priority) {
                return 0;
            }
        }

        currentThread->state = OS_THREAD_STATE_READY;
        SetRun(currentThread);
    }

    if (RunQueueBits == 0) {
        // Nothing can run: idle until an interrupt makes a thread ready.
        __OSCurrentThread = nullptr;
        sIdle = true;

        do {
            hostInterruptWait();
            dispatchInterrupts();
        } while (RunQueueBits == 0);

        sIdle = false;
    }

    RunQueueHint = FALSE;
    OSPriority priority = __cntlzw(RunQueueBits);
    OSThreadQueue* queue = &RunQueue[priority];
    OSThread* nextThread;
    DequeueHead(queue, nextThread, link);

    if (queue->head == 0) {
        RunQueueBits &= ~(1 << (31 - priority));
    }

    nextThread->queue = 0;
    nextThread->state = OS_THREAD_STATE_RUNNING;
    __OSCurrentThread = nextThread;

    HostThread* next = findHostThread(nextThread);
    if (next == tSelf) {
        return nextThread;
    }

    // This thread's interrupt state, given back when it runs again: the next thread brings its
    // own (it used to inherit ours, so a thread switched to from a disabled section kept
    // interrupts off; GameScene's wave data wait then never saw its disc read complete).
    BOOL level = rvlInterruptLevel();
    if (!next->mStarted) {
        next->mStarted = true;
        if (!hostThreadCreate(threadMain, next, next->mStackSize)) {
            hostFatal("Could not create a host thread.");
        }
    } else {
        hostSemaphoreSignal(next->mSemaphore);
    }

    waitForBaton();
    rvlSetInterruptLevel(level);
    return 0;
}

extern "C" {

void __OSReschedule(void) {
    if (RunQueueHint) {
        SelectThread(FALSE);
    }
}

void OSInitThreadQueue(OSThreadQueue* queue) {
    queue->head = queue->tail = 0;
}

OSThread* OSGetCurrentThread(void) {
    return __OSCurrentThread;
}

BOOL OSIsThreadSuspended(OSThread* thread) {
    return thread->suspend > 0 ? TRUE : FALSE;
}

BOOL OSIsThreadTerminated(OSThread* thread) {
    return isDead(thread) ? TRUE : FALSE;
}

s32 OSDisableScheduler(void) {
    BOOL enabled = OSDisableInterrupts();
    s32 count = Reschedule++;
    OSRestoreInterrupts(enabled);
    return count;
}

s32 OSEnableScheduler(void) {
    BOOL enabled = OSDisableInterrupts();
    s32 count = Reschedule--;
    OSRestoreInterrupts(enabled);
    return count;
}

void OSYieldThread(void) {
    BOOL enabled = OSDisableInterrupts();
    SelectThread(TRUE);
    OSRestoreInterrupts(enabled);
}

BOOL OSCreateThread(OSThread* thread, void* (*func)(void*), void* param, void* stack, u32 stackSize, OSPriority priority, u16 attr) {
    if (priority < 0 || 31 < priority) {
        return FALSE;
    }

    thread->state = OS_THREAD_STATE_READY;
    thread->attr = static_cast< u16 >(attr & 1);
    thread->priority = thread->base = priority;
    thread->suspend = 1;
    thread->value = (void*)-1;
    thread->mutex = NULL;
    OSInitThreadQueue(&thread->queueJoin);
    thread->queueMutex.head = thread->queueMutex.tail = NULL;
    thread->stackBase = static_cast< u8* >(stack);
    thread->stackEnd = reinterpret_cast< u32* >(static_cast< u8* >(stack) - stackSize);
    thread->error = 0;
    thread->specific[0] = thread->specific[1] = 0;

    BOOL enabled = OSDisableInterrupts();
    HostThread* host = allocHostThread(thread);
    host->mFunc = func;
    host->mParam = param;
    host->mStackSize = stackSize;
    EnqueueTail(&__OSActiveThreadQueue, thread, linkActive);
    OSRestoreInterrupts(enabled);
    return TRUE;
}

void OSExitThread(void* val) {
    OSDisableInterrupts();
    OSThread* currentThread = OSGetCurrentThread();

    if (currentThread->attr & 1) {
        DequeueItem(&__OSActiveThreadQueue, currentThread, linkActive);
        currentThread->state = 0;
    } else {
        currentThread->state = OS_THREAD_STATE_MORIBUND;
        currentThread->value = val;
    }

    __OSUnlockAllMutex(currentThread);
    OSWakeupThread(&currentThread->queueJoin);
    RunQueueHint = TRUE;
    __OSReschedule();

    // Only reached if no other thread was selected; there is nothing left to run here.
    hostFatal("OSExitThread returned.");
}

void OSCancelThread(OSThread* thread) {
    BOOL enabled = OSDisableInterrupts();

    switch (thread->state) {
    case OS_THREAD_STATE_READY:
        if (!IsSuspended(thread->suspend)) {
            UnsetRun(thread);
        }
        break;
    case OS_THREAD_STATE_RUNNING:
        RunQueueHint = TRUE;
        break;
    case OS_THREAD_STATE_WAITING:
        DequeueItem(thread->queue, thread, link);
        thread->queue = NULL;
        if (!IsSuspended(thread->suspend) && thread->mutex) {
            UpdatePriority(thread->mutex->thread);
        }
        break;
    default:
        OSRestoreInterrupts(enabled);
        return;
    }

    if (thread->attr & 1) {
        DequeueItem(&__OSActiveThreadQueue, thread, linkActive);
        thread->state = 0;
    } else {
        thread->state = OS_THREAD_STATE_MORIBUND;
    }

    __OSUnlockAllMutex(thread);

    // Let a parked host thread notice it was cancelled and end itself.
    HostThread* host = findHostThread(thread);
    if (host != nullptr && host->mStarted && host != tSelf) {
        hostSemaphoreSignal(host->mSemaphore);
    }

    OSWakeupThread(&thread->queueJoin);
    __OSReschedule();
    OSRestoreInterrupts(enabled);
}

static BOOL isThreadActive(OSThread* thread) {
    if (thread->state == 0) {
        return FALSE;
    }

    for (OSThread* active = __OSActiveThreadQueue.head; active; active = active->linkActive.next) {
        if (thread == active) {
            return TRUE;
        }
    }

    return FALSE;
}

BOOL OSJoinThread(OSThread* thread, void** val) {
    BOOL enabled = OSDisableInterrupts();

    if (!(thread->attr & 1) && thread->state != OS_THREAD_STATE_MORIBUND && thread->queueJoin.head == NULL) {
        OSSleepThread(&thread->queueJoin);
        if (!isThreadActive(thread)) {
            OSRestoreInterrupts(enabled);
            return FALSE;
        }
    }

    if (((volatile OSThread*)thread)->state == OS_THREAD_STATE_MORIBUND) {
        if (val) {
            *val = thread->value;
        }

        DequeueItem(&__OSActiveThreadQueue, thread, linkActive);
        thread->state = 0;
        OSRestoreInterrupts(enabled);
        return TRUE;
    }

    OSRestoreInterrupts(enabled);
    return FALSE;
}

void OSDetachThread(OSThread* thread) {
    BOOL enabled = OSDisableInterrupts();

    thread->attr |= 1;
    if (thread->state == OS_THREAD_STATE_MORIBUND) {
        DequeueItem(&__OSActiveThreadQueue, thread, linkActive);
        thread->state = 0;
    }

    OSWakeupThread(&thread->queueJoin);
    OSRestoreInterrupts(enabled);
}

s32 OSResumeThread(OSThread* thread) {
    BOOL enabled = OSDisableInterrupts();
    s32 suspendCount = thread->suspend--;

    if (thread->suspend < 0) {
        thread->suspend = 0;
    } else if (thread->suspend == 0) {
        switch (thread->state) {
        case OS_THREAD_STATE_READY:
            thread->priority = __OSGetEffectivePriority(thread);
            SetRun(thread);
            break;
        case OS_THREAD_STATE_WAITING:
            DequeueItem(thread->queue, thread, link);
            thread->priority = __OSGetEffectivePriority(thread);
            EnqueuePrio(thread->queue, thread, link);
            if (thread->mutex) {
                UpdatePriority(thread->mutex->thread);
            }
            break;
        }

        __OSReschedule();
    }

    OSRestoreInterrupts(enabled);
    return suspendCount;
}

s32 OSSuspendThread(OSThread* thread) {
    BOOL enabled = OSDisableInterrupts();
    s32 suspendCount = thread->suspend++;

    if (suspendCount == 0) {
        switch (thread->state) {
        case OS_THREAD_STATE_RUNNING:
            RunQueueHint = TRUE;
            thread->state = OS_THREAD_STATE_READY;
            break;
        case OS_THREAD_STATE_READY:
            UnsetRun(thread);
            break;
        case OS_THREAD_STATE_WAITING:
            DequeueItem(thread->queue, thread, link);
            thread->priority = 32;
            EnqueueTail(thread->queue, thread, link);
            if (thread->mutex) {
                UpdatePriority(thread->mutex->thread);
            }
            break;
        }

        __OSReschedule();
    }

    OSRestoreInterrupts(enabled);
    return suspendCount;
}

void OSSleepThread(OSThreadQueue* queue) {
    BOOL enabled = OSDisableInterrupts();
    OSThread* currentThread = OSGetCurrentThread();

    currentThread->state = OS_THREAD_STATE_WAITING;
    currentThread->queue = queue;
    EnqueuePrio(queue, currentThread, link);
    RunQueueHint = TRUE;
    __OSReschedule();
    OSRestoreInterrupts(enabled);
}

void OSWakeupThread(OSThreadQueue* queue) {
    BOOL enabled = OSDisableInterrupts();

    while (queue->head) {
        OSThread* thread;
        DequeueHead(queue, thread, link);
        thread->state = OS_THREAD_STATE_READY;

        if (!IsSuspended(thread->suspend)) {
            SetRun(thread);
        }
    }

    __OSReschedule();
    OSRestoreInterrupts(enabled);
}

BOOL OSSetThreadPriority(OSThread* thread, OSPriority priority) {
    if (priority < 0 || priority > 31) {
        return FALSE;
    }

    BOOL enabled = OSDisableInterrupts();

    if (thread->base != priority) {
        thread->base = priority;
        UpdatePriority(thread);
        __OSReschedule();
    }

    OSRestoreInterrupts(enabled);
    return TRUE;
}

OSPriority OSGetThreadPriority(OSThread* thread) {
    return thread->base;
}

static void SleepAlarmHandler(OSAlarm* alarm, OSContext* context) {
    OSResumeThread(static_cast< OSThread* >(OSGetAlarmUserData(alarm)));
}

void OSSleepTicks(OSTime tick) {
    BOOL enabled = OSDisableInterrupts();
    OSThread* current = OSGetCurrentThread();

    if (current == NULL) {
        OSRestoreInterrupts(enabled);
        return;
    }

    OSAlarm sleepAlarm;
    OSCreateAlarm(&sleepAlarm);
    OSSetAlarmTag(&sleepAlarm, reinterpret_cast< u32 >(current));
    OSSetAlarmUserData(&sleepAlarm, current);
    OSSetAlarm(&sleepAlarm, tick, SleepAlarmHandler);
    OSSuspendThread(current);
    OSCancelAlarm(&sleepAlarm);
    OSRestoreInterrupts(enabled);
}

// The game never inspects contexts on the 3DS; one shared context keeps callers happy.
static OSContext sDummyContext;

OSContext* OSGetCurrentContext(void) {
    return &sDummyContext;
}

}  // extern "C"

// ##############################################################################################
// # Alarms                                                                                     #
// #                                                                                            #
// # OS alarms in one list sorted by their time; the host timer thread (host/thread.c) is set   #
// # to the first one and raises HOST_IRQ_TIMER, and fireAlarms runs the handlers that are due, #
// # setting periodic alarms again.                                                             #
// ##############################################################################################
static OSAlarm* sAlarmHead;

static void updateTimer() {
    hostInterruptSetTimer(sAlarmHead != nullptr ? rvlOSTimeToHostTick(sAlarmHead->fire) : 0);
}

static void insertAlarm(OSAlarm* alarm, OSTime fire, OSAlarmHandler handler) {
    if (alarm->period > 0) {
        OSTime now = OSGetTime();
        fire = alarm->start;
        if (alarm->start < now) {
            fire += alarm->period * ((now - alarm->start) / alarm->period + 1);
        }
    }

    alarm->handler = handler;
    alarm->fire = fire;

    OSAlarm* prev = nullptr;
    OSAlarm* next = sAlarmHead;
    while (next != nullptr && next->fire <= fire) {
        prev = next;
        next = next->next;
    }

    alarm->prev = prev;
    alarm->next = next;
    if (next != nullptr) {
        next->prev = alarm;
    }
    if (prev != nullptr) {
        prev->next = alarm;
    } else {
        sAlarmHead = alarm;
        updateTimer();
    }
}

static void removeAlarm(OSAlarm* alarm) {
    if (alarm->next != nullptr) {
        alarm->next->prev = alarm->prev;
    }
    if (alarm->prev != nullptr) {
        alarm->prev->next = alarm->next;
    } else {
        sAlarmHead = alarm->next;
        updateTimer();
    }

    alarm->next = alarm->prev = nullptr;
}

static void fireAlarms() {
    OSTime now = OSGetTime();

    while (sAlarmHead != nullptr && sAlarmHead->fire <= now) {
        OSAlarm* alarm = sAlarmHead;
        OSAlarmHandler handler = alarm->handler;
        removeAlarm(alarm);
        alarm->handler = nullptr;

        if (alarm->period > 0) {
            insertAlarm(alarm, 0, handler);
        }

        handler(alarm, &sDummyContext);
    }

    updateTimer();
}

extern "C" {

void OSCreateAlarm(OSAlarm* alarm) {
    alarm->handler = nullptr;
    alarm->tag = 0;
}

void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler) {
    BOOL enabled = OSDisableInterrupts();
    alarm->period = 0;
    insertAlarm(alarm, OSGetTime() + tick, handler);
    OSRestoreInterrupts(enabled);
}

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler) {
    BOOL enabled = OSDisableInterrupts();
    alarm->period = period;
    alarm->start = start;
    insertAlarm(alarm, 0, handler);
    OSRestoreInterrupts(enabled);
}

void OSCancelAlarm(OSAlarm* alarm) {
    BOOL enabled = OSDisableInterrupts();
    if (alarm->handler != nullptr) {
        removeAlarm(alarm);
        alarm->handler = nullptr;
    }
    OSRestoreInterrupts(enabled);
}

void OSSetAlarmTag(OSAlarm* alarm, u32 tag) {
    alarm->tag = tag;
}

void OSSetAlarmUserData(OSAlarm* alarm, void* data) {
    alarm->userData = data;
}

void* OSGetAlarmUserData(const OSAlarm* alarm) {
    return alarm->userData;
}

}  // extern "C"

// ##############################################################################################
// # Interrupt delivery                                                                         #
// #                                                                                            #
// # dispatchInterrupts runs the handler of every pending interrupt, with interrupts disabled,  #
// # never nested; rvlInterruptsEnabled calls it when the game enables interrupts again.        #
// ##############################################################################################
static bool sInInterrupt;

// Runs the handlers of every pending interrupt. Called with interrupts disabled.
static void dispatchInterrupts() {
    if (sInInterrupt) {
        return;
    }

    sInInterrupt = true;
    unsigned pending = hostInterruptTakePending();

    if (pending & HOST_IRQ_TIMER) {
        fireAlarms();
    }
    if (pending & HOST_IRQ_VBLANK) {
        rvlVIRetraceInterrupt();
    }
    if (pending & HOST_IRQ_IO) {
        rvlIOInterrupt();
    }
    if (pending & HOST_IRQ_AUDIO) {
        rvlDSPWatchdog();
        rvlAIInterrupt();
    }
    if (pending & HOST_IRQ_DSP) {
        rvlDSPInterrupt();
    }

    sInInterrupt = false;
}

void rvlInterruptsEnabled() {
    if (sInInterrupt || sIdle || !hostInterruptPending()) {
        return;
    }

    // Timed for the profile log ('Interrupts ms'): handlers (64) and the game threads they wake,
    // such as JAudio's, running before this thread goes on (65). Both run on the caller's time:
    // in the Comet Observatory's busy places updateRhythm's critical section took 3.9 ms a loop
    // (CIA 0.20.7), most of it presumably here.
    BOOL enabled = OSDisableInterrupts();
    {
        SysTimer3DS timer3DS(64);
        dispatchInterrupts();
    }
    OSRestoreInterrupts(enabled);

    // Like returning from an interrupt on the Wii: a woken thread may now preempt us.
    BOOL inner = OSDisableInterrupts();
    {
        SysTimer3DS timer3DS(65);
        __OSReschedule();
    }
    OSRestoreInterrupts(inner);
}

// ##############################################################################################
// # Stall report and startup                                                                   #
// #                                                                                            #
// # What every game thread is doing, for debug.c's stall report; rvlInit makes the thread the  #
// # program started on the Wii's default thread and starts the host's interrupt threads.       #
// ##############################################################################################
// For the stall report (debug.c, on the retrace thread): what every game thread is doing.
extern "C" void rvlDebugLogThreads(void) {
    hostLog("threads: current %p, scheduler off %ld, run bits %08lX, interrupts %d, in interrupt %d, idle %d\n",
            static_cast< void* >(__OSCurrentThread), static_cast< long >(Reschedule), static_cast< unsigned long >(RunQueueBits),
            rvlInterruptLevel(), sInInterrupt, sIdle);
    for (int i = 0; i < kMaxThreads; i++) {
        OSThread* thread = sHostThreads[i].mThread;
        if (thread != nullptr) {
            hostLog("thread %p: priority %ld, state %d, suspend %ld, waits for mutex %p held by %p\n", static_cast< void* >(thread),
                    static_cast< long >(thread->priority), thread->state, static_cast< long >(thread->suspend),
                    static_cast< void* >(thread->mutex), thread->mutex != nullptr ? static_cast< void* >(thread->mutex->thread) : nullptr);
        }
    }
}

extern "C" void rvlInit(void) {
    OSThread* thread = &DefaultThread;
    thread->state = OS_THREAD_STATE_RUNNING;
    thread->attr = 1;
    thread->priority = thread->base = 16;
    thread->suspend = 0;
    thread->value = (void*)-1;
    thread->mutex = NULL;
    OSInitThreadQueue(&thread->queueJoin);
    thread->queueMutex.head = thread->queueMutex.tail = NULL;

    HostThread* host = allocHostThread(thread);
    host->mStarted = true;
    tSelf = host;
    __OSCurrentThread = thread;

    RunQueueBits = 0;
    RunQueueHint = FALSE;
    for (int prio = 0; prio <= 31; prio++) {
        OSInitThreadQueue(&RunQueue[prio]);
    }

    OSInitThreadQueue(&__OSActiveThreadQueue);
    EnqueueTail(&__OSActiveThreadQueue, thread, linkActive);
    Reschedule = 0;

    hostInterruptInit();
}
