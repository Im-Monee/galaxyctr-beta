#include "Game/Camera/CameraCover.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Screen/ImageEffectLocalUtil.hpp"
#include "Game/Util.hpp"
#include <JSystem/JUtility/JUTTexture.hpp>

void CameraCover_FORCE_MATCH_SDATA2() {
    (void)JGeometry::TUtil< f32 >::PI();
    (void)60.0f;
    (void)180.0f;
}

namespace {
    const f32 sAngleThreshold = (JGeometry::TUtil< f32 >::PI() * 60.0f) / 180.0f;
};  // namespace

CameraCover::CameraCover(const char* pName) : NameObj(pName) {
    _3C = 0;
    _40 = 0;
    _41 = false;
    _44 = 1;
    mActor = new CaptureScreenActor(MR::DrawType_CaptureScreenCamera, "Camera");
    MR::connectToScene(this, MR::MovementType_CameraCover, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_CameraCover);
    MR::joinToNameObjGroup(this, "IgnorePauseNameObj");
    _C.identity();
    mActor->initWithoutIter();
}

void CameraCover::movement() {
    if (isCameraHopping()) {
        if (!_3C) {
            // ##############################################################################################
            // # No still picture over a camera cut on the 3DS                                              #
            // #                                                                                            #
            // # The Wii copies the screen here, outside the draw, while it still holds the last frame,     #
            // # and draw() shows that copy for a few frames to hide the cut. On the 3DS the copy opens     #
            // # the new frame first (platform/3ds/host/gfx.c, beginFrame), which clears the screen: the    #
            // # copy was empty, and while the "Camera" timing held, the water that shows the screen behind #
            // # it (OceanRing, Loopdeeloop) went dark. So no copy is made, draw() shows nothing and the    #
            // # "Indirect" copy of each frame goes on.                                                     #
            // ##############################################################################################
#ifndef __3DS__
            MR::startToCaptureScreen("Camera");
            CategoryList::execute((MR::DrawType)0x2E);
#endif
            _3C = _44 + 1;
        }
    }

    if (_3C && !--_3C) {
        MR::endToCaptureScreen("Camera");
    }

    copyCamera();
    _40 = 0;
    _41 = false;
}

void CameraCover::draw() const {
    // 3DS: no copy of the screen to show over a camera cut (movement, above).
#ifdef __3DS__
    return;
#endif
    if (_3C) {
        JUTTexture texture(MR::getScreenResTIMG(), 0);

        ImageEffectLocalUtil::setupDrawTexture();
        ImageEffectLocalUtil::drawTexture(&texture, 1, 0, 255, (ImageEffectLocalUtil::ETexDrawType)0);
    }
}

void CameraCover::cover(u32 a1) {
    _44 = a1;
    _41 = true;
}

bool CameraCover::isCameraHopping() const {
    if (_40) {
        return false;
    }

    return _41;
}

void CameraCover::copyCamera() {
    _C.set(MR::getCameraInvViewMtx());
}

CameraCover::~CameraCover() {
}
