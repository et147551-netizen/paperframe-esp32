#include "battery.h"

#include <math.h>

// JavaScript's `Math.round`: half away from zero, which for the non-negative values here is
// `floor(x + 0.5)`. Not `lround()`, which rounds half to even on some libcs -- the same distinction
// `epd_epdopt.c`'s `epd_clamp_byte()` carries for the same reason.
static int js_round(double x)
{
    return (int)floor(x + 0.5);
}

// **Double precision, and the expressions are the page's character for character.** An integer form
// was written first and `tools/battery_parity.py` caught it on its first run: the page computes
// `(v - 3.2) / 0.2 * 5` in floating point, and at 3300 mV that is 2.499999999999991 rather than 2.5,
// so `Math.round` gives 2 where exact arithmetic gives 3. 71 of 5001 millivolts differed.
//
// The quirk is reproduced rather than corrected, because the acceptance test for a port is
// "identical" and not "better" -- `src/core/epd_epdopt.c` says the same thing about four of its own
// oddities, and the cost of being cleverer here is a page and a panel that disagree about the same
// battery. The ESP32-S3's FPU is single precision so these doubles are emulated; it is a handful of
// operations once per photograph, against a 15,014.6 ms refresh.
//
// If the page's curve is ever rewritten, rewrite this to match it and let the parity check decide
// whether the two agree. Do not "fix" one side alone.
int battery_percent_from_mv(uint16_t mv)
{
    const double v = (double)mv / 1000.0;
    if (v >= 4.2) {
        return 100;
    }
    if (v <= 3.2) {
        return 0;
    }
    if (v >= 4.0) {
        return js_round(85 + (v - 4.0) / 0.2 * 15);
    }
    if (v >= 3.8) {
        return js_round(50 + (v - 3.8) / 0.2 * 35);
    }
    if (v >= 3.6) {
        return js_round(15 + (v - 3.6) / 0.2 * 35);
    }
    if (v >= 3.4) {
        return js_round(5 + (v - 3.4) / 0.2 * 10);
    }
    return js_round((v - 3.2) / 0.2 * 5);
}
