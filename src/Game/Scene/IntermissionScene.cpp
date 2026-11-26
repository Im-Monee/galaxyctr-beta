#include "Game/Scene/IntermissionScene.hpp"
#include "Game/Util/ActorSensorUtil.hpp"
#include "Game/Util/Color.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include <JSystem/JUtility/JUTVideo.hpp>
#include <cstdio>
#include <va_list.h>
// 3DS: host.h, the render target's clear (IntermissionScene::draw).
#ifdef __3DS__
#include "host.h"
#endif

IntermissionScene::IntermissionScene() : Scene("IntermissionScene") {
    _54 = 0;
    mState[0] = 0;
}

void IntermissionScene::update() {
    _54++;
}

void IntermissionScene::draw() const {
    // ##############################################################################################
    // # The black screen between scenes                                                            #
    // #                                                                                            #
    // # While a scene loads the Wii shows this black box over the whole screen. On the 3DS the     #
    // # box did not show and the clear colour (30, 30, 200) stayed: blue while loading, before   #
    // # the title and behind the lives count (owner, CIA 0.21.9). The intermission scene is the    #
    // # first thing drawn in a frame (GameSystem::draw): the render target is cleared to black,    #
    // # the same image as the opaque full-screen box. The frames drawn here are counted in the     #
    // # log, to tell this case from frames reaching the screen without this draw.                  #
    // ##############################################################################################
#ifdef __3DS__
    static unsigned long sDrawnFrames;
    if (!hostGfxFrameDropped()) {
        hostGfxClearFrame(0x000000FF);
        if (++sDrawnFrames <= 4 || sDrawnFrames % 120 == 0) {
            hostLog("intermission: black frame %lu drawn\n", sDrawnFrames);
        }
    }
#else
    J2DOrthoGraphSimple graph;
    graph.setPort();
    graph.setColor(static_cast< const GXColor& >(GXColor(Color8(0, 0, 0, 255))));

    f32 height = static_cast< s32 >(JUTVideo::getManager()->getRenderMode()->efbHeight);
    f32 width = MR::getScreenWidth();
    graph.fillBox(0.0f, 0.0f, width, height);
#endif
}

void IntermissionScene::setCurrentSceneControllerState(const char* pState, ...) {
    va_list list;
    va_start(list, pState);
    vsnprintf(mState, sizeof(mState), pState, list);
    va_end(list);
    _54 = 0;
}
