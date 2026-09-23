// Ticket 60 gate B: what does ONE TLS session cost this board in internal RAM?
//
// FR-10 makes Google Photos a requirement rather than an idea, and the frame's site has
// Wi-Fi and nothing else -- so there is nowhere to run a companion machine and the board
// itself must speak HTTPS. Ticket 57 section 5 named this measurement as the one that could
// end the discussion cheaply; FR-10 turns it into a number the design has to live within.
// Either way it is the same number and nothing should be built before it exists.
//
// WHY IT IS THE DANGEROUS ONE. Internal RAM is the scarce resource here, not PSRAM
// (docs/board-and-storage.md). Shipping idle is int_free ~72 KB and dma_largest
// ~31 KB, and BELOW ABOUT 2 KB OF dma_largest lwIP silently drops arriving frames: the
// board stops answering ICMP and TCP with a perfectly healthy console. The existing worst
// cases leave very little room -- an upload's thumbnail sidecar takes int_min to 1,995 B,
// and an ordinary on-demand SMB fetch takes it to 5,419 B with dma_largest at 5,888 B.
// Adding two small tasks was once enough to make httpd_start() return ESP_ERR_HTTPD_TASK.
//
// THIS BUILD IS STATION-ONLY ON PURPOSE: no panel, no AP, no mDNS, no web server, no
// storage, no SMB. It measures the TLS increment IN ISOLATION, exactly as env:smbprobe
// measures the SMB one. THE COMBINED CASE IS NOT THIS BUILD AND IS THE ONE THAT DECIDES
// THE FEATURE -- a TLS fetch while httpd, the mirror and a refresh are live. This arm
// exists first because if the increment alone does not fit, the combined case cannot.
//
// It prints raw counters and computes no verdict. Ticket 03 cost a re-run because a derived
// boolean in first-cut instrumentation measured something other than what its name said.
//
// int_min IS A SINCE-BOOT WATERMARK AND CANNOT ATTRIBUTE A FALL to a stage; that left two
// figures unexplained until 2026-09-11. So a sampler task tracks the minimum of int_free
// and dma_largest BETWEEN STAGE MARKS at 20 ms, and every stamp prints the stage's own
// trough beside the since-boot one.
//
// PRIVACY (owner's instruction, 2026-09-12). The share URL carries a `key` that is a
// bearer capability to the whole album, and a photo URL fetches a photograph to anyone
// holding it. NEITHER IS EVER PRINTED. The log carries the host, the path length and the
// byte counts, which is everything the measurement needs.
//
// Credentials and URLs are build flags only, and nothing is committed:
//
//   [env:gphotosprobe] in the git-ignored platformio_local.ini
//
// The probe refuses to run with any of them unset and says so.

#ifdef BUILD_GPHOTOSPROBE

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/opt.h"

#include "app_gphotos.h" // for the shared mbedTLS condition line, nothing else
#include "app_settings.h"

#ifndef GPHOTOSPROBE_WIFI_SSID
#define GPHOTOSPROBE_WIFI_SSID ""
#endif
#ifndef GPHOTOSPROBE_WIFI_PASS
#define GPHOTOSPROBE_WIFI_PASS ""
#endif
// The RESOLVED share URL -- https://photos.google.com/share/...?key=... -- not the
// photos.app.goo.gl short link.
//
// The short link answers 302 with a real Location header (measured on the PC side,
// 2026-09-12), so following it is one extra session of exactly the shape measured here.
// Resolving it is not this arm's question and doing it inline would put a redirect loop
// between the instrument and the number.
#ifndef GPHOTOSPROBE_URL
#define GPHOTOSPROBE_URL ""
#endif
// One photo URL WITH the size suffix, e.g. {base}=w400-h600-c. The frame fetches from a
// second host (lh3.googleusercontent.com or photos.fife.usercontent.google.com), so it is a
// second TLS session against a different SNI name and belongs in the measurement.
#ifndef GPHOTOSPROBE_IMG_URL
#define GPHOTOSPROBE_IMG_URL ""
#endif

// Ten, because this project reports medians over at least ten with spread, and one heap
// sample is noise exactly as one refresh timing is.
#ifndef GPHOTOSPROBE_RUNS
#define GPHOTOSPROBE_RUNS 10
#endif

// esp_http_client's receive buffer. INTERNAL RAM, and therefore an arm rather than a
// detail: it is the knob to try first if the answer is "it does not fit". Printed with
// every run so a capture carries the value it was taken at.
#ifndef GPHOTOSPROBE_HTTP_BUF
#define GPHOTOSPROBE_HTTP_BUF 2048
#endif

// The album body measured 1,229,126 B at 167 photographs and grows ~794 B per photograph,
// so 4 MB holds roughly 3,600. PSRAM, where ~6.36 MB is free.
#define GPHOTOSPROBE_BODY_MAX (4 * 1024 * 1024)

#ifndef GPHOTOSPROBE_TASK_STACK
#define GPHOTOSPROBE_TASK_STACK 16384
#endif

static bool s_sta_connected;
static char s_ip[16];
static uint8_t *s_body;
static SemaphoreHandle_t s_task_done;

// ------------------------------------------------------------------ instrument

// The stage trough. int_min is since-boot and says nothing about WHEN or UNDER WHAT, which
// is the whole reason app_heapwatch exists on the shipping build. Here the sampler resets
// these at each stage mark, so every printed figure is attributable to the stage it names.
static volatile uint32_t s_stage_int_min;
static volatile uint32_t s_stage_dma_min;
static volatile bool s_sampling;

static void stage_reset(void)
{
    s_stage_int_min = 0xFFFFFFFFu;
    s_stage_dma_min = 0xFFFFFFFFu;
}

static void sampler_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (!s_sampling) {
            continue;
        }
        const uint32_t i = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const uint32_t d = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
        if (i < s_stage_int_min) {
            s_stage_int_min = i;
        }
        if (d < s_stage_dma_min) {
            s_stage_dma_min = d;
        }
    }
}

// dma_largest is printed because it is the number that matters: below about 2 KB lwIP
// drops arriving frames silently. int_free alone has looked healthy while that happened.
static void stamp(int run, const char *stage)
{
    const uint32_t si = s_stage_int_min;
    const uint32_t sd = s_stage_dma_min;
    printf("@@GP %d %s t_us=%lld int_free=%u int_min=%u dma_largest=%u "
           "stage_int_min=%u stage_dma_min=%u psram_free=%u\n",
           run, stage, (long long)esp_timer_get_time(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
           (unsigned)(si == 0xFFFFFFFFu ? 0 : si), (unsigned)(sd == 0xFFFFFFFFu ? 0 : sd),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    fflush(stdout);
    stage_reset();
}

static TaskStatus_t s_tasks[24];

static void print_task_table(const char *when)
{
    const UBaseType_t n = uxTaskGetSystemState(s_tasks, 24, NULL);
    printf("# tasks (%s):", when);
    for (UBaseType_t i = 0; i < n; i++) {
        printf(" %s=%u", s_tasks[i].pcTaskName,
               (unsigned)(s_tasks[i].usStackHighWaterMark * sizeof(StackType_t)));
    }
    printf("\n");
    fflush(stdout);
}

// The URL is a capability. Print what identifies the request and nothing that performs it.
static void print_url_shape(const char *label, const char *url)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *slash = strchr(p, '/');
    const int hostlen = slash ? (int)(slash - p) : (int)strlen(p);
    printf("# %s host=\"%.*s\" rest_len=%u (url withheld: it is a capability)\n", label,
           hostlen, p, (unsigned)(slash ? strlen(slash) : 0));
    fflush(stdout);
}

// ----------------------------------------------------------------------- wi-fi

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e = (const wifi_event_sta_disconnected_t *)data;
        s_sta_connected = false;
        s_ip[0] = '\0';
        printf("# sta disconnected, reason=%u\n", e->reason);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        s_sta_connected = true;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        printf("# sta got ip %s\n", s_ip);
    }
}

static esp_err_t wifi_station_up(const char *ssid, const char *pass)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event, NULL, NULL));

    wifi_config_t cfg = {0};
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", ssid);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", pass);

    // The bench AP advertises WPA3-Personal first. Without these three the station
    // associates and then dies in the 4-way handshake as reason=204, which reads like a
    // wrong password and is not one (env:smbprobe, 2026-09-04).
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_connect());

    for (int i = 0; i < 200 && !s_sta_connected; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return s_sta_connected ? ESP_OK : ESP_ERR_TIMEOUT;
}

// -------------------------------------------------------------------- one fetch

// Returns the status code, or a negative esp_err_t. Body bytes land in PSRAM and are
// counted, never kept beyond the round.
static int fetch_once(int run, const char *what, const char *url)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = GPHOTOSPROBE_HTTP_BUF,
        .buffer_size_tx = 1024,
        .timeout_ms = 30000,
        // Renamed from "M5PaperColor/gphotosprobe" on 2026-09-18 (ticket 65 item 1). The VALUE is
        // arbitrary but not cosmetic: app_gphotos.c's UA note measured 120 KB of album shell on this
        // string against a browser one, and that figure was taken under the old spelling.
        .user_agent = "PaperFrame/gphotosprobe",
        .keep_alive_enable = false,
    };

    stage_reset();
    stamp(run, "before_init");

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        printf("@@GP %d %s init_failed\n", run, what);
        return -1;
    }
    stamp(run, "after_init");

    // The handshake is where the certificate bundle is walked and where the 16 KB mbedTLS
    // input buffer is allocated. If anything refuses to fit, it refuses here.
    const int64_t t_open = esp_timer_get_time();
    esp_err_t err = esp_http_client_open(c, 0);
    const int64_t dt_open = esp_timer_get_time() - t_open;
    if (err != ESP_OK) {
        printf("@@GP %d %s open rc=%s dt_us=%lld\n", run, what, esp_err_to_name(err),
               (long long)dt_open);
        stamp(run, "open_failed");
        esp_http_client_cleanup(c);
        return -2;
    }
    printf("@@GP %d %s open rc=OK dt_us=%lld\n", run, what, (long long)dt_open);
    stamp(run, "handshake");

    const int64_t clen = esp_http_client_fetch_headers(c);
    const int status = esp_http_client_get_status_code(c);
    printf("@@GP %d %s headers status=%d content_length=%lld\n", run, what, status,
           (long long)clen);
    stamp(run, "headers");

    const int64_t t_read = esp_timer_get_time();
    size_t total = 0;
    int r;
    while ((r = esp_http_client_read(c, (char *)s_body + total,
                                     (int)(GPHOTOSPROBE_BODY_MAX - total))) > 0) {
        total += (size_t)r;
        if (total >= GPHOTOSPROBE_BODY_MAX) {
            printf("# %s body hit the %d byte buffer; truncated\n", what,
                   GPHOTOSPROBE_BODY_MAX);
            break;
        }
    }
    const int64_t dt_read = esp_timer_get_time() - t_read;
    printf("@@GP %d %s body bytes=%u dt_us=%lld last_read=%d\n", run, what, (unsigned)total,
           (long long)dt_read, r);
    stamp(run, "body");

    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    stamp(run, "closed");
    return status;
}

// ---------------------------------------------------------------------- driver

static void probe_task(void *arg)
{
    (void)arg;
    int ok_html = 0;
    int ok_img = 0;

    printf("# %d rounds. Each round is one HTTPS session to the album host and, if "
           "configured, one to the photo host.\n", GPHOTOSPROBE_RUNS);
    fflush(stdout);

    for (int run = 1; run <= GPHOTOSPROBE_RUNS; run++) {
        const int s = fetch_once(run, "html", GPHOTOSPROBE_URL);
        if (s == 200) {
            ok_html++;
        }
        print_task_table("after_html");

        if (GPHOTOSPROBE_IMG_URL[0]) {
            // Spaced, for the reason every measurement here is: back-to-back operations on
            // this board have looked far more repeatable than spaced ones, and the
            // difference was the spacing.
            vTaskDelay(pdMS_TO_TICKS(2000));
            const int si = fetch_once(run, "img", GPHOTOSPROBE_IMG_URL);
            if (si == 200) {
                ok_img++;
            }
            print_task_table("after_img");
        }

        printf("# probe task stack high-water: %u bytes free of %d\n",
               (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
               GPHOTOSPROBE_TASK_STACK);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    printf("# gphotosprobe complete: html %d/%d, img %d/%d\n", ok_html, GPHOTOSPROBE_RUNS,
           ok_img, GPHOTOSPROBE_IMG_URL[0] ? GPHOTOSPROBE_RUNS : 0);
    stamp(0, "end");
    print_task_table("end");
    xSemaphoreGive(s_task_done);
    vTaskDelete(NULL);
}

void app_main(void)
{
    printf("\n# gphotosprobe: ticket 60 gate B, what one TLS session costs in internal RAM\n");

    // The capture carries its own validity condition rather than relying on the env name.
    // The TCP window is a PAIR and raising one alone collapses receive 7.6x; the shipping
    // application runs at 5760/6 and that is the condition this arm wants.
    printf("# lwip tcp: wnd=%d recvmbox=%d | http_buf=%d\n",
           CONFIG_LWIP_TCP_WND_DEFAULT, CONFIG_LWIP_TCP_RECVMBOX_SIZE,
           GPHOTOSPROBE_HTTP_BUF);
    // The mbedTLS knobs moved out of the line above and into app_gphotos.h's shared one, because
    // ticket 60's knob arm changes them and env:frame has to print the SAME line for the two runs
    // to be comparable. The anchor is `^# mbedtls:`; the 2026-09-12 gate-B capture carries the
    // older `| mbedtls in=` form and nothing parses it.
    printf(GPHOTOS_MBEDTLS_COND_FMT, GPHOTOS_MBEDTLS_COND_ARGS);
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL
    printf("# cert bundle: FULL, max_certs=%d\n", CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_MAX_CERTS);
#else
    printf("# cert bundle: not FULL -- check sdkconfig before trusting a handshake result\n");
#endif
    fflush(stdout);

    esp_err_t err = app_settings_init();
    if (err != ESP_OK) {
        printf("# app_settings_init failed: %s\n", esp_err_to_name(err));
        return;
    }

    // NVS first, build flags second -- the same order env:smbprobe uses, so configuring
    // Wi-Fi once in the browser configures this build too and no secret needs to be in a
    // file. This PC cannot join a Wi-Fi network from a command line, so the flag fallback
    // stays.
    char ssid[APP_SETTINGS_SSID_SIZE] = {0};
    char pass[APP_SETTINGS_PASS_SIZE] = {0};
    app_settings_wifi_ssid(ssid, sizeof(ssid));
    app_settings_wifi_password(pass, sizeof(pass));
    const char *cred_source = "nvs";
    if (!ssid[0]) {
        snprintf(ssid, sizeof(ssid), "%s", GPHOTOSPROBE_WIFI_SSID);
        snprintf(pass, sizeof(pass), "%s", GPHOTOSPROBE_WIFI_PASS);
        cred_source = "build-flags";
    }
    printf("# wifi credentials from %s, ssid=\"%s\" pass_len=%u\n", cred_source, ssid,
           (unsigned)strlen(pass));

    if (!ssid[0] || !GPHOTOSPROBE_URL[0]) {
        // A probe that silently fetched nothing would report a clean run and a tiny heap
        // cost. That is the false all-clear ticket 18 produced three times over.
        printf("# REFUSING TO RUN: build without a station or a URL.\n");
        printf("# Required: GPHOTOSPROBE_WIFI_SSID GPHOTOSPROBE_WIFI_PASS (or NVS)\n");
        printf("#           GPHOTOSPROBE_URL -- the RESOLVED share URL, not the short link\n");
        printf("# Optional: GPHOTOSPROBE_IMG_URL (a {base}=w400-h600-c photo URL)\n");
        printf("#           GPHOTOSPROBE_RUNS GPHOTOSPROBE_HTTP_BUF\n");
        return;
    }
    print_url_shape("album", GPHOTOSPROBE_URL);
    if (GPHOTOSPROBE_IMG_URL[0]) {
        print_url_shape("photo", GPHOTOSPROBE_IMG_URL);
    } else {
        printf("# no GPHOTOSPROBE_IMG_URL: the photo-host session is SKIPPED, out loud. "
               "The album host alone is half the answer.\n");
    }

    stage_reset();
    stamp(0, "boot");
    print_task_table("boot");

    // PSRAM, allocated once before any measurement so no round pays for it and no internal
    // RAM does either.
    s_body = heap_caps_malloc(GPHOTOSPROBE_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (!s_body) {
        printf("# PSRAM allocation failed for the %d byte body buffer\n",
               GPHOTOSPROBE_BODY_MAX);
        return;
    }
    printf("# body buffer: %d bytes in psram\n", GPHOTOSPROBE_BODY_MAX);
    stamp(0, "buffers");

    if (wifi_station_up(ssid, pass) != ESP_OK) {
        printf("# sta did not associate; nothing below this line is measurable\n");
        return;
    }
    stamp(0, "wifi");
    print_task_table("wifi");

    s_task_done = xSemaphoreCreateBinary();
    if (!s_task_done) {
        printf("# no memory for the completion semaphore\n");
        return;
    }

    // Higher priority than the fetcher so a blocking handshake cannot starve the observer
    // of it -- the same reason env:smbprobe's watcher outranks its reader.
    s_sampling = true;
    if (xTaskCreate(sampler_task, "gpsample", 2560, NULL, 6, NULL) != pdPASS) {
        printf("# xTaskCreate(gpsample) failed; stage troughs will read 0 and int_min is "
               "since-boot, so the run cannot attribute a fall. Treat it as invalid.\n");
        s_sampling = false;
    }

    // main's stack is 4608 bytes and a TLS handshake does not fit in it; libsmb2's connect
    // path did not either (env:smbprobe, 184 boots of stack overflow). 16 KB is a starting
    // point and the high-water mark printed each round is the measurement -- a fetch task
    // in env:frame has to be budgeted out of internal RAM.
    if (xTaskCreate(probe_task, "gphotos", GPHOTOSPROBE_TASK_STACK, NULL, 5, NULL) != pdPASS) {
        printf("# xTaskCreate(gphotos) failed at %d bytes of stack\n", GPHOTOSPROBE_TASK_STACK);
        return;
    }

    // A silent console has meant three different things on this board -- powered off, hung,
    // and a task blocked in a library call -- and they are not distinguishable without
    // something that prints on its own.
    while (xSemaphoreTake(s_task_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        stamp(0, "alive");
    }
}

#endif // BUILD_GPHOTOSPROBE
