#include "board_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"

#include "dns_server.h"

static const char *TAG = "wifi";

#define AP_CHANNEL 6
#define AP_MAX_CONN 4
#define RETRY_INTERVAL_MS 10000
#define RETRY_TICK_MS 500
#define SCAN_TIMEOUT_MS 6000

// FR-2.5's original shape: shut the AP after N minutes with no client, *while the station link is
// up*, off by default. **Superseded by ticket 67's window** (board_wifi.h), which closes the AP
// whether or not there is a station link and, unlike this, does not leave it closed for ever. Kept
// compiled out rather than deleted only so the requirement's own form stays readable beside the
// deviation; `-DFRAME_AP_ALWAYS_ON` is the way back to an AP that never closes.
#ifndef WIFI_AP_AUTO_OFF_ENABLE
#define WIFI_AP_AUTO_OFF_ENABLE 0
#endif
#ifndef WIFI_AP_AUTO_OFF_TIMEOUT_MIN
#define WIFI_AP_AUTO_OFF_TIMEOUT_MIN 10
#endif

static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static dns_server_handle_t s_dns;

static SemaphoreHandle_t s_mutex;
static bool s_sta_connected;
static bool s_retry_paused;
static bool s_retry_enabled;   // false after a deliberate disconnect
// Ticks left before retry_task() applies new credentials. A COUNTDOWN and not a flag: the
// point is to let the caller's HTTP reply leave first, and a flag consumed on the next tick
// gives a delay of anywhere from 0 to RETRY_TICK_MS depending on where the request landed in
// the tick. Measured 2026-09-10 over nine POSTs -- seven answered in 85-233 ms, and the two
// that did not sent their headers and then lost the body, with the disconnect logged in the
// same millisecond as the connect. Two ticks makes the delay 500-1000 ms rather than 0-500.
#define APPLY_DELAY_TICKS 2
static uint8_t s_apply_ticks;
static char s_ip[16];
static char s_ssid[33];        // the SSID the retry loop targets
static char s_password[64];
static char s_last_error[32];
static char s_ap_ssid[33];
// Whether the access point actually carries WPA2 right now. Ticket 30 step 2, and it is a fact
// about the radio rather than a build flag on purpose: app_server.c asks this before it will put
// the API token in a redirect, and "was FRAME_AP_OPEN defined" and "is the AP encrypted" are two
// questions that a half-applied config can answer differently. The one that must gate the token is
// this one.
static bool s_ap_secured;
// The password board_wifi_ap_secure() accepted, kept so an AP that comes back after a window
// closes comes back ENCRYPTED. Ticket 67: without this the re-open would silently be an open AP,
// which is the one state `app_server.c` withholds the API token on and exactly the kind of thing
// that reads as working.
static char s_ap_password[64];

// The window (ticket 67). `s_ap_up` is this module's own belief rather than a read of the radio,
// because esp_wifi_get_mode() cannot say whether WE raised the AP or the driver is mid-transition,
// and every decision below wants the former.
static bool s_ap_up;
static int64_t s_ap_deadline_us;  // when the window ends, absent a client holding it open
static int64_t s_ap_cap_us;       // and the bound on how long a client may hold it

// Defined below, beside the rest of the window; called from retry_task(), which is above it.
static void service_ap_window(void);

static void lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

static void copy_str(char *dst, const char *src, size_t size)
{
    if (!dst || size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= size) {
        n = size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void board_wifi_ap_ssid(char *out, size_t size)
{
    // The BASE (station) MAC, not the softAP's. They differ by one in the last byte, and the
    // shipping firmware names the AP from the base MAC -- so the M5Paper Color's AP ends in
    // A1B2C3, matching every document and label that mentions that unit. Reading
    // ESP_MAC_WIFI_SOFTAP here produced ...43E21D on hardware.
    //
    // **The prefix was "PaperColor-" until 2026-09-17** and is the generic product name now, the
    // project having two boards. A phone that had joined the old SSID does not know the new one:
    // rejoining needs the panel's QR again (ticket 30 step 2). Documents written before the rename
    // name the old SSID, and the MAC suffix is what identifies a unit across it.
    char id[7];
    board_wifi_unit_id(id, sizeof(id), true);
    snprintf(out, size, "PaperFrame-%s", id);
}

void board_wifi_unit_id(char *out, size_t size, bool upper)
{
    if (!out || size < 7) {
        if (out && size) {
            out[0] = '\0';
        }
        return;
    }
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, size, upper ? "%02X%02X%02X" : "%02x%02x%02x", mac[3], mac[4], mac[5]);
}

// The reason codes worth naming. Anything else is reported by number, because a
// number that can be looked up beats a label that is wrong.
static const char *disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "auth failed";
    case WIFI_REASON_NO_AP_FOUND:
        return "no such network";
    case WIFI_REASON_ASSOC_FAIL:
        return "association failed";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "beacon timeout";
    default:
        return NULL;
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *e = (const wifi_event_sta_disconnected_t *)data;
            lock();
            s_sta_connected = false;
            s_ip[0] = '\0';
            const char *name = disconnect_reason_name(e->reason);
            if (name) {
                copy_str(s_last_error, name, sizeof(s_last_error));
            } else {
                snprintf(s_last_error, sizeof(s_last_error), "disconnected (%u)", e->reason);
            }
            unlock();
            ESP_LOGW(TAG, "sta disconnected, reason=%u", e->reason);
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
        case WIFI_EVENT_AP_STADISCONNECTED:
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        lock();
        s_sta_connected = true;
        s_last_error[0] = '\0';
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        unlock();
        ESP_LOGI(TAG, "sta got ip %s", s_ip);
    }
}

// FR-2.2: DHCP option 114, so a phone that asks where the portal is gets told. The
// server has to be stopped to set an option and started again afterwards
// (app_server.cpp:1335-1343).
static void set_captive_portal_uri(void)
{
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) != ESP_OK) {
        return;
    }
    char uri[32];
    snprintf(uri, sizeof(uri), "http://" IPSTR, IP2STR(&ip_info.ip));

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(s_ap_netif));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(
        s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(s_ap_netif));
    ESP_LOGI(TAG, "captive portal uri %s", uri);
}

static esp_err_t start_mdns(const char *device_name)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        return err;
    }
    err = mdns_hostname_set(device_name);
    if (err != ESP_OK) {
        return err;
    }
    // The human-readable name a service browser shows. Generic since 2026-09-17: one binary per
    // board, and this string is in the half that both boards share.
    mdns_instance_name_set("PaperFrame digital frame");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mdns %s.local", device_name);
    return ESP_OK;
}

esp_err_t board_wifi_set_hostname(const char *device_name)
{
    if (!device_name || !device_name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    return mdns_hostname_set(device_name);
}

// The one place a station connect is configured and started. `drop_first` is for a
// change of network, where the old association has to go before the new one can be
// attempted; a plain retry has nothing to drop.
static void sta_connect_now(const char *ssid, const char *pass, const char *why, bool drop_first)
{
    wifi_config_t cfg = {0};
    copy_str((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    copy_str((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));

    // Without these three, an AP that leads with WPA3-Personal takes the association
    // and then times out in the 4-way handshake: measured on 2026-09-04 as
    // `reason=204` (HANDSHAKE_TIMEOUT) exactly 10 s after `assoc -> run`, which looks
    // like a wrong password and is not one. The same AP then associated first try with
    // these set, and negotiated WPA2-PSK. Which of the three is load-bearing here is
    // NOT separated -- all three went in together and the fix was not re-bisected.
    //
    // They used to be set here and NOT in board_wifi_connect(), so a network chosen in
    // the Web UI missed them and could only associate on the retry ten seconds later.
    // One function now, so that cannot drift apart again.
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (drop_first) {
        esp_wifi_disconnect();
    }
    const esp_err_t err = esp_wifi_connect();
    ESP_LOGI(TAG, "sta connect to \"%s\" (%s): %s", ssid, why, esp_err_to_name(err));
}

// One task owns reconnection. It ticks rather than sleeping the whole interval so a
// pause (a scan) or a deliberate disconnect takes effect promptly.
static void retry_task(void *arg)
{
    (void)arg;
    uint32_t since_try_ms = RETRY_INTERVAL_MS;  // first attempt happens immediately

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(RETRY_TICK_MS));

        lock();
        const bool want = s_retry_enabled && !s_retry_paused && !s_sta_connected && s_ssid[0];
        // The countdown only runs when it could be acted on: a scan pauses the loop and makes
        // esp_wifi_connect() fail, so a pending apply waits the scan out rather than being
        // spent on a tick that cannot use it.
        bool apply = false;
        if (s_apply_ticks && !s_retry_paused) {
            apply = (--s_apply_ticks == 0);
        }
        char ssid[33];
        char pass[64];
        copy_str(ssid, s_ssid, sizeof(ssid));
        copy_str(pass, s_password, sizeof(pass));
        unlock();

        // Ticket 67's window. **This replaced FR-2.5's own auto-off block, which was compiled out
        // here and has been deleted rather than left beside it**: that block called
        // esp_wifi_set_mode(WIFI_MODE_STA) directly, so enabling it would have taken the AP down
        // behind s_ap_up's back and left every question about the radio answered wrongly. One owner
        // for the AP's state or none.
        service_ap_window();

        // Credentials handed over by board_wifi_connect(), applied here rather than in its
        // caller: the caller is an HTTP handler that has to answer over the very link this
        // drops. Checked BEFORE `want`, which requires !s_sta_connected and would otherwise
        // skip the case the whole thing is for -- changing to another network while still
        // associated with the old one.
        if (apply && ssid[0]) {
            since_try_ms = 0;
            sta_connect_now(ssid, pass, "requested", true);
            continue;
        }

        if (!want) {
            since_try_ms = RETRY_INTERVAL_MS;
            continue;
        }

        since_try_ms += RETRY_TICK_MS;
        if (since_try_ms < RETRY_INTERVAL_MS) {
            continue;
        }
        since_try_ms = 0;

        sta_connect_now(ssid, pass, "retry", false);
    }
}

esp_err_t board_wifi_init(const char *device_name, const char *ssid, const char *password)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event,
                                                        NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event,
                                                        NULL, NULL));

    board_wifi_ap_ssid(s_ap_ssid, sizeof(s_ap_ssid));

    // **STA, not APSTA: the access point is NOT raised at boot** (ticket 67, and a deliberate
    // deviation from FR-2.1's "start in AP+STA mode"). It is raised by
    // board_wifi_ap_window_open(), whose only caller is the connect card -- the 5 s button hold,
    // and a first boot on a device that has never paired. Nothing here sets the AP config either:
    // esp_wifi_set_config(WIFI_IF_AP, ...) refuses a mode with no AP in it, so apply_ap_config()
    // belongs to the open and not to init.
    //
    // The AP netif above still exists, with its 192.168.4.1 and its DHCP server, so the DNS
    // server below and board_wifi_status() work unchanged while the radio carries no AP.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Every A query answered with the softAP's own address (FR-2.2).
    dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&dns_cfg);
    if (!s_dns) {
        ESP_LOGE(TAG, "dns server failed to start; the captive portal will not open");
    }

    const esp_err_t mdns_err = start_mdns(device_name);
    if (mdns_err != ESP_OK) {
        ESP_LOGE(TAG, "mdns: %s", esp_err_to_name(mdns_err));
    }

    lock();
    s_retry_enabled = true;
    copy_str(s_ssid, ssid ? ssid : "", sizeof(s_ssid));
    copy_str(s_password, password ? password : "", sizeof(s_password));
    unlock();

    // 3 KB. As with the buttons: the high-water mark that suggested 2048 was taken on a
    // device that has never made a station connection, which is the only interesting
    // thing this task does (ticket 21).
    BaseType_t ok = xTaskCreate(retry_task, "wifi_retry", 3072, NULL, 4, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "ap \"%s\" ch%d at " IPSTR " -- DOWN until a window opens (ticket 67)",
                 s_ap_ssid, AP_CHANNEL, IP2STR(&ip_info.ip));
    }

#ifdef FRAME_AP_ALWAYS_ON
    // The way back to FR-2.1's permanent access point, and it is one flag for both halves: the AP
    // comes up here and service_ap_window() returns without looking at the clock.
    ESP_LOGW(TAG, "FRAME_AP_ALWAYS_ON: the access point will not close itself");
    board_wifi_ap_window_open();
#endif
    return ESP_OK;
}

// The whole AP config, built from this module's own state, every time.
//
// Re-applied rather than patched: esp_wifi_get_config() would give back what we set, and building it
// twice from the same source is one description of the AP instead of two that can disagree. Ticket 67
// makes that matter more than it did -- the AP now goes down and comes back, and this is the single
// place that decides what it comes back as.
static esp_err_t apply_ap_config(void)
{
    wifi_config_t ap_cfg = {0};
    copy_str((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    ap_cfg.ap.channel = AP_CHANNEL;
    ap_cfg.ap.max_connection = AP_MAX_CONN;

    lock();
    const bool secured = s_ap_secured;
    char password[sizeof(s_ap_password)];
    copy_str(password, s_ap_password, sizeof(password));
    unlock();

    if (secured) {
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        copy_str((char *)ap_cfg.ap.password, password, sizeof(ap_cfg.ap.password));
        // WPA2-only, no downgrade: WIFI_AUTH_WPA_WPA2_PSK would also accept TKIP, and there is no
        // client in this device's life that needs it.
        ap_cfg.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;  // FR-2.1's original, and -DFRAME_AP_OPEN's
    }
    return esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
}

esp_err_t board_wifi_ap_secure(const char *password)
{
    // WPA2's own bound, checked here rather than trusted: esp_wifi_set_config() accepts a short
    // passphrase and the AP then comes up in a state nothing can join, which on a frame with no
    // station link is the one failure ticket 30 was written in two steps to avoid. **8 is also
    // exactly what the password is since ticket 66**, so this bound is now load-bearing rather than
    // roomy.
    if (!password || strlen(password) < 8 || strlen(password) > 63) {
        ESP_LOGE(TAG, "ap password is %u chars; WPA2 wants 8-63. Leaving the AP OPEN.",
                 password ? (unsigned)strlen(password) : 0u);
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    copy_str(s_ap_password, password, sizeof(s_ap_password));
    s_ap_secured = true;
    const bool up = s_ap_up;
    unlock();

    // Only reaches the radio while the AP is actually up; a window that opens later picks it up from
    // s_ap_password. **s_ap_secured is set either way on purpose**, because it answers "will this AP
    // be encrypted", which is what gates the token in app_server.c, and the answer does not change
    // while the radio is down -- there is nothing to join at all then.
    if (up) {
        const esp_err_t err = apply_ap_config();
        if (err != ESP_OK) {
            lock();
            s_ap_secured = false;
            unlock();
            ESP_LOGE(TAG, "ap set_config: %s -- the AP stays OPEN", esp_err_to_name(err));
            return err;
        }
    }

    // **The password is not logged at all, not even a prefix.** It used to print four characters,
    // which was a quarter of the old 16; ticket 66 made it 8, where four characters is half of it --
    // and this output ends up in capture files under .scratch/ that are not git-ignored.
    // `app_auth_log_prefix()`'s `ap password fp=` is where identity comes from.
    ESP_LOGI(TAG, "ap \"%s\" is now WPA2-PSK (%u chars)%s", s_ap_ssid, (unsigned)strlen(password),
             up ? "" : " -- stored; the AP is down until a window opens");
    return ESP_OK;
}

// ------------------------------------------------------------------ the window (ticket 67)

esp_err_t board_wifi_ap_window_open(void)
{
    const int64_t now = esp_timer_get_time();

    lock();
    const bool was_up = s_ap_up;
    s_ap_deadline_us = now + (int64_t)FRAME_AP_WINDOW_MS * 1000;
    if (!was_up) {
        // The cap runs from the OPEN and is not extended by a re-press, so holding the button every
        // twenty minutes cannot walk the AP past it either.
        s_ap_cap_us = now + (int64_t)FRAME_AP_WINDOW_CAP_MS * 1000;
    }
    s_ap_up = true;
    unlock();

    if (was_up) {
        ESP_LOGI(TAG, "ap window extended to %u s", (unsigned)(FRAME_AP_WINDOW_MS / 1000u));
        return ESP_OK;
    }

    // APSTA before the config: esp_wifi_set_config(WIFI_IF_AP, ...) returns ESP_ERR_WIFI_IF when the
    // current mode has no AP in it, so the order here is not cosmetic.
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = apply_ap_config();
    }
    if (err != ESP_OK) {
        lock();
        s_ap_up = false;
        unlock();
        ESP_LOGE(TAG, "ap window open failed: %s", esp_err_to_name(err));
        return err;
    }

    // Re-applied on every open rather than once at init: the DHCP option lives on the AP netif and
    // this costs nothing, where discovering that it had been lost costs a phone that does not open
    // the portal (FR-2.2).
    set_captive_portal_uri();

    // Seconds, not minutes: the shipping window is 30 min and an integer division printed "1 min"
    // for a 90-second bench override, which is a log line under-reporting a duration -- the class of
    // thing docs/agents/method.md keeps a rule about.
    ESP_LOGI(TAG, "ap \"%s\" UP for %u s (%s)", s_ap_ssid,
             (unsigned)(FRAME_AP_WINDOW_MS / 1000u), s_ap_secured ? "WPA2" : "OPEN");
    return ESP_OK;
}

esp_err_t board_wifi_ap_close(void)
{
    lock();
    const bool was_up = s_ap_up;
    s_ap_up = false;
    s_ap_deadline_us = 0;
    s_ap_cap_us = 0;
    unlock();

    // STA and not NULL: the station link is the frame's normal surface and must not be touched.
    const esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ap close: %s", esp_err_to_name(err));
        return err;
    }
    if (was_up) {
        ESP_LOGI(TAG, "ap \"%s\" DOWN", s_ap_ssid);
    }
    return ESP_OK;
}

bool board_wifi_ap_is_up(void)
{
    lock();
    const bool up = s_ap_up;
    unlock();
    return up;
}

uint32_t board_wifi_ap_window_left_s(void)
{
    lock();
    const bool up = s_ap_up;
    const int64_t deadline = s_ap_deadline_us;
    unlock();
    if (!up) {
        return 0;
    }
    const int64_t left = deadline - esp_timer_get_time();
    return left > 0 ? (uint32_t)(left / 1000000) : 0;
}

// Runs on the retry task's 500 ms tick. Closes the window when its time is up and no client is
// associated; a client past the deadline holds it open, but only as far as the cap.
static void service_ap_window(void)
{
#ifdef FRAME_AP_ALWAYS_ON
    return;
#else
    lock();
    const bool up = s_ap_up;
    const int64_t deadline = s_ap_deadline_us;
    const int64_t cap = s_ap_cap_us;
    unlock();
    if (!up) {
        return;
    }

    const int64_t now = esp_timer_get_time();
    if (now < deadline) {
        return;
    }

    wifi_sta_list_t sta_list;
    const int clients = esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK ? (int)sta_list.num : 0;
    if (clients > 0 && now < cap) {
        // Push the deadline out by another tick's worth so the log line below, when it finally
        // fires, is about a radio nobody was using.
        lock();
        s_ap_deadline_us = now + (int64_t)RETRY_TICK_MS * 1000;
        unlock();
        return;
    }

    ESP_LOGI(TAG, "ap window closing: %d client(s), %s", clients,
             clients > 0 ? "CAP REACHED" : "idle at the deadline");
    board_wifi_ap_close();
#endif
}

bool board_wifi_ap_secured(void)
{
    lock();
    const bool secured = s_ap_secured;
    unlock();
    return secured;
}

esp_err_t board_wifi_connect(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    copy_str(s_ssid, ssid, sizeof(s_ssid));
    copy_str(s_password, password ? password : "", sizeof(s_password));
    s_last_error[0] = '\0';
    s_retry_enabled = true;
    s_apply_ticks = APPLY_DELAY_TICKS;
    unlock();

    // The radio is deliberately NOT touched here. This used to call esp_wifi_disconnect()
    // and esp_wifi_connect() itself, and its only caller is h_wifi_config(), which builds
    // and sends its JSON reply afterwards -- so on a request that arrived over the station
    // link the reply went down a link that had just been removed. Measured on 2026-09-09
    // (ticket 10): curl got http=000, and a user changing networks through the Web UI always
    // saw a failed request whether the new credentials were good or not. retry_task() picks
    // the pending credentials up within RETRY_TICK_MS, which is after the handler has
    // answered.
    return ESP_OK;
}

esp_err_t board_wifi_disconnect_keep_ap(void)
{
    lock();
    s_retry_enabled = false;
    s_apply_ticks = 0;   // a deliberate disconnect outranks a queued connect
    s_ssid[0] = '\0';
    s_password[0] = '\0';
    s_sta_connected = false;
    s_ip[0] = '\0';
    unlock();

    const esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        return err;
    }
    return ESP_OK;
}

esp_err_t board_wifi_scan(board_wifi_network_t *out, size_t max, size_t *count)
{
    if (!out || !count || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;

    lock();
    s_retry_paused = true;
    unlock();

    wifi_scan_config_t cfg = {0};
    cfg.show_hidden = false;
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    cfg.scan_time.active.min = 100;
    cfg.scan_time.active.max = 300;

    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err == ESP_OK) {
        uint16_t found = (uint16_t)max;
        wifi_ap_record_t *records = calloc(max, sizeof(wifi_ap_record_t));
        if (!records) {
            esp_wifi_scan_stop();
            err = ESP_ERR_NO_MEM;
        } else {
            err = esp_wifi_scan_get_ap_records(&found, records);
            if (err == ESP_OK) {
                for (uint16_t i = 0; i < found && i < max; i++) {
                    copy_str(out[i].ssid, (const char *)records[i].ssid, sizeof(out[i].ssid));
                    out[i].rssi = records[i].rssi;
                    out[i].secure = records[i].authmode != WIFI_AUTH_OPEN;
                    (*count)++;
                }
            }
            free(records);
        }
    }

    lock();
    s_retry_paused = false;
    unlock();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
    }
    return err;
}

void board_wifi_status(board_wifi_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    lock();
    out->sta_connected = s_sta_connected;
    copy_str(out->ip, s_ip, sizeof(out->ip));
    copy_str(out->ssid, s_sta_connected ? s_ssid : "", sizeof(out->ssid));
    copy_str(out->last_error, s_last_error, sizeof(out->last_error));
    copy_str(out->ap_ssid, s_ap_ssid, sizeof(out->ap_ssid));
    unlock();

    esp_netif_ip_info_t ip_info;
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        snprintf(out->ap_ip, sizeof(out->ap_ip), IPSTR, IP2STR(&ip_info.ip));
    }

    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        out->ap_clients = sta_list.num;
    }

    // Asked of the driver rather than remembered from the last event, for the same reason
    // sta_connected is: a cached signal strength is a number that goes stale without saying so.
    // Outside the lock because it does not read our state, and only when joined -- the call
    // fails with ESP_ERR_WIFI_NOT_CONNECT otherwise, which would leave a stale value here.
    if (out->sta_connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi = ap.rssi;
        }
    }
}
