// The panel interface: what the application layer is allowed to know about the E Ink
// controller in front of it.
//
// It is deliberately tiny, because that is what the code turned out to need. app_display.c
// -- the whole render path, 902 lines -- calls exactly ONE function from the driver,
// epd_refresh_frame(). Everything else it uses (epd_canvas_*, epd_dither_*, epd_adjust_*,
// epd_auto_*, epd_classify_*, epd_image_*) is portable computation in src/core/ that has
// never known what panel it is feeding. So this header does not invent an abstraction; it
// writes down one that was already there.
//
// WHAT IS NOT HERE, and why:
//
//   * pins and the SPI clock. EPD_PIN_BUSY/RST/DC/CS and the panel's clock rate are the
//     BOARD's wiring, not the controller's -- the same ED2208-family part sits on
//     different GPIOs on the two boards. They live in the board pin header, reached
//     through board.h.
//   * the sequencing switch, the BUSY budgets and the probe hooks. EPD_SEQ_STOCK /
//     EPD_SEQ_BUSY exist because ticket 09 MEASURED the difference on the EL040EF1 (597 ms
//     per refresh, twice, an hour apart) and EPD_SEQ_STOCK is what every figure in
//     docs/measurements.md is compared against. They are that panel's, so they stay
//     in its own header and only its own measurement entry points reach for them.
//   * any timing. There is no refresh duration, FRS value or colour claim in this file.
//     Those are per panel and per measurement session.
//
// EPD_FRS_STOCK is in the interface but its VALUE is not: each panel header defines its own
// shipping waveform-rate byte (0x08 on the EL040EF1, 0x03 on the ED2208-GCA). That is what
// lets app_display.c pass EPD_FRS_STOCK unchanged on either board.

#ifndef EPD_PANEL_H
#define EPD_PANEL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "epd_cmds.h"   // command numbers and the six palette indices -- shared
#include "epd_format.h" // epd_timings_t, and the frame packing, from src/core/
#include "epd_geom.h"   // EPD_WIDTH / EPD_HEIGHT / EPD_FRAME_BYTES, per panel

// The panel-private half: sequencing, budgets, EPD_FRS_STOCK, and any probe hooks the
// panel's own measurement envs need. Keyed off the BOARD flag rather than a separate panel
// flag -- each board carries exactly one panel, so a second flag would only be a second
// thing to get out of step.
#if defined(BOARD_M5PAPER_COLOR)
#include "epd_el040ef1.h"
#elif defined(BOARD_RETERMINAL_E1002)
#include "epd_ed2208gca.h"
#else
#error "No board selected: define BOARD_M5PAPER_COLOR or BOARD_RETERMINAL_E1002"
#endif

// Brings up the panel on a bus somebody else owns: epd_init() calls board_spi_init() and
// then adds only its own SPI device. Ticket 03 is why the bus is not the panel's to create.
esp_err_t epd_init(void);

// Fills `t` with the phase durations. `frs` overrides the waveform-rate byte in the init
// list; pass EPD_FRS_STOCK to leave it at the panel's shipping value.
esp_err_t epd_refresh_solid(uint8_t color, uint8_t frs, epd_timings_t *t);

// `packed` is EPD_FRAME_BYTES of 4bpp palette indices, two pixels per byte, high nibble
// first -- the output of epd_dither.c. It may live in PSRAM; the driver copies it into its
// own DMA-capable buffer before transfer.
esp_err_t epd_refresh_frame(const uint8_t *packed, uint8_t frs, epd_timings_t *t);

// The panel's own temperature sensor, where it has a readable one. Returns
// ESP_ERR_NOT_SUPPORTED on a panel whose read path has not been established -- which is not
// the same as a panel that reports 0 C, and callers must tell the two apart rather than
// treating a failure as a reading. Ticket 18 is the whole account of how expensive that
// distinction was to learn here.
esp_err_t epd_read_temperature(int offset_steps, int *temp_c);

// True if BUSY currently reads idle. BUSY_N is low = busy, high = idle; every wait in the
// driver is for a RISING edge. The original handoff document has this inverted.
bool epd_busy_is_idle(void);

#endif // EPD_PANEL_H
