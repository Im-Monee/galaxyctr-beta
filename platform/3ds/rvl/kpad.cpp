// ##############################################################################################
// # The Wii Remote and Nunchuk from the 3DS controls                                           #
// #                                                                                            #
// # The game reads a Wii Remote with a Nunchuk through KPAD and WPAD. Here those functions     #
// # answer from the 3DS's buttons, circle pad and touch screen (host/main.c, hostInputGet),    #
// # with the mapping below. The touch screen is the remote's pointer.                          #
// ##############################################################################################
// Wii Remote + Nunchuk (KPAD/WPAD) from the 3DS controls.
//
// Channel 0 is a Wii Remote with a Nunchuk attached; the other channels are empty.
//   circle pad -> Nunchuk stick        A, B -> A (jump); A + B together -> A + B (title screen)
//   Y -> shake (spin)
//   L / ZL -> Z (crouch; with A or B, long jump)           X -> 1      ZR -> C
//   R -> B (trigger: shoots Star Bits) while the pointer shows (touch screen or C-Stick cursor),
//        else Z (ground pound in the air). Star Bits shoot with R only: A + B is also the
//        remote's B (title screen, menus' back) but the shooting code asks for R (Pad3DS.hpp).
//   d-pad -> d-pad                     START -> +          SELECT -> -
//   touch screen -> pointer (the remote "points" while the screen is touched)
// A shake is a few samples of large alternating acceleration, which the game's swing
// detection (WPadHVSwing) sees as a flick of the remote.

#include "Pad3DS.hpp"
#include "host.h"
#include "rvl_internal.h"
#include <revolution.h>
#include <revolution/kpad.h>
#include <revolution/wpad.h>
#include <math.h>
#include <string.h>

static WPADConnectCallback sConnectCallback[WPAD_MAX_CONTROLLERS];
static WPADExtensionCallback sExtensionCallback[WPAD_MAX_CONTROLLERS];
static bool sAnnounced;
static u32 sLastHold;
static u32 sShakeFrames;
static bool sLastShakeKey;
static f32 sPointerX, sPointerY;
static bool sPointerShown;
static bool sStarBitKey;  // the 3DS R button at the last read (rvl3DSStarBitKeyHeld)
static f32 sStickX;       // the circle pad across at the last read (rvl3DSSurfRayAcceleration)

// Frames of a shake, and the remote's acceleration (in g) at rest: held level, pointing at
// the screen.
static const u32 kShakeLength = 6;
static const Vec kRestAcceleration = {0.0f, -1.0f, 0.0f};

// ##############################################################################################
// # Buttons and the stick                                                                      #
// #                                                                                            #
// # The 3DS buttons as Wii Remote and Nunchuk buttons (R depends on whether the pointer        #
// # shows), and the circle pad as the Nunchuk stick, with a dead zone.                         #
// ##############################################################################################
static u32 wiiButtons(u32 keys, bool pointing) {
    u32 buttons = 0;
    if (keys & (HOST_KEY_A | HOST_KEY_B)) buttons |= WPAD_BUTTON_A;
    // A and B together are the remote's A + B, which the title screen asks for.
    if ((keys & (HOST_KEY_A | HOST_KEY_B)) == (HOST_KEY_A | HOST_KEY_B)) buttons |= WPAD_BUTTON_B;
    if (keys & HOST_KEY_X) buttons |= WPAD_BUTTON_1;
    if (keys & (HOST_KEY_L | HOST_KEY_ZL)) buttons |= WPAD_BUTTON_Z;
    // With the C-Stick cursor R was Z too, so shooting crouched Mario instead (a tester's report).
    if (keys & HOST_KEY_R) buttons |= pointing ? WPAD_BUTTON_B : WPAD_BUTTON_Z;
    if (keys & HOST_KEY_ZR) buttons |= WPAD_BUTTON_C;
    if (keys & HOST_KEY_START) buttons |= WPAD_BUTTON_PLUS;
    if (keys & HOST_KEY_SELECT) buttons |= WPAD_BUTTON_MINUS;
    if (keys & HOST_KEY_DUP) buttons |= WPAD_BUTTON_UP;
    if (keys & HOST_KEY_DDOWN) buttons |= WPAD_BUTTON_DOWN;
    if (keys & HOST_KEY_DLEFT) buttons |= WPAD_BUTTON_LEFT;
    if (keys & HOST_KEY_DRIGHT) buttons |= WPAD_BUTTON_RIGHT;
    return buttons;
}

static f32 stickAxis(s32 value) {
    f32 axis = value / 150.0f;
    if (axis > 1.0f) {
        axis = 1.0f;
    } else if (axis < -1.0f) {
        axis = -1.0f;
    }
    // Small dead zone around the centre, like a Nunchuk at rest.
    return (axis > -0.1f && axis < 0.1f) ? 0.0f : axis;
}

extern "C" {

// ##############################################################################################
// # Remote settings and WPAD                                                                   #
// #                                                                                            #
// # Settings for the remote's sensors are accepted and do not apply. The remote is always      #
// # connected with its Nunchuk on channel 0; rumble and the remote's speaker do nothing.       #
// ##############################################################################################
void KPADInit(void) {
}

void KPADReset(void) {
    sLastHold = 0;
    sShakeFrames = 0;
    sLastShakeKey = false;
    sPointerShown = false;
    sPointerX = 0.0f;
    sPointerY = 0.0f;
}

// The emulated remote is driven by direct 3DS controls; Wii sensor smoothing parameters do
// not apply to the touch screen or circle pad, but validate and retain the supported channel.
static bool validChannel(s32 chan) {
    return chan >= 0 && chan < WPAD_MAX_CONTROLLERS;
}

void KPADSetBtnRepeat(s32 chan, f32 delay_sec, f32 pulse_sec) {
    (void)delay_sec;
    (void)pulse_sec;
    (void)validChannel(chan);
}

void KPADSetPosParam(s32 chan, f32 play_radius, f32 sensitivity) {
    (void)play_radius;
    (void)sensitivity;
    (void)validChannel(chan);
}

void KPADSetHoriParam(s32 chan, f32 play_radius, f32 sensitivity) {
    (void)play_radius;
    (void)sensitivity;
    (void)validChannel(chan);
}

void KPADSetDistParam(s32 chan, f32 play_radius, f32 sensitivity) {
    (void)play_radius;
    (void)sensitivity;
    (void)validChannel(chan);
}

void KPADSetAccParam(s32 chan, f32 play_radius, f32 sensitivity) {
    (void)play_radius;
    (void)sensitivity;
    (void)validChannel(chan);
}

void KPADSetSensorHeight(s32 chan, f32 level) {
    (void)level;
    (void)validChannel(chan);
}

void WPADRegisterAllocator(WPADAlloc alloc, WPADFree free) {
    // The virtual controller owns no dynamically allocated Wii Bluetooth work area.
    (void)alloc;
    (void)free;
}

u32 WPADGetWorkMemorySize(void) {
    // Input state is held in the port's static KPAD structures, not a Wii Bluetooth heap.
    return 0;
}

u8 WPADGetSensorBarPosition(void) {
    return WPAD_SENSOR_BAR_POS_TOP;
}

void WPADSetAutoSleepTime(u8 minutes) {
    (void)minutes;
}

void WPADDisconnect(s32 chan) {
    if (chan == 0) {
        // Re-arm the virtual connection notification after the game resets its pad manager.
        sAnnounced = false;
        sLastHold = 0;
    }
}

void WPADControlMotor(s32 chan, u32 command) {
    // The 3DS has no Wii Remote motor channel; the virtual controller has no rumble device.
    (void)chan;
    (void)command;
}

BOOL WPADIsSpeakerEnabled(s32 chan) {
    (void)chan;
    return FALSE;
}

s32 WPADControlSpeaker(s32 chan, u32 command, WPADCallback callback) {
    (void)command;
    if (callback != nullptr) {
        callback(chan, WPAD_ERR_TRANSFER);
    }
    return WPAD_ERR_TRANSFER;
}

u8 WPADGetSpeakerVolume(void) {
    return 0;
}

BOOL WPADCanSendStreamData(s32 chan) {
    (void)chan;
    return FALSE;
}

s32 WPADSendStreamData(s32 chan, void* data, u16 length) {
    (void)data;
    (void)length;
    return chan < 0 || chan >= WPAD_MAX_CONTROLLERS ? WPAD_ERR_INVALID : WPAD_ERR_TRANSFER;
}

WPADConnectCallback WPADSetConnectCallback(s32 chan, WPADConnectCallback callback) {
    WPADConnectCallback previous = nullptr;
    if (chan >= 0 && chan < WPAD_MAX_CONTROLLERS) {
        previous = sConnectCallback[chan];
        sConnectCallback[chan] = callback;
    }
    return previous;
}

WPADExtensionCallback WPADSetExtensionCallback(s32 chan, WPADExtensionCallback callback) {
    WPADExtensionCallback previous = nullptr;
    if (chan >= 0 && chan < WPAD_MAX_CONTROLLERS) {
        previous = sExtensionCallback[chan];
        sExtensionCallback[chan] = callback;
    }
    return previous;
}

s32 WPADProbe(s32 chan, u32* type) {
    if (chan != 0) {
        return WPAD_ERR_NO_CONTROLLER;
    }
    if (type != nullptr) {
        *type = WPAD_DEV_FREESTYLE;
    }
    return WPAD_ERR_NONE;
}

s32 WPADGetInfoAsync(s32 chan, WPADInfo* info, WPADCallback callback) {
    if (chan != 0) {
        return WPAD_ERR_NO_CONTROLLER;
    }
    memset(info, 0, sizeof(*info));
    info->dpd = TRUE;
    info->attach = TRUE;
    info->battery = 4;
    info->led = 1;
    if (callback != nullptr) {
        callback(chan, WPAD_ERR_NONE);
    }
    return WPAD_ERR_NONE;
}

// ##############################################################################################
// # Reading the remote                                                                         #
// #                                                                                            #
// # One sample per call: buttons with their pressed and released edges, the stick, the pointer #
// # from the touch screen, and the acceleration of a shake while Y is held.                    #
// ##############################################################################################
s32 KPADRead(s32 chan, KPADStatus* samples, u32 length) {
    // The connection is reported on the first read, once the game's pad objects exist.
    if (!sAnnounced) {
        sAnnounced = true;
        if (sConnectCallback[0] != nullptr) {
            sConnectCallback[0](0, WPAD_ERR_NONE);
        }
        if (sExtensionCallback[0] != nullptr) {
            sExtensionCallback[0](0, WPAD_DEV_FREESTYLE);
        }
    }

    if (chan != 0 || length == 0) {
        if (length != 0) {
            memset(samples, 0, sizeof(KPADStatus));
            samples->wpad_err = WPAD_ERR_NO_CONTROLLER;
        }
        return 0;
    }

    HostInput input;
    hostInputGet(&input);

    KPADStatus* s = &samples[0];
    memset(s, 0, sizeof(*s));

    // The pointer shows once the touch screen or the C-Stick has placed it (below), and stays.
    u32 hold = wiiButtons(input.held, input.touching || sPointerShown);
    sStarBitKey = (input.held & HOST_KEY_R) != 0;
    s->hold = hold;
    s->trig = hold & ~sLastHold;
    s->release = sLastHold & ~hold;
    sLastHold = hold;

    // Shake: starts when Y is pressed.
    bool shakeKey = (input.held & HOST_KEY_Y) != 0;
    if (shakeKey && !sLastShakeKey) {
        sShakeFrames = kShakeLength;
    }
    sLastShakeKey = shakeKey;

    s->acc = kRestAcceleration;
    if (sShakeFrames > 0) {
        f32 swing = (sShakeFrames & 1) ? 2.5f : -2.5f;
        s->acc.x = swing;
        s->acc.z = swing;
        sShakeFrames--;
    }
    s->acc_value = 1.0f;

    // Pointer (-1..1 over the screen, y down): the C-Stick moves a cursor that stays where it
    // is left (owner's request); touching the touch screen places it directly.
    static const s32 kCstickDeadZone = 20;
    static const f32 kCstickSpeed = 1.0f / (155.0f * 40.0f);  // full tilt: half the screen in ~0.7 s
    if (input.touching) {
        sPointerX = input.touchX / 160.0f - 1.0f;
        sPointerY = input.touchY / 120.0f - 1.0f;
        sPointerShown = true;
    } else if (input.cstickX * input.cstickX + input.cstickY * input.cstickY > kCstickDeadZone * kCstickDeadZone) {
        sPointerX += input.cstickX * kCstickSpeed;
        sPointerY -= input.cstickY * kCstickSpeed;
        sPointerX = sPointerX < -1.0f ? -1.0f : sPointerX > 1.0f ? 1.0f : sPointerX;
        sPointerY = sPointerY < -1.0f ? -1.0f : sPointerY > 1.0f ? 1.0f : sPointerY;
        sPointerShown = true;
    }
    if (sPointerShown) {
        s->pos.x = sPointerX;
        s->pos.y = sPointerY;
        s->dpd_valid_fg = 2;
    } else {
        s->dpd_valid_fg = 0;
    }
    s->horizon.x = 1.0f;
    s->dist = 1.0f;

    s->dev_type = WPAD_DEV_FREESTYLE;
    s->wpad_err = WPAD_ERR_NONE;
    s->data_format = WPAD_FMT_FREESTYLE_ACC_DPD;

    s->ex_status.fs.stick.x = stickAxis(input.stickX);
    sStickX = s->ex_status.fs.stick.x;
    s->ex_status.fs.stick.y = stickAxis(input.stickY);
    s->ex_status.fs.acc = kRestAcceleration;
    s->ex_status.fs.acc_value = 1.0f;
    return 1;
}

}  // extern "C"

bool rvl3DSStarBitKeyHeld() {
    return sStarBitKey;
}

// The remote upright, tilted by the circle pad (Pad3DS.hpp).
void rvl3DSSurfRayAcceleration(float* x, float* y, float* z) {
    const f32 angle = sStickX * 1.5707964f;
    *x = sinf(angle);
    *y = 0.0f;
    *z = -cosf(angle);
}
