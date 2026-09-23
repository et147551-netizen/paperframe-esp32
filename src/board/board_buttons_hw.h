// The one thing a board has to supply for board_buttons.c to run on it: which pin each of the
// three roles is, and what to call it in a log.
//
// Everything else -- the 10 ms poll, the three-sample debounce, the 5 s long press, the 200 ms
// hold tick, and the rule that a press which already fired its long press is not also a click
// -- is board-independent and lives in board_buttons.c. Splitting it that way during the
// ticket 43 layering pass is what stopped a second board meaning a second copy of 170 lines of
// debounce logic; what actually differed was three GPIO numbers.
//
// ACTIVE LOW WITH A PULL-UP is assumed by board_buttons.c and is true of both boards here
// (the M5Paper Color has external pull-ups; the reTerminal E1002's three buttons pull to
// ground, per refs/esp32-photoframe's board header). A board where that is not true needs a
// flag here and a branch there -- do not silently invert a level in the table.
//
// Board-internal header. The interface up is board_buttons.h.

#ifndef BOARD_BUTTONS_HW_H
#define BOARD_BUTTONS_HW_H

#include "board_buttons.h"

typedef struct {
    int gpio;
    const char *name;
} board_button_desc_t;

// Indexed by board_button_t, so the order is the enum's order and not the board's silkscreen.
extern const board_button_desc_t BOARD_BUTTONS[BOARD_BUTTON_COUNT];

#endif // BOARD_BUTTONS_HW_H
