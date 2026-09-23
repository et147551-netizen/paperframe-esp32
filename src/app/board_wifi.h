// AP+STA networking, the captive portal and mDNS.
//
// Ticket .scratch/digital-frame/issues/10; satisfies FR-2.1 through FR-2.5. The
// behaviour is the shipping firmware's, reproduced rather than redesigned
// (refs/M5PaperColor-UserDemo/main/apps/app_manager/app_manager.cpp:73-102, 651-673).
//
// Two of those behaviours look like bugs and are not:
//
//   * The access point is OPEN. So is the upload endpoint behind it. That is the
//     shipping security posture, inherited knowingly (see the note in the ticket).
//   * AP auto-off exists but is DISABLED by default. A frame that silently stops
//     answering is worse than one that leaves an open AP up.
//
// One thing here IS a fix to the upstream: mDNS starts at boot on every interface,
// not only once a station joins the softAP -- which upstream meant it never worked
// in station-only operation (FR-2.3).

#ifndef BOARD_WIFI_H
#define BOARD_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Matches the shipping firmware's cache entry, which is what the web UI renders
// (app_server.h:17-21).
typedef struct {
    char ssid[33];
    int rssi;
    bool secure;
} board_wifi_network_t;

typedef struct {
    bool sta_connected;
    char ip[16];           // station IP, "" when not connected
    char ssid[33];         // the SSID we are connected to, "" when not
    int rssi;              // dBm of the AP we are joined to, 0 when not connected
    char last_error[32];   // last station failure, "" when none
    char ap_ssid[33];
    char ap_ip[16];
    int ap_clients;
} board_wifi_status_t;

// Brings up netif, the event loop, AP+STA, the captive-portal DNS responder and mDNS,
// and starts the station retry task. `device_name` is the mDNS hostname; it must
// already be normalised (app_settings_normalize_device_name()).
//
// If `ssid` is non-empty a station connect is attempted immediately; a failure is not
// an error here, the retry loop owns it (FR-2.4).
esp_err_t board_wifi_init(const char *device_name, const char *ssid, const char *password);

// `PaperFrame-XXXXXX` from the low three bytes of the MAC ("PaperColor-" before 2026-09-17).
// Available before init.
void board_wifi_ap_ssid(char *out, size_t size);

// The six hex digits that identify this unit on their own: the low three bytes of the BASE
// (station) MAC, which is the pair the AP name and every label on the bench are written from.
// `upper` because the two consumers disagree about case and neither may guess -- the SSID is
// uppercase, and `device_name` is normalised to [a-z0-9-] so it cannot be. Needs `size` >= 7.
//
// It exists so that the AP name and the default device name cannot drift apart: they are the same
// unit, and a bench with two frames on it tells them apart by these six characters.
void board_wifi_unit_id(char *out, size_t size, bool upper);

void board_wifi_status(board_wifi_status_t *out);

// Saves nothing -- persistence is app_settings'. Returns immediately; poll
// board_wifi_status() for the outcome.
//
// The connect itself is performed by the retry task, within half a second, and NOT before
// this returns. That is for the sake of the caller: it is an HTTP handler whose reply may
// have to travel over the station link this is about to drop, and it could not answer at
// all while the disconnect happened first (ticket 10, measured as curl http=000).
esp_err_t board_wifi_connect(const char *ssid, const char *password);

// FR-2.4: drops the station link and leaves the AP up. Also stops the retry loop, so a
// deliberate disconnect is not undone three seconds later.
esp_err_t board_wifi_disconnect_keep_ap(void);

// Blocking scan, up to `max` results, newest scan replaces the previous one. Pauses the
// retry loop for the duration: a connect attempt in flight makes esp_wifi_scan_start()
// fail and the UI shows an empty list (app_server.cpp:305-311).
esp_err_t board_wifi_scan(board_wifi_network_t *out, size_t max, size_t *count);

// Restarts mDNS under a new hostname, for a device_name change over HTTP.
esp_err_t board_wifi_set_hostname(const char *device_name);

// Ticket 30 step 2: puts WPA2-PSK on the access point, which comes up OPEN.
//
// Called once, after app_auth_init(), because that is where the password comes from and because
// esp_fill_random() is only a true RNG once the radio is up -- so there is a window of a second or
// so at every boot in which the AP is open. Nothing is listening on it yet: app_server_start() has
// not run.
//
// A password outside WPA2's 8-63 characters is REFUSED and the AP stays open, which is the safe
// direction: an AP nobody can join is unrecoverable on a frame with no station link, and this
// device's whole recovery ladder assumes FR-2.1's access point is there.
esp_err_t board_wifi_ap_secure(const char *password);

// Whether the AP is encrypted right now -- the radio's state, not a build flag's.
//
// **app_server.c gates the token-bearing captive-portal redirect on this**, and that is the whole
// reason it exists: handing the API token to whoever joins an OPEN access point is what
// docs/agents/web-api.md warns against, and a runtime question cannot be got wrong by a flag that
// reached one file and not another (docs/agents/build-system.md's third trap).
bool board_wifi_ap_secured(void);

// --------------------------------------------------------------- the access point's window
//
// Ticket 67. **The access point is not a permanent surface any more: it is raised for a window and
// it takes itself down again.** The operator's reasoning, 2026-09-19: the AP exists for first setup
// and for pushing photographs onto a frame that has no home Wi-Fi, both of which are things somebody
// does while standing at the frame. A radio that is up for thirty minutes a month can have its 4-way
// handshake captured for thirty minutes a month, which is the only lever this project has on the
// 40-bit passphrase (`app_auth.h`), and a radio that is down draws no power.
//
// **This is FR-2.5 turned on and inverted.** The shipping firmware already had "shut the AP down
// after 10 minutes with no clients, while the station link is up", compiled out by default. What is
// below drops the station-link condition -- a frame with no home Wi-Fi is exactly the case the AP is
// for, so it must close there too -- and adds the half FR-2.5 has no notion of: the AP is DOWN until
// something asks for it.
//
// **Two things raise it, and both mean a person is at the frame:** the 5 s button hold that draws the
// connect card, and a boot on a device that has never paired (ticket 66's `paired_once`), which is
// first setup and cannot ask for a button press it has no way to describe.
//
// The recovery ladder is unchanged in kind and worth re-reading with this in mind: the AP is still
// the way back into a frame whose credentials never associate, and it is still reached by physical
// access -- now a button rather than a radio that was already on.

// How long a window lasts. Overridable so a run can watch a whole open-and-close inside one capture.
#ifndef FRAME_AP_WINDOW_MS
#define FRAME_AP_WINDOW_MS (30u * 60u * 1000u)
#endif

// **A client associated past the deadline holds the window open**, because cutting the radio under
// somebody's upload is worse than the exposure of a few more minutes -- and then this is the bound
// on how long a forgotten phone can hold it. Four windows.
#ifndef FRAME_AP_WINDOW_CAP_MS
#define FRAME_AP_WINDOW_CAP_MS (4u * FRAME_AP_WINDOW_MS)
#endif

// Raises the AP if it is down and (re)starts its window at the full length. Re-applies the AP
// config, so an AP that comes back is WPA2 again if board_wifi_ap_secure() ever succeeded -- the one
// thing that must not be forgotten across a close.
esp_err_t board_wifi_ap_window_open(void);

// Takes it down now, whatever the window says. The station link is untouched.
esp_err_t board_wifi_ap_close(void);

// Whether the radio is carrying an AP right now. Unlike board_wifi_ap_secured(), a false here means
// there is nothing to join at all.
bool board_wifi_ap_is_up(void);

// Seconds left before the AP closes itself: 0 when it is down, and the remaining time otherwise --
// which may keep resetting while a client is associated, up to FRAME_AP_WINDOW_CAP_MS from the open.
uint32_t board_wifi_ap_window_left_s(void);

#endif // BOARD_WIFI_H
