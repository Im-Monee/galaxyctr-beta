#include "Game/Screen/PictureBookCloseButton.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Screen/ButtonPaneController.hpp"
#include "Game/Util/GamePadUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#ifdef __3DS__
// 3DS: while this button shows, the 3DS B button is the remote's B, the back (Pad3DS.hpp).
#include "Pad3DS.hpp"
#endif

namespace NrvPictureBookCloseButton {
    NEW_NERVE(PictureBookCloseButtonNrvSelect, PictureBookCloseButton, Select);
    NEW_NERVE(PictureBookCloseButtonNrvSelected, PictureBookCloseButton, Selected);
    NEW_NERVE(PictureBookCloseButtonNrvNotSelected, PictureBookCloseButton, NotSelected);
};  // namespace NrvPictureBookCloseButton

PictureBookCloseButton::PictureBookCloseButton(bool canCloseWithPad)
    : LayoutActor("絵本閉じるボタン", true), mPaneCtrl(nullptr), mCanCloseWithPad(canCloseWithPad) {
}

void PictureBookCloseButton::init(const JMapInfoIter& rIter) {
    initLayoutManager("BackButton", 1);
    MR::connectToSceneLayoutDecoration(this);

    mPaneCtrl = new ButtonPaneController(this, "Back", "BoxButton", 0, true);

    initNerve(GET_NERVE(PictureBookCloseButton, PictureBookCloseButtonNrvSelect));
}

void PictureBookCloseButton::appear() {
    LayoutActor::appear();
    mPaneCtrl->appear();
    MR::requestMovementOn(this);
    setNerve(GET_NERVE(PictureBookCloseButton, PictureBookCloseButtonNrvSelect));
}

void PictureBookCloseButton::disappear() {
    mPaneCtrl->disappear();
    setNerve(GET_NERVE(PictureBookCloseButton, PictureBookCloseButtonNrvNotSelected));
}

bool PictureBookCloseButton::trySelect() {
    if (mPaneCtrl->trySelect()) {
        MR::startSystemSE("SE_SY_GALAXY_DECIDE_CANCEL");
        MR::startCSSound("CS_CLICK_CLOSE", nullptr, 0);
        setNerve(GET_NERVE(PictureBookCloseButton, PictureBookCloseButtonNrvSelected));

        return true;
    }

    if (!mPaneCtrl->isAppearing() && MR::testCorePadTriggerB(WPAD_CHAN0) && mCanCloseWithPad) {
        mPaneCtrl->disappear();
        MR::startSystemSE("SE_SY_GALAXY_DECIDE_CANCEL");
        MR::startCSSound("CS_CLICK_CLOSE", nullptr, 0);
        setNerve(GET_NERVE(PictureBookCloseButton, PictureBookCloseButtonNrvSelected));

        return true;
    }

    return false;
}

bool PictureBookCloseButton::isSelected() const {
    return isNerve(GET_NERVE(PictureBookCloseButton, PictureBookCloseButtonNrvSelected));
}

void PictureBookCloseButton::control() {
#ifdef __3DS__
    // 3DS: tells the controls that the close button shows (Pad3DS.hpp).
    if (!mPaneCtrl->isHidden()) {
        rvl3DSNoteBackButtonShown();
    }
#endif
    mPaneCtrl->update();

    if (mPaneCtrl->isPointingTrigger()) {
        MR::startSystemSE("SE_SY_BUTTON_CURSOR_ON");
    }
}

void PictureBookCloseButton::exeSelect() {
}

void PictureBookCloseButton::exeSelected() {
    if (mPaneCtrl->isHidden()) {
        kill();
    }
}

void PictureBookCloseButton::exeNotSelected() {
    if (mPaneCtrl->isHidden()) {
        kill();
    }
}
