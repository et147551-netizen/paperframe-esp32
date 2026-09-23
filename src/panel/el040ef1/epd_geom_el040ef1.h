// EL040EF1 geometry -- the M5Paper Color's 4.0" Spectra 6 panel.
//
// MEASURED, not read: every figure in docs/agents/measurements.md was taken on this panel
// at this size. Reached through epd_geom.h, never included directly.

#ifndef EPD_GEOM_EL040EF1_H
#define EPD_GEOM_EL040EF1_H

// 2:3, portrait-native. docs/agents/measurements.md's whole geometry table -- which source
// aspect ratio produces a matte, which a contiguous region, which the strided form -- is a
// function of this ratio and of nothing else. Ticket 62 is where that gets re-derived for
// the 5:3 panel; do not read a row of that table as if it were panel-independent.
#define EPD_WIDTH 400
#define EPD_HEIGHT 600

#endif // EPD_GEOM_EL040EF1_H
