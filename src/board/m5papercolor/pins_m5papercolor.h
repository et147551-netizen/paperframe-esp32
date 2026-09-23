// M5Paper Color (EL040EF1) wiring: every GPIO number, I2C address and bus fact for this
// board, in one file.
//
// MEASURED / SCHEMATIC-CONFIRMED. Sources are docs/board-pinmap.md,
// docs/research/reference-source-review.md §12, and refs/M5Unified for the pin map. Reached
// through board.h; never include it directly, or a file will compile against this board on
// a build meant for another one.
//
// The rule this file exists to enforce: a GPIO number appears HERE and nowhere else. As of
// the layering pass, `git grep 'gpio_set_level\|gpio_config'` over src/core and src/app
// returns only app/trace.c's debug pin, and that is the invariant to keep.

#ifndef PINS_M5PAPERCOLOR_H
#define PINS_M5PAPERCOLOR_H

#define BOARD_NAME "m5papercolor"

// ------------------------------------------------------------------ shared I2C
//
// One bus carries the PM1 power manager, the SHT40, and the RX8130CE RTC (schematic p.3).
#define BOARD_I2C_SDA 3
#define BOARD_I2C_SCL 2
#define BOARD_I2C_FREQ_HZ 100000

// -------------------------------------------------------------------- SPI2 bus
//
// Shared by the EPD panel and the microSD, which is what ticket 03's storage lock exists
// for. The bus belongs to neither of its two users: both call board_spi_init().
#define BOARD_SPI_HOST SPI2_HOST
#define BOARD_SPI_PIN_MOSI 13
#define BOARD_SPI_PIN_MISO 14
#define BOARD_SPI_PIN_SCLK 15

// microSD chip select. Nothing drove this pin before ticket 03 -- it was an undriven input
// in every build, which leaves a fitted card free to read the panel's frame transfer as its
// own traffic. board_spi_init() parks it high.
#define BOARD_SPI_PIN_SD_CS 47

// Bus-wide transfer ceiling. Matches the panel's frame chunk, which is the largest single
// transaction either device makes.
#define BOARD_SPI_MAX_XFER 32768

// -------------------------------------------------------------- the panel's pins
//
// These are the BOARD's wiring, not the controller's: the same ED2208-family part sits on
// different GPIOs on the reTerminal E1002. That is why they are here and not in the panel
// driver, where they used to live.
#define BOARD_EPD_PIN_BUSY 11
#define BOARD_EPD_PIN_RST 12
#define BOARD_EPD_PIN_DC 43
#define BOARD_EPD_PIN_CS 44

// Stock clock. Start here to reproduce the baseline before changing anything; research §3
// caps the total win from raising it at ~190 ms.
#define BOARD_EPD_SPI_FREQ_HZ 4000000

// --------------------------------------------------------------- buttons and LED
//
// The buttons are named by POSITION, not by letter: the case carries no A/B/C markings, and
// the operator settled the layout on the unit on 2026-09-03. A board whose buttons are
// labelled differently maps its own three onto the same three roles in board_buttons.h --
// the reTerminal E1002's green / left / right, for instance.
#define BOARD_BTN_TOP_GPIO 1  // the one on its own at the top
#define BOARD_BTN_UP_GPIO 10
#define BOARD_BTN_DOWN_GPIO 9

// Two WS2812B driven over RMT, not a plain GPIO LED.
#define BOARD_LED_GPIO 21

// ------------------------------------------------------------------------ audio
//
// An ES8311 codec on the shared I2C bus plus a speaker amplifier, fed over I2S0. The codec is
// held in reset until BOARD_CODEC_EN_GPIO goes high, which is why it never answers the boot
// scan. From refs/M5Unified/src/M5Unified.cpp:637-638 and :2970-2977.
#define BOARD_CODEC_EN_GPIO 45
#define BOARD_SPK_EN_GPIO 46
#define ES8311_I2C_ADDR 0x18
#define BOARD_I2S_MCLK_GPIO 42
#define BOARD_I2S_BCK_GPIO 40
#define BOARD_I2S_WS_GPIO 41
#define BOARD_I2S_DOUT_GPIO 38

// ------------------------------------------------------------------ microSD rail
//
// The card's rail needs a moment after PM1 GPIO3 goes high. GUESS: neither M5Unified nor the
// UserDemo documents a settle time, and with no card here it could not be measured. 50 ms
// costs nothing at boot; sweep 0/10/50/200 once a card exists.
#define BOARD_CARD_POWER_SETTLE_MS 50

// -------------------------------------------------------------- battery-backed state
//
// FR-5.6's slideshow index and the catalogue epoch cursor live in the RX8130's four bytes of
// battery-backed RAM. Chosen over NVS to avoid ~288 flash writes a day (ticket 37), and it is
// what the shipping firmware uses. Four bytes is this RTC's whole RAM, so it is a ceiling and
// not a preference -- see board.h's board_state_*() for what a board without any of it does.
#define BOARD_STATE_BYTES 4

#endif // PINS_M5PAPERCOLOR_H
