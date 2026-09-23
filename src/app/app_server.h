// The HTTP API of FR-3 and M5Stack's index.html.
//
// Ticket .scratch/digital-frame/issues/11. The route table and every payload shape are
// the shipping firmware's (app_server.cpp:1361-1383, app_server.h:17-44) because the UI
// is a port: 139 KB of working page, embedded gzipped from assets/index.html.
// If a response shape here drifts from the reference, the page silently stops working.
//
// Two deliberate departures:
//
//   * There is no Ezdata. GET /api/modes advertises mode_1 alone, POST /api/mode/switch
//     refuses anything else, and there is no /api/mode/mode_2/config route -- ticket 22
//     cut the cloud mode out of the page too, so nothing asks for one.
//   * POST /api/system/reset returns 501. FR-8.2 ends in a PM1 power-off, and this
//     board does not come back from one without a hand on the power button, so it is
//     not implemented while -DBOARD_NO_POWER_OFF is set (ticket 16 owns it).

#ifndef APP_SERVER_H
#define APP_SERVER_H

#include "esp_err.h"

#include "board_storage.h"

// Starts httpd on port 80. app_settings, board_wifi, board_storage and app_display must
// all be up first.
esp_err_t app_server_start(void);

// Hands POST /api/storage/rescan the caller's media watch -- the SAME object the 10 s poll
// uses, not a copy, because the route's whole job is to clear a fallback lock that the poll
// will then act on. Without this the route answers 503. See h_storage_rescan().
void app_server_set_storage_watch(board_storage_watch_t *w);

esp_err_t app_server_stop(void);

// Milliseconds since the last request was answered, for FR-7.2's idle timer (a client
// talking to the UI must block a power-off). UINT32_MAX before the first request.
uint32_t app_server_ms_since_activity(void);

#endif // APP_SERVER_H
