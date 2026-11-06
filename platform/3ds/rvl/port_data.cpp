// ############################################################################
// # Game data the Wii keeps inside main.dol, read from the SD card            #
// #                                                                          #
// # The Wii executable carries three data files in its .rodata:              #
// #   cStoryProgressTable  (StoryProgressTable.bcsv, the story's steps)       #
// #   cGalaxyInfoTable     (GalaxyInfoTable.bcsv, the galaxies' list)          #
// #   cErrorMessageArchive (ErrorMessageArchive.arc, disc/save error texts)   #
// # The Wii build gets them from build/RMGE01/include/assets/*.hpp, byte     #
// # arrays extracted from the user's main.dol.                               #
// #                                                                          #
// # The 3DS build must not contain any Nintendo data, so that its CIA can be #
// # shared. Instead, the PC converter extracts these three files from the    #
// # user's own main.dol, converts them like every other game file, and       #
// # writes them to sdmc:/smg/port/ (tools/3ds/convert_smg_files.py, block    #
// # "Game data kept inside main.dol"). Here they are read into arrays of the #
// # same names before the game starts, so the game code that uses them       #
// # (GalaxyMap.cpp, GameDataHolder.cpp, GameSystemObjHolder.cpp and others)  #
// # is unchanged. The decomp's three .cpp files that include the Wii arrays  #
// # skip that include under __3DS__ (src/Game/System/ErrorMessageArchive.cpp,#
// # GalaxyInfoTable.cpp, StoryProgressTable.cpp).                            #
// ############################################################################

#include "host.h"
#include <revolution/types.h>
#include <stdio.h>

// The game's headers declare these as "extern const u8 name[]" (for example
// include/Game/System/GalaxyInfoTable.hpp). That header is not included here on
// purpose: these definitions are writable, because they are filled at startup.
// The names link to the same symbols (a C++ variable's symbol is its name alone).
//
// Sizes are the largest each file can be after conversion, with room to spare:
// RMGE01's files are 0x1E0, 0xD20 and 0xA4A0 bytes. Alignments match the Wii's
// (32 for the archive, which JKRMemArchive reads in place; 4 for the tables).
// The archive must be writable: JKRMemArchive stores pointers in its file entries.
__attribute__((aligned(4))) u8 cStoryProgressTable[0x400];
__attribute__((aligned(4))) u8 cGalaxyInfoTable[0x1400];
__attribute__((aligned(32))) u8 cErrorMessageArchive[0xC000];

// ############################################################################
// # Loading at startup                                                       #
// ############################################################################

namespace {
    // One file of sdmc:/smg/port/ into its array. A missing or too large file
    // stops the game with a message saying what to do: without these tables the
    // game cannot build its galaxy list, so there is nothing sensible to run.
    void loadPortFile(const char* pName, u8* pDst, unsigned long capacity) {
        char path[96];
        snprintf(path, sizeof(path), "sdmc:/smg/port/%s", pName);
        void* file = hostFileOpenMode(path, 1);  // 1 = read (host.h)
        if (file == nullptr) {
            hostFatal("%s is missing.\nRun the converter again on your game files: it writes this file "
                      "from your copy of the game.", path);
        }
        long size = hostFileSizeMode(file);
        if (size <= 0 || static_cast< unsigned long >(size) > capacity) {
            hostFatal("%s has an unexpected size (%ld bytes).\nRun the converter again on your game files.", path, size);
        }
        long read = hostFileReadNext(file, pDst, static_cast< unsigned long >(size));
        hostFileCloseMode(file);
        if (read != size) {
            hostFatal("%s could not be read.", path);
        }
    }
}  // namespace

// Called by the host's main() right after rvlInit, before the game starts (host.h).
extern "C" void rvlLoadPortData(void) {
    loadPortFile("StoryProgressTable.bcsv", cStoryProgressTable, sizeof(cStoryProgressTable));
    loadPortFile("GalaxyInfoTable.bcsv", cGalaxyInfoTable, sizeof(cGalaxyInfoTable));
    loadPortFile("ErrorMessageArchive.arc", cErrorMessageArchive, sizeof(cErrorMessageArchive));
}
