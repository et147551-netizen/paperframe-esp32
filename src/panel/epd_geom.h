// The panel's geometry, and the only thing src/core/ is allowed to know about a panel.
//
// WHY THIS FILE EXISTS SEPARATELY FROM epd_panel.h. The core layer is defined as the
// ESP-IDF-free half -- it is what env:native compiles and what the 324 host tests cover --
// and epd_panel.h declares esp_err_t returns, so core cannot include it. But core does
// need the geometry: epd_format.c sizes a frame with it, and app_smb_sync.h derives its
// resize edge from it. So the geometry gets its own header with no ESP-IDF in it, and that
// header is the one dependency core has on panel/.
//
// The dimensions stay COMPILE-TIME CONSTANTS. One binary drives one board, the board is
// chosen by a build flag, and every buffer sized from these is sized once at startup --
// so making them runtime accessors would be a large change that buys nothing this project
// needs. The seven files that use EPD_WIDTH / EPD_HEIGHT / EPD_FRAME_BYTES were not
// touched when this header was introduced, which is the point.
//
// Ticket .scratch/digital-frame/issues/43 seam 1: these used to be defined twice, here and
// in epd_image.c, with nothing relating them, so a port could change one and still compile.
// That was fixed on 2026-09-11 and there is now exactly one definition per panel. Keep it
// that way: `grep -rn 'define EPD_WIDTH' src/` should return one line per panel directory and
// nothing else. Use `grep -r`, not `git grep` -- git grep skips untracked files, so a freshly
// added third panel's header is exactly what it would not show you.

#ifndef EPD_GEOM_H
#define EPD_GEOM_H

#if defined(BOARD_M5PAPER_COLOR)
#include "epd_geom_el040ef1.h"
#elif defined(BOARD_RETERMINAL_E1002)
#include "epd_geom_ed2208gca.h"
#else
#error "No board selected: define BOARD_M5PAPER_COLOR or BOARD_RETERMINAL_E1002"
#endif

// Two pixels per byte, high nibble first, on every panel here -- it is the controller
// family's frame format rather than a panel's, so it is derived once and not per panel.
#define EPD_FRAME_BYTES ((EPD_WIDTH * EPD_HEIGHT) / 2)

#endif // EPD_GEOM_H
