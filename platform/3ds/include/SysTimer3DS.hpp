#pragma once

// ##############################################################################################
// # Timing parts of the update                                                                 #
// #                                                                                            #
// # A scoped timer for the profile log (debug.txt profile=1): the update's parts, by number,   #
// # listed below.                                                                              #
// ##############################################################################################
// Times a part of the update for the 3DS profile log as kind "sys" (platform/3ds/gx/native_submit.cpp).
extern "C" void rvlGXCategoryMark(int kind, int type, unsigned long long* pMark);
extern "C" void rvlGXCategoryEnd(int kind, int type, const unsigned long long* pMark);

// Types used: 30 Mario::update, 31 checkAllWall, 32 updateGroundInfo, 33 updateBehavior, 34 controlMain,
// 40 strike ball, 41 strike line, 42 strike point, 43 gravity, 44 SensorHitChecker::movement, 45 doObjCol,
// 46 updateRhythm, 47 WPadHolder, 48 FunctionAsyncExecutor, 49 StarPointerDirector; MarioActor's parts:
// 50 life and player mode, 51 bind ratio and effects, 52 punching, 53 gravity vector, 54 swing, throw
// vector and pointer lock-on, 55 updateForCamera, 56 tornado and coin pull, 57 action trigger,
// controller swings and 2D, 58 rotation, taking, search light, throwing, bee wings, fairy star.
// Consecutive phases of one function, each timed as its own type (next ends the one before; the
// destructor ends the last, whatever return leaves the function). Mario::update: 59 binder, base
// and head points, at-field, ceiling; 60 walls, ground, floor and wall damage; 61 press damage,
// slider, forced jump, swimming, 2D and first-person checks, stick and lock-on; 62 actionMain;
// 63 front floor, slopes, pushes, areas, rails, steps, bumps, cube warp, spin punch, timers.
struct SysPhase3DS {
    SysPhase3DS() : mType(-1) {}
    ~SysPhase3DS() {
        if (mType >= 0) {
            rvlGXCategoryEnd(6, mType, mMark);
        }
    }
    void next(int type) {
        if (mType >= 0) {
            rvlGXCategoryEnd(6, mType, mMark);
        }
        mType = type;
        rvlGXCategoryMark(6, type, mMark);
    }
    int mType;
    unsigned long long mMark[2];
};

struct SysTimer3DS {
    SysTimer3DS(int type) : mType(type) { rvlGXCategoryMark(6, type, mMark); }
    ~SysTimer3DS() { rvlGXCategoryEnd(6, mType, mMark); }
    int mType;
    unsigned long long mMark[2];
};
