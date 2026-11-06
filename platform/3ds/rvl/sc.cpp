// Wii system settings, answered from the 3DS settings where they make sense.

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

// The top screen (400x240) is wide, so the game uses its 16:9 layouts.
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
