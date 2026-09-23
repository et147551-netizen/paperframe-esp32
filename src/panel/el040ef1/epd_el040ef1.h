// EL040EF1: the M5Paper Color's 4.0" Spectra 6 panel, and everything about driving it that
// is NOT part of the panel interface.
//
// Reached through epd_panel.h, which includes this file when BOARD_M5PAPER_COLOR is
// selected. Do not include it directly from the application layer -- what is in here is
// either a measurement condition of this panel or a probe hook for one of its tickets, and
// an application that reaches for either has crossed a layer.
//
// Not M5GFX. M5GFX's structure actively obstructs the measurement: dithering is welded into
// the transfer loop and the fixed delays live inside a private method.
//
// The sequence reproduced here is the stock M5GFX/Waveshare one, delays included, so that
// issues/06 measures the shipping configuration. issues/06 has since settled H1 -- BUSY
// covers the PON->DRF interval with a median of 130.9 ms against an 80 ms floor -- and
// issues/09 then measured the tightened sequence and looked at the glass. EPD_SEQ_BUSY is
// the default; EPD_SEQ_STOCK still reproduces the shipping sequence exactly, which is what
// the measurements are compared against.

#ifndef EPD_EL040EF1_H
#define EPD_EL040EF1_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// The shipping waveform rate. `frs` is the PLL byte (0x30), and the whole of Phase 0 was
// about it. Every caller outside this panel's own measurement envs passes EPD_FRS_STOCK and
// does not care what it is -- which is exactly why the name is in the interface and the
// value is here. Research §3 caps the total win from raising the SPI clock at ~190 ms.
#define EPD_FRS_STOCK 0x08

// BUSY wait budgets. M5GFX uses 20 s throughout, but the FRS sweep goes down to 0x00, where
// the circulating table reports 37.7 s -- a 20 s timeout would abort the slowest settings
// and record them as failures.
#define EPD_BUSY_TIMEOUT_MS 30000
#define EPD_DRF_TIMEOUT_MS 90000

// ------------------------------------------------------------------- sequencing
//
// The stock sequence carries three unconditional 200 ms sleeps -- after PON, after the
// refresh BTST2 and after POF -- which the baseline in .scratch/digital-frame/issues/17
// accounts for as 592 ms of a 15571 ms refresh. issues/09 measures what removing them
// recovers, so both sequences have to exist at once and be selectable inside one
// thermally-matched session.
//
// EPD_SEQ_BUSY drops those three sleeps and relies on the BUSY waits that already bracket
// PON, DRF and POF. It saves 597 ms per refresh, measured twice an hour apart (597.2 and
// 596.4 ms), and it is the DEFAULT since 2026-09-03 -- the six-colour chart was scanned
// under both sequences and they differ by 1.9 LSB of 255, less than the panel's own 4.7 LSB
// between a first and second draw of identical content.
//
// EPD_SEQ_STOCK is kept, not as a fallback but as the thing every future measurement is
// compared against: it is the shipping firmware's sequence.
//
// THIS SWITCH DOES NOT GENERALISE. It is two measured alternatives for THIS panel, and the
// ED2208-GCA has one sequence of a different shape (no BTST2 resend between PON and DRF, no
// fixed delays, a DSLP at the end). That is why it is here and not in epd_panel.h, and why
// only harness_main.c and colour_chart_main.c -- this panel's own measurement entry points
// -- ever call the setter.
//
// One thing neither session tested: a refresh issued IMMEDIATELY after POF returns. Every
// run here left >=15 s of idle, and so does the application. If a caller ever refreshes
// back to back, that is new territory for the dropped POF delay.
typedef enum {
    EPD_SEQ_STOCK = 0, // M5GFX/Waveshare, fixed delays included
    EPD_SEQ_BUSY = 1,  // BUSY waits only, floor enforced explicitly -- default
} epd_seq_t;

// The one hard constraint in the panel's timing diagram. Enforced in BOTH modes: under
// EPD_SEQ_STOCK it never fires, which is the point -- the floor is then a property of the
// code rather than of a delay that happens to cover it.
#define EPD_PON_TO_DRF_FLOOR_MS 80

void epd_set_sequencing(epd_seq_t mode); // default EPD_SEQ_BUSY
epd_seq_t epd_get_sequencing(void);

// ---------------------------------------------------- about epd_read_temperature()
//
// Declared in epd_panel.h. Two things about it on THIS panel are part of the contract
// rather than incidental, and both were expensive:
//
// The reply comes back on MOSI, not MISO: the panel's only data pin is SI0 (J5 pin 33), and
// the MISO GPIO belongs to the microSD. Reading MISO returned 0x00 or 0xFF -- the values a
// floating line takes, which decode to the plausible 0 C and -1 C that ticket 18 exists
// because of. Verified on hardware 2026-09-03; the raw bytes and the 2x2 that settled it are
// in .scratch/digital-frame/issues/18.
//
//   * it RESETS THE PANEL on the way out. The panel keeps driving SI0 after CS is
//     deasserted, and a microSD on the same bus then answers nothing until RST_N is
//     pulsed -- measured 2026-09-04, ticket 08. Callers get a panel in its post-reset
//     state, which the refresh path establishes for itself anyway;
//   * the FIRST call after boot returns 0 C. Later ones are real. Whatever the mechanism,
//     it takes one other transaction on the bus -- a refresh or an sdspi attach -- before
//     the reply arrives. Do not choose a waveform on a cold read.

// ------------------------------------------------------- ticket 18 probe hooks
//
// Compiled only into env:tempprobe. The question is which host pin carries the panel's
// reply: epd_read_temperature() reads MISO, but the schematic
// (reference-source-review.md §12.4) shows the panel's only data pin is SI0 = MOSI. Both
// are read here, along with a second readable register, so "the read mechanism is broken"
// can be told apart from "the sensor is broken" without warming anything.
#ifdef BUILD_TEMPPROBE

// Two bytes per read, not one: if the controller needs a dummy cycle between the command
// and the reply, the payload shows up in the second byte instead of being silently lost.
// Raw bytes only -- no decode happens in the firmware.
typedef struct {
    uint8_t tsc_miso[2];         // A: today's path, full duplex on MISO
    uint8_t tsc_3wire[2];        // B: same commands, half-duplex 3-wire on MOSI
    uint8_t flg_3wire[2];        // C: FLG 0x71 on MOSI -- tests the mechanism alone
    uint8_t tsc_3wire_no_tse[2]; // D: TSC on MOSI with TSE never written
    uint8_t flg_miso[2];         // E: FLG on MISO, so A/B and C/E form a 2x2
} epd_probe_sample_t;

// The reset pulse the refresh path does internally. Exposed because the module manual marks
// SPI invalid until RST_N has risen, which is a third candidate cause the ticket does not
// list: nothing has ever reset the panel before a temperature read.
esp_err_t epd_probe_reset(void);

// Reset, then the stock init list and TRES, then release CS. No PON, no DRF -- this leaves
// the panel configured but unpowered, to test whether it answers register reads only once it
// has been set up.
esp_err_t epd_probe_configure(uint8_t frs);

esp_err_t epd_probe_sample(int offset_steps, epd_probe_sample_t *out);

// TSE with the given TO[3:0] offset, `settle_ms` of wait, then TSC. Sweeping settle_ms
// against a known offset step is how the conversion delay is measured rather than guessed.
esp_err_t epd_probe_offset_step(int offset_steps, uint32_t settle_ms, uint8_t *out);

#endif // BUILD_TEMPPROBE

#endif // EPD_EL040EF1_H
