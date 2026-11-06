#include "host.h"

#include <3ds.h>
#include <malloc.h>
#include <time.h>
#include <unistd.h>

// Bounds of the application heap that libctru sets up for malloc.
extern u32 __ctru_heap;
extern u32 __ctru_heap_size;

int hostLowMemory(void) {
    return osGetMemRegionSize(MEMREGION_APPLICATION) <= 96 * 1024 * 1024;
}

// libctru's split gives the linear heap 32 MB: too much of 96 MB, where the textures it holds can
// be dropped and made again (jut_texture.cpp) while the game's heaps cannot.
#define LOW_MEMORY_LINEAR_HEAP (22 * 1024 * 1024)
extern u32 __ctru_linear_heap_size;
void __real___system_allocateHeaps(void);

void __wrap___system_allocateHeaps(void) {
    if (hostLowMemory()) {
        Handle limit = 0;
        s64 maxCommit = 0, currentCommit = 0;
        ResourceLimitType type = RESLIMIT_COMMIT;
        if (R_SUCCEEDED(svcGetResourceLimit(&limit, CUR_PROCESS_HANDLE))) {
            svcGetResourceLimitLimitValues(&maxCommit, limit, &type, 1);
            svcGetResourceLimitCurrentValues(&currentCommit, limit, &type, 1);
            svcCloseHandle(limit);
            u32 remaining = (u32)(maxCommit - currentCommit) & ~0xFFFu;
            if (remaining > LOW_MEMORY_LINEAR_HEAP) {
                __ctru_linear_heap_size = LOW_MEMORY_LINEAR_HEAP;
                __ctru_heap_size = remaining - LOW_MEMORY_LINEAR_HEAP;
            }
        }
    }
    __real___system_allocateHeaps();
}

unsigned long hostGetFreeMemory(void) {
    u32 heapEnd = __ctru_heap + __ctru_heap_size;
    u32 brk = (u32)sbrk(0);
    struct mallinfo info = mallinfo();
    return (heapEnd - brk) + info.fordblks;
}

void* hostAllocAligned(unsigned long size, unsigned long alignment) {
    return memalign(alignment, size);
}

// osGetTime() counts milliseconds from 1900-01-01; the Wii counts from 2000-01-01.
#define MS_FROM_1900_TO_2000 3155673600000ULL
#define SECONDS_FROM_1970_TO_2000 946684800LL

long long hostGetMillisecondsSince2000(void) {
    return (long long)(osGetTime() - MS_FROM_1900_TO_2000);
}

void hostGetCalendarTime(long long msecSince2000, int* pYear, int* pMonth, int* pDay, int* pHour, int* pMinute,
                         int* pSecond, int* pWeekDay, int* pYearDay) {
    time_t seconds = (time_t)(msecSince2000 / 1000 + SECONDS_FROM_1970_TO_2000);
    struct tm date;
    gmtime_r(&seconds, &date);

    // Same conventions as the Wii's OSCalendarTime: full year, month from 0.
    *pYear = date.tm_year + 1900;
    *pMonth = date.tm_mon;
    *pDay = date.tm_mday;
    *pHour = date.tm_hour;
    *pMinute = date.tm_min;
    *pSecond = date.tm_sec;
    *pWeekDay = date.tm_wday;
    *pYearDay = date.tm_yday;
}

int hostGetSystemLanguage(void) {
    u8 language = CFG_LANGUAGE_EN;

    if (R_SUCCEEDED(cfguInit())) {
        CFGU_GetSystemLanguage(&language);
        cfguExit();
    }

    return language;
}

int hostIsValidRange(unsigned long address, unsigned long size) {
    // The program (from 0x00100000) and the heap that holds the game's memory.
    unsigned long heapEnd = __ctru_heap + __ctru_heap_size;
    return address >= 0x00100000 && address < heapEnd && size <= heapEnd - address;
}

void hostGetHeapRange(unsigned long* pStart, unsigned long* pEnd) {
    *pStart = __ctru_heap;
    *pEnd = __ctru_heap + __ctru_heap_size;
}

void* hostLinearAlloc(unsigned long size) {
    return linearAlloc(size);
}
