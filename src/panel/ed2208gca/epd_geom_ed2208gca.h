// ED2208-GCA geometry -- the reTerminal E1002's 7.3" Spectra 6 panel.
//
// READ, NOT MEASURED. 800x480 comes from the TRES payload `03 20 01 E0` in
// refs/esp32-photoframe/components/epaper_driver_ed2208_gca/src/driver_ed2208_gca.c and
// from the Seeed wiki's specification. No panel of this kind has been on this bench, so
// nothing derived from these numbers may be quoted as a measurement -- see ticket 63.
//
// Reached through epd_geom.h, never included directly.

#ifndef EPD_GEOM_ED2208GCA_H
#define EPD_GEOM_ED2208GCA_H

// 5:3, LANDSCAPE-native, against the EL040EF1's portrait 2:3. This is ticket 43's seam 3
// and the one thing about this port that does not follow from recompiling: the matte
// versus crop decision in epd_fit_centre() flips for the common case (a phone photograph
// is portrait -- on 2:3 it nearly fills, on 5:3 it is a tall sliver between two wide
// mattes), the default `rotation` means something different, and the Web UI's
// fitImageToContainer() parity condition has to be re-argued. Ticket 62 owns all of that.
// This header only makes the assumption visible; it does not resolve it.
#define EPD_WIDTH 800
#define EPD_HEIGHT 480

// 1.6x the pixels of the EL040EF1, so 192,000 frame bytes against 120,000 and a 1.152 MB
// canvas against 720 kB. Comfortable in 8 MB of PSRAM -- and the per-pixel stages were
// measured area-proportional on 2026-09-06, so the auto flow's 1,917 ms and the
// diffusion's 603 ms PROJECT to roughly 3.1 s and 1.0 s. A projection, not a figure.

#endif // EPD_GEOM_ED2208GCA_H
