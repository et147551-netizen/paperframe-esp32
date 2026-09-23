// The playback engine: automatic advance, manual navigation, and an index that
// survives a power cut.
//
// Ticket .scratch/digital-frame/issues/13; satisfies FR-5.3 through FR-5.8. Ported from
// refs/M5PaperColor-UserDemo/main/apps/local_photo_slideshow/local_photo_slideshow.cpp,
// whose structure is sound and whose edge cases are earned.
//
// Four things here are load-bearing and easy to lose in a rewrite:
//
//   * Pending and current index are SEPARATE. A navigation sets pending and waits a
//     1 s settle window; ten rapid "next" presses cost one refresh, not ten. If the
//     settled pending index equals what is displayed, the refresh is cancelled
//     outright (:426-441).
//   * NO_PHOTO is 0xFFFF, not 0. "Nothing is displayed" is a different state from
//     "image 0 is displayed", and every wrap-around checks for it. An empty directory
//     is normal (FR-5.7).
//   * The index lives in the RX8130's battery-backed RAM, not NVS, so it survives the
//     power cut that ticket 15 will make routine (FR-5.6).
//   * The directory is re-scanned before every navigation, because the web UI can
//     delete the image being displayed (FR-5.8).

#ifndef APP_SLIDESHOW_H
#define APP_SLIDESHOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define APP_SLIDESHOW_NO_PHOTO 0xFFFFu

typedef struct {
    uint16_t current_index;
    uint16_t pending_index;
    size_t count;            // photos found by the last scan
    bool refresh_pending;    // inside the settle window
    char current_name[64];
    // The image the settle window is about to draw, which is NOT always current_name -- and the
    // on-demand cache needs it (ticket 37 Phase 3): evicting the photograph that is one second
    // from being drawn produces a failed render for no reason at all.
    char pending_name[64];
} app_slideshow_state_t;

// Scans /data, restores the stored index and displays that image. Starting with an
// empty directory is not an error.
esp_err_t app_slideshow_start(void);

// Drives the settle window and the automatic advance. Call from the application loop;
// it returns immediately when there is nothing to do.
void app_slideshow_update(void);

// FR-5.5. Both re-scan first, then set the pending index and start the settle window.
void app_slideshow_next(void);
void app_slideshow_prev(void);

// Jump straight to a name, for POST /api/photos/display. Bypasses the settle window --
// a click in the UI is not a held button.
esp_err_t app_slideshow_show_name(const char *name);

// Tells the slideshow that the displayed image may no longer exist (a delete over
// HTTP). Re-scans and clamps at the next update rather than mid-request.
void app_slideshow_invalidate(void);

void app_slideshow_get_state(app_slideshow_state_t *out);

#endif // APP_SLIDESHOW_H
