// The three side buttons, as three ROLES rather than three pins.
//
// Ticket .scratch/digital-frame/issues/14; satisfies FR-6.1 and FR-6.2. Without M5Unified this
// module owns debouncing, click detection and long-press timing -- all of it in
// board_buttons.c, which is board-independent.
//
// **The buttons are named by position, not by letter**, because the M5Paper Color's case
// carries no A/B/C markings and the operator settled the layout on the unit on 2026-09-03.
// Each board maps its own three physical buttons onto these three roles in its own
// board_buttons_*.c: TOP is the one that carries the special action (the orientation toggle
// and the 5 s access-point hold), UP and DOWN move through the pictures.
//
// The reTerminal E1002's are labelled differently -- green (wake), left, right -- and the
// mapping is a decision rather than a transcription; its board file says which it made and why.
//
// FR-6.4 (a tone per press) is not this module's: frame_main.c's callback calls board_audio_beep().

#ifndef BOARD_BUTTONS_H
#define BOARD_BUTTONS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "board.h" // the board selection, its pin header, and BOARD_* constants

typedef enum {
    BOARD_BUTTON_TOP = 0, // the special action: orientation, and the 5 s AP hold
    BOARD_BUTTON_UP,      // previous picture
    BOARD_BUTTON_DOWN,    // next picture
    BOARD_BUTTON_COUNT,
} board_button_t;

typedef enum {
    BOARD_BUTTON_CLICK = 0,
    BOARD_BUTTON_LONG_PRESS,   // crossed the 5 s mark, once per press (FR-6.3)
    BOARD_BUTTON_HOLD_TICK,    // every 200 ms while a press is being counted
} board_button_event_t;

// Called from the button task. Keep it short: post to a queue or set a flag rather than
// refreshing the panel from inside it.
typedef void (*board_button_cb_t)(board_button_t button, board_button_event_t event,
                                  uint32_t held_ms);

esp_err_t board_buttons_init(board_button_cb_t cb);

// Current debounced state, for anything that wants to poll instead.
bool board_button_is_down(board_button_t button);

const char *board_button_name(board_button_t button);
int board_button_gpio(board_button_t button);

#endif // BOARD_BUTTONS_H
