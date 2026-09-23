// The reTerminal E1002's three buttons, mapped onto board_buttons.h's three roles.
//
// READ, NOT MEASURED: the pins come from
// refs/esp32-photoframe/components/board_hal/include/board_seeedstudio_reterminal_e1002.h --
// GPIO3 green (its wake key), GPIO5 left, GPIO4 right.
//
// THE MAPPING IS A DECISION. That firmware assigns green = wake/select, left = rotate, right =
// clear; this application's roles are different, so the assignment is re-made rather than
// copied. Green is the one on its own and the deep-sleep wake pin, so it takes TOP -- the
// special action, meaning the orientation toggle and the 5 s access-point hold. Left and right
// become UP and DOWN, with left = previous, which is the reading a row of pictures gives.
//
// GPIO3 is also a strapping pin on the ESP32-S3 (JTAG source select). It reads fine as an input
// after boot, which is what the reference does with it, but a button HELD DOWN THROUGH RESET
// pulls it low at strap time. Whether that matters on this board has not been checked and
// cannot be from here; ticket 63 should hold each button through a reset once.

#include "board_buttons_hw.h"

#ifdef BOARD_RETERMINAL_E1002

const board_button_desc_t BOARD_BUTTONS[BOARD_BUTTON_COUNT] = {
    [BOARD_BUTTON_TOP] = {BOARD_BTN_TOP_GPIO, "GREEN"},
    [BOARD_BUTTON_UP] = {BOARD_BTN_UP_GPIO, "LEFT"},
    [BOARD_BUTTON_DOWN] = {BOARD_BTN_DOWN_GPIO, "RIGHT"},
};

#endif // BOARD_RETERMINAL_E1002
