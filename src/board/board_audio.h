// The frame's sound: FR-1.2's boot sound and FR-6.4's tone per button press. Ticket 16.
//
// Two boards, two very different devices behind one interface:
//
//   * M5Paper Color: an ES8311 codec and a speaker amplifier on I2S0. Both are powered, and the
//     I2S channel exists, ONLY while a sound is playing -- I2S DMA buffers come out of internal
//     RAM, the scarce resource here, and an idle frame holds none of it. See board_audio_m5.c.
//   * reTerminal E1002: a magnetic buzzer on one GPIO, driven by LEDC as a square wave. It cannot
//     play a recording, so its boot sound is a short chime. See board_audio_e1002.c.
//
// Every call BLOCKS for the length of the sound, and all of them share one lock, so a press during
// the boot sound waits for it rather than cutting in. The board picks the pitches; a caller says
// only which sound it wants.

#ifndef BOARD_AUDIO_H
#define BOARD_AUDIO_H

#include "esp_err.h"

#include "board_buttons.h" // board_button_t

// Configure the pins, leaving everything powered down. Idempotent. A failure leaves the two
// play calls below as no-ops, and nothing else depends on sound working.
esp_err_t board_audio_init(void);

// FR-1.2. Whether to play it (the `boot_sound` setting) is the caller's decision.
void board_audio_play_boot(void);

// FR-6.4: a short tone for one press, pitched by which button it was.
void board_audio_beep(board_button_t button);

// One line for the boot log: what this board plays, and through what.
const char *board_audio_describe(void);

#endif // BOARD_AUDIO_H
