// Ticket 23: what does an SMB2 session cost in internal RAM?
// Ticket 24: and does board_smb.* itself work on hardware?
//
// Internal RAM is the scarce resource on this board, not PSRAM (ticket 21). A fresh
// boot idled at int_free ~= 18 KB when this was written, and a 1 MB upload with a client
// associated drives int_min to 650-900 bytes. An SMB session adds a TCP socket, its lwIP
// buffers, NTLMSSP crypto and -- measured -- SMB2 signing to exactly that budget.
//
// THE 18 KB IS HISTORY, NOT THE CURRENT BOARD: the 120 KB panel frame buffer moved to PSRAM
// on 2026-09-04 and env:frame has idled at ~71-74 KB since. It is left in the past tense
// because the rounds below were taken against it and the comparison is the point of them.
//
// CONFIG_SPIRAM_USE_MALLOC=y *should* put libsmb2's allocations in PSRAM, where 7.5 MB
// is free at all times. That hypothesis is refuted: CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
// is 16384, so every allocation libsmb2 makes is smaller than the threshold and lands
// in internal RAM anyway.
//
// This build is station-only on purpose: no panel, no AP, no DNS responder, no mDNS,
// no web server, no storage. It measures the SMB increment in isolation. The combined
// case -- a sync against an upload and a refresh -- belongs to ticket 25.
//
// It prints raw counters and timestamps at every stage and computes no verdict.
// Ticket 03 cost a re-run because a derived boolean in first-cut instrumentation was
// measuring something other than what its name said.
//
// **2026-09-04, ticket 24.** The rounds used to call libsmb2 inline. They now call
// board_smb.* -- the module ticket 25 will actually use -- so that the module is
// exercised on hardware rather than merely written, and so that ticket 23's medians can
// be re-taken with it linked. The instrument is deliberately unchanged: the @@SMB stamp
// line, the stage names, the 5 s spacing, the heartbeat and the 16 KB task are all as
// they were, because changing the instrument and the subject in the same run destroys
// the comparison that is the point of it.
//
// The station credentials come from NVS when the Web UI has written them there, and from
// build flags otherwise. The SMB credentials are build flags only. Nothing is committed:
//
//   pio run -e smbprobe -a '--project-option=build_flags=... '
//
// or, more practically, a [env:smbprobe] section in a git-ignored
// platformio_local.ini. The probe refuses to run with any of them unset and says so.

#ifdef BUILD_SMBPROBE

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <errno.h>

#include "lwip/sockets.h"
#include "mbedtls/sha256.h"
#include "app_settings.h"
#include "board_smb.h"
#include "smb_aes_hw.h"

// The mid-payload arm reads three fields of libsmb2's private context struct. This is the
// only place in the project that includes either header, and board_smb.c must never do it:
// see the comment on board_smb_context() and the one at the @@RECV line below.
#include "smb2/libsmb2.h"
#include "smb2/smb2.h"
#include "libsmb2-private.h"

// Every one of these must be supplied at build time. Empty is a hard stop, not a
// default -- a probe that silently connects to nothing would report a clean run.
#ifndef SMBPROBE_WIFI_SSID
#define SMBPROBE_WIFI_SSID ""
#endif
#ifndef SMBPROBE_WIFI_PASS
#define SMBPROBE_WIFI_PASS ""
#endif
#ifndef SMBPROBE_HOST
#define SMBPROBE_HOST ""
#endif
#ifndef SMBPROBE_SHARE
#define SMBPROBE_SHARE ""
#endif
#ifndef SMBPROBE_PATH
#define SMBPROBE_PATH ""
#endif
#ifndef SMBPROBE_USER
#define SMBPROBE_USER ""
#endif
#ifndef SMBPROBE_PASS
#define SMBPROBE_PASS ""
#endif
#ifndef SMBPROBE_DOMAIN
#define SMBPROBE_DOMAIN ""
#endif
// Optional: a share this account can reach but not read, for the DENIED arm. Absent, the
// arm is skipped out loud. A skipped arm that says so is fine; a skipped arm that looks
// like a pass is the false all-clear ticket 18 produced three times in a row.
#ifndef SMBPROBE_DENIED_SHARE
#define SMBPROBE_DENIED_SHARE ""
#endif

// Twenty runs, up from ten. Medians and spread need repetition -- one heap sample is
// noise, exactly as one refresh timing is -- but the count is now set by a second and
// stricter requirement. The stall rate is one read in ~4.5, so a run that happens to
// contain no stall demonstrates nothing about the deadline that is meant to recover from
// one, while looking exactly like a pass. Twenty reads should contain about four. A run
// reporting `recovered=0` on every round is not evidence and has to be repeated: this is
// the ticket-18 shape, an experiment incapable of producing the other answer.
#ifndef SMBPROBE_RUNS
#define SMBPROBE_RUNS 20
#endif

// Ticket 25's per-file ceiling, and the shape it needs: read the whole file into PSRAM,
// then take the storage lock for a short FAT write. PSRAM holds 7.5 MB free at all times.
//
// -DSMBPROBE_BUF_INTERNAL puts the destination in internal RAM instead, which is what
// ticket 23's inline probe did (a 4 KB static buffer) when it completed 10/10 rounds at
// this same TCP window. The module stalled in round 4 twice with a PSRAM destination, and
// that is the only difference left in the data path, so it is an arm rather than a theory.
// The buffer has to be small enough to fit: internal free is ~247 KB in this build.
#ifdef SMBPROBE_BUF_INTERNAL
#define SMBPROBE_FILE_MAX (192 * 1024)
#define SMBPROBE_BUF_CAPS MALLOC_CAP_INTERNAL
#define SMBPROBE_BUF_WHERE "internal"
#else
#define SMBPROBE_FILE_MAX (4 * 1024 * 1024)
#define SMBPROBE_BUF_CAPS MALLOC_CAP_SPIRAM
#define SMBPROBE_BUF_WHERE "psram"
#endif

// Ticket 25's scan cap, and ticket 23's reason for it: smb2_opendir() materialises the
// whole directory before the first readdir returns, at roughly 0.5 KB per entry.
#define SMBPROBE_LIST_MAX 200

static bool s_sta_connected;
static char s_ip[16];

// Both in PSRAM. The entry array alone is ~54 KB at the cap, which is three times the
// internal RAM this board has spare.
static uint8_t *s_file_buf;
static board_smb_entry_t *s_entries;

// ------------------------------------------------------------------ instrument

// Raw counters only. The verdict is computed at analysis time, where it is free to be
// wrong twice.
static void stamp(int run, const char *stage)
{
    printf("@@SMB %d %s t_us=%lld int_free=%u int_min=%u psram_free=%u psram_min=%u\n",
           run, stage, (long long)esp_timer_get_time(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    fflush(stdout);
}

// The array is static deliberately. A local one overflowed main's 3584-byte stack
// outright in ticket 21 and the board panicked before printing anything.
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

    // The AP on this bench advertises WPA3-Personal first (the Windows profile lists
    // WPA3/GCMP-256 ahead of WPA2/CCMP). Without these three the station associates and
    // then dies in the 4-way handshake -- observed 2026-09-04 as `reason=204`
    // (HANDSHAKE_TIMEOUT) exactly 10 s after `assoc -> run`, which reads like a wrong
    // password and is not one. SAE needs H2E offered, and WPA3 requires PMF.
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Modem sleep off. With the default WIFI_PS_MIN_MODEM the probe blocked inside the
    // read stage -- twice in round 4, once in round 6 -- with the system alive, the smb
    // task never returning, and smb2_set_timeout(10) not rescuing it. Power save turned
    // out not to be the cause (WIFI_PS_NONE made it stall *earlier*); the TCP receive
    // window was, and sdkconfig.smbprobe carries CONFIG_LWIP_TCP_WND_DEFAULT=16384. This
    // line stays so that the run is comparable with the one that established that.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_wifi_connect());

    // 20 s. The WSL Android side measured a connect timeout at 5 s against a real NAS;
    // association is the slower half and this is not the number under test.
    for (int i = 0; i < 200 && !s_sta_connected; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return s_sta_connected ? ESP_OK : ESP_ERR_TIMEOUT;
}

// ----------------------------------------------------------------- the watcher

// A 4 KB read stalls mid-file about once every 175 PDUs and never returns; ticket 23's
// correction has the seven runs. Every hypothesis tested so far has been about the ESP's
// configuration, and the one thing nobody has looked at is whether the bytes are arriving.
// This PC is the SMB server, so the wire could answer it -- but a packet capture needs
// elevation (pktmon, netsh trace) that was not available, so the question is asked from
// the device instead.
//
// The reader task is inside a synchronous libsmb2 call holding the module's lock. This task
// therefore takes the fd once, after connect, and looks at the socket rather than the
// module. What the answer decides:
//
//   readable, bytes pending -> the data arrived and libsmb2 is not consuming it. Its own
//       state machine, and the async API gives a deadline that works.
//   not readable, nothing pending -> nothing is coming. The server or the path stopped, and
//       no client-side API changes that -- only a deadline plus reconnect, and the cause
//       still needs the wire.
static volatile int64_t s_read_begin_us;    // 0 when no read is in flight

// The first run of this watcher only ever sampled stalled reads -- it waited 2.5 s before
// printing, and a healthy read finishes in 1.9 s. It reported readable=0 for 267 s, which
// looks exactly like an answer and is not one: an instrument that has never returned 1
// cannot distinguish "nothing is arriving" from "this measurement does not work". That is
// the ticket-18 shape, an experiment incapable of producing the other outcome.
//
// So it samples every read now, healthy ones included, and the healthy samples are the
// positive control. The question the log has to answer first is whether readable=1 ever
// appears at all.
// Sampled at 20 ms and reported once a second as a count, rather than one line per sample.
// The first controlled run sampled at 250 ms and caught readable=1 exactly once in 80
// healthy samples -- enough to prove select() works on this fd, and far too thin to put
// next to 571 stalled samples. At 20 ms a healthy read of ~45 ms round trips yields many
// samples per arrival, which is what makes the contrast a measurement instead of an
// anecdote. Counting rather than printing keeps the console out of the experiment: a line
// per sample at 115200 baud is a perturbation of the thing being measured.
#define WATCH_PERIOD_MS 20
#define WATCH_REPORT_EVERY 50 // samples, so one line per second

static void watch_task(void *arg)
{
    (void)arg;
    int samples = 0;
    int hits = 0;
    int whits = 0;
    int lines = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WATCH_PERIOD_MS));

        // Asked every sample rather than latched once before the read. A read that hits
        // its deadline now drops the session and reconnects, so the fd changes underneath
        // this task -- a latched one would spend the rest of the run watching a closed
        // socket and reporting readable=0, which is the stall's own signature and would
        // fake it perfectly.
        const int fd = board_smb_fd();
        const int64_t began = s_read_begin_us;
        if (fd < 0 || began == 0) {
            samples = 0;
            hits = 0;
            whits = 0;
            lines = 0;
            continue;
        }
        const int64_t age_ms = (esp_timer_get_time() - began) / 1000;

        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        FD_ZERO(&wr);
        FD_SET(fd, &wr);
        struct timeval tv = {0, 0};
        const int sel = select(fd + 1, &rd, &wr, NULL, &tv);
        samples++;
        if (sel > 0 && FD_ISSET(fd, &rd)) {
            hits++;
        }
        // Write readiness separates "the server stopped answering" from "our request never
        // got out". A socket that is not writable has data stuck in its own send queue,
        // which would put the fault on this side of the link rather than the server's.
        if (sel > 0 && FD_ISSET(fd, &wr)) {
            whits++;
        }

        // FIONREAD is not available on this fd -- ioctl returns errno 88, ENOSYS, because
        // IDF's VFS layer does not forward it to lwIP. So "how many bytes are waiting" is
        // not answerable here; "is anything waiting" is, and that is the question.

        if (samples % WATCH_REPORT_EVERY == 0) {
            // The mid-payload arm. board_smb.h claims, from reading libsmb2's source, that
            // a stalled read is parked in smb2->pdu -- a slot that smb2_timeout_pdus()
            // never scans -- which is why no smb2_set_timeout() value could ever have
            // rescued one. That is an inference until this line runs.
            //
            //   recv_state != 0 (SMB2_RECV_SPL) during a stall  -> confirmed: a response
            //       header arrived, the PDU is off both queues, the payload never came.
            //   recv_state == 0 with pdu=0                      -> refuted: nothing was
            //       received for this command at all, it is still on waitqueue, and the
            //       library's own timeout should have fired. Then the explanation is
            //       something else and this module's comment has to be rewritten.
            //
            // Reading a library's private struct from another task is a layering violation
            // twice over, and it stays inside this probe. The reads are single words taken
            // while the reader task may be writing them, so a torn value is possible; the
            // arm is a repeated observation over a stall lasting minutes, not one sample,
            // which is what makes that acceptable rather than merely convenient.
            const struct smb2_context *ctx = board_smb_context();
            if (ctx) {
                printf("@@RECV age_ms=%lld state=%d num_done=%u spl=%u pdu=%d\n",
                       (long long)age_ms, (int)ctx->recv_state, (unsigned)ctx->in.num_done,
                       (unsigned)ctx->spl, ctx->pdu ? 1 : 0);
            }
            printf("@@WATCH age_ms=%lld fd=%d samples=%d readable=%d writable=%d\n",
                   (long long)age_ms, fd, WATCH_REPORT_EVERY, hits, whits);
            fflush(stdout);
            hits = 0;
            whits = 0;

            // SO_ERROR is read once, and late. Reading it CLEARS it, so it is a
            // perturbation of the thing being measured -- taken only after ten seconds of
            // stall, by which point the behaviour is established and the run has already
            // recorded what it came for.
            if (++lines == 10) {
                int soerr = 0;
                socklen_t len = sizeof(soerr);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0) {
                    printf("@@WATCH so_error=%d (read once, and it clears the flag)\n",
                           soerr);
                } else {
                    printf("@@WATCH so_error=unreadable\n");
                }
                fflush(stdout);
            }
        }
    }
}

// ------------------------------------------------------------------ the config

static void base_config(board_smb_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->host, sizeof(cfg->host), "%s", SMBPROBE_HOST);
    snprintf(cfg->share, sizeof(cfg->share), "%s", SMBPROBE_SHARE);
    snprintf(cfg->path, sizeof(cfg->path), "%s", SMBPROBE_PATH);
    snprintf(cfg->user, sizeof(cfg->user), "%s", SMBPROBE_USER);
    snprintf(cfg->password, sizeof(cfg->password), "%s", SMBPROBE_PASS);
    snprintf(cfg->domain, sizeof(cfg->domain), "%s", SMBPROBE_DOMAIN);
    cfg->enabled = true;
}

// ------------------------------------------------------------------- one round

// Returns 0 on a complete round trip. Every failure is reported as the module's own
// verdict; board_smb.c logs the raw libsmb2 string alongside it at warning level, which
// is what makes the classifier's unmapped-status branch diagnosable rather than merely
// defensible.
static int probe_once(int run)
{
    stamp(run, "begin");

    board_smb_config_t cfg;
    base_config(&cfg);

    // Kept as a stage even though the module owns context creation now: it marks the
    // instant immediately before connect, so the row still lines up with ticket 23's.
    stamp(run, "ctx");

    const int64_t t_connect = esp_timer_get_time();
    board_smb_err_t err = board_smb_connect(&cfg);
    printf("@@SMB %d connect rc=%d dt_us=%lld verdict=\"%s\"\n", run, (int)err,
           (long long)(esp_timer_get_time() - t_connect), board_smb_err_str(err));
    fflush(stdout);
    if (err != BOARD_SMB_OK) {
        stamp(run, "connect_failed");
        return -1;
    }
    stamp(run, "connected");

    const int64_t t_list = esp_timer_get_time();
    size_t count = 0;
    err = board_smb_list(s_entries, SMBPROBE_LIST_MAX, &count, NULL, NULL);
    if (err != BOARD_SMB_OK) {
        printf("@@SMB %d list rc=%d dt_us=%lld verdict=\"%s\"\n", run, (int)err,
               (long long)(esp_timer_get_time() - t_list), board_smb_err_str(err));
        board_smb_disconnect();
        stamp(run, "list_failed");
        return -1;
    }
    // `entries` counts regular files only -- the module skips directories and . / .. ,
    // where ticket 23's inline loop counted every dirent. A count below its 129 is that
    // difference, not a lost file. `cap` is printed so a full listing is visible as full.
    printf("@@SMB %d list rc=0 dt_us=%lld entries=%u cap=%d first=\"%s\" size=%llu\n", run,
           (long long)(esp_timer_get_time() - t_list), (unsigned)count, SMBPROBE_LIST_MAX,
           count ? s_entries[0].name : "", count ? (unsigned long long)s_entries[0].size : 0ULL);
    stamp(run, "listed");

    if (count == 0) {
        printf("# run %d: no regular file in the path; read stage skipped\n", run);
        board_smb_disconnect();
        stamp(run, "done");
        return 0;
    }

    const char *name = s_entries[0].name;
    const uint64_t size = s_entries[0].size;

    const int64_t t_read = esp_timer_get_time();
    size_t len = 0;
    s_read_begin_us = t_read;
    err = board_smb_read_file(name, s_file_buf, SMBPROBE_FILE_MAX, &len);
    s_read_begin_us = 0;
    const int64_t dt_read = esp_timer_get_time() - t_read;
    // A healthy read of this file is ~1.9 s (ticket 23, ten reads, 155,803 bytes). Anything
    // past the chunk deadline can only be a read that hit it, dropped the session and came
    // back -- the recovery this whole change exists to produce. Printed as its own field so
    // the run can be counted rather than read, and cross-checked against board_smb.c's own
    // "session abandoned" warnings in the same capture.
    const int recovered = dt_read > (int64_t)BOARD_SMB_CHUNK_DEADLINE_MS * 1000;
    printf("@@SMB %d read rc=%d dt_us=%lld bytes=%llu of=%llu recovered=%d verdict=\"%s\"\n",
           run, (int)err, (long long)dt_read, (unsigned long long)len,
           (unsigned long long)size, recovered, board_smb_err_str(err));
    fflush(stdout);
    if (err != BOARD_SMB_OK) {
        board_smb_disconnect();
        stamp(run, "read_failed");
        return -1;
    }

    // Byte-exactness. The inline probe read 155,803 of 155,803 bytes ten times and never
    // looked at one of them. Host side is Get-FileHash -Algorithm SHA256 on the same file
    // over the share -- no digest implementation on either end that could be wrong twice.
    uint8_t digest[32];
    char hex[65];
    if (mbedtls_sha256(s_file_buf, len, digest, 0) == 0) {
        for (int i = 0; i < 32; i++) {
            snprintf(hex + i * 2, 3, "%02x", digest[i]);
        }
        // The recovery arm's actual acceptance test. A resumed read reassembles the file
        // from two sessions across a reopen at an offset, and "it returned OK" says
        // nothing about whether the seam is in the right place -- an off-by-one there is a
        // corrupt photograph that every rc=0 in this log would call a success. So the
        // digest of every run is compared against the first run's, and a recovered read
        // has to produce the same bytes as a clean one or the change is refuted.
        static char first_hex[65];
        const char *match = "first";
        if (first_hex[0]) {
            match = strcmp(first_hex, hex) == 0 ? "yes" : "NO";
        } else {
            snprintf(first_hex, sizeof(first_hex), "%s", hex);
        }
        printf("@@SMB %d sha256 len=%u name=\"%s\" digest=%s match=%s\n", run,
               (unsigned)len, name, hex, match);
    } else {
        printf("@@SMB %d sha256 FAILED\n", run);
    }
    fflush(stdout);
    stamp(run, "read");

    board_smb_disconnect();
    stamp(run, "done");
    return 0;
}

// --------------------------------------------------------------- boundary arms

// Two reads whose sizes bracket the buffer, run once. The module reports a file larger
// than the buffer as a failure rather than handing back a short read the caller cannot
// tell from a whole file -- so the question is where it puts the boundary, and an
// off-by-one there is a 4 MB photograph that never syncs.
static void size_boundary_arms(const char *name, uint64_t size)
{
    board_smb_config_t cfg;
    base_config(&cfg);
    if (board_smb_connect(&cfg) != BOARD_SMB_OK) {
        printf("# boundary arms skipped: connect failed\n");
        return;
    }

    // Far too small. Expect NOTFOUND and len 0, after one round trip rather than a whole
    // file's worth.
    size_t len = 12345;
    board_smb_err_t err = board_smb_read_file(name, s_file_buf, 1024, &len);
    printf("@@ARM oversize max=1024 rc=%d len=%u verdict=\"%s\" expect=\"not found\"\n",
           (int)err, (unsigned)len, board_smb_err_str(err));

    // Exactly the file's size. It fits; OK with len == size is the only correct answer.
    len = 12345;
    err = board_smb_read_file(name, s_file_buf, (size_t)size, &len);
    printf("@@ARM exactfit max=%llu rc=%d len=%u verdict=\"%s\" expect=\"ok\"\n",
           (unsigned long long)size, (int)err, (unsigned)len, board_smb_err_str(err));
    fflush(stdout);

    board_smb_disconnect();
}

// ------------------------------------------------------- read depth sweep (ticket 33)

#ifndef SMBPROBE_DEPTH_PASSES
#define SMBPROBE_DEPTH_PASSES 10
#endif

// Round-robin, not blocked. A share that warms up, a radio that drifts or a server that
// caches would all show up as a monotonic trend across a blocked sweep and be read as a
// depth effect; interleaved, that same trend lands on every depth equally.
static const int k_depths[] = {1, 2, 4, 8, 16};

// One session for the whole sweep, deliberately: the question is what a read costs, and a
// connect per sample would add ~60 ms of session setup to every row. board_smb_read_file()
// reconnects by itself if a stall takes the session, so this does not hide a failure.
static void read_depth_sweep(const char *name, uint64_t size, const char *arm, int dialect)
{
    board_smb_config_t cfg;
    base_config(&cfg);
    board_smb_set_dialect(dialect);
    if (board_smb_connect(&cfg) != BOARD_SMB_OK) {
        printf("# depth sweep \"%s\" skipped: connect failed at dialect 0x%04x\n", arm,
               (unsigned)dialect);
        board_smb_set_dialect(0);
        return;
    }
    // The negotiated max read size is not the dialect, but it is a fingerprint of the
    // negotiation: two arms that print the same one did not negotiate differently, and an
    // arm whose numbers move while this does not is measuring something else.
    printf("# arm \"%s\": requested dialect 0x%04x, max_read_size=%u, aes=%s\n", arm,
           (unsigned)dialect,
           (unsigned)smb2_get_max_read_size((struct smb2_context *)board_smb_context()),
           smb_aes_hw_enabled() ? "hardware" : "software");

    // The trace is one printf per group, so at depth 1 it is 39 lines a read and at depth
    // 16 it is 3. Left on it would pay the deeper arms a bonus in serial traffic they did
    // not earn, which is the wrong direction to be wrong in.
    board_smb_set_trace(false);

    printf("# depth sweep \"%s\": %d passes over %u depths, one session, trace off, file "
           "\"%s\" of %llu bytes\n",
           arm, SMBPROBE_DEPTH_PASSES,
           (unsigned)(sizeof(k_depths) / sizeof(k_depths[0])), name,
           (unsigned long long)size);
    fflush(stdout);

    char first_hex[65] = {0};
    for (int pass = 1; pass <= SMBPROBE_DEPTH_PASSES; pass++) {
        for (size_t k = 0; k < sizeof(k_depths) / sizeof(k_depths[0]); k++) {
            const int depth = k_depths[k];
            board_smb_set_read_depth(depth);

            // board_smb_read_file() reconnects between its own attempts but refuses to
            // start without a session, so a stall that outlives all three attempts leaves
            // every later read in the sweep returning ERR_CONNECT in ~10 us. The first run
            // of this arm lost 47 of 50 samples that way and the survivors looked like a
            // depth effect. Reconnecting here is what the mirror does per run.
            if (!board_smb_is_connected()) {
                base_config(&cfg);
                const board_smb_err_t re = board_smb_connect(&cfg);
                printf("# sweep \"%s\": session was gone, reconnect rc=%d\n", arm, (int)re);
                fflush(stdout);
            }

            size_t len = 0;
            const int64_t t0 = esp_timer_get_time();
            const board_smb_err_t err =
                board_smb_read_file(name, s_file_buf, SMBPROBE_FILE_MAX, &len);
            const int64_t dt = esp_timer_get_time() - t0;

            // The digest is the arm's acceptance test, not a nicety: a wrong offset in the
            // group scheduler produces a corrupt file that every rc=0 here would call a
            // success. Compared against the first read of the sweep, whatever depth that
            // was, because all of them must produce the same bytes.
            char hex[65] = "n/a";
            uint8_t digest[32];
            if (err == BOARD_SMB_OK && mbedtls_sha256(s_file_buf, len, digest, 0) == 0) {
                for (int i = 0; i < 32; i++) {
                    snprintf(hex + i * 2, 3, "%02x", digest[i]);
                }
            }
            const char *match = "first";
            if (first_hex[0]) {
                match = strcmp(first_hex, hex) == 0 ? "yes" : "NO";
            } else {
                snprintf(first_hex, sizeof(first_hex), "%s", hex);
            }

            wifi_ap_record_t ap;
            const int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;

            printf("@@DEPTH arm=%s pass=%d depth=%d rc=%d dt_us=%lld bytes=%u of=%llu "
                   "int_free=%u int_min=%u rssi=%d match=%s digest=%s\n",
                   arm, pass, depth, (int)err, (long long)dt, (unsigned)len,
                   (unsigned long long)size,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), rssi,
                   match, hex);
            fflush(stdout);

            // Spaced, for the reason every other measurement here is: back-to-back
            // operations on this board have looked far more repeatable than spaced ones
            // and the difference was the spacing.
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    board_smb_set_read_depth(BOARD_SMB_READ_DEPTH);
    board_smb_set_trace(true);
    board_smb_disconnect();
    board_smb_set_dialect(0);
    printf("# depth sweep \"%s\" done\n", arm);
    fflush(stdout);
}

// The empty-listing contract, asserted rather than assumed: a caller asking for at most
// zero entries gets OK with zero, not an error. Conflating "no files" with "failed" is the
// bug ComittoNxA shipped in 1c6e844; treating a failure as empty is its equal and
// opposite. Run once, outside the measured rounds: as a per-round check it added a second
// full smb2_opendir() to every round, which is both traffic ticket 23's rounds did not
// carry and a difference in the very comparison the rounds exist to make.
static void empty_listing_arm(void)
{
    board_smb_config_t cfg;
    base_config(&cfg);
    if (board_smb_connect(&cfg) != BOARD_SMB_OK) {
        printf("# list_zero arm skipped: connect failed\n");
        return;
    }
    size_t count = 12345;
    const board_smb_err_t err = board_smb_list(s_entries, 0, &count, NULL, NULL);
    printf("@@ARM list_zero rc=%d count=%u verdict=\"%s\" expect=\"ok\"\n", (int)err,
           (unsigned)count, board_smb_err_str(err));
    fflush(stdout);
    board_smb_disconnect();
}

// ------------------------------------------------- the big-directory arm (ticket 37 Phase 0)

// Can a directory of ~1,070 entries be listed at all, and what does it cost in internal RAM?
// Ticket 37 wants an index of a WHOLE share so the frame can pick from all of it at random
// instead of from the alphabetically-last 200; that is only possible if smb2_opendir on such a
// directory fits. libsmb2 materialises the entire listing before the first smb2_readdir()
// returns and has no batched API, so this is the one question the design cannot be built past.
//
// PRE-REGISTERED, and the arithmetic that says "impossible" is already refuted rather than
// untested. app_smb_sync.h:109 quotes 0.5 KB an entry, which puts 1,070 entries at ~535 KB and
// this board has nothing like that; docs/agents/measurements.md says in its own heading that
// the figure "cannot be extrapolated -- it was a decomposition of one listing, not a slope",
// and the two dated points are 129 entries -> ~60 KB and 243 entries -> ~61 KB, about 9 bytes
// an entry. So:
//
//   confirmed -> the dip at ~1,070 entries stays near ~61-70 KB and the call returns OK. The
//       flattening holds, a whole-share index is affordable, and ticket 37 Phase 1 builds it
//       in one pass.
//   refuted -> the dip scales, or the call fails for want of memory. Then the index has to be
//       built with a CURSOR over several runs, which is more code, and finding that out here
//       costs one arm instead of a rewrite.
//   invalid, NOT inconclusive -> if `dirents` is not in the output. Every earlier figure was
//       labelled with the count that survived the cap, and that is how a 243-entry measurement
//       came to be written down as a 200-entry one. max=0 is passed for exactly this reason:
//       the arm keeps nothing, so `kept` cannot be mistaken for the directory's size.
//
// IT RUNS WITH THE OTHER ARMS, BEFORE THE MEASUREMENT ROUNDS, for smb_task()'s own reason: a
// stalled read never returns, so anything sequenced after the rounds may never be collected at
// all. Placing it last "so it cannot hurt the rounds" was tried first and is exactly backwards
// -- it makes the one arm this instrument was rebuilt for depend on twenty stall-prone reads
// finishing. The cost of being early is that it could in principle exhaust internal RAM and
// take the rounds with it; env:smbprobe has ~247 KB free and disconnects immediately after, so
// that trade is the cheap direction.
#ifndef SMBPROBE_BIG_PATH
// The share root, which on this bench is where the 1,070 files are. SMBPROBE_PATH points at a
// subfolder for the read arms, so this is deliberately its own flag rather than reusing it.
#define SMBPROBE_BIG_PATH ""
#endif

static void big_directory_arm(void)
{
    board_smb_config_t cfg;
    base_config(&cfg);
    snprintf(cfg.path, sizeof(cfg.path), "%s", SMBPROBE_BIG_PATH);

    printf("# big-directory arm: share=\"%s\" path=\"%s\" (empty means the share root)\n",
           cfg.share, cfg.path);
    stamp(0, "bigdir_before");

    if (board_smb_connect(&cfg) != BOARD_SMB_OK) {
        printf("@@BIGDIR skipped=connect_failed\n");
        fflush(stdout);
        return;
    }

    // max=0 with a real destination: board_smb_list refuses a NULL `out` but keeps nothing at
    // zero, so the listing is paid for and not stored. `truncated` is expected true for any
    // directory with an accepted entry in it, and says nothing here beyond "more than zero".
    size_t count = 12345;
    bool truncated = false;
    const int64_t t0 = esp_timer_get_time();
    const board_smb_err_t err = board_smb_list(s_entries, 0, &count, NULL, &truncated);
    const int64_t dt = esp_timer_get_time() - t0;

    board_smb_list_stats_t st;
    board_smb_get_list_stats(&st);
    printf("@@BIGDIR rc=%d verdict=\"%s\" dt_us=%lld kept=%u truncated=%d dirents=%u files=%u "
           "accepted=%u open_ms=%u walk_ms=%u int_free=%u/%u/%u int_min=%u/%u "
           "dma_largest=%u/%u\n",
           (int)err, board_smb_err_str(err), (long long)dt, (unsigned)count, truncated ? 1 : 0,
           (unsigned)st.dirents, (unsigned)st.files, (unsigned)st.accepted,
           (unsigned)st.open_ms, (unsigned)st.walk_ms, (unsigned)st.int_free_before,
           (unsigned)st.int_free_open, (unsigned)st.int_free_after, (unsigned)st.int_min_before,
           (unsigned)st.int_min_after, (unsigned)st.dma_largest_before,
           (unsigned)st.dma_largest_after);
    fflush(stdout);

    board_smb_disconnect();
    stamp(0, "bigdir_after");
    print_task_table("bigdir");
}

// -------------------------------------------------------------- failure arms

// One round per failure class, each derived at runtime from the configured credentials so
// that a single build covers all of them. Ticket 23 measured these as libsmb2 *strings*
// and ticket 24 tested the classifier against those strings on the host; what has never
// run is the real path -- smb2_get_error() -> fail_locked() -> board_smb_classify() -> a
// verdict -- on the device. That is the shape of the ComittoNxA bug this module exists to
// avoid: every host test green, the defect visible only on hardware.
static void failure_arm(const char *label, const board_smb_config_t *cfg,
                        const char *expect)
{
    const int64_t t0 = esp_timer_get_time();
    board_smb_err_t err = board_smb_connect(cfg);
    const char *phase = "connect";
    if (err == BOARD_SMB_OK) {
        // Connect succeeded, so the failure this arm is after is at the listing.
        size_t count = 0;
        err = board_smb_list(s_entries, SMBPROBE_LIST_MAX, &count, NULL, NULL);
        phase = "list";
        if (err == BOARD_SMB_OK) {
            printf("@@ARM %s phase=%s rc=0 dt_us=%lld verdict=\"ok\" expect=\"%s\" "
                   "entries=%u\n",
                   label, phase, (long long)(esp_timer_get_time() - t0), expect,
                   (unsigned)count);
            fflush(stdout);
            board_smb_disconnect();
            return;
        }
    }
    printf("@@ARM %s phase=%s rc=%d dt_us=%lld verdict=\"%s\" expect=\"%s\"\n", label,
           phase, (int)err, (long long)(esp_timer_get_time() - t0),
           board_smb_err_str(err), expect);
    fflush(stdout);
    // No disconnect: on any failure the module has already destroyed the context and set
    // its handle to NULL, which is the never-cache-a-failure rule the WSL side paid for.
    printf("@@ARM %s connected_after=%d\n", label, (int)board_smb_is_connected());
    fflush(stdout);
}

static void failure_arms(void)
{
    board_smb_config_t cfg;

    base_config(&cfg);
    strncat(cfg.password, "x", sizeof(cfg.password) - strlen(cfg.password) - 1);
    failure_arm("auth", &cfg, "auth");
    stamp(0, "arm_auth");

    base_config(&cfg);
    strncat(cfg.share, "-nope", sizeof(cfg.share) - strlen(cfg.share) - 1);
    failure_arm("share", &cfg, "share");
    stamp(0, "arm_share");

    base_config(&cfg);
    snprintf(cfg.path, sizeof(cfg.path), "does-not-exist");
    failure_arm("notfound", &cfg, "not found");
    stamp(0, "arm_notfound");

    if (SMBPROBE_DENIED_SHARE[0]) {
        base_config(&cfg);
        snprintf(cfg.share, sizeof(cfg.share), "%s", SMBPROBE_DENIED_SHARE);
        cfg.path[0] = '\0';
        failure_arm("denied", &cfg, "denied");
    } else {
        printf("# DENIED arm skipped: no SMBPROBE_DENIED_SHARE\n");
    }
    stamp(0, "arm_denied");

    // TEST-NET-1 (RFC 5737). Never routable, so this is the dead-host case and not a
    // machine that might answer. Costs about 11 s: smb2_set_timeout does fire for connect.
    base_config(&cfg);
    snprintf(cfg.host, sizeof(cfg.host), "192.0.2.1");
    failure_arm("connect", &cfg, "connect");
    stamp(0, "arm_connect");
}

// ---------------------------------------------------------------------- driver

#ifndef SMBPROBE_TASK_STACK
#define SMBPROBE_TASK_STACK 16384
#endif

static SemaphoreHandle_t s_task_done;

static void smb_task(void *arg)
{
    (void)arg;
    int ok = 0;
    char first_name[BOARD_SMB_NAME_SIZE] = {0};
    uint64_t first_size = 0;

    // Round 1 first, then every arm, then the rest of the rounds.
    //
    // The order is not cosmetic. A read stalls mid-file often enough that a run has been
    // ended by one at rounds 1, 2, 4, 4 and 7 -- including twice with ticket 23's own
    // unmodified inline probe, which is why the stall is known not to be this module's.
    // A stalled task never returns, so anything sequenced after the rounds is evidence
    // that may simply never be collected. The arms are deterministic, cost about 15 s in
    // total, and are what ticket 24 is actually about; the rounds after them are the
    // statistics, and losing some of those to a stall costs a median's precision rather
    // than the ticket's answer.
    if (probe_once(1) == 0) {
        ok++;
    }
    // The name comes from the LIST stage, so it is taken whether or not the read succeeded.
    // It used to be inside the success branch, and on 2026-09-06 a round-1 read that failed
    // took the boundary arms and the whole depth sweep with it -- "no file was read" three
    // times over, from a run whose listing had worked perfectly. A failing subject must not
    // blind the instrument.
    if (s_entries[0].name[0]) {
        snprintf(first_name, sizeof(first_name), "%s", s_entries[0].name);
        first_size = s_entries[0].size;
    }
    print_task_table("run");
    vTaskDelay(pdMS_TO_TICKS(2000));

    if (first_name[0]) {
        size_boundary_arms(first_name, first_size);
    } else {
        printf("# boundary arms skipped: no file was read\n");
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Ticket 33, before the measurement rounds for the same reason the other arms are: a
    // stall ends the task, and the rounds are statistics while this is the answer.
    //
    // Two arms, differing only in the dialect asked for, because the dialect chooses the
    // signature algorithm and ticket 33 measured signature verification as 21 of the 29 ms
    // a 4 KB read costs. `any` is what the frame ships and is the control; `smb210` takes
    // libsmb2's HMAC-SHA256 branch instead of its software AES-CMAC one. If the two arms
    // agree, the AES branch is not the cost and the reading is wrong.
    if (first_name[0]) {
        // Hardware AES against the component's own portable C, one variable, one run. The
        // hw arm is the shipping path; the sw arm is what it replaced, and it is also the
        // arm that says whether the wrap has anything to do with the wrong-signature
        // failures that appear above depth 1 at this TCP window.
        smb_aes_hw_set_enabled(true);
        read_depth_sweep(first_name, first_size, "aes-hw", 0);
        vTaskDelay(pdMS_TO_TICKS(2000));
        smb_aes_hw_set_enabled(false);
        read_depth_sweep(first_name, first_size, "aes-sw", 0);
        smb_aes_hw_set_enabled(true);
    } else {
        printf("# depth sweep skipped: no file was read\n");
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    empty_listing_arm();
    vTaskDelay(pdMS_TO_TICKS(2000));

    failure_arms();
    vTaskDelay(pdMS_TO_TICKS(2000));

    // WITH THE ARMS, NOT AFTER THE ROUNDS, and the first version of this got it backwards.
    // "Run it last so nothing above depends on it surviving" sounds prudent and is the wrong
    // way round on this instrument: a read stalls mid-file often enough that this task has been
    // ended by one at rounds 1, 2, 4, 4 and 7, and a stalled task never returns. Anything
    // sequenced after the rounds is evidence that may simply never be collected -- which is
    // what the comment above the round-1 call already says, in as many words. This arm is the
    // answer this flash exists for; the rounds are statistics that have been taken before.
    big_directory_arm();

    printf("# arms done; %d measurement rounds follow\n", SMBPROBE_RUNS - 1);
    fflush(stdout);

    for (int run = 2; run <= SMBPROBE_RUNS; run++) {
        // Space the runs out. Back-to-back operations on this board have looked ten
        // times more repeatable than spaced ones before, and the difference was the
        // spacing (docs/agents/method.md, the refresh timings).
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (probe_once(run) == 0) {
            ok++;
        }
        print_task_table("run");
    }

    printf("# smb task stack high-water mark: %u bytes free of %d\n",
           (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
           SMBPROBE_TASK_STACK);
    printf("# smbprobe complete: %d/%d rounds succeeded\n", ok, SMBPROBE_RUNS);
    stamp(0, "end");
    print_task_table("end");
    xSemaphoreGive(s_task_done);
    vTaskDelete(NULL);
}

static bool credentials_present(const char *ssid)
{
    return ssid[0] && SMBPROBE_HOST[0] && SMBPROBE_SHARE[0] && SMBPROBE_USER[0] &&
           SMBPROBE_PASS[0];
}

void app_main(void)
{
    printf("\n# smbprobe: tickets 23 and 24, board_smb.* against a real share\n");

    // NVS first, build flags second. The station credentials the frame uses are the ones
    // the Web UI wrote (`/api/wifi/config` -> app_settings_set_wifi), and NVS survives an
    // app upload -- so configuring Wi-Fi once in a browser configures this build too, and
    // no Wi-Fi secret has to exist in any file. The build-flag fallback stays because this
    // PC cannot join a Wi-Fi network programmatically: `netsh wlan connect` needs the
    // machine's location permission, which is set to Deny, so on 2026-09-04 the Web UI
    // could not be reached from here at all.
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
        snprintf(ssid, sizeof(ssid), "%s", SMBPROBE_WIFI_SSID);
        snprintf(pass, sizeof(pass), "%s", SMBPROBE_WIFI_PASS);
        cred_source = "build-flags";
    }
    printf("# wifi credentials from %s, ssid=\"%s\" pass_len=%u\n", cred_source, ssid,
           (unsigned)strlen(pass));

    // Persist what we are about to use, into the same NVS keys the Web UI writes
    // (`/api/wifi/config` -> app_settings_set_wifi). That is what lets env:frame be
    // flashed afterwards and come up on the station without anyone typing anything --
    // which matters here because this PC cannot join the frame's own AP: `netsh wlan
    // connect` needs the machine's location permission and it is set to Deny.
    if (strcmp(cred_source, "build-flags") == 0) {
        const esp_err_t serr = app_settings_set_wifi(ssid, pass);
        printf("# seeded NVS wifi_ssid/wifi_pass: %s\n", esp_err_to_name(serr));
    }

    if (!credentials_present(ssid)) {
        // A run against an unauthenticated or absent share would pass while proving
        // nothing about the NTLMSSP and signing paths -- which are precisely the parts
        // that broke twice on the WSL Android side. That is a false all-clear of the
        // shape ticket 18 produced three times in a row, so it is refused here.
        printf("# REFUSING TO RUN: build without credentials.\n");
        printf("# Required: SMBPROBE_WIFI_SSID SMBPROBE_WIFI_PASS SMBPROBE_HOST\n");
        printf("#           SMBPROBE_SHARE SMBPROBE_USER SMBPROBE_PASS\n");
        printf("# Optional: SMBPROBE_PATH SMBPROBE_DOMAIN SMBPROBE_RUNS\n");
        printf("#           SMBPROBE_DENIED_SHARE (the DENIED arm, else skipped)\n");
        printf("# The share must be a real NAS with a real password. An open share\n");
        printf("# makes this experiment incapable of producing a different answer.\n");
        return;
    }

    stamp(0, "boot");
    print_task_table("boot");

    // PSRAM, both of them, allocated once before any measurement so that no round pays
    // for them and no internal RAM does either. 7.5 MB is free at all times (ticket 21).
    s_file_buf = heap_caps_malloc(SMBPROBE_FILE_MAX, SMBPROBE_BUF_CAPS);
    s_entries = heap_caps_malloc(sizeof(board_smb_entry_t) * SMBPROBE_LIST_MAX,
                                 MALLOC_CAP_SPIRAM);
    if (!s_file_buf || !s_entries) {
        printf("# PSRAM allocation failed: file_buf=%p entries=%p\n", s_file_buf,
               (void *)s_entries);
        return;
    }
    // Zeroed: smb_task reads s_entries[0] to pick the file for the boundary arms, and a
    // run that fails before listing must not leave it reading uninitialised PSRAM.
    memset(s_entries, 0, sizeof(board_smb_entry_t) * SMBPROBE_LIST_MAX);
    printf("# buffers: file=%d bytes in %s, entries=%u bytes in psram (%d entries)\n",
           SMBPROBE_FILE_MAX, SMBPROBE_BUF_WHERE,
           (unsigned)(sizeof(board_smb_entry_t) * SMBPROBE_LIST_MAX), SMBPROBE_LIST_MAX);
    stamp(0, "buffers");

    if (wifi_station_up(ssid, pass) != ESP_OK) {
        printf("# sta did not associate; nothing below this line is measurable\n");
        return;
    }
    stamp(0, "wifi");
    print_task_table("wifi");

    // The rounds run in their own task, not in main. main's stack is 4608 bytes and
    // libsmb2's connect path does not fit in it: the first attempt died with
    // "***ERROR*** A stack overflow in task main has been detected" between the `ctx` and
    // `connected` stamps, on every one of 184 boots. 16 KB is a starting point, not a
    // measurement -- the point of printing this task's own high-water mark below is that
    // ticket 25 has to budget the sync task out of internal RAM, where 650 bytes was once
    // the floor. Ticket 23 measured the peak at 5,208 bytes and settled on 8 KB for the
    // sync task; this stays at 16 KB so the figure is taken the same way twice.
    s_task_done = xSemaphoreCreateBinary();
    if (!s_task_done) {
        printf("# no memory for the completion semaphore\n");
        return;
    }
    if (xTaskCreate(smb_task, "smb", SMBPROBE_TASK_STACK, NULL, 5, NULL) != pdPASS) {
        printf("# xTaskCreate(smb) failed at %d bytes of stack\n", SMBPROBE_TASK_STACK);
        return;
    }
    // Higher priority than the reader so that a stall cannot starve the observer of it.
    if (xTaskCreate(watch_task, "smbwatch", 3072, NULL, 6, NULL) != pdPASS) {
        printf("# xTaskCreate(smbwatch) failed; the stall question goes unasked\n");
    }
    // Heartbeat while the rounds run. A silent console has meant three different things on
    // this board -- powered off, hung, and a task blocked in a library call -- and they are
    // not distinguishable without something that prints on its own.
    while (xSemaphoreTake(s_task_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        stamp(0, "alive");
    }
}

#endif // BUILD_SMBPROBE
