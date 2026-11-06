#pragma once

// Build switches. Showcase builds (the developer's CIA script and tools/beta/build_builder.py rewrite these lines
// for each build; the committed values are the normal development build).
//   SMG_SHOWCASE     1: the bottom screen shows only the port's title text instead of the log
//                    (the log still goes to sdmc:/smg/log.txt; a fatal error is still shown)
//   SMG_START_STAGE  "EggStarGalaxy:1": start that stage and scenario without a debug.txt
//   SMG_START_DEMO   "grandstar1": with SMG_START_STAGE "AstroGalaxy:4", the story's demos after the
//                    first Grand Star (debug.txt demo=, hostDebugStartDemo)
//   SMG_PUBLIC_BETA  1: the public beta's program (tools/beta/build_builder.py sets it with
//                    SMG_SHOWCASE 1): the title screen, the beta's bottom screen text (main.c)
//   SMG_SHOWCASE_PROFILE  1: the profile timers on from the start (debug.txt profile=1), for
//                    the timing lines of the log
//   SMG_SHOWCASE_NATIVE   no longer read: the native renderer is always on; the build scripts
//                    still set it
//   SMG_DIAG_CLEAR_ONLY, SMG_DIAG_FIRST_DRAW_ONLY  diagnostics of the first GPU hang (CIA 0.10.x,
//                    gfx.c): the first frame only cleared (magenta with both), then only one
//                    draw sent and its buffers logged
#define SMG_SHOWCASE 0
#define SMG_PUBLIC_BETA 0
#define SMG_SHOWCASE_NATIVE 0
#define SMG_SHOWCASE_PROFILE 0
#define SMG_DIAG_CLEAR_ONLY 0
#define SMG_DIAG_FIRST_DRAW_ONLY 0
#define SMG_START_STAGE ""
#define SMG_START_DEMO ""
