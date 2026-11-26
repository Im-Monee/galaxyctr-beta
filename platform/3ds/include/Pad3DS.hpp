#pragma once

// ##############################################################################################
// # The 3DS buttons behind the remote's                                                        #
// #                                                                                            #
// # platform/3ds/rvl/kpad.cpp turns the 3DS controls into a Wii Remote. A few game checks need #
// # to know which 3DS button pressed a remote button: Star Bits shoot with R only (the owner's #
// # choice), while A + B, which is the remote's B for the title screen and the menus' back,    #
// # must not shoot them (StarPieceDirector.cpp, TicoFat.cpp).                                  #
// ##############################################################################################

// Whether the 3DS R button was held at the last remote read.
bool rvl3DSStarBitKeyHeld();

// ##############################################################################################
// # The remote's tilt on the manta from the circle pad                                         #
// #                                                                                            #
// # Riding the manta (SurfRay.cpp, SurfRayTutorial.cpp) the Wii Remote is held upright and     #
// # tilted left or right to steer. The 3DS has no remote to tilt (kpad.cpp reports one at      #
// # rest), so there the circle pad's left and right stand for it: fully over is a remote       #
// # tilted 90 degrees, the acceleration in g as a Wii Remote reads it (x = sin, z = -cos).     #
// ##############################################################################################
void rvl3DSSurfRayAcceleration(float* x, float* y, float* z);

// ##############################################################################################
// # B as the menus' back                                                                       #
// #                                                                                            #
// # On the Wii the "Back" buttons of the menus (BackButton: scenario select, galaxy confirm,   #
// # the map; the album's close button, PictureBookCloseButton) also answer the remote's B.     #
// # On the 3DS, B is the remote's A (jump) everywhere else; while one of those buttons shows,   #
// # kpad.cpp makes it the remote's B instead (the owner's request). The buttons report every   #
// # update in which they show.                                                                 #
// ##############################################################################################
void rvl3DSNoteBackButtonShown();
