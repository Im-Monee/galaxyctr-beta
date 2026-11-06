// ##############################################################################################
// # The 3DS program: start, log, quit                                                          #
// #                                                                                            #
// # main() checks the console, starts the screens, the log and the debug options, sets up the  #
// # emulated Wii OS (platform/3ds/rvl) and the game data, then hands over to the game          #
// # (smgMain), which never returns. This file also holds the log, fatal errors, the per-frame  #
// # main loop, the controls and quitting.                                                      #
// ##############################################################################################
#include "host.h"
#include "showcase.h"
#include "build_version.h"

#include <3ds.h>
#include <3ds/romfs.h>
#include <3ds/services/fs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

// Entry point of the game, defined in src/Game/System/GameSystem.cpp. It never returns.
void smgMain(void);

static FILE* sLogFile;
static bool sConsoleLog = !SMG_SHOWCASE;  // log lines also on the bottom screen
static bool sCstick;  // irrst (C-Stick, ZL/ZR) available

// ##############################################################################################
// # The log over the network                                                                   #
// #                                                                                            #
// # Only for a .3dsx started from the PC with 3dslink (development): the CIA never has a       #
// # 3dslink host and skips this.                                                               #
// ##############################################################################################
// Started from the PC with 3dslink: the log also goes to the PC (stderr over the network,
// shown by "3dslink -s"); the bottom screen keeps its own copy on stdout.
static bool sLinked;

static void connectToPc(void) {
    if (__3dslink_host.s_addr == 0) {
        return;
    }
    static const u32 kSocBufferSize = 0x40000;
    u32* buffer = (u32*)memalign(0x1000, kSocBufferSize);
    if (buffer == NULL || R_FAILED(socInit(buffer, kSocBufferSize))) {
        return;
    }
    // Like link3dsConnectToHost(false, true), but giving up after about 10 seconds: with no
    // receiver on the PC (or a firewall dropping the packets) that call waits for minutes, and
    // right after the launch the Wi-Fi may not be up yet, so failed attempts are retried.
    printf("Connecting to the PC for the log...\n");
    gfxFlushBuffers();
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(LINK3DS_COMM_PORT);
    address.sin_addr = __3dslink_host;
    for (int attempt = 0; attempt < 10 && !sLinked; attempt++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            return;
        }
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        u64 started = osGetTime();
        connect(sock, (struct sockaddr*)&address, sizeof(address));
        struct pollfd wait = {sock, POLLOUT, 0};
        // Connected when the socket becomes writable without an error flag (the 3DS sockets do
        // not report SO_ERROR).
        if (poll(&wait, 1, 1000) == 1 && (wait.revents & (POLLERR | POLLHUP)) == 0 && (wait.revents & POLLOUT) != 0) {
            fcntl(sock, F_SETFL, flags);
            dup2(sock, STDERR_FILENO);
            sLinked = true;
        } else {
            close(sock);
            u64 spent = osGetTime() - started;
            if (spent < 1000) {
                svcSleepThread((1000 - spent) * 1000000LL);
            }
        }
    }
    if (!sLinked) {
        printf("No PC log (the log is in sdmc:/smg/log.txt).\n");
    }
}

// ##############################################################################################
// # The log file                                                                               #
// #                                                                                            #
// # sdmc:/smg/log.txt, overwritten at every start. Lines collect in a buffer in memory; a      #
// # thread of its own writes them to the card every quarter second, so the game does not wait  #
// # for the card on every line and the log still reaches the card while the game hangs. In     #
// # development builds the lines also show on the bottom screen.                               #
// ##############################################################################################
static void logFlushThread(void* arg);
static LightLock sLogLock;
static Thread sLogThread;
static volatile bool sLogThreadRunning = true;

static void openLogFile(void) {
    mkdir("sdmc:/smg", 0777);
    // O_SYNC: libctru's SD driver only commits a write to the card (FS_WRITE_FLUSH) for files
    // opened with it. Without it the log reached the card at fclose only, and a run that did not
    // quit cleanly (a hang, then the power button) left no log at all.
    int fd = open("sdmc:/smg/log.txt", O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0666);
    sLogFile = fd >= 0 ? fdopen(fd, "w") : NULL;
    LightLock_Init(&sLogLock);
    if (sLogFile != NULL) {
        // Above the game threads (0x30), so it runs even while one of them never yields.
        sLogThread = threadCreate(logFlushThread, NULL, 0x2000, 0x2E, 0, false);
    }
}

// Log lines collect in memory and a thread of their own commits them to the card every
// LOG_FLUSH_MS. Committing every line (O_SYNC) took the loading thread to the card a thousand
// times before the first frame; the thread still writes the log while the game hangs.
#define LOG_BUFFER_SIZE (64 * 1024)
#define LOG_FLUSH_MS 250
static char sLogBuffer[LOG_BUFFER_SIZE];
static unsigned long sLogLength;

// Time the game's threads spent logging (lines buffered or written), for hostLogLoadProfile.
static unsigned long long sLogTicks;
static u64 sStartTime;  // osGetTime at main
static unsigned long sLogWrites;

// Commits the buffered lines. With sLogLock held.
static void flushLogLocked(void) {
    if (sLogFile != NULL && sLogLength != 0) {
        fwrite(sLogBuffer, 1, sLogLength, sLogFile);
        fflush(sLogFile);
    }
    sLogLength = 0;
}

static void hostLogFlush(void) {
    LightLock_Lock(&sLogLock);
    flushLogLocked();
    LightLock_Unlock(&sLogLock);
}

static void logFlushThread(void* arg) {
    (void)arg;
    while (sLogThreadRunning) {
        svcSleepThread(LOG_FLUSH_MS * 1000000LL);
        hostLogFlush();
    }
}

// At quit, before exit frees the heap the threads' stacks live on: a thread still running then
// faults (Luma crash dump after START+SELECT in CIA 0.11.15). Game threads are all blocked.
static void stopHostThreads(void) {
    hostInterruptShutdown();
    hostAudioShutdown();
    if (sLogThread != NULL) {
        sLogThreadRunning = false;
        threadJoin(sLogThread, U64_MAX);
        threadFree(sLogThread);
        sLogThread = NULL;
    }
}

void hostVLog(const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    if (sConsoleLog) {
        vprintf(fmt, args);
        // The console is on the single-buffered bottom screen: flushing is enough. Swapping
        // would also flip the top screen, which belongs to the renderer.
        gfxFlushBuffers();
    }
    if (sLinked) {
        va_list net;
        va_copy(net, copy);
        vfprintf(stderr, fmt, net);
        va_end(net);
    }

    if (sLogFile != NULL) {
        unsigned long long start = svcGetSystemTick();
        LightLock_Lock(&sLogLock);
        va_list measure;
        va_copy(measure, copy);
        int length = vsnprintf(NULL, 0, fmt, measure);
        va_end(measure);
        if (length > 0 && sLogLength + (unsigned long)length >= LOG_BUFFER_SIZE) {
            flushLogLocked();
        }
        if (length > 0 && (unsigned long)length < LOG_BUFFER_SIZE) {
            vsnprintf(sLogBuffer + sLogLength, LOG_BUFFER_SIZE - sLogLength, fmt, copy);
            sLogLength += (unsigned long)length;
        } else if (length > 0) {
            vfprintf(sLogFile, fmt, copy);  // longer than the buffer: straight to the card
            fflush(sLogFile);
        }
        LightLock_Unlock(&sLogLock);
        sLogTicks += svcGetSystemTick() - start;
        sLogWrites++;
    }

    va_end(copy);
}

void hostLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    hostVLog(fmt, args);
    va_end(args);
}

// ##############################################################################################
// # Mii data from the console                                                                  #
// #                                                                                            #
// # The game shows the player's Miis (rvl/rfl.cpp). The 3DS keeps them in Mii Maker's          #
// # database, CFL_DB.dat in the shared extdata, and their face parts in CFL_Res.dat inside a   #
// # system title; both are read from the console itself, never shipped with the port.          #
// ##############################################################################################
int hostMiiDatabase(void* pData, unsigned long capacity, unsigned long* pSize) {
    if (pSize != NULL) {
        *pSize = 0;
    }
    if (pData == NULL || pSize == NULL) {
        return 0;
    }
    // The shared extdata of Miis and Play Coins, on the NAND.
    static const u32 kPath[3] = {MEDIATYPE_NAND, 0xF000000B, 0};
    FS_Archive archive;
    Result rc = FSUSER_OpenArchive(&archive, ARCHIVE_SHARED_EXTDATA, (FS_Path){PATH_BINARY, sizeof(kPath), kPath});
    if (R_FAILED(rc)) {
        hostLog("mii: shared extdata not opened (%08lx)\n", (unsigned long)rc);
        return 0;
    }
    Handle file;
    rc = FSUSER_OpenFile(&file, archive, fsMakePath(PATH_ASCII, "/CFL_DB.dat"), FS_OPEN_READ, 0);
    if (R_FAILED(rc)) {
        hostLog("mii: CFL_DB.dat not opened (%08lx)\n", (unsigned long)rc);
        FSUSER_CloseArchive(archive);
        return 0;
    }
    u32 read = 0;
    rc = FSFILE_Read(file, &read, 0, pData, (u32)capacity);
    FSFILE_Close(file);
    FSUSER_CloseArchive(archive);
    if (R_FAILED(rc)) {
        hostLog("mii: CFL_DB.dat not read (%08lx)\n", (unsigned long)rc);
        return 0;
    }
    *pSize = read;
    return 1;
}

int hostCflResource(const void** data, unsigned long* size) {
    static void* resource;
    static unsigned long resourceSize;
    static int attempted;
    static const u64 titleId = 0x0004009B00010202ULL;
    static const char* archiveName = "$CFLRES";
    if (data == NULL || size == NULL) {
        return 0;
    }
    if (attempted) {
        *data = resource;
        *size = resourceSize;
        return resource != NULL;
    }
    attempted = 1;
    Result rc = romfsMountFromTitle(titleId, MEDIATYPE_NAND, archiveName);
    if (R_SUCCEEDED(rc)) {
        FILE* file = fopen("$CFLRES:/CFL_Res.dat", "rb");
        if (file != NULL && fseek(file, 0, SEEK_END) == 0) {
            long fileSize = ftell(file);
            rewind(file);
            if (fileSize > 0 && fileSize <= 8 * 1024 * 1024) {
                resource = malloc((size_t)fileSize);
                if (resource != NULL && fread(resource, 1, (size_t)fileSize, file) == (size_t)fileSize) {
                    resourceSize = (unsigned long)fileSize;
                    hostLog("mii: loaded system CFL resource (%lu bytes)\n", resourceSize);
                } else {
                    free(resource);
                    resource = NULL;
                }
            }
        }
        if (file != NULL) {
            fclose(file);
        }
        romfsUnmount(archiveName);
    }
    if (resource == NULL) {
        hostLog("mii: system CFL_Res.dat unavailable\n");
    }
    *data = resource;
    *size = resourceSize;
    return resource != NULL;
}

// ##############################################################################################
// # Fatal errors, the main loop and quitting                                                   #
// #                                                                                            #
// # hostFatal shows its message on the bottom screen even in a showcase build, and waits for   #
// # START. hostMainLoop runs once per game frame: it reads the controls and ends the game when #
// # START and SELECT are held together; hostQuit then runs the Wii shutdown callbacks and      #
// # stops the host threads before exit frees the heap their stacks are on.                     #
// ##############################################################################################
void hostFatal(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (!sConsoleLog) {
        consoleClear();
        sConsoleLog = true;
    }
    hostLog("\nFATAL: ");
    hostVLog(fmt, args);
    hostLog("\nPress START to exit.\n");
    va_end(args);

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gspWaitForVBlank();
    }

    hostLogFlush();
    if (sLogFile != NULL) {
        fclose(sLogFile);
    }
    gfxExit();
    exit(1);
}

void hostStubHit(const char* name) {
    hostLog("stub: %s\n", name);
}

int hostMainLoop(void) {
    if (!aptMainLoop()) {
        return 0;
    }

    hidScanInput();
    if (sCstick) {
        irrstScanInput();
    }
    if ((hidKeysHeld() & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT)) {
        return 0;
    }

    return 1;
}

void hostQuit(void) {
    hostLog("Quit requested.\n");
    rvlShutdown();
    hostLog("Quitting.\n");
    stopHostThreads();
    hostLogFlush();
    if (sLogFile != NULL) {
        fclose(sLogFile);
    }
    gfxExit();
    exit(0);
}

unsigned long long hostGetTick(void) {
    return svcGetSystemTick();
}

unsigned long long hostGetTickFrequency(void) {
    return SYSCLOCK_ARM11;
}

// ##############################################################################################
// # The main thread's stack                                                                    #
// #                                                                                            #
// # Its size, and how much of it the game has used: the free part is painted with a pattern at #
// # the start, and the lowest word no longer holding it is the deepest point reached (logged   #
// # with the memory line of gfx.c).                                                            #
// ##############################################################################################
// The game runs on the main thread. libctru gives it a stack of __stacksize__ bytes at the start of
// the heap (system/stack_adjust.s), not the rsf's StackSize; its default of 32 KB overflowed after
// falling off the Observatory (Luma dump in CIA 0.19.13: sp 0x08000000, write at 0x07FFFFD8).
u32 __stacksize__ = 256 * 1024;

// Stack use measured by painting the free part at start and finding the lowest overwritten word.
extern char* fake_heap_start;  // the stack's top, set by libctru before main
#define STACK_PAINT 0x5AC3A55Au
static u32* sStackBottom;

static __attribute__((noinline)) void paintMainStack(void) {
    u32* top = (u32*)fake_heap_start;
    sStackBottom = top - __stacksize__ / 4;
    u32* below = (u32*)__builtin_frame_address(0) - 256;  // leave this frame and the callers alone
    for (u32* word = sStackBottom; word < below; word++) {
        *word = STACK_PAINT;
    }
}

unsigned long hostMainStackUsed(void) {
    u32* word = sStackBottom;
    if (word == NULL) {
        return 0;
    }
    u32* top = word + __stacksize__ / 4;
    while (word < top && *word == STACK_PAINT) {
        word++;
    }
    return (unsigned long)((char*)top - (char*)word);
}

// ##############################################################################################
// # New 3DS family only                                                                        #
// #                                                                                            #
// # The game needs the New 3DS's memory and CPU: on an original 3DS it does not fit in memory  #
// # and runs far below game speed (DETAILS.md, section 11). A CIA installer cannot refuse a    #
// # console model, so the check happens here, before anything else: the bottom screen explains #
// # why and the app closes on START. The low-memory mode (hostLowMemory) is separate: it is    #
// # the memory layout the app was started with, while this checks the console itself, so the   #
// # Old 3DS test build still runs on a New model.                                              #
// ##############################################################################################
static void refuseOriginal3DS(void) {
    bool isNew = false;
    // A failed query is treated as a New model: refusing a console by mistake is worse.
    if (R_FAILED(APT_CheckNew3DS(&isNew)) || isNew) {
        return;
    }
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    printf("\x1b[10;1HThis port needs a New Nintendo 3DS,");
    printf("\x1b[11;1HNew 3DS XL or New 2DS XL.");
    printf("\x1b[13;1HThe original 3DS, 3DS XL and 2DS do");
    printf("\x1b[14;1Hnot have enough memory to run it.");
    printf("\x1b[17;1HPress START to exit.");
    while (aptMainLoop()) {
        gspWaitForVBlank();
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
    }
    gfxExit();
    exit(0);
}

// ##############################################################################################
// # main                                                                                       #
// #                                                                                            #
// # The order matters: the console check before the screens are set up, the CPU speed before   #
// # the work starts, the screens before the log (which writes to the bottom screen), the       #
// # renderer before the game world, and the game data (rvl/port_data.cpp) after the emulated   #
// # Wii OS and before the game.                                                                #
// ##############################################################################################
int main(void) {
    refuseOriginal3DS();
    paintMainStack();
    sStartTime = osGetTime();
    // Run the CPU at 804 MHz and enable the extra cache on New 3DS models, through ptm:sysm: without
    // that service in the rsf this does nothing, silently (CIAs up to 0.16.9). Not in 96 MB, which is
    // the Old 3DS's mode: a New model running the Old 3DS test build must have its speed too.
    if (!hostLowMemory()) {
        osSetSpeedupEnable(true);
    }

    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);

    // ##############################################################################################
    // # Bottom screen text of the showcase and public beta builds                                  #
    // #                                                                                            #
    // # These builds show a fixed text instead of the log (showcase.h); the log still goes to      #
    // # sdmc:/smg/log.txt, which the next start overwrites: hence the beta's request to send it,   #
    // # with any crash dump, first. The console is 40 x 30 characters; "\x1b[row;columnH" places a #
    // # line, 1-based, and each line is centred by its column.                                     #
    // ##############################################################################################
    if (SMG_PUBLIC_BETA) {
        printf("\x1b[12;6HGalaxyCTR - First Public Beta");    // 29 characters
        printf("\x1b[15;9HREPORT CRASH FILE + LOGS");         // 24 characters
        printf("\x1b[16;12HBEFORE RE-PLAYING!");              // 18 characters
        gfxFlushBuffers();
    } else if (SMG_SHOWCASE) {
        printf("\x1b[13;3HSuper Mario Galaxy - 3DS Native Port");
        printf("\x1b[15;2HTHIS IS UNFINISHED, WORK IN PROGRESS.");
        gfxFlushBuffers();
    }
    openLogFile();
    connectToPc();
    hostGfxInit();
    sCstick = R_SUCCEEDED(irrstInit());

    hostLog("Super Mario Galaxy - 3DS port\n");
    hostLog("CIA version: %s\n", SMG_CIA_VERSION);
    hostLog("Hold START+SELECT to quit.\n\n");
    if (!hostLowMemory()) {
        // Asked here as well, with its results: osSetSpeedupEnable alone left a CIA slow (0.16.10).
        Result init = ptmSysmInit();
        Result config = R_SUCCEEDED(init) ? PTMSYSM_ConfigureNew3DSCPU(3) : 0;
        if (R_SUCCEEDED(init)) {
            ptmSysmExit();
        }
        hostLog("New 3DS speedup: service 0x%08lX, configure 0x%08lX\n", (unsigned long)init, (unsigned long)config);
    }
    hostDebugInit();

    rvlInit();
    rvlLoadPortData();  // game data from main.dol, converted on the PC (platform/3ds/rvl/port_data.cpp)
    smgMain();

    hostFatal("smgMain returned");
}

// ##############################################################################################
// # Controls                                                                                   #
// #                                                                                            #
// # The 3DS's buttons, circle pad, C-Stick (New 3DS, through irrst) and touch screen, once per #
// # frame for rvl/kpad.cpp; debug.txt press= adds simulated buttons.                           #
// ##############################################################################################
void hostInputGet(HostInput* pInput) {
    circlePosition circle;
    touchPosition touch;
    hidCircleRead(&circle);
    hidTouchRead(&touch);

    pInput->held = hidKeysHeld() | hostDebugPressedKeys();
    pInput->stickX = circle.dx;
    pInput->stickY = circle.dy;
    pInput->cstickX = pInput->cstickY = 0;
    if (sCstick) {
        circlePosition cstick;
        irrstCstickRead(&cstick);
        pInput->cstickX = cstick.dx;
        pInput->cstickY = cstick.dy;
    }
    pInput->touching = (pInput->held & KEY_TOUCH) != 0;
    pInput->touchX = touch.px;
    pInput->touchY = touch.py;
}

// ##############################################################################################
// # Loading profile                                                                            #
// #                                                                                            #
// # Logged at the first scene frame (debug.c): where the seconds since the start went.         #
// ##############################################################################################
// Where the time before the first scene frame went: game file opens and reads, and log writes.
void hostLogLoadProfile(void) {
    unsigned long opens, reads;
    unsigned long long openTicks, readTicks, readBytes;
    hostFileStats(&opens, &openTicks, &reads, &readTicks, &readBytes);
    const unsigned long long msTicks = SYSCLOCK_ARM11 / 1000;
    const unsigned long long logTicks = sLogTicks;
    hostLog("load profile: %lu ms since start; files: %lu opens %lu ms, %lu reads %lu KB %lu ms; log: %lu lines %lu ms\n",
            (unsigned long)(osGetTime() - sStartTime), opens, (unsigned long)(openTicks / msTicks), reads,
            (unsigned long)(readBytes / 1024), (unsigned long)(readTicks / msTicks), sLogWrites, (unsigned long)(logTicks / msTicks));
}
