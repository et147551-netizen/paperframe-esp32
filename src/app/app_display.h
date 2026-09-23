// One task owns the panel.
//
// Everything that wants a picture on the glass asks here: the web server ("display this
// now"), the slideshow (ticket 13), and the boot path. Nothing else calls
// epd_refresh_frame(), and nothing else holds the canvas.
//
// The reason it is a task with a request slot rather than a function call is NFR-5: a
// refresh takes 15.57 s on this unit and holds the SPI bus for all of it. An HTTP
// handler that rendered inline would hold a httpd worker for that long and the UI would
// stop answering -- so a request is posted, the handler returns immediately, and the UI
// polls app_display_state() to see what happened.
//
// A request that arrives while a refresh is running replaces any pending one and is
// taken when the panel is free. Requests are not queued deeper than that on purpose: if
// three photos are requested during one refresh, the last is the one anyone wants.

#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool busy;               // a refresh is in flight
    bool pending;            // a request is waiting for the panel
    char current[64];        // basename of what is on the glass, "" if nothing
    char last_error[48];     // "" when the last render succeeded
    uint32_t renders;        // completed refreshes since boot
    uint32_t failures;
    float last_total_ms;     // last refresh, panel time only
} app_display_state_t;

// Allocates the canvas and the packed frame in PSRAM and starts the task. The panel and
// the SPI bus must already be up (epd_init()).
esp_err_t app_display_init(uint8_t rotation);

// FR-5.2. Applies to the next render; call app_display_redraw() to re-render what is
// already shown.
void app_display_set_rotation(uint8_t rotation);

// Which palette photographs are quantised against; an epd_palette_id_t. Same contract as
// the rotation setter -- it applies to the next render, so a caller that wants the picture
// on the glass to change follows it with app_display_redraw(). Out-of-range ids are
// ignored, matching the rotation setter rather than falling back to the default.
void app_display_set_palette(uint8_t palette);

// Whether to classify each photograph and take its tone, range and dithering settings from the
// class -- epdoptimize's auto flow (src/core/epd_auto.h). Same contract again: it applies to the next
// render.
//
// **This overrides the filename rule documented below**, which is why the setting defaults off
// (app_settings.h). It also costs about half a megabyte of PSRAM the first time it runs, and
// nothing of internal RAM.
void app_display_set_auto_adjust(bool on);

// Whether the quantiser is Floyd-Steinberg error diffusion (src/core/epd_diffuse.h) rather than M5GFX's
// row-wise pair search. Same contract: it applies to the next render.
//
// Independent of the auto flow above -- see app_settings.h for why the two are separate settings.
// It does **not** override the nearest choice, from either the filename rule or the auto plan:
// upstream's `quantizationOnly` and FR-3.3's "Nearest" are the same request, and this setting is
// about how the *dithered* path dithers. Costs no memory and is faster than what it replaces.
void app_display_set_dither_diffuse(bool on);

// Whether a picture whose orientation disagrees with the frame's is turned 90 degrees so that
// it fills the panel (epd_fit_wants_rotate(), and app_settings.h for the threshold argument).
// Applies to the next render; it changes nothing on the card and nothing about the `rotation`
// setting, which stays what the frame is hung as.
void app_display_set_auto_rotate(bool on);

// Renders `path` (a file under /data) and refreshes. Returns immediately.
//
// The quantiser is chosen from the filename: a name beginning "imageN" renders nearest,
// anything else dithered (FR-3.3, local_photo_slideshow.cpp:468-470).
esp_err_t app_display_request(const char *path);

// Re-renders whatever is currently displayed, for an orientation change.
esp_err_t app_display_redraw(void);

// Clears the panel to white and forgets the current image (FR-5.7, an empty directory).
esp_err_t app_display_request_blank(void);

// One full-screen flat in a NATIVE PANEL COLOUR -- ticket 68's maintenance course. `colour_index` is
// an EPD_COLOR_* from src/panel/epd_cmds.h; anything epd_color_valid() rejects, index 4 (orange)
// included, returns ESP_ERR_INVALID_ARG here rather than failing a refresh later.
//
// **This is the one render path that does not go through the canvas**, which is what the owner
// asked for: the palette, the quantiser, the dither and the auto flow are all bypassed, so the
// panel is driven with the index given and a person looking at the glass is looking at the ink
// rather than at a colour-reduction decision. Like the blank, it forgets the current image, so a
// rotation change does not redraw a flat.
esp_err_t app_display_request_flat(uint8_t colour_index);

// FR-6.3's pairing screen, which since ticket 66 is a connect card: two QR codes and the same
// information in words.
//
// **The text is why this struct exists.** The screen used to be two unlabelled QR squares, which
// a phone can read and a person cannot -- so an owner on a PC had no way to learn the access
// point's password, the frame's address or the pairing code, and could not get in at all. Every
// field below is a string the caller has already formatted; this module keeps knowing nothing
// about Wi-Fi or authentication and draws what it is given.
//
// Empty fields are skipped rather than drawn blank, so a frame with no station link simply has one
// fewer line.
typedef struct {
    char join[160];   // WIFI:S:<ssid>;T:WPA;P:<pass>;; -- the QR a phone scans to join
    char url[160];    // http://<ap-ip>/?t=<token> -- the QR that pairs a browser in one tap
    char ssid[33];    // the access point's name, in words
    // Its password, in words. **8 Crockford base32 characters since 2026-09-19** -- the owner
    // asked for the shortest typeable one (ticket 66) -- so it is uppercase and it must still be
    // drawn verbatim: a WPA2 passphrase is compared byte-for-byte. 17 was the old 16-hex form.
    char ap_pass[9];
    char code[12];    // the pairing code, "XXXX-XXXX", for whoever has no camera
    // "PAPERFRAME-A1B2C3.LOCAL", the name a browser can be given. 72 = APP_SETTINGS_NAME_SIZE plus
    // ".local" and room to spare, spelled as a number because this header must not depend on
    // app_settings.h -- the same arithmetic app_server.c's host_ok() does.
    char host[72];
    char ap_ip[16];   // the address over the access point
    char sta_ip[16];  // the address over the house LAN, "" when the station is down
} app_display_card_t;

// Draws the card and refreshes. Returns immediately; `card` is copied.
//
// **What goes on this panel stays on it with the power off.** E Ink holds its image, so a frame
// that is switched off and carried away still shows whatever was last drawn -- and here that is an
// access-point password, an API token and a pairing code. Whoever calls this owns clearing it
// again; see ticket 30 and frame_main.c's clear_pairing_if_due(), and note that ONE REFRESH IS NOT
// A CLEAR on this panel.
esp_err_t app_display_request_card(const app_display_card_t *card);

void app_display_state(app_display_state_t *out);

// True while a refresh is in flight. The slideshow uses it to avoid stacking requests,
// and ticket 15 will use it to block a power-off mid-refresh.
bool app_display_busy(void);

// Blocks until the panel is idle or `timeout_ms` passes. Returns false on timeout.
bool app_display_wait_idle(uint32_t timeout_ms);

#endif // APP_DISPLAY_H
