// The stock EL040EF1 init sequence. **Re-sourced to the panel vendor on 2026-09-18** (ticket 65
// item 2): every row below is now cited to Waveshare's own 4inch e-Paper (E) driver,
// refs/waveshare-e-Paper/E-paper_Separate_Program/4inch_e-Paper_E/ESP32/EPD_4in0e.cpp (Waveshare
// team, V1.0 2024-08-20, MIT notice in the file header, clone HEAD a794fbc), whose ESP32 and
// Arduino_R4 copies are byte-identical in EPD_4IN0E_Init(). It was previously transcribed from
// refs/M5GFX/src/lgfx/v1/panel/Panel_ED2208.hpp:52-67 -- LovyanGFX's file, FreeBSD (BSD-2-Clause),
// (c) lovyan03, inside the MIT-licensed M5GFX repository, (c) 2021 M5Stack. Licence texts:
// LICENSES/Waveshare-MIT.txt and LICENSES/LovyanGFX-BSD-2-Clause.txt.
//
// NOT ONE BYTE OF THE ARRAY CHANGED IN THAT RE-SOURCING, WITH ONE EXCEPTION THAT IS STILL
// M5GFX'S AND IS FLAGGED AT THE ROW ITSELF: PFS (0x03). The vendor sends 0x00 0x54 0x00 0x44
// (EPD_4in0e.cpp:160-164) and M5GFX sends 0x03 0x54 0x00 0x44. This is the "the two init lists do
// NOT agree on PFS" that ticket 43's closure recorded, now attributed: it is M5GFX that deviates
// from the vendor, not the other way round. A third source agrees with the vendor --
// refs/E-Ink_Spectra_E6/epdflash/src/EPD_4in0e.c:38 (psiegl, HEAD 7189afe) records the original as
// "0x00, 0x54, 0x00, 0x44" before substituting its own 0x30. **The value here is left at M5GFX's
// 0x03 deliberately**, because it is what the device shipped running and the FRS sweep's zero point
// is the stock configuration; the datasheet reading that says why it is probably inert, and the
// reason "probably" is not good enough to change it unobserved, are in ticket 65 item 2.
//
// DO NOT "improve" it. The baseline must be the stock configuration or the FRS sweep in
// issues/08 has no zero point, and psiegl's five deviations must not be adopted piecemeal
// -- see .scratch/phase0-preflight/issues/06 for what each of them decodes to.
//
// Header-only and free of ESP-IDF dependencies so it builds under env:native, where
// test/test_epd_core compares it against an independently written literal.

#ifndef EPD_INIT_LIST_H
#define EPD_INIT_LIST_H

#include <stdint.h>

// Format: command, length, data... repeated, terminated by EPD_INIT_END.
#define EPD_INIT_END 0xFF

// Line numbers are EPD_4in0e.cpp's, the vendor source named above.
static const uint8_t epd_init_list[] = {
    0xAA, 6, 0x49, 0x55, 0x20, 0x08, 0x09, 0x18, // CMDH   :127-133
    0x01, 1, 0x3F,                               // PWR    :135-136
    0x00, 2, 0x5F, 0x69,                         // PSR    :138-140
    0x05, 4, 0x40, 0x1F, 0x1F, 0x2C,             // BTST1  :142-146
    0x08, 4, 0x6F, 0x1F, 0x1F, 0x22,             // BTST3  :148-152
    0x06, 4, 0x6F, 0x1F, 0x17, 0x17,             // BTST2  :154-158
    // PFS :160-164 -- AND THE ONE BYTE THAT IS NOT THE VENDOR'S. The vendor's first byte is 0x00;
    // 0x03 below is M5GFX's. See the header. SPD1656 Rev 1.1 defines only A[5:4] (T_VDS_OFF) in
    // this byte and shows the rest as 0, so 0x03 sets bits documented as don't-care -- which makes
    // it *likely* inert and is not the same as having seen that it is.
    0x03, 4, 0x03, 0x54, 0x00, 0x44,             // PFS
    0x60, 2, 0x02, 0x00,                         // TCON   :166-168
    0x30, 1, 0x08,                               // PLL    :170-171  <- FRS. The whole project is this byte.
    0x50, 1, 0x3F,                               // CDI    :173-174
    0xE3, 1, 0x2F,                               // PWS    :182-183
    0x84, 1, 0x01,                               // T_VDCS :185-186
    EPD_INIT_END, EPD_INIT_END,
};

// TRES (0x61) is NOT part of the list above, and the two sources differ in WHERE it goes rather
// than in what it says. The vendor sends it inside the init sequence between CDI and PWS
// (EPD_4in0e.cpp:176-180); M5GFX sends it separately, immediately after the list and after another
// BUSY wait (Panel_ED2208.cpp:305-309), which is what this driver does. The payload is identical
// either way: width MSB, width LSB, height MSB, height LSB. 400 = 0x0190, 600 = 0x0258.
static const uint8_t epd_tres_payload[] = {0x01, 0x90, 0x02, 0x58};

// Two details of the stock refresh that the research document does not record, read first-hand from
// Panel_ED2208.cpp:316-344 and **confirmed against the vendor** at EPD_4in0e.cpp:94-116, where
// EPD_4IN0E_TurnOnDisplay() does the same thing in the same order. Here the two sources agree
// exactly, so this block no longer rests on M5GFX. The harness must reproduce them or the
// "stock baseline" is not stock.
//
//  1. BTST2 (0x06) is sent a SECOND time, between PON and DRF, and its last byte is
//     0x27 -- not the 0x17 used in the init list above. Same command, different payload.
//  2. DRF (0x12) and POF (0x02) each take a 0x00 data byte. They are not bare commands.
//
// The stock sequence in full:
//     PON(0x04) -> wait busy -> delay(200)
//     BTST2(0x06) 6F 1F 17 27 -> delay(200)
//     DRF(0x12) 00 -> wait busy
//     POF(0x02) 00 -> wait busy -> delay(200)
static const uint8_t epd_btst2_refresh_payload[] = {0x6F, 0x1F, 0x17, 0x27};

#endif // EPD_INIT_LIST_H
