#pragma once

// Shared between the files of the Wii SDK replacement layer (game world only).

#include <revolution/types.h>
#include <revolution/os/OSTime.h>

// Converts a Wii OSTime to the host's system tick, for the interrupt timer.
u64 rvlOSTimeToHostTick(OSTime time);

// Called when interrupts go from disabled to enabled: delivers pending interrupts.
void rvlInterruptsEnabled();

// The running thread's interrupt enable state, kept per thread by the context switch (the Wii
// keeps it in each thread's saved MSR), without delivering anything.
extern "C" BOOL rvlInterruptLevel();
extern "C" void rvlSetInterruptLevel(BOOL level);

// Interrupt handlers, run by the dispatcher in os_thread.cpp with interrupts disabled.
void rvlVIRetraceInterrupt();
void rvlIOInterrupt();
void rvlAIInterrupt();
void rvlDSPInterrupt();
void rvlDSPWatchdog();
