#include "board_usb_msc.h"

#include "esp_system.h"

#if defined(BOARD_M5PAPER_COLOR)

#include <stdio.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

#include "board_buttons.h"
#include "board_led.h"
#include "board_storage.h"

// Any value a cold RTC RAM is unlikely to hold. Cleared on every read, so it arms one boot.
#define USB_MSC_MAGIC 0x55534244u  // "USBD"

static RTC_NOINIT_ATTR uint32_t s_request;

// No host within this long after the button was pressed means there is no cable, or no PC at
// the other end of it; a frame that waited for ever would look dead.
#define ENUMERATE_TIMEOUT_MS 60000
// How long "not mounted, or suspended" must hold before it counts as the cable going away. A
// host briefly suspends and resumes during enumeration, and one bad sample must not end the mode.
#define GONE_DEBOUNCE_MS 2000
#define POLL_MS 100

static volatile bool s_button;

static void on_button(board_button_t button, board_button_event_t event, uint32_t held_ms)
{
    (void)button;
    (void)held_ms;
    if (event == BOARD_BUTTON_CLICK) {
        s_button = true;
    }
}

// **The PHY selection does not come back by itself.** tinyusb_driver_install() maps the internal
// PHY to USB-OTG through RTC_CNTL_USB_CONF (usb_wrap_ll_phy_enable_external()), an RTC-domain
// register that esp_restart() leaves alone -- so without this the next boot's console, and
// esptool's auto-reset, would stay on a controller that nothing drives until the power button.
static void phy_to_serial_jtag(void)
{
    usb_serial_jtag_ll_phy_enable_external(false);
}

bool board_usb_msc_pending(void)
{
    const bool armed = s_request == USB_MSC_MAGIC;
    s_request = 0;
    phy_to_serial_jtag();
    return armed && esp_reset_reason() == ESP_RST_SW;
}

esp_err_t board_usb_msc_request(void)
{
    s_request = USB_MSC_MAGIC;
    printf("# usb msc: restarting into USB drive mode\n");
    fflush(stdout);
    esp_restart();
    return ESP_OK;
}

static void leave(const char *why)
{
    // Printed for a capture on a UART, not for COM11: the console's PHY is the drive's by now.
    printf("# usb msc: leaving (%s)\n", why);
    fflush(stdout);
    tinyusb_driver_uninstall();
    phy_to_serial_jtag();
    esp_restart();
}

void board_usb_msc_run(void)
{
    printf("# usb msc: USB drive mode, media=%s -- COM11 goes away now and comes back on exit\n",
           board_storage_get_media() == BOARD_STORAGE_MEDIA_SD ? "sd" : "flash");
    fflush(stdout);

    board_buttons_init(on_button);
    board_led_set(BOARD_LED_IDLE);

    esp_err_t err = board_storage_hand_to_usb();
    if (err != ESP_OK) {
        printf("# usb msc: hand-over failed: %s\n", esp_err_to_name(err));
        leave("hand-over failed");
    }
    const tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        printf("# usb msc: driver install failed: %s\n", esp_err_to_name(err));
        leave("driver install failed");
    }

    const int64_t t0 = esp_timer_get_time();
    bool was_mounted = false;
    int64_t gone_since = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        const int64_t now = esp_timer_get_time();

        if (s_button) {
            leave("button");
        }
        const bool here = tud_mounted() && !tud_suspended();
        if (here) {
            was_mounted = true;
            gone_since = 0;
        }
        if (!was_mounted) {
            if (now - t0 > (int64_t)ENUMERATE_TIMEOUT_MS * 1000) {
                leave("no host");
            }
            continue;
        }
        // tud_msc_start_stop_cb() mounts the volume back to the application on an eject
        // (esp_tinyusb's tinyusb_msc.c), so ownership returning IS the eject.
        if (!board_storage_host_owns()) {
            leave("ejected");
        }
        if (!here) {
            if (!gone_since) {
                gone_since = now;
            } else if (now - gone_since > (int64_t)GONE_DEBOUNCE_MS * 1000) {
                leave("host gone");
            }
        }
    }
}

#else  // no USB to the connector on this board

bool board_usb_msc_pending(void)
{
    return false;
}

esp_err_t board_usb_msc_request(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void board_usb_msc_run(void)
{
    esp_restart();
}

#endif
