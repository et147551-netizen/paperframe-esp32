// Ticket 27, and the number ticket 23 left unexplained: what is this board's TCP ceiling?
//
// Ticket 23 measured a 155,803-byte SMB read at a median 1849.4 ms -- 84 KB/s, ~45 ms per
// 4 KB round trip -- and wrote down that this is "an order of magnitude below what the link
// can do" and "close to a delayed-ACK interval and is not explained here". Ticket 25 then
// designed the whole mirror around that figure (its per-file budget, its window count, its
// decision to stop httpd) and re-measured 76-82 KB/s in the shipping mirror. So an
// unexplained number is load-bearing for a shipped design, and nobody knows whether the
// cause is the network, lwIP's receive window, the 4096-byte chunking or libsmb2.
//
// This build answers the first of those without touching src/board_smb.c at all. It is
// station-only on purpose -- no panel, no AP, no storage, no httpd, no SMB -- so the number
// is the transport and not a mixture.
//
// THE HYPOTHESIS, written down before the run: 84 KB/s is not the link, and the ~45 ms per
// 4 KB is a round-trip artefact of request-response chunking rather than a bandwidth limit.
//
//   confirmed -> iperf clears roughly 1 MB/s or better in at least one direction. The SMB
//       figure is then a protocol/chunking problem and board_smb.c's chunk size is next.
//   refuted   -> iperf also lands near 84 KB/s. The ceiling is the board, the radio or
//       lwIP, board_smb.c is exonerated, and ticket 25's design is right for the wrong
//       documented reason.
//   INVALID, not merely inconclusive, if the TCP window the binary was built with is not
//       recorded next to the number. Which is why this file prints it -- see below.
//
// THE WINDOW IS THE INDEPENDENT VARIABLE, not a detail. sdkconfig.frame carries
// CONFIG_LWIP_TCP_WND_DEFAULT=5760 (the IDF stock value) and sdkconfig.smbprobe carries
// 16384, because ticket 23 found 4 KB reads at 5760 stall every ~150-200 PDUs and that
// 4 KB + 16384 was the only no-stall combination in its table. Both values are committed,
// but only in the generated per-env sdkconfig files -- so the setting does not propagate to
// a new environment, and deleting a generated sdkconfig to pick up a sdkconfig.defaults
// edit (the procedure docs/agents/build-system.md documents) silently resets it to 5760.
//
// So this env is run TWICE, once at each value, and sdkconfig.iperf is the knob:
//
//   arm A: CONFIG_LWIP_TCP_WND_DEFAULT=5760   -- the shipping frame's condition
//   arm B: CONFIG_LWIP_TCP_WND_DEFAULT=16384  -- the condition SMB needed to stop stalling
//
// Two runs that differ only there separate "lwIP's window" from every other candidate,
// which is the discrimination ticket 23 could not make. Nothing is written into
// sdkconfig.defaults: 16384 there would change env:frame's window with no measurement
// behind it, and 5760 there would reset env:smbprobe and destroy ticket 23's conditions.
//
// THE PEER IS tools/iperf_peer.py, NOT iperf3. espressif/iperf's server performs no
// handshake at all -- it accepts a connection and counts the bytes recvfrom() returns --
// so an iperf3 client, which needs a control channel and a cookie exchange, cannot talk to
// it. There is also no iperf binary of any version on the bench PC. A plain socket script
// is therefore not an approximation of the peer; against this implementation it is exactly
// equivalent, and it needs nothing installed.
//
// TWO ARMS, in this order, and the order is deliberate:
//
//   1. client (board -> PC). The board connects out, so a peer that is not running fails
//      fast and says so.
//   2. server (board <- PC). espressif/iperf calls accept() once per instance with no
//      surrounding loop, and a blocking accept() is not interrupted by the instance's own
//      tick timer -- so an instance nobody connects to parks this task indefinitely. The
//      client arm having already succeeded is what establishes the peer is there before
//      this arm can hang on it. The heartbeat keeps a hang visible and attributable.
//
// Raw counters only, no derived verdict: ticket 03 cost a re-run because a boolean in
// first-cut instrumentation was measuring something other than what its name said.
//
// Only espressif/iperf (the core engine) is taken. Its sibling espressif/iperf-cmd is
// deliberately NOT taken -- it exists to expose the engine as an esp_console command, and a
// console brings a task and a stack to a board where adding two small tasks was once enough
// to make httpd_start() return ESP_ERR_HTTPD_TASK. See .scratch/digital-frame/issues/21.
// Ticket 31 records iperf-cmd's console dependency as iperf's; it is the sibling's, and the
// engine's own idf_component.yml depends on nothing but idf >= 4.3.

#ifdef BUILD_IPERF

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/opt.h"

#include "iperf.h"

#include "app_settings.h"

// The bench PC on LAN1, which is also the SMB server ticket 23's number was measured
// against. Overridable so this is not the only address the build can ever be pointed at.
#ifndef IPERF_PEER_IP
#define IPERF_PEER_IP "192.168.1.10"
#endif

// Station credentials. NVS first, these second -- see the comment in app_main().
#ifndef IPERF_WIFI_SSID
#define IPERF_WIFI_SSID ""
#endif
#ifndef IPERF_WIFI_PASS
#define IPERF_WIFI_PASS ""
#endif

// Ten samples per arm, per this project's standing rule that a median needs at least ten
// runs with its spread reported. Ten seconds each: long enough that TCP slow start is a
// small share of the window, short enough that ten of them plus the reverse arm fit in one
// capture.
#ifndef IPERF_RUNS
#define IPERF_RUNS 10
#endif
#ifndef IPERF_SECS
#define IPERF_SECS 10
#endif

// The gap between samples. The peer script closes and reconnects in it, and on the server
// arm it is also the window in which a refused connect tells the peer the next instance is
// not open yet. Long enough for TIME_WAIT on the peer not to matter.
#define IPERF_GAP_MS 3000

// The board listens here on the server arm; it pushes to the peer's port on the client arm.
// Two different ports so the peer script can hold both roles at once without either
// listener shadowing the other.
#define IPERF_BOARD_PORT 5001
#define IPERF_PEER_PORT 5002

// ------------------------------------------------------------------ instrument

// Labels for the report hook below, which iperf calls from its own task and cannot be
// passed context. Written only between instances, read only during one.
static const char *s_arm = "none";
static int s_run;

static void stamp(const char *stage)
{
    printf("@@IPERF %s run=%d %s t_us=%lld int_free=%u int_min=%u int_largest=%u "
           "dma_free=%u dma_largest=%u\n",
           s_arm, s_run, stage, (long long)esp_timer_get_time(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    fflush(stdout);
}

// Static for the reason smbprobe's is: a local array of these overflowed main's stack
// outright in ticket 21 and the board panicked before printing anything.
static TaskStatus_t s_tasks[28];

static void print_task_table(const char *when)
{
    const UBaseType_t n = uxTaskGetSystemState(s_tasks, 28, NULL);
    printf("# tasks (%s):", when);
    for (UBaseType_t i = 0; i < n; i++) {
        printf(" %s=%u", s_tasks[i].pcTaskName,
               (unsigned)(s_tasks[i].usStackHighWaterMark * sizeof(StackType_t)));
    }
    printf("\n");
    fflush(stdout);
}

// iperf_report_output() is declared weak by the component precisely so it can be replaced.
// The default prints a human-readable iperf table; this one prints the same numbers in the
// project's @@ stamp form, in BYTES and SECONDS, and computes no rate. A rate is a division
// that is free to be done wrong at analysis time, where being wrong costs nothing; doing it
// here would cost a rebuild, a flash and a run.
//
// period_bytes is a double in the component's own struct -- not a rounding choice made here.
void iperf_report_output(const iperf_report_t *report)
{
    if (report == NULL) {
        return;
    }
    switch (report->report_type) {
    case IPERF_REPORT_PERIOD:
    case IPERF_REPORT_SUMMARY:
        printf("@@IPERF %s run=%d %s id=%d start_s=%u end_s=%u bytes=%.0f total_bytes=%llu\n",
               s_arm, s_run,
               report->report_type == IPERF_REPORT_SUMMARY ? "summary" : "period",
               (int)report->instance_id, (unsigned)report->traffic.period_start_sec,
               (unsigned)report->traffic.end_sec, report->traffic.period_bytes,
               (unsigned long long)report->traffic.total_transfer_bytes);
        break;
    case IPERF_REPORT_CONNECT_INFO:
        // The connect-info payload's fields are not read here on purpose: this file has
        // never compiled against that struct, and a guess at a member name is a build
        // failure discovered after the flash. The marker is enough -- it says the peer
        // arrived, which is the only thing this arm needs from it.
        printf("@@IPERF %s run=%d connected id=%d\n", s_arm, s_run,
               (int)report->instance_id);
        break;
    default:
        break;
    }
    fflush(stdout);
}

// ----------------------------------------------------------------------- wi-fi

static bool s_sta_connected;
static char s_ip[16];

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
    fflush(stdout);
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

    // The house AP leads with WPA3-Personal. Without these three the station associates and
    // then dies in the 4-way handshake with reason=204, which reads exactly like a wrong
    // password and is not one. Same three lines as src/board_wifi.c and smbprobe_main.c.
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Modem sleep off, matching env:smbprobe. Power save is a confound on a throughput
    // measurement and this run is not the place to characterise it -- and leaving it on
    // would make these numbers incomparable with the SMB ones they exist to explain.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_wifi_connect());

    for (int i = 0; i < 200 && !s_sta_connected; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return s_sta_connected ? ESP_OK : ESP_ERR_TIMEOUT;
}

// The radio is a live confound where 84 KB/s is not, so RSSI is sampled per run rather than
// once. A run whose association degraded halfway is a run to discard, and that is only
// visible if it was recorded.
static void print_rssi(void)
{
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        printf("@@IPERF %s run=%d rssi=%d channel=%u\n", s_arm, s_run, (int)ap.rssi,
               (unsigned)ap.primary);
    } else {
        printf("@@IPERF %s run=%d rssi=unavailable\n", s_arm, s_run);
    }
    fflush(stdout);
}

// ------------------------------------------------------------------- the arms

static esp_ip_addr_t ipv4_of(const char *s)
{
    esp_ip_addr_t a = {0};
    a.type = ESP_IPADDR_TYPE_V4;
    esp_netif_str_to_ip4(s, &a.u_addr.ip4);
    return a;
}

// One instance per sample, because the component's TCP server calls accept() exactly once
// and ends when that connection does. So a sample is an instance, not a connection inside
// a long-lived listener.
static void run_arm(const char *name, bool as_server)
{
    s_arm = name;
    const esp_ip_addr_t peer = ipv4_of(IPERF_PEER_IP);
    const esp_ip_addr_t self = ipv4_of(s_ip);

    for (int run = 1; run <= IPERF_RUNS; run++) {
        s_run = run;
        print_rssi();
        stamp("run-begin");

        iperf_cfg_t cfg = as_server
                              ? (iperf_cfg_t)IPERF_DEFAULT_CONFIG_SERVER(IPERF_FLAG_TCP, self)
                              : (iperf_cfg_t)IPERF_DEFAULT_CONFIG_CLIENT(IPERF_FLAG_TCP, peer);
        // KBYTES_PER_SEC so the component's own default report, if it is ever re-enabled,
        // is in the same unit as the 84 KB/s this run exists to explain. The stamp lines
        // above carry bytes and seconds regardless.
        cfg.format = KBYTES_PER_SEC;
        cfg.time = IPERF_SECS;
        cfg.interval = 1; // one period report a second, so slow start is visible
        if (as_server) {
            cfg.sport = IPERF_BOARD_PORT;
        } else {
            cfg.dport = IPERF_PEER_PORT;
        }

        const iperf_id_t id = iperf_start_instance(&cfg);
        if (id < 0) {
            printf("@@IPERF %s run=%d start_failed id=%d\n", s_arm, s_run, (int)id);
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(IPERF_GAP_MS));
            continue;
        }

        // Wait the instance out rather than polling it. The instance's own tick timer ends
        // it at cfg.time; the slack covers the connect and the peer's reconnect. The
        // heartbeat is what makes a blocked accept() on the server arm visible as a stall
        // at a known point instead of a silent console.
        const int64_t deadline = esp_timer_get_time() + (int64_t)(IPERF_SECS + 8) * 1000000;
        while (esp_timer_get_time() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            iperf_traffic_report_t tr = {0};
            if (iperf_get_traffic_report(id, &tr) == ESP_OK) {
                printf("@@IPERF %s run=%d alive end_s=%u total_bytes=%llu\n", s_arm, s_run,
                       (unsigned)tr.end_sec,
                       (unsigned long long)tr.total_transfer_bytes);
            } else {
                printf("@@IPERF %s run=%d alive no_report\n", s_arm, s_run);
            }
            fflush(stdout);
        }

        // Unconditional, and it is allowed to fail. An instance that already ended on its
        // own timer returns an error here, and an instance that did not is one this run
        // must not carry into the next sample.
        const esp_err_t serr = iperf_stop_instance(id);
        printf("@@IPERF %s run=%d stopped rc=%s\n", s_arm, s_run, esp_err_to_name(serr));
        stamp("run-end");
        vTaskDelay(pdMS_TO_TICKS(IPERF_GAP_MS));
    }
    s_arm = "none";
    s_run = 0;
}

// iperf's traffic task is its own; this one only sequences the arms. 4096 bytes because it
// holds no buffers and calls nothing deep -- the component allocates its own.
#define IPERF_SEQ_STACK 4096

static void seq_task(void *arg)
{
    (void)arg;

    // Client first. The board connects out, so an absent peer fails fast and names itself,
    // rather than parking the server arm in a blocking accept() -- see the header comment.
    run_arm("client", false);
    print_task_table("after-client");

    run_arm("server", true);
    print_task_table("after-server");

    printf("# seq task stack high-water mark: %u bytes free of %d\n",
           (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
           IPERF_SEQ_STACK);
    printf("# iperf complete: %d runs per arm at %d s\n", IPERF_RUNS, IPERF_SECS);
    stamp("end");
    print_task_table("end");
    vTaskDelete(NULL);
}

void app_main(void)
{
    printf("\n# iperf: ticket 27, the TCP ceiling behind ticket 23's unexplained 84 KB/s\n");

    // The measurement's validity condition, printed before anything is measured. A
    // throughput figure without the window it was taken under is not a weak measurement,
    // it is an unusable one -- and this build is meant to be run twice at two window
    // values, so which one this binary is cannot be left to be inferred from the env name.
    printf("# window: CONFIG_LWIP_TCP_WND_DEFAULT=%d CONFIG_LWIP_TCP_SND_BUF_DEFAULT=%d "
           "CONFIG_LWIP_TCP_MSS=%d\n",
           CONFIG_LWIP_TCP_WND_DEFAULT, CONFIG_LWIP_TCP_SND_BUF_DEFAULT, CONFIG_LWIP_TCP_MSS);

    // The receive mailbox belongs in the validity condition too, and this line exists
    // because leaving it out already produced one invalid run. IDF's own Kconfig
    // (components/lwip/Kconfig:690) gives the requirement as WND/MSS + 2 and says that a
    // full mailbox makes "LWIP drop the packets". A first attempt at the 16384 arm raised
    // the window alone, leaving the mailbox at its default 6 against a requirement of 14,
    // and the board's TCP *receive* rate collapsed from 893 KB/s to 118 KB/s while its send
    // rate doubled. That looked exactly like a discovery about the board and was a
    // misconfiguration. So the window is not the condition on its own -- the pair is.
    printf("# recvmbox: CONFIG_LWIP_TCP_RECVMBOX_SIZE=%d, IDF wants >= WND/MSS+2 = %d%s\n",
           CONFIG_LWIP_TCP_RECVMBOX_SIZE,
           CONFIG_LWIP_TCP_WND_DEFAULT / CONFIG_LWIP_TCP_MSS + 2,
           CONFIG_LWIP_TCP_RECVMBOX_SIZE >=
                   (CONFIG_LWIP_TCP_WND_DEFAULT / CONFIG_LWIP_TCP_MSS + 2)
               ? " OK"
               : " *** TOO SMALL: lwIP will drop packets, receive rate is not the link ***");
    printf("# lwip: TCP_WND=%d TCP_SND_BUF=%d TCP_MSS=%d (as the stack actually sees them)\n",
           (int)TCP_WND, (int)TCP_SND_BUF, (int)TCP_MSS);
    printf("# iperf bufs: tcp_rx=%d tcp_tx=%d peer=%s:%d board_port=%d\n",
           (int)IPERF_DEFAULT_TCP_RX_LEN, (int)IPERF_DEFAULT_TCP_TX_LEN, IPERF_PEER_IP,
           IPERF_PEER_PORT, IPERF_BOARD_PORT);
    fflush(stdout);

    // NVS first, build flags second -- the same order and the same keys as smbprobe, so
    // that Wi-Fi configured once in the Web UI configures this build too and no Wi-Fi
    // secret has to exist in any file. The fallback stays because this PC cannot join a
    // Wi-Fi network from a command line: netsh wlan connect needs the machine's location
    // permission and it is denied by group policy.
    esp_err_t err = app_settings_init();
    if (err != ESP_OK) {
        printf("# app_settings_init failed: %s\n", esp_err_to_name(err));
        return;
    }

    char ssid[APP_SETTINGS_SSID_SIZE] = {0};
    char pass[APP_SETTINGS_PASS_SIZE] = {0};
    app_settings_wifi_ssid(ssid, sizeof(ssid));
    app_settings_wifi_password(pass, sizeof(pass));
    const char *cred_source = "nvs";
    if (!ssid[0]) {
        snprintf(ssid, sizeof(ssid), "%s", IPERF_WIFI_SSID);
        snprintf(pass, sizeof(pass), "%s", IPERF_WIFI_PASS);
        cred_source = "build-flags";
    }
    printf("# wifi credentials from %s, ssid=\"%s\" pass_len=%u\n", cred_source, ssid,
           (unsigned)strlen(pass));

    if (!ssid[0]) {
        printf("# REFUSING TO RUN: no station credentials.\n");
        printf("# Set them in the Web UI once (env:frame, POST /api/wifi/config), or\n");
        printf("# supply -DIPERF_WIFI_SSID / -DIPERF_WIFI_PASS in platformio_local.ini.\n");
        return;
    }

    stamp("boot");
    print_task_table("boot");

    if (wifi_station_up(ssid, pass) != ESP_OK) {
        printf("# sta did not associate; nothing below this line is measurable\n");
        return;
    }
    stamp("wifi");
    print_task_table("wifi");

    printf("# start the peer FIRST and leave it running:\n");
    printf("#   python tools/iperf_peer.py --listen-port %d --board-port %d\n",
           IPERF_PEER_PORT, IPERF_BOARD_PORT);
    fflush(stdout);

    // The arms run in their own task, not in main. main's stack is 4608 bytes and this
    // project has twice paid for finding that out the hard way.
    xTaskCreate(seq_task, "iperfseq", IPERF_SEQ_STACK, NULL, 4, NULL);
}

#endif // BUILD_IPERF
