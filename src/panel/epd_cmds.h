// SPD1656-family command constants, shared by every panel this project drives.
//
// Transcribed from refs/E-Ink_Spectra_E6/epdflash/include/EPD_commands.h, which is
// the reverse-engineered command spec and the only description of this controller's
// register set that exists. The controller is SPD1656-*family* but not an SPD1656;
// see docs/research/reference-source-review.md §11.1 before treating a SPD1656 bit
// definition as authoritative here.
//
// This file was `epd_regs.h` and carried the panel's geometry too. The geometry moved to
// epd_geom.h, which selects a per-panel header, because it is the one thing here that is
// NOT shared: the EL040EF1 is 400x600 and the reTerminal E1002's ED2208-GCA is 800x480.
// What stayed is shared by construction -- the command numbers are the controller
// family's, and the six palette indices are the ink's.
//
// Header-only and free of ESP-IDF dependencies so it builds under env:native too.

#ifndef EPD_CMDS_H
#define EPD_CMDS_H

// Commands used across Spectra 6 4.0" / 7.3" / 13.3" and ACeP 5.65".
#define EPD_CMD_PSR 0x00   // Panel setting
#define EPD_CMD_PWR 0x01   // Power setting
#define EPD_CMD_POF 0x02   // Power off
#define EPD_CMD_PFS 0x03   // Power off sequence setting
#define EPD_CMD_PON 0x04   // Power on
#define EPD_CMD_BTST1 0x05 // Booster soft start 1
#define EPD_CMD_BTST2 0x06 // Booster soft start 2
#define EPD_CMD_DSLP 0x07  // Deep sleep
#define EPD_CMD_BTST3 0x08 // Booster soft start 3
#define EPD_CMD_DTM 0x10   // Data start transmission
#define EPD_CMD_DRF 0x12   // Display refresh
#define EPD_CMD_AUTO 0x17  // Auto sequence
#define EPD_CMD_PLL 0x30   // PLL control -- this is FRS, the register the project is about
#define EPD_CMD_TSC 0x40   // Temperature sensor calibration (read)
#define EPD_CMD_TSE 0x41   // Temperature sensor enable / offset
#define EPD_CMD_CDI 0x50   // VCOM and data interval setting
#define EPD_CMD_TCON 0x60  // TCON setting
#define EPD_CMD_TRES 0x61  // Resolution setting
#define EPD_CMD_FLG 0x71   // Get status flag
#define EPD_CMD_T_VDCS 0x84
#define EPD_CMD_PWS 0xE3 // Power saving
#define EPD_CMD_CMDH 0xAA

// AUTO (0x17) payloads. M5GFX does not use AUTO; it issues PON / DRF / POF
// separately. Evaluated in issues/09.
#define EPD_AUTO_PON_DRF_POF 0xA5
#define EPD_AUTO_PON_DRF_POF_DSLP 0xA7

// Inert on the M5Paper Color: the panel's external I2C temperature sensor pins
// (J5 24 TSCL / 25 TSDA) are not connected on this board, confirmed by reading the
// schematic (§12.4). TSW and TSR are passthroughs to a chip that is not there.
// Temperature is read through TSC (0x40) only, with TSE bit 7 = 0.
#define EPD_CMD_TSW 0x42
#define EPD_CMD_TSR 0x43

// Tested and confirmed NOT working on the 4.0" panel (EPD_commands.h:61-68).
// Listed so they are not rediscovered.
//   PBC 0x44, REV 0x70, CRC 0x72, ROTP 0xA2

// Spectra 6 palette indices, from refs/E-Ink_Spectra_E6/epdflash/include/common.h:21-30
// and refs/M5GFX/src/lgfx/v1/panel/Panel_ED2208.cpp:32-38, which agree.
//
// Index 4 is ORANGE and is valid on NEITHER panel this project drives. It was once
// annotated "7.3\" only", which invited the guess that the reTerminal E1002's 7.3" panel
// has a seventh colour; it does not. refs/esp32-photoframe/components/epaper/include/
// epaper.h defines exactly these six for its ED2208-GCA driver and skips 0x4. So
// epd_color_valid()'s rejection of index 4 is right on both, and EPD_PALETTE_COUNT stays 6.
//
// The handoff document's "4=blue, 5=green" mapping is wrong and would render blue as
// invalid and green as blue (§1.2).
#define EPD_COLOR_BLACK 0x0
#define EPD_COLOR_WHITE 0x1
#define EPD_COLOR_YELLOW 0x2
#define EPD_COLOR_RED 0x3
#define EPD_COLOR_ORANGE_UNUSED 0x4
#define EPD_COLOR_BLUE 0x5
#define EPD_COLOR_GREEN 0x6

#endif // EPD_CMDS_H
