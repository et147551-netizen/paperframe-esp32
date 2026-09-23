// The ED2208-GCA init sequence, for the reTerminal E1002's 7.3" Spectra 6 panel.
//
// TRANSCRIBED FROM A THIRD PARTY, NOT FROM A DATASHEET AND NOT MEASURED:
// refs/esp32-photoframe/components/epaper_driver_ed2208_gca/src/driver_ed2208_gca.c,
// send_init_sequence(), MIT, HEAD bf02982. Its BTST2 line is annotated "Seeed_GFX tuned" there,
// so at least one of these payloads is somebody's tuning rather than a vendor default.
//
// HOW IT DIFFERS FROM THE EL040EF1'S LIST, which matters because ticket 43 predicted the two
// would "agree byte for byte" on ten commands and that was not quite right:
//
//   * PFS/POFS (0x03) is `00 54 00 44` here against `03 54 00 44` there. The first byte
//     differs. Ticket 43's comment of 2026-09-15 listed PFS among the agreeing commands; that
//     is corrected in the ticket. **And this list is the one with the majority behind it**
//     (ticket 65 item 2, 2026-09-18): Waveshare's own 4in0e driver and psiegl both send `00`
//     here, so the EL040EF1's `03` is M5GFX's own deviation rather than a panel difference. Do
//     not "align" the two lists on that basis -- the EL040EF1's value is what its device shipped
//     running and nobody has observed the byte doing anything.
//   * BTST2 (0x06) is `6F 1F 16 25` against `6F 1F 17 17`.
//   * PLL (0x30) is 0x03 against 0x08 -- the waveform rate, EPD_FRS_STOCK.
//   * TRES (0x61) is IN THIS LIST, encoding 800x480, where M5GFX sends it separately after a
//     second BUSY wait. Keeping it in the list is this reference's shape and is followed.
//   * THE ORDER DIFFERS: POFS before BTST1, BTST2 before BTST3, PLL before CDI, and PWS last.
//     Whether any of that is load-bearing is unknown. It is reproduced exactly rather than
//     normalised to the EL040EF1's order, because a reordered init list is precisely the kind
//     of change that produces a blank panel with no error.
//
// Header-only and free of ESP-IDF dependencies, so test/test_epd_core can compare it on the
// host under env:native_e1002.

#ifndef EPD_INIT_LIST_ED2208GCA_H
#define EPD_INIT_LIST_ED2208GCA_H

#include <stdint.h>

// Format: command, length, data... repeated, terminated by EPD_INIT_END. Same walk as the
// EL040EF1's list, so epd_send_init_list() is shared in shape if not in file.
#define EPD_INIT_END 0xFF

static const uint8_t epd_init_list[] = {
    0xAA, 6, 0x49, 0x55, 0x20, 0x08, 0x09, 0x18, // CMDH
    0x01, 1, 0x3F,                               // PWR
    0x00, 2, 0x5F, 0x69,                         // PSR
    0x03, 4, 0x00, 0x54, 0x00, 0x44,             // PFS -- first byte 0x00, not 0x03
    0x05, 4, 0x40, 0x1F, 0x1F, 0x2C,             // BTST1
    0x06, 4, 0x6F, 0x1F, 0x16, 0x25,             // BTST2 -- "Seeed_GFX tuned"
    0x08, 4, 0x6F, 0x1F, 0x1F, 0x22,             // BTST3
    0x30, 1, 0x03,                               // PLL <- FRS
    0x50, 1, 0x3F,                               // CDI
    0x60, 2, 0x02, 0x00,                         // TCON
    0x61, 4, 0x03, 0x20, 0x01, 0xE0,             // TRES = 800x480, in the list
    0x84, 1, 0x01,                               // T_VDCS
    0xE3, 1, 0x2F,                               // PWS
    EPD_INIT_END, EPD_INIT_END,
};

// DSLP (0x07) takes 0xA5 and the reference issues it at the END OF EVERY REFRESH, not only on
// the way into deep sleep. That is a real behavioural difference from the EL040EF1 path, which
// leaves the panel powered off but awake: a deep-sleeping controller needs the hardware reset
// this driver already does at the start of every refresh before it will answer again, so the two
// are consistent -- but a caller that expects to read a register between refreshes will find
// nothing on this board. Nothing does; epd_read_temperature() is unsupported here.
#define EPD_DSLP_PAYLOAD 0xA5

#endif // EPD_INIT_LIST_ED2208GCA_H
