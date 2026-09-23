// The M5Paper Color's three side buttons: which pin is which position.
//
// docs/board-pinmap.md for the pins; the positions are the operator's, read off the unit on
// 2026-09-03, because the case carries no A/B/C markings. That answers Open question 1 in
// docs/requirements/digital-frame.md, and it means the shipping firmware's own assignment does
// not map onto the positions the way its usage page describes: the firmware puts the
// orientation toggle and the 5 s access-point hold on GPIO10, which is the MIDDLE button,
// while the usage page calls that action "the top button". Position wins here -- the special
// action goes on TOP, and UP and DOWN move through the pictures the way their names say.
//
// FR-6.4's tone per press is board_audio_m5.c's. It keeps the shipping firmware's pitch per PIN
// (GPIO10 121, GPIO9 120, GPIO1 119), not per position.

#include "board_buttons_hw.h"

#ifdef BOARD_M5PAPER_COLOR

const board_button_desc_t BOARD_BUTTONS[BOARD_BUTTON_COUNT] = {
    [BOARD_BUTTON_TOP] = {BOARD_BTN_TOP_GPIO, "TOP"},
    [BOARD_BUTTON_UP] = {BOARD_BTN_UP_GPIO, "UP"},
    [BOARD_BUTTON_DOWN] = {BOARD_BTN_DOWN_GPIO, "DOWN"},
};

#endif // BOARD_M5PAPER_COLOR
