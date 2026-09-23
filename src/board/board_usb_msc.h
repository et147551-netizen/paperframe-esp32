// USB drive mode: /data handed to a PC over the USB-C port as a mass-storage device.
//
// Ticket .scratch/digital-frame/issues/09 (FR-4.4). **Entered and left by a restart, never
// switched live**, and that is the whole design:
//
//   * The M5Paper Color's USB-C has ONE internal PHY. tinyusb_driver_install() takes it from
//     USB-Serial-JTAG, so COM11's console and esptool's auto-reset disappear for as long as the
//     drive is up. A mode that is only up when somebody asked for it keeps them the rest of the
//     time (operator, 2026-09-22: on demand, from a Web UI button, not always-on).
//   * While the host owns the volume NOTHING of the application runs -- no slideshow, no mirror,
//     no upload, no Wi-Fi. So there is no race between the host's FAT and ours to decide: it
//     cannot happen, and board_storage_prepare_access()'s reclaim never yanks the drive from a
//     host mid-write.
//
// The request is a word in RTC no-init memory, which survives esp_restart() and NOT a power
// cycle -- so pressing the power button always comes back as a normal frame.
//
// The reTerminal E1002 cannot do this at all: its USB-C is a CH340C on UART0
// (docs/board-pinmap.md), so the SoC's USB never reaches the connector. Everything here is a
// stub there and BOARD_HAS_USB_MSC is 0.

#ifndef BOARD_USB_MSC_H
#define BOARD_USB_MSC_H

#include <stdbool.h>

#include "esp_err.h"

#include "board.h"

#if defined(BOARD_M5PAPER_COLOR)
#define BOARD_HAS_USB_MSC 1
#else
#define BOARD_HAS_USB_MSC 0
#endif

// Called first thing in app_main(). True when the previous boot asked for USB drive mode AND this
// boot is the software reset it asked with; the request is consumed either way, so a crash in USB
// mode cannot loop back into it.
//
// On the M5Paper Color this also hands the PHY back to USB-Serial-JTAG, whatever the reset was:
// the selection lives in an RTC register that a software reset does not clear, so without this a
// panic out of USB mode would leave COM11 dead until a power cycle.
bool board_usb_msc_pending(void);

// Records the request and restarts. Does not return on a board that has the mode;
// ESP_ERR_NOT_SUPPORTED on one that does not. The caller is responsible for having quiesced
// whatever was writing /data -- see h_storage_usb() in app_server.c.
esp_err_t board_usb_msc_request(void);

// Hands the already-mounted /data volume to the host and waits for the way out: the host
// ejecting it, the cable going away, a button press, or no host at all within a minute. Then
// restarts into a normal boot. Never returns.
void board_usb_msc_run(void);

#endif // BOARD_USB_MSC_H
