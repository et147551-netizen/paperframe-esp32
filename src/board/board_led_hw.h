// The two things a board has to supply for board_led.c's animation to run on it.
//
// The split came out of the ticket 43 layering pass, and it went here rather than being
// duplicated because the answer to "what is board-specific about the LED?" turned out to be
// two functions out of 260 lines. The state machine, the patterns, the timings, the
// transient-falls-back-to-resting rule that ticket 55 turns on, and the borrowed 20 ms tick
// hook are all in board_led.c and are the same on every board. What differs is the light:
// two WS2812B over RMT on the M5Paper Color, one monochrome active-low GPIO on the
// reTerminal E1002.
//
// This is a board-internal header. Nothing above src/board/ includes it; the interface up is
// board_led.h.

#ifndef BOARD_LED_HW_H
#define BOARD_LED_HW_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "board.h" // the selected board's pin header: BOARD_LED_GPIO and friends

// Bring the light up: any rail it sits behind, and the driver that talks to it. Called once
// from board_led_init(), before the animation task starts. Returning an error stops
// board_led_init() -- a board with no indicator at all should return ESP_OK and make
// board_led_hw_paint() a no-op, because the caller's fallback for "no LED" is worse than a
// dark one: FR-9 exists because a board with no light and no sound cannot be told from a
// board that is switched off.
esp_err_t board_led_hw_init(void);

// Show this colour. 0-255 per channel, already at the brightness the pattern wants.
//
// TWO THINGS ARE THE IMPLEMENTATION'S JOB, not the caller's. First, suppressing a repaint
// when the colour has not changed: this is called every 20 ms and a WS2812 refresh is a
// blocking RMT transaction, which on a device counting milliamps would be fifty wasted
// transactions a second. Second, deciding what a colour means on hardware that cannot show
// it -- a single monochrome LED has to answer "lit or not", and the honest reading is
// whether any channel is non-zero, so the patterns' duty cycles survive and only the colour
// coding is lost. Say which in the file, because a caller reading board_led.h's "cyan rather
// than green so working and done differ at a glance" needs to know that on this board they
// do not.
void board_led_hw_paint(uint8_t r, uint8_t g, uint8_t b);

// Whether this board's light can show a colour at all.
//
// **The one capability board_led.c has to branch on, and it exists for exactly one pattern.**
// BOARD_LED_FAULT is BOARD_LED_IDLE's duty in red, so on a board that answers false here the two
// are the same light and ticket 55's persistent fault cannot be seen — which
// board_led_e1002.c documented as a deliberate gap until the operator made the blink state the
// designated way to tell a resting frame from a broken one (2026-09-20, ticket 68 §11). Then a
// fault that looks like resting stops being a documented gap and becomes a wrong answer.
//
// A capability rather than an `#ifdef BOARD_*` in board_led.c: that file is board-independent by
// construction and this header is where the differences are declared. A third board with a
// two-colour LED would answer true and need nothing else.
bool board_led_hw_has_colour(void);

// What board_led_init() prints once, so a boot log says what indicator this board has.
const char *board_led_hw_describe(void);

#endif // BOARD_LED_HW_H
