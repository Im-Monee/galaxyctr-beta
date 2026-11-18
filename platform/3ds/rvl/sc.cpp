// ##############################################################################################
// # Wii system settings (SC)                                                                   #
// #                                                                                            #
// # Answered from the 3DS settings where they make sense, with fixed answers for what the 3DS  #
// # does not have (progressive scan, PAL 60 Hz).                                               #
// ##############################################################################################
#include "host.h"
#include <revolution/sc.h>

extern "C" {

// The USA version ships English, French and Spanish text; other languages fall back to English.
u8 SCGetLanguage(void) {
    switch (hostGetSystemLanguage()) {
    case HOST_LANGUAGE_FRENCH:
        return SC_LANG_FRENCH;
    case HOST_LANGUAGE_SPANISH:
        return SC_LANG_SPANISH;
    default:
        return SC_LANG_ENGLISH;
    }
}

// The top screen (400x240) is exactly 5:3, between the original game's 4:3 and 16:9 layouts.
// The wide path is kept, and its aspect constants are the 5:3 values (see the
// MR::isScreen16Per9 call sites).
u8 SCGetAspectRatio(void) {
    return SC_ASPECT_RATIO_16x9;
}

u8 SCGetSoundMode(void) {
    return SC_SOUND_MODE_STEREO;
}

u8 SCGetProgressiveMode(void) {
    return 0;
}

u8 SCGetEuRgb60Mode(void) {
    return 0;
}

}  // extern "C"
