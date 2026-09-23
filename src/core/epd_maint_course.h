// The maintenance course: ten full-screen flats in the panel's own native colours.
//
// Ticket 68, the operator's request of 2026-09-20 -- a service mode that "ignores the palette and
// steps the panel through the standard colours it can show, in order", like an LCD's R-G-B service
// screen. The order is the whole of what this file holds, and it is here rather than in
// src/app/app_maint.c so the host suite can pin it without an ESP-IDF in sight.
//
// **THE ORDER IS A GHOSTING RULE, NOT A PANEL-PHYSICS RULE**, and the distinction matters because
// the obvious physics story is wrong here. There is no "hard transition" to sequence around: the
// DRF is a fixed-length global waveform -- epd_format.c models it as a constant 926,000
// frame-seconds with no image term, and it measures the same ~14.5 s at stock FRS across colour
// charts, photographs and 1-bit screens alike (docs/agents/measurements.md). Every refresh drives
// all six pigments through the full cycle regardless of what was on the glass, so no sequence is
// electrically kinder to the panel than another.
//
// What forces an order is the MEASURED GHOST. A 1-bit black-on-white pairing screen stayed plainly
// legible through the next full refresh, and **only a white render in between removed it
// completely** (docs/agents/measurements.md:123-133, ticket 30). A flat inspected straight after
// another flat therefore carries the previous one -- which defeats the entire purpose of a mode
// whose output a person looks at. Hence white between every pair.
//
// **Green and blue get the cleanest slots on purpose.** They are the two colours that fail first on
// this panel: at FRS 0x07 green moves (64,107,80) -> (102,124,57) and blue (51,98,146) ->
// (84,121,150) while white, black, red and yellow all move under 4 LSB, both drifting toward white
// and toward each other -- "what an incompletely-driven pigment looks like" (measurements.md).
// If under-driving is ever going to show, it shows there, so neither may be read through another
// colour's residue.
//
// **It ends on white**, which is the vendor precaution: GooDisplay's own Precautions item 5 says to
// refresh every 24 h or risk image sticking, and to ship and store showing a fully white image
// (docs/research/reference-source-review.md:283). Ending white also lands the panel in exactly the
// state ticket 68's other half wants for the inactive window, so the course needs no eleventh
// refresh to clean up after itself -- and needs no leading white either, because the window edge
// has already parked the panel there.

#ifndef EPD_MAINT_COURSE_H
#define EPD_MAINT_COURSE_H

#include <stdint.h>

// K W R W Y W G W B W.
#define EPD_MAINT_COURSE_STEPS 10

// The native colour index for one step, or 0xFF for a step outside 0..EPD_MAINT_COURSE_STEPS-1.
//
// 0xFF rather than a white fallback: a caller that has walked off the end must not quietly refresh
// the panel one more time, and epd_color_valid() rejects 0xFF, so app_display_request_flat() turns
// the mistake into ESP_ERR_INVALID_ARG instead of an eleventh flat.
uint8_t epd_maint_course_colour(int step);

// "black", "white", "red", "yellow", "green", "blue" -- for the one console line per step, so a
// capture can be counted by colour rather than by index. "?" outside the range.
const char *epd_maint_course_name(int step);

// The same word for a colour index rather than a step, which is what app_display's `# render` line
// has to hand. "?" for an index this panel cannot render, including 4.
const char *epd_maint_colour_name(uint8_t colour);

// ------------------------------------------------------------------- the clear cycle
//
// A SECOND sequence, and a different job from the course above: W B W B W B W, three black passes
// each bracketed by white. The course is for a person to LOOK at; this is for a panel that has kept
// a faint trace of a previous picture. Operator's request, 2026-09-20.
//
// **Seven refreshes and not nine.** "White, black, white, three times" shares the whites between
// cycles, because two consecutive identical white renders clear nothing the first one did not and
// e-paper has a finite refresh count — the same argument that made the course weekly rather than
// nightly (ticket 68 §5). Three black passes bracketed by white is the intent, and this is it.
//
// **It starts white and ends white.** Starting white is why this sequence needs no leading prime the
// way a manually started course does; ending white is the vendor's ship-and-store state, and it
// leaves the panel where the standby wants it.
//
// **WHAT IS NOT KNOWN: whether this removes a ghost that one white render did not.** One white IS
// measured to remove a plainly legible 1-bit black-on-white ghost completely (2026-09-10, ticket 30,
// `docs/agents/measurements.md`), so the escalation exists for the case beyond that one — and that
// case has never been produced here, deliberately: the operator declined to manufacture a ghost to
// test it (2026-09-20). So this is a mechanism offered on the vendor precaution and the physics of a
// full-frame DRF, **not a measured remedy**, and nothing may cite it as one.
// **REPEATABLE since 2026-09-20, and the repeat is free because the sequence is an alternation.**
// The operator asked for a continuous mode; what shipped is a bounded count, because time is the
// wrong unit for this — 8 h of continuous clearing is 1,920 refreshes on the M5Paper Color and 932
// on the ED2208-GCA, so the same "two hours" costs twice as much on one board as the other, and
// either number is two orders of magnitude past the ~16 automatic advances a day this frame does.
// A count of passes is board-independent and is the thing that might help as well as the thing that
// costs.
//
// **The whites stay shared across repeats too.** `cycles` counts three-black-pass cycles, and N of
// them is `6N + 1` steps rather than `7N`: W (BW BW BW) x N. Two consecutive identical whites at a
// seam clear nothing the first did not — the same argument that made one cycle seven steps and not
// nine. Every length is therefore odd, which is what keeps every repeat count starting and ending on
// white.
#define EPD_MAINT_CLEAR_BLACKS_PER_CYCLE 3
// Five, and the bound is an argument rather than a round number: 5 cycles is 31 refreshes, ~7.8 min
// here and ~16 min on the E1002. Past that the cost is in the territory ticket 68 §5 argues about
// while the benefit is still unmeasured — nothing shows that even ONE cycle beats the single white
// render that is measured to clear a plainly legible ghost completely.
#define EPD_MAINT_CLEAR_CYCLES_MAX 5

// Steps for `cycles` repetitions, or 0 for a count outside 1..EPD_MAINT_CLEAR_CYCLES_MAX.
int epd_maint_clear_steps(int cycles);

// The native colour index for one step of a `steps`-long clear, or 0xFF outside 0..steps-1. Same
// 0xFF contract as the course, and for the same reason.
//
// **`steps` is a parameter and not a constant, because the length is the caller's now.** This
// function only knows that the alternation starts white; how long it runs is the repeat count the
// request carried.
uint8_t epd_maint_clear_colour(int step, int steps);

// "white" / "black" for the log line.
const char *epd_maint_clear_name(int step, int steps);

#endif // EPD_MAINT_COURSE_H
