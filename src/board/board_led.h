// The two WS2812B status LEDs.
//
// Part of ticket .scratch/digital-frame/issues/16 (FR-9.1, FR-9.2), taken early and on
// purpose: with no LED and no sound, **there is no way to tell whether this board is
// switched on**. The panel holds its last image without power, the console is silent, and
// esptool talks to a board that is off — so on 2026-09-03 the owner had to open
// Windows Device Manager to find out whether a power-button press had worked.
//
// Two facts about the hardware, both from docs/board-pinmap.md and refs/_pm1.txt:
//
//   * The LEDs are on GPIO21, two of them, WS2812B-4020, driven over RMT.
//   * They are powered from the PM1's 3.3 V LDO and gated by its LED_EN pin — PWR_CFG
//     (0x06) bits [2] and [4]. Both are 1 in the register's 0x17 reset value, so they are
//     usually already on; this module reads the register, sets them if they are not, and
//     logs the raw byte either way rather than assuming.
//
// **Nothing here is ever steadily lit.** The shipping firmware's LED task never leaves
// the LEDs on: every state settles into a 50 ms pulse at half brightness every 2 s, a
// 2.5 % duty cycle (`refs/M5PaperColor-UserDemo/main/hal/hal.cpp:725-820`). On a frame
// that is meant to run for weeks off a battery, a steady indicator is a power budget
// nobody agreed to spend. The patterns below are that firmware's, transcribed.

#ifndef BOARD_LED_H
#define BOARD_LED_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "board.h" // the board selection, its pin header, and BOARD_* constants

typedef enum {
    BOARD_LED_OFF = 0,
    // Powered and running: green, 50 ms every 2 s. This is the resting state and the one
    // that answers "is it on?".
    BOARD_LED_IDLE,
    // A refresh is in flight: four cyan blinks of 100 ms, then a cyan pulse every 2 s
    // until the refresh ends. Cyan rather than green so "working" and "done" differ at a
    // glance during the 15.6 s in which the panel still shows the old image.
    BOARD_LED_REFRESH,
    // A render finished: four green blinks of 100 ms, then back to IDLE by itself.
    BOARD_LED_SUCCESS,
    // The last operation failed: red, fading from full to dark over ~1 s, then back to
    // IDLE. The reference returns to whatever state preceded the error; there is only one
    // resting state here, so it returns to that.
    BOARD_LED_ERROR,
    // A button is being held and the 5 s count is running: white, 100 ms every 200 ms.
    // The one state that is deliberately busy -- it exists to say "keep holding".
    BOARD_LED_HOLD,
    // A fault that is STILL TRUE: red, 50 ms every 2 s -- IDLE's own duty, so this differs
    // from the resting state by colour alone and costs the same power (ticket 55 §3).
    //
    // Deliberately NOT an extension of BOARD_LED_ERROR, which is transient by design and
    // whose caller at app_display.c:705 wants it that way: a failed render retries on the
    // next advance and the one-second fade is the right shape for that. This state is for the
    // two faults nothing will clear by itself, chosen by the owner on 2026-09-10 from the
    // survey in ticket 55 §4: httpd_start() failed, so there is no web UI at all, and /data
    // did not mount. Both are invisible on the panel -- the frame draws the four factory
    // photographs and looks healthy -- and the first cannot be reported over HTTP by
    // definition. Set it through board_led_set_resting() rather than board_led_set(), so a
    // render's SUCCESS does not fall back past it.
    BOARD_LED_FAULT,
} board_led_state_t;

// Enables the LED rail if it is not already on and starts the animation task. Safe to
// call once; a second call is a no-op.
esp_err_t board_led_init(void);

// The animation task picks this up within its 20 ms tick. Any task may call it.
// SUCCESS and ERROR are transient: they play and then fall back to the RESTING state on
// their own -- which is IDLE unless board_led_set_resting() has said otherwise.
void board_led_set(board_led_state_t state);

// What the transient states fall back to, and what nothing else overwrites. IDLE at boot.
//
// This exists because the fallback used to be the literal BOARD_LED_IDLE, so on a frame whose
// web UI failed to start a single successful render would put the LED back to the green pulse
// that means "on and working" and leave it there -- reporting health while the fault it had
// just been told about was still true. Ticket 55 §2 is that defect; a poll that merely
// re-asserts the fault every 10 s does not close it, because the ten seconds are exactly when
// somebody looks.
void board_led_set_resting(board_led_state_t state);

board_led_state_t board_led_get(void);

// Installs a function called on every 20 ms tick of the LED task, from that task.
//
// It is here because this task is the only thing in the application that already wakes at that
// rate, and ticket 47's internal-RAM sampler needs a clock more than it needs a task -- a task of
// its own would take a 2 KB stack out of the pool it is watching, and
// docs/board-and-storage.md records that two small tasks were once enough to make
// httpd_start() fail. What the borrowing costs the measurement is in app_heapwatch.h: this task runs at priority 3, below the display task and
// smbsync, so it is not a fair sampler of a trough those two hold the CPU through.
//
// The hook must not block and must not assume it can log: it runs on a 1.5 KB stack alongside a
// blocking RMT transaction. Pass NULL to remove it. Installing it is the application's business,
// so nothing here has to know what it does.
void board_led_set_tick_hook(void (*hook)(void));

#endif // BOARD_LED_H
