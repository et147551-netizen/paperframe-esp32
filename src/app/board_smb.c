// See board_smb.h.

#include "board_smb.h"

#include <stdio.h>
#include <string.h>
#include <sys/poll.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "smb2/libsmb2.h"
#include "smb2/smb2.h"

static const char *TAG = "smb";

// Seconds, per libsmb2 command. Kept because it still bounds the parts of connect that
// run before there is a socket to poll. It is NOT what bounds a read -- see the header
// for why it cannot be.
#define BOARD_SMB_TIMEOUT_S 10

// Three, up from two. Measured: a stall every ~4.5 files, so two attempts leave ~5 % of
// photographs failing and three leave ~1 %, and a recovered attempt costs a 59 ms
// reconnect plus a 1.9 s re-read against a 15.6 s panel refresh. Still bounded, for the
// reason the two was: ComittoNxA's WorkStream.read() recurses on a read failure with no
// bound at all, which on this board's trimmed stacks is a panic rather than a slow
// failure.
#define BOARD_SMB_READ_ATTEMPTS 3

// How long one poll() may sleep. The deadline is checked once per slice, so this is the
// granularity of every deadline below; 200 ms is far finer than the shortest of them and
// costs five wakeups a second on a task that is otherwise blocked.
#define BOARD_SMB_POLL_SLICE_MS 200

static struct smb2_context *s_smb2;
static board_smb_config_t s_cfg;
static SemaphoreHandle_t s_mutex;
static int s_dialect; // 0 = let the server pick, which is the library's default

void board_smb_set_dialect(int version) { s_dialect = version; }

static void lock(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

static const char *phase_str(board_smb_phase_t phase)
{
    switch (phase) {
    case BOARD_SMB_PHASE_CONNECT:
        return "connect";
    case BOARD_SMB_PHASE_LIST:
        return "list";
    case BOARD_SMB_PHASE_READ:
        return "read";
    }
    return "?";
}

static inline int64_t deadline_in(int ms)
{
    return esp_timer_get_time() + (int64_t)ms * 1000;
}

// ------------------------------------------------------------------ the service loop
//
// Every libsmb2 call in this module goes through here. See the header for why the
// synchronous API cannot be used: its own wait loop cannot see a command that has stalled
// between its response header and its payload, so no timeout it offers can fire for one.

typedef enum {
    SMB_SERVICE_OK = 0,   // the callback fired
    SMB_SERVICE_DEADLINE, // the deadline passed with the command still in flight
    SMB_SERVICE_FATAL,    // poll() or smb2_service() failed; the context is unusable
} smb_service_t;

// Lives on the caller's stack for the whole of one command, deliberately. libsmb2's own
// sync wrappers heap-allocate this and have their callbacks free it on a cancellation,
// because their frame is gone by then; ours is not. smb2_destroy_context() invokes the
// callback of every queued PDU before returning (managed_components/sahlberg__libsmb2/
// lib/init.c:326-344), so an abandoned command writes here while the frame is still live,
// and no command can reference it after the destroy returns.
struct smb_wait {
    int done;
    int status;
    void *ptr;
};

// Where one command's wall clock went, for the ~45 ms per 4 KB round trip that ticket 23
// measured and did not explain. Raw stamps only: a derived "the server was slow" boolean
// is a second thing that can be wrong, and on this board finding out costs a rebuild.
// Filled by service_until() when the caller passes one; NULL everywhere else.
struct smb_stamps {
    int64_t written_us;  // the smb2_service() that put the request on the wire returned
    int64_t first_in_us; // the first poll() that came back readable for this command
    // How many times round the loop, and how many of those had data. Two counters rather
    // than a guess: a long first-byte-to-done interval is either a payload arriving in many
    // pieces or a loop turning over without any, and the two have different causes.
    int polls;
    int readable;
};

// For commands whose result is a status: connect, pread, close, disconnect.
static void wait_cb_status(struct smb2_context *smb2, int status, void *command_data,
                           void *private_data)
{
    (void)smb2;
    (void)command_data;
    struct smb_wait *w = private_data;
    w->done = 1;
    w->status = status;
}

// For commands whose result is a handle: open, opendir. These report failure as a NULL
// handle and never touch status -- copied from libsmb2's own open_cb/opendir_cb, where
// the same asymmetry lives.
static void wait_cb_ptr(struct smb2_context *smb2, int status, void *command_data,
                        void *private_data)
{
    (void)smb2;
    (void)status;
    struct smb_wait *w = private_data;
    w->done = 1;
    w->ptr = command_data;
}

// Drives the context until the callback fires or the deadline passes. Never destroys the
// context -- the caller decides what an abandoned command means.
//
// It deliberately does not call smb2_timeout_pdus(). That is the mechanism this module
// exists to route around, and it is also private to the library.
// `n` commands, not one: the read path keeps several preads in flight and every other
// caller passes 1. One loop rather than two, because the loop is what this module is.
static smb_service_t service_until(struct smb_wait *w, int n, int64_t deadline_us,
                                   struct smb_stamps *st)
{
    for (;;) {
        bool pending = false;
        for (int i = 0; i < n; i++) {
            if (!w[i].done) {
                pending = true;
                break;
            }
        }
        if (!pending) {
            return SMB_SERVICE_OK;
        }

        const int64_t left_us = deadline_us - esp_timer_get_time();
        if (left_us <= 0) {
            return SMB_SERVICE_DEADLINE;
        }
        int ms = (int)(left_us / 1000);
        if (ms > BOARD_SMB_POLL_SLICE_MS) {
            ms = BOARD_SMB_POLL_SLICE_MS;
        }
        if (ms < 1) {
            ms = 1;
        }

        const int fd = (int)smb2_get_fd(s_smb2);
        if (fd < 0) {
            // Connect, before there is a socket: address resolution and the first
            // connect() attempt run with no fd to wait on. Sleeping the slice keeps the
            // deadline honest without depending on poll()'s handling of a negative fd.
            vTaskDelay(pdMS_TO_TICKS(ms));
            continue;
        }

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = (short)smb2_which_events(s_smb2);
        pfd.revents = 0;

        const int rc = poll(&pfd, 1, ms);
        if (st) {
            st->polls++;
        }
        if (rc < 0) {
            ESP_LOGW(TAG, "poll failed on fd %d", fd);
            return SMB_SERVICE_FATAL;
        }
        if (rc == 0) {
            continue;
        }
        // Before the service call: this is when the socket had data, which is the arrival
        // the round-trip question is about.
        if (st && (pfd.revents & POLLIN)) {
            st->readable++;
            if (!st->first_in_us) {
                st->first_in_us = esp_timer_get_time();
            }
        }
        const bool wrote = (pfd.revents & POLLOUT) != 0;
        if (smb2_service(s_smb2, pfd.revents) < 0) {
            ESP_LOGW(TAG, "smb2_service: %s", smb2_get_error(s_smb2));
            return SMB_SERVICE_FATAL;
        }
        // After it: smb2_write_to_socket() runs inside smb2_service(), so the bytes are
        // with lwIP by the time it returns.
        if (st && !st->written_us && wrote) {
            st->written_us = esp_timer_get_time();
        }
    }
}

// ------------------------------------------------------------------ session teardown

// Always both halves. Leaving a destroyed context in s_smb2 is the cached-failure bug
// with extra steps.
//
// The disconnect is bounded like everything else here: smb2_disconnect_share() is
// synchronous, so on a peer that has stopped answering it would hang in exactly the place
// this module is meant to stop hanging. If the deadline passes, the context is destroyed
// regardless -- which is all a caller of this function ever wanted.
#define BOARD_SMB_DISCONNECT_DEADLINE_MS 3000

static void teardown_locked(bool disconnect)
{
    if (!s_smb2) {
        return;
    }
    if (disconnect) {
        struct smb_wait w = {0};
        if (smb2_disconnect_share_async(s_smb2, wait_cb_status, &w) == 0) {
            (void)service_until(&w, 1, deadline_in(BOARD_SMB_DISCONNECT_DEADLINE_MS), NULL);
        }
    }
    smb2_destroy_context(s_smb2);
    s_smb2 = NULL;
}

// The server answered, and what it answered with is a failure. Classified and reported.
static board_smb_err_t fail_locked(board_smb_phase_t phase, bool disconnect)
{
    const char *err = s_smb2 ? smb2_get_error(s_smb2) : NULL;
    const board_smb_err_t code = board_smb_classify(phase, err);
    // The raw string as well as the verdict: the classifier's unmapped-status branch is a
    // judgement call, and this line is what makes it diagnosable.
    ESP_LOGW(TAG, "%s -> %s (libsmb2: %s)", phase_str(phase), board_smb_err_str(code),
             err ? err : "");
    teardown_locked(disconnect);
    return code;
}

// The server did not answer at all. Nothing here is classifiable, so it is not run
// through the classifier; the session is dropped without a disconnect, because the
// disconnect would be asking the same silent peer one more question.
//
// One byte of honesty about the cost: libsmb2 frees the command that was mid-payload
// without calling its callback (lib/init.c:340-343), so the ~40-byte struct that command
// allocated for its own bookkeeping leaks. At the measured stall rate that is tens of
// bytes per sync, on a board where internal RAM is the scarce resource -- small, but not
// zero, and recorded here rather than discovered later.
static board_smb_err_t abandon_locked(board_smb_phase_t phase, smb_service_t sv)
{
    ESP_LOGW(TAG, "%s %s -- dropping the session (libsmb2: %s)", phase_str(phase),
             sv == SMB_SERVICE_DEADLINE ? "hit its deadline" : "lost its socket",
             smb2_get_error(s_smb2) ? smb2_get_error(s_smb2) : "");
    teardown_locked(false);

    // A deadline during connect is ERR_CONNECT, not ERR_TIMEOUT. There is no session yet,
    // so "the server stopped responding" and "cannot reach the server" are the same event
    // seen from here, and the second is the one a user can act on. Measured 2026-09-04:
    // without this, the dead-host arm reported "the server stopped responding" for an
    // address with nothing at it, where every previous run said "cannot reach the server".
    // ERR_TIMEOUT earns its place by being the *mid-transfer* answer -- host reachable,
    // credentials right, this file failed -- and it stops meaning that if connect uses it.
    if (phase == BOARD_SMB_PHASE_CONNECT) {
        return BOARD_SMB_ERR_CONNECT;
    }
    return sv == SMB_SERVICE_DEADLINE ? BOARD_SMB_ERR_TIMEOUT : BOARD_SMB_ERR_CONNECT;
}

// ------------------------------------------------------------------ connect

// s_cfg is already set. Split out of board_smb_connect so that the read path can
// re-establish a session it just abandoned without deadlocking on the module's mutex.
static board_smb_err_t connect_locked(void)
{
    s_smb2 = smb2_init_context();
    if (!s_smb2) {
        ESP_LOGE(TAG, "smb2_init_context failed");
        return BOARD_SMB_ERR_CONNECT;
    }

    if (s_dialect) {
        // Ticket 33: the dialect picks the signature algorithm, and this server requires
        // signing. Left at the library's default unless something asked.
        smb2_set_version(s_smb2, (enum smb2_negotiate_version)s_dialect);
    }
    smb2_set_user(s_smb2, s_cfg.user);
    smb2_set_password(s_smb2, s_cfg.password);
    if (s_cfg.domain[0]) {
        smb2_set_domain(s_smb2, s_cfg.domain);
    }
    smb2_set_security_mode(s_smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_timeout(s_smb2, BOARD_SMB_TIMEOUT_S);

    struct smb_wait w = {0};
    if (smb2_connect_share_async(s_smb2, s_cfg.host, s_cfg.share, s_cfg.user,
                                 wait_cb_status, &w) < 0) {
        return fail_locked(BOARD_SMB_PHASE_CONNECT, false);
    }

    const smb_service_t sv =
        service_until(&w, 1, deadline_in(BOARD_SMB_CONNECT_DEADLINE_MS), NULL);
    if (sv != SMB_SERVICE_OK) {
        return abandon_locked(BOARD_SMB_PHASE_CONNECT, sv);
    }
    if (w.status != 0) {
        return fail_locked(BOARD_SMB_PHASE_CONNECT, false);
    }
    return BOARD_SMB_OK;
}

board_smb_err_t board_smb_connect(const board_smb_config_t *cfg)
{
    if (!cfg || !cfg->host[0] || !cfg->share[0]) {
        return BOARD_SMB_ERR_CONNECT;
    }

    lock();
    teardown_locked(true);
    s_cfg = *cfg;
    const board_smb_err_t code = connect_locked();
    unlock();
    return code;
}

// ------------------------------------------------------------------ list

// Filled by every board_smb_list(); read with board_smb_get_list_stats(). Not behind a build
// flag: it is eleven counters written once per listing, and the header says why the dirent
// count in particular has to exist rather than be inferred from the kept count.
static board_smb_list_stats_t s_list_stats;

// Deliberately does NOT take the module lock, for board_smb_fd()'s reason: a read holds that
// lock for up to BOARD_SMB_FILE_DEADLINE_MS, so a diagnostics caller that wanted last run's
// counters would block for two minutes to read eleven words. The struct is written only inside
// the lock and only between an opendir and its closedir, so a caller reading it after a
// listing has returned sees a settled copy; a caller reading it *during* one gets a mix of two
// runs, which is why nothing here is a derived value that a mix could make plausible.
void board_smb_get_list_stats(board_smb_list_stats_t *out)
{
    if (!out) {
        return;
    }
    *out = s_list_stats;
}

board_smb_err_t board_smb_list(board_smb_entry_t *out, size_t max, size_t *count,
                               board_smb_accept_fn accept, bool *truncated)
{
    return board_smb_list_path(NULL, BOARD_SMB_LIST_FILES, out, max, count, accept, truncated);
}

board_smb_err_t board_smb_list_path(const char *path, board_smb_list_what_t what,
                                    board_smb_entry_t *out, size_t max, size_t *count,
                                    board_smb_accept_fn accept, bool *truncated)
{
    if (!out || !count) {
        return BOARD_SMB_ERR_NOTFOUND;
    }
    *count = 0;
    if (truncated) {
        *truncated = false;
    }

    lock();
    if (!s_smb2) {
        unlock();
        return BOARD_SMB_ERR_CONNECT;
    }
    const char *dir_path = path ? path : s_cfg.path;

    memset(&s_list_stats, 0, sizeof(s_list_stats));
    s_list_stats.int_free_before = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_list_stats.int_min_before =
        (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    s_list_stats.dma_largest_before =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    const int64_t t_open0 = esp_timer_get_time();

    struct smb_wait w = {0};
    if (smb2_opendir_async(s_smb2, dir_path, wait_cb_ptr, &w) != 0) {
        const board_smb_err_t code = fail_locked(BOARD_SMB_PHASE_LIST, true);
        unlock();
        return code;
    }

    const smb_service_t sv = service_until(&w, 1, deadline_in(BOARD_SMB_LIST_DEADLINE_MS), NULL);
    if (sv != SMB_SERVICE_OK) {
        const board_smb_err_t code = abandon_locked(BOARD_SMB_PHASE_LIST, sv);
        unlock();
        return code;
    }

    struct smb2dir *dir = w.ptr;
    if (!dir) {
        const board_smb_err_t code = fail_locked(BOARD_SMB_PHASE_LIST, true);
        unlock();
        return code;
    }

    // THE PEAK IS HERE, not in the walk below: smb2_opendir has already materialised the whole
    // directory by the time it returns, so this sample is the one the RAM question is about.
    s_list_stats.open_ms = (uint32_t)((esp_timer_get_time() - t_open0) / 1000);
    s_list_stats.int_free_open = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const int64_t t_walk0 = esp_timer_get_time();

    // smb2_readdir() walks a listing libsmb2 has already materialised in full; it issues
    // no request and cannot block, so it needs no deadline. That materialisation is also
    // why the caller's cap matters -- ticket 23 measured a 129-entry listing at 765 ms and
    // most of the session's ~60 KB transient peak.
    struct smb2dirent *ent;
    while ((ent = smb2_readdir(s_smb2, dir)) != NULL) {
        s_list_stats.dirents++;
        const bool is_dir = (ent->st.smb2_type == SMB2_TYPE_DIRECTORY);
        const bool wanted_type =
            (what == BOARD_SMB_LIST_DIRS) ? is_dir : (ent->st.smb2_type == SMB2_TYPE_FILE);
        if (!wanted_type) {
            continue;
        }
        // "." and ".." are directories, so they have to be skipped by name whichever type is
        // being collected -- and under BOARD_SMB_LIST_DIRS they are the two that would
        // otherwise turn every discovery walk into a loop.
        if (strcmp(ent->name, ".") == 0 || strcmp(ent->name, "..") == 0) {
            continue;
        }
        // `files` keeps its meaning -- entries of the requested kind that are not . or .. --
        // so a DIRS walk reports directories here. The name stays because every capture and
        // every figure in docs/agents/measurements.md is written against it, and a FILES walk
        // is what all of those were.
        s_list_stats.files++;
        if (accept && !accept(ent->name)) {
            continue; // not something the caller would keep, so it costs no slot
        }
        s_list_stats.accepted++;
        if (*count >= max) {
            // Full is not an error; the caller asked for at most `max`. But it is not
            // silence either -- keep walking so the flag reflects the whole directory,
            // and say so. smb2_readdir() is iterating a listing libsmb2 has already
            // materialised, so finishing the walk costs no round trips.
            if (truncated) {
                *truncated = true;
            }
            continue;
        }
        board_smb_entry_t *e = &out[(*count)++];
        snprintf(e->name, sizeof(e->name), "%s", ent->name);
        e->size = ent->st.smb2_size;
        e->mtime = ent->st.smb2_mtime;
    }
    smb2_closedir(s_smb2, dir);

    s_list_stats.walk_ms = (uint32_t)((esp_timer_get_time() - t_walk0) / 1000);
    s_list_stats.int_free_after = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_list_stats.int_min_after = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    s_list_stats.dma_largest_after =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    // One line per listing, at info: it is what makes any capture in .scratch/captures/ carry
    // its own entry count, which is exactly what the earlier ones do not.
    ESP_LOGI(TAG,
             "list: dirents=%u files=%u accepted=%u kept=%u open=%u ms walk=%u ms "
             "int_free %u -> %u -> %u int_min %u -> %u dma_largest %u -> %u",
             (unsigned)s_list_stats.dirents, (unsigned)s_list_stats.files,
             (unsigned)s_list_stats.accepted, (unsigned)*count, (unsigned)s_list_stats.open_ms,
             (unsigned)s_list_stats.walk_ms, (unsigned)s_list_stats.int_free_before,
             (unsigned)s_list_stats.int_free_open, (unsigned)s_list_stats.int_free_after,
             (unsigned)s_list_stats.int_min_before, (unsigned)s_list_stats.int_min_after,
             (unsigned)s_list_stats.dma_largest_before,
             (unsigned)s_list_stats.dma_largest_after);

    unlock();
    // Zero files in a directory that opened is a success with count 0, not an error.
    return BOARD_SMB_OK;
}

// ------------------------------------------------------------------ read

// How many preads are in flight at once. Runtime rather than a build flag so that a sweep
// is one flash and one capture on one association, which is what makes its points
// comparable with each other; ticket 33 has the table.
static int s_read_depth = BOARD_SMB_READ_DEPTH;

void board_smb_set_read_depth(int depth)
{
    if (depth < 1) {
        depth = 1;
    }
    if (depth > BOARD_SMB_READ_DEPTH_MAX) {
        depth = BOARD_SMB_READ_DEPTH_MAX;
    }
    s_read_depth = depth;
}

int board_smb_read_depth(void) { return s_read_depth; }

static bool s_trace = true;

void board_smb_set_trace(bool on) { s_trace = on; }

// What one group of preads did. The caller carries on from `next`, the first byte the group
// did not deliver -- which is NOT simply "off plus what came back": replies can land out of
// order, so a group's contribution is its contiguous prefix and anything above a hole is
// discarded and re-requested.
typedef enum {
    GROUP_OK = 0,   // the contiguous prefix advanced; keep going
    GROUP_EOF,      // a slot returned zero bytes: the file ended
    GROUP_FAILED,   // the server reported a failure; the session is still up
    GROUP_ABANDONED // the deadline passed; the session is gone and `fh` is dangling
} group_result_t;

// One group of up to s_read_depth preads into `buf` at ascending offsets from `off`,
// bounded. This is what ticket 33 is: the transport receives at 893 KB/s and one request at
// a time got 84, so the requests go out together and the replies are collected together.
static group_result_t read_group_locked(struct smb2fh *fh, uint8_t *buf, uint64_t off,
                                        uint64_t size, int64_t file_deadline,
                                        uint64_t *next)
{
    struct smb_wait w[BOARD_SMB_READ_DEPTH_MAX];
    uint64_t slot_off[BOARD_SMB_READ_DEPTH_MAX];
    uint32_t slot_want[BOARD_SMB_READ_DEPTH_MAX];

    *next = off;

    int n = 0;
    uint64_t at = off;
    while (n < s_read_depth && at < size) {
        const uint64_t left = size - at;
        const uint32_t want =
            left > BOARD_SMB_READ_CHUNK ? (uint32_t)BOARD_SMB_READ_CHUNK : (uint32_t)left;
        memset(&w[n], 0, sizeof(w[n]));
        slot_off[n] = at;
        slot_want[n] = want;
        if (smb2_pread_async(s_smb2, fh, buf + at, want, at, wait_cb_status, &w[n]) < 0) {
            break;
        }
        at += want;
        n++;
    }
    if (n == 0) {
        // Nothing could even be queued. Not a stall -- a failure the caller retries.
        return GROUP_FAILED;
    }

#ifdef BOARD_SMB_TRACE_CHUNKS
    struct smb_stamps st = {0};
    struct smb_stamps *stp = s_trace ? &st : NULL;
    const int64_t t_queue = esp_timer_get_time();
#else
    struct smb_stamps *stp = NULL;
#endif

    // One group, one chunk deadline. A group of at most s_read_depth 4 KB reads that cannot
    // finish inside BOARD_SMB_CHUNK_DEADLINE_MS is a stall by exactly the standard one
    // chunk was held to: the group should cost milliseconds, not seconds.
    int64_t d = deadline_in(BOARD_SMB_CHUNK_DEADLINE_MS);
    if (file_deadline < d) {
        d = file_deadline;
    }
    const smb_service_t sv = service_until(w, n, d, stp);

#ifdef BOARD_SMB_TRACE_CHUNKS
    // Off in every env but smbprobe, and switchable there. Four absolute stamps rather than
    // three differences, because ticket 03 established that a derived figure in first-cut
    // instrumentation is a second thing that can be wrong and costs a flash to find out.
    // q -> w is this board queueing and writing, w -> i is the network and the server,
    // i -> d is collecting the payloads.
    if (s_trace) {
        printf("@@SMBCHUNK off=%llu slots=%d want=%u sv=%d q=%lld w=%lld i=%lld d=%lld "
               "polls=%d readable=%d\n",
               (unsigned long long)off, n, (unsigned)(at - off), (int)sv,
               (long long)t_queue, (long long)st.written_us, (long long)st.first_in_us,
               (long long)esp_timer_get_time(), st.polls, st.readable);
        fflush(stdout);
    }
#endif

    if (sv != SMB_SERVICE_OK) {
        // The contiguous prefix has to be worked out BEFORE abandoning: destroying the
        // context invokes the callback of every still-queued PDU (lib/init.c:326-344), so
        // afterwards `done` no longer distinguishes what arrived from what was cancelled.
        for (int i = 0; i < n; i++) {
            if (!w[i].done || w[i].status < 0 || (uint32_t)w[i].status < slot_want[i]) {
                break;
            }
            *next = slot_off[i] + slot_want[i];
        }
        (void)abandon_locked(BOARD_SMB_PHASE_READ, sv);
        return GROUP_ABANDONED;
    }

    // Slots were queued at ascending offsets, so index order is offset order and the first
    // short or failed slot is where the contiguous prefix ends.
    for (int i = 0; i < n; i++) {
        // read_cb hands back the byte count on success and -errno on failure; end of file
        // is a count of zero, not an error.
        const int r = w[i].status;
        if (r < 0) {
            return GROUP_FAILED;
        }
        *next = slot_off[i] + (uint64_t)(uint32_t)r;
        if ((uint32_t)r < slot_want[i]) {
            // Short of what was asked for. At zero bytes the file is simply shorter than
            // the create reply said it was, which is an end of file and not a fault; above
            // zero, the rest of this group sits above a hole and is re-requested.
            return r == 0 ? GROUP_EOF : GROUP_OK;
        }
    }
    return GROUP_OK;
}

static void close_locked(struct smb2fh *fh)
{
    if (!s_smb2 || !fh) {
        return;
    }
    struct smb_wait w = {0};
    if (smb2_close_async(s_smb2, fh, wait_cb_status, &w) < 0) {
        return;
    }
    (void)service_until(&w, 1, deadline_in(BOARD_SMB_CLOSE_DEADLINE_MS), NULL);
}

board_smb_err_t board_smb_read_file(const char *name, uint8_t *buf, size_t max,
                                    size_t *len)
{
    if (!name || !name[0]) {
        return BOARD_SMB_ERR_NOTFOUND;
    }
    // Built here rather than inside the worker, so the worker has exactly one notion of what a
    // path is. s_cfg is only read under the lock elsewhere; it is written once by connect and
    // this is the same task that calls it, which is the existing arrangement rather than a new
    // assumption.
    char path[BOARD_SMB_PATH_SIZE + BOARD_SMB_NAME_SIZE + 2];
    snprintf(path, sizeof(path), "%s%s%s", s_cfg.path, s_cfg.path[0] ? "/" : "", name);
    return board_smb_read_file_path(path, buf, max, len);
}

board_smb_err_t board_smb_read_file_path(const char *full, uint8_t *buf, size_t max,
                                         size_t *len)
{
    if (!full || !full[0] || !buf || !len) {
        return BOARD_SMB_ERR_NOTFOUND;
    }
    *len = 0;

    lock();
    if (!s_smb2) {
        unlock();
        return BOARD_SMB_ERR_CONNECT;
    }

    char path[BOARD_SMB_PATH_SIZE + BOARD_SMB_NAME_SIZE + 2];
    snprintf(path, sizeof(path), "%s", full);

    // One budget for the whole call, not one per attempt: what a caller needs bounded is
    // how long board_smb_read_file() can take, and three attempts each with their own
    // ceiling would multiply it.
    const int64_t file_deadline = deadline_in(BOARD_SMB_FILE_DEADLINE_MS);

    uint64_t off = 0;
    board_smb_err_t code = BOARD_SMB_OK;

    // Reopen-and-retry from the saved offset, which is what makes a server's idle-timeout
    // disconnect invisible -- and, since the deadline above, what makes a stall invisible
    // too. Before the deadline existed this loop could not run at all in a stall, because
    // the attempt that stalled never returned.
    for (int attempt = 1; attempt <= BOARD_SMB_READ_ATTEMPTS; attempt++) {
        if (!s_smb2) {
            // The previous attempt was abandoned and took the session with it.
            code = connect_locked();
            if (code != BOARD_SMB_OK) {
                unlock();
                return code;
            }
        }

        struct smb_wait w = {0};
        if (smb2_open_async(s_smb2, path, 0 /* O_RDONLY */, wait_cb_ptr, &w) != 0) {
            code = fail_locked(BOARD_SMB_PHASE_READ, true);
            unlock();
            return code;
        }
        int64_t open_deadline = deadline_in(BOARD_SMB_OPEN_DEADLINE_MS);
        if (file_deadline < open_deadline) {
            open_deadline = file_deadline;
        }
        const smb_service_t sv = service_until(&w, 1, open_deadline, NULL);
        if (sv != SMB_SERVICE_OK) {
            code = abandon_locked(BOARD_SMB_PHASE_READ, sv);
            continue;
        }
        struct smb2fh *fh = w.ptr;
        if (!fh) {
            code = fail_locked(BOARD_SMB_PHASE_READ, true);
            unlock();
            return code;
        }

        // The size, for free. The create reply carries end-of-file and
        // smb2_lseek(SEEK_END) reads it back out of the handle -- libsmb2.h:737-741 states
        // that it does not call fstat and does not block. That is what lets the whole read
        // be planned before it starts, which is what a group of preads needs, and it also
        // answers the too-big case without fetching a byte. It moves the handle's own
        // offset, which nothing here uses: every read is a pread with an explicit offset.
        uint64_t size = 0;
        if (smb2_lseek(s_smb2, fh, 0, SEEK_END, &size) < 0) {
            // Only reachable with a NULL context or handle, both already ruled out above.
            // A guard rather than a path, because the alternative is trusting `size`.
            close_locked(fh);
            ESP_LOGW(TAG, "%s: no size from the open reply", path);
            unlock();
            return BOARD_SMB_ERR_NOTFOUND;
        }
        if (size > (uint64_t)max) {
            // Reported as a failure rather than handed back as a short read the caller
            // cannot tell from a whole file. A file of exactly `max` bytes fits: the
            // boundary was measured on 2026-09-04 and env:smbprobe's exactfit arm pins it.
            close_locked(fh);
            ESP_LOGW(TAG, "%s is %llu bytes and does not fit in %u", path,
                     (unsigned long long)size, (unsigned)max);
            unlock();
            return BOARD_SMB_ERR_NOTFOUND;
        }

        bool retry = false;
        bool abandoned = false;
        while (off < size) {
            uint64_t next = off;
            const group_result_t g =
                read_group_locked(fh, buf, off, size, file_deadline, &next);
            off = next;
            if (g == GROUP_ABANDONED) {
                abandoned = retry = true;
                break;
            }
            if (g == GROUP_FAILED) {
                retry = true;
                break;
            }
            if (g == GROUP_EOF) {
                break; // the file is shorter than the open reply said
            }
        }

        if (!abandoned) {
            close_locked(fh);
        }

        if (!retry) {
            *len = (size_t)off;
            unlock();
            return BOARD_SMB_OK;
        }
        ESP_LOGW(TAG, "read of %s failed at offset %llu (attempt %d/%d)%s", path,
                 (unsigned long long)off, attempt, BOARD_SMB_READ_ATTEMPTS,
                 abandoned ? ", session abandoned" : "");
        if (esp_timer_get_time() >= file_deadline) {
            ESP_LOGW(TAG, "read of %s is out of budget after %d attempts", path, attempt);
            break;
        }
    }

    if (s_smb2) {
        code = fail_locked(BOARD_SMB_PHASE_READ, true);
    } else if (code == BOARD_SMB_OK) {
        code = BOARD_SMB_ERR_TIMEOUT;
    }
    unlock();
    return code;
}

void board_smb_disconnect(void)
{
    lock();
    teardown_locked(true);
    unlock();
}

int board_smb_fd(void)
{
    // No lock, on purpose. See the header.
    return s_smb2 ? (int)smb2_get_fd(s_smb2) : -1;
}

void *board_smb_context(void)
{
    // No lock, on purpose. See the header.
    return s_smb2;
}

bool board_smb_is_connected(void)
{
    lock();
    const bool up = s_smb2 != NULL;
    unlock();
    return up;
}
