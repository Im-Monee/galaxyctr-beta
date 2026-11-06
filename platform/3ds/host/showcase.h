#pragma once

// Showcase builds (tools/3ds/build_cia_showcase.ps1 rewrites these lines for each build; the
// committed values are the normal development build).
//   SMG_SHOWCASE     1: the bottom screen shows only the port's title text instead of the log
//                    (the log still goes to sdmc:/smg/log.txt; a fatal error is still shown)
//   SMG_START_STAGE  "EggStarGalaxy:1": start that stage and scenario without a debug.txt
//   SMG_START_DEMO   "grandstar1": with SMG_START_STAGE "AstroGalaxy:4", the story's demos after the
//                    first Grand Star (debug.txt demo=, hostDebugStartDemo)
//   SMG_PUBLIC_BETA  1: the public beta's program (tools/beta/build_builder.py sets it with
//                    SMG_SHOWCASE 1): the title screen, the beta's bottom screen text (main.c)
#define SMG_SHOWCASE 0
#define SMG_PUBLIC_BETA 0
#define SMG_SHOWCASE_NATIVE 0
#define SMG_SHOWCASE_PROFILE 0
#define SMG_DIAG_CLEAR_ONLY 0
#define SMG_DIAG_FIRST_DRAW_ONLY 0
#define SMG_START_STAGE ""
#define SMG_START_DEMO ""
