#include "Game/Screen/BackButton.hpp"
#include "Game/Screen/ButtonPaneController.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#ifdef __3DS__
// 3DS: while this button shows, the 3DS B button is the remote's B, the back (Pad3DS.hpp).
#include "Pad3DS.hpp"
#endif

BackButton::BackButton(const char* pName, bool param2) : LayoutActor(pName, true), mPaneCtrl(), _24(), _25(true) {
    if (param2) {
        MR::connectToSceneLayoutDecoration(this);
    }
}

void BackButton::appear() {
    LayoutActor::appear();
    mPaneCtrl->appear();

    _24 = false;
}

void BackButton::disappear() {
    mPaneCtrl->disappear();
}

bool BackButton::isHidden() const {
    return mPaneCtrl->isHidden();
}

bool BackButton::isAppearing() const {
    return mPaneCtrl->isAppearing();
}

bool BackButton::isDisappearing() const {
    return mPaneCtrl->isDisappearing();
}

bool BackButton::isPointing() const {
    return mPaneCtrl->isPointing();
}

BackButton::~BackButton() {
}

void BackButton::init(const JMapInfoIter& rIter) {
    initLayoutManager("BackButton", 1);
    MR::createAndAddPaneCtrl(this, "Back", 1);

    mPaneCtrl = new ButtonPaneController(this, "Back", "BoxButton", 0, true);
}

void BackButton::control() {
#ifdef __3DS__
    // 3DS: tells the controls that a "Back" button shows (Pad3DS.hpp).
    if (!mPaneCtrl->isHidden()) {
        rvl3DSNoteBackButtonShown();
    }
#endif
    if (mPaneCtrl->isPointingTrigger()) {
        MR::startSystemSE("SE_SY_BUTTON_CURSOR_ON");
    }

    if (_25 && mPaneCtrl->trySelect()) {
        if (!_24) {
            MR::startSystemSE("SE_SY_GALAXY_DECIDE_CANCEL");
        }

        _24 = true;
    }

    mPaneCtrl->update();
}
