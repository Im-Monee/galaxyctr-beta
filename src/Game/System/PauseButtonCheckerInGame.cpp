#include "Game/System/PauseButtonCheckerInGame.hpp"
#include "Game/Util/GamePadUtil.hpp"
// 3DS: host.h, the port's host interface.
#ifdef __3DS__
#include "host.h"
#endif

namespace {
    static const s32 sHoldCounterMax = 12;
};  // namespace

PauseButtonCheckerInGame::PauseButtonCheckerInGame() : mPlusHoldFrame(0), mMinusHoldFrame(0) {
}

#ifdef __3DS__
namespace {
    // The retrace of the last update (hostVBlankCount).
    unsigned long sLastVBlank;

    // The hold counted in 60 Hz retraces, not in updates: the console updates the game at 15-30
    // fps, and 12 updates were up to a second of holding START before the pause menu opened. A
    // count crossing the Wii's 12 stops on it, which isPermitTo*Pause checks for equality.
    s32 holdFrames(s32 frames, s32 elapsed) {
        if (frames < ::sHoldCounterMax && frames + elapsed >= ::sHoldCounterMax) {
            return ::sHoldCounterMax;
        }
        return frames + elapsed;
    }
};  // namespace
#endif

void PauseButtonCheckerInGame::update() {
#ifdef __3DS__
    const unsigned long vblank = hostVBlankCount();
    s32 elapsed = static_cast< s32 >(vblank - sLastVBlank);
    sLastVBlank = vblank;
    elapsed = elapsed < 1 ? 1 : elapsed > 8 ? 8 : elapsed;  // a game update after a long stop counts as a slow one

    mPlusHoldFrame = MR::testCorePadButtonPlus(WPAD_CHAN0) ? holdFrames(mPlusHoldFrame, elapsed) : 0;
    mMinusHoldFrame = MR::testCorePadButtonMinus(WPAD_CHAN0) ? holdFrames(mMinusHoldFrame, elapsed) : 0;
    return;
#endif
    if (MR::testCorePadButtonPlus(WPAD_CHAN0)) {
        mPlusHoldFrame++;
    } else {
        mPlusHoldFrame = 0;
    }

    if (MR::testCorePadButtonMinus(WPAD_CHAN0)) {
        mMinusHoldFrame++;
    } else {
        mMinusHoldFrame = 0;
    }
}

bool PauseButtonCheckerInGame::isPermitToPlusPause() const {
    return isPermitToPause() && mPlusHoldFrame == ::sHoldCounterMax;
}

bool PauseButtonCheckerInGame::isPermitToMinusPause() const {
    return isPermitToPause() && mMinusHoldFrame == ::sHoldCounterMax;
}

bool PauseButtonCheckerInGame::isPermitToPause() const {
    return !MR::isOperatingWPad(WPAD_CHAN0);
}
