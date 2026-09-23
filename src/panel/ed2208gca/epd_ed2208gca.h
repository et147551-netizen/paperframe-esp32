// ED2208-GCA: the reTerminal E1002's 7.3" Spectra 6 panel, and everything about driving it
// that is not part of the panel interface.
//
// Reached through epd_panel.h when BOARD_RETERMINAL_E1002 is selected. Compare
// epd_el040ef1.h -- that file is mostly measurements, and this one has none, because no panel
// of this kind has been on this bench. Ticket 63.
//
// WHAT IS DELIBERATELY ABSENT, and each absence is a decision:
//
//   * NO epd_seq_t, no EPD_SEQ_STOCK / EPD_SEQ_BUSY, no epd_set_sequencing(). Those two
//     alternatives exist on the EL040EF1 because ticket 09 measured the difference between
//     them (597 ms per refresh, twice, an hour apart) and because EPD_SEQ_STOCK is the
//     reference every figure in docs/measurements.md is compared against. This panel's
//     reference sequence has a different shape entirely -- no BTST2 resend between PON and
//     DRF, no fixed delays, a DSLP at the end -- so there is one sequence here and inventing
//     a second to mirror an interface would be inventing a measurement condition.
//   * NO BUSY or DRF timeout tuned to a sweep. The values below are generous rather than
//     derived; a 7.3" panel's refresh has not been timed here.
//   * NO probe hooks. Ticket 18's five read variants are a question about the EL040EF1's
//     wiring, already answered.

#ifndef EPD_ED2208GCA_H
#define EPD_ED2208GCA_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// The shipping waveform rate, PLL (0x30). 0x03 here against the EL040EF1's 0x08 -- read from
// the reference driver, not chosen, and NOT comparable to this project's FRS sweep: that sweep
// was run on a different panel at a different resolution, and its whole point was that the byte
// changes the waveform's visible outcome. Do not carry an FRS conclusion across.
#define EPD_FRS_STOCK 0x03

// Generous, and knowingly so. The EL040EF1's 30 s / 90 s were sized against a measured sweep
// that reached 37.7 s at its slowest setting; nothing equivalent has been run here, and a
// timeout that is too tight turns a slow panel into a failed one. Re-derive in ticket 63 from
// the first ten refreshes, and do not quote these as expectations.
#define EPD_BUSY_TIMEOUT_MS 30000
#define EPD_DRF_TIMEOUT_MS 90000

// The BUSY wait polls, and these are its two intervals. Both come from the reference driver's
// wait_busy(): a 10 ms settle before the first sample, because BUSY is read after the command has
// already gone out, and 10 ms between samples. The settle is load-bearing -- see busy_wait() for
// what an unsettled poll would do, and for why an edge-only wait failed on the glass.
#define BUSY_SETTLE_MS 10
#define BUSY_POLL_MS 10

#endif // EPD_ED2208GCA_H
