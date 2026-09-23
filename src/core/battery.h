// Battery voltage to a charge percentage.
//
// **This is a PORT of `batteryPercentFromMV()` in `assets/index.html`, not a second opinion.** That
// function has been the web UI's readout since before the matte band existed; the band needs the same
// number for its icon (ticket 64), and two implementations of one curve would have the page and the
// glass disagreeing about the same battery with nothing to say which was right. `tools/battery_parity.py`
// compares them at every millivolt, the way `tools/ink_preview_parity.py` compares the two quantisers
// -- that check is what makes "a port" a claim rather than an intention.
//
// The curve is five straight segments through a lithium cell's discharge knee: 4.2 V is 100 %, 3.2 V
// is 0 %, and the middle is deliberately not linear because a Li-ion cell spends most of its charge
// between 3.6 and 4.0 V. Nothing here was measured on this board's cell -- it is the page's curve, and
// the page's comment is the whole of its provenance.

#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>

// 0-100, saturating at both ends. 0 for a reading of 0 mV, which is what a board that cannot measure
// its cell reports -- so a caller that wants to tell "empty" from "unknown" has to check the
// millivolts itself, exactly as `app_server.c`'s `/api/battery` does.
int battery_percent_from_mv(uint16_t mv);

#endif // BATTERY_H
