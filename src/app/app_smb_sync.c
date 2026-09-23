#include "app_smb_sync.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_clock.h"
#include "app_heapwatch.h"
#include "app_server.h"
#include "app_settings.h"
#include "app_slideshow.h"
#include "board_smb.h"
#include "board_storage.h"
#include "board_wifi.h"
#include "epd_dither.h"
#include "epd_image.h"
#include "geo_city.h"
#include "img_dims.h"
#include "img_resize.h"
#include "img_scale.h"
#include "photo_list.h"
#include "smb_catalog.h"
#include "smb_evict.h"
#include "smb_manifest.h"
#include "smb_window.h"

static const char *TAG = "smbsync";

#define SYNC_TASK_STACK 8192
#define SYNC_TASK_PRIO 4

// The connect test does a strict subset of what the sync task does -- connect and nothing
// else -- so the sync task's own stack is an upper bound that has already been exercised.
// Its high-water mark is printed at every test so this can be cut on evidence rather than
// guessed downwards now.
#define TEST_TASK_STACK 8192

// 160 KB holds 500 records with ~260 bytes of name and path each, which is generous for
// a flat photo directory and cheap against the 7.5 MB of PSRAM that is free at all
// times. Running out is not a corruption -- smb_manifest_add() refuses cleanly -- but it
// would leave a mirrored file with no ownership record, and an unowned file is one the
// mirror will never clean up.
#define MANIFEST_ARENA 163840

// A serialised manifest that would not fit this is one this device cannot have written.
#define MANIFEST_FILE_MAX 262144

// The share catalogue (ticket 37). 4096 paths at ~64 bytes each, and the serialised form is a
// little larger than the arena because each line also carries a size and an mtime.
#define CATALOG_ARENA 262144
#define CATALOG_FILE_MAX 524288

// ------------------------------------------------------------------ module state

static SemaphoreHandle_t s_mutex;
static app_smb_sync_status_t s_status;

// What is in /data now. Its arena is allocated once and lives for the life of the
// process; the manifest is the ownership record and has to be readable at any moment.
static smb_manifest_t s_have;
static char *s_have_arena;

// Built during a run and swapped in at the end. Allocated per run.
static smb_manifest_t s_next;
static char *s_next_arena;

static board_smb_entry_t *s_entries;
static size_t s_entry_count;
// The share held more photographs than SMB_SYNC_LIST_MAX, so s_entries is a window onto
// it rather than the whole thing. Everything downstream that treats absence from
// s_entries as absence from the share has to know (ticket 27).
static bool s_listing_truncated;
static uint16_t s_plan[SMB_SYNC_LIST_MAX]; // indices into s_entries still to fetch
static uint16_t s_plan_count;
static uint16_t s_plan_pos;

static bool s_task_running;
static bool s_run_active; // a run spans several windows
static bool s_manual_request;

// The connect test. Its own task and its own flag: it must not share s_cfg with a run, and
// it must not overlap one, because board_smb.c holds a single libsmb2 session.
static bool s_test_request;
static bool s_test_task_running;
static board_smb_config_t s_test_cfg;
// When the current run first became due, or INT64_MIN when nothing is waiting. The
// deadline below is measured from this rather than from the last activity, so a client
// that keeps resetting the activity timer cannot also keep resetting the deadline.
static int64_t s_wait_since_us = INT64_MIN;
static int64_t s_last_window_end_us;
static int64_t s_last_run_end_us = INT64_MIN;
static uint64_t s_cap_bytes;
static uint16_t s_cap_files;

// The configuration the current run started with. Copied once so a settings change
// mid-run cannot make the manifest describe a different share than the files do.
static board_smb_config_t s_cfg;

// The raw `smb_path` setting, which is now a COMMA-SEPARATED LIST of folders (ticket 37).
// s_cfg.path is element 0 of it -- the folder the mirror itself works in -- so a device
// configured with a single folder behaves exactly as before.
static char s_folder_list[BOARD_SMB_PATH_SIZE];

// What is on the SHARE, as opposed to s_have which is what is on the card. Both the struct and
// its arena are in PSRAM and neither may become a file-static: smb_catalog_t carries
// rec[SMB_CATALOG_MAX], which is ~98 KB, and internal RAM is the scarce resource here.
//
// Built ONE FOLDER PER RUN, round-robin through the list, because a listing costs 142.5 bytes
// per directory entry and env:frame can afford a few hundred entries but not a few thousand
// (docs/measurements.md, 2026-09-06).
//
// The round-robin cursor is NOT a static here: it lives in the catalogue and is persisted in
// /data/.smbdir's header, because a cursor that resets on boot means a frame which restarts
// often never advances past folder 0. That is not hypothetical -- it happened twice on
// 2026-09-06, and the second time was the owner rebooting the board.
static smb_catalog_t *s_catalog;
static char *s_cat_arena;

// On-demand (ticket 37 Phase 3). The want list is catalogue indices the selector has asked for,
// nearest first, and by contract they are ones it already knows are absent from the card -- so
// nothing here has to touch the filesystem to find out whether there is work.
static uint16_t s_want[SMB_SYNC_WANT_MAX];
static uint8_t s_want_count;
static bool s_on_demand;
// Whether the "paused, /data is not the card" line has been printed for the current spell on
// the internal volume. Per transition, not per tick.
static bool s_media_warned;
// A media change that arrived while a window was running, deferred to the next tick rather
// than applied under the worker's feet.
static bool s_media_reload_due;
// One bit per catalogue index, set on a successful fetch (ticket 44). 512 B of .bss against
// env:frame's ~72 KB idle internal headroom.
//
// AN INDEX IS NOT A PHOTOGRAPH, and that is the whole caveat: the hourly refresh renumbers the
// catalogue, so this reads a fetch of a DIFFERENT photograph at a reused index as a repeat. The
// counter it makes is a lower bound on distinct photographs; app_smb_sync.h says how far under
// it ran when this was measured, and ticket 45 owns making it exact. It is never cleared, which
// keeps the bound monotone rather than resetting it every hour.
static uint8_t s_fetch_seen[SMB_CATALOG_MAX / 8];
// The catalogue refresh keeps its own clock. Under on-demand a want-driven window opens every few
// minutes, and if the period were measured from the last window of any kind the hourly refresh
// would be pushed back by every fetch and never happen.
static int64_t s_last_catalog_us = INT64_MIN;

static void lock(void)
{
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

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// --------------------------------------------------------------- paths and files

// The full SMB path of a listing entry, which is what the manifest keys on and what the
// name hash covers.
static void full_path(char *out, size_t size, const char *name)
{
    snprintf(out, size, "%s%s%s", s_cfg.path, s_cfg.path[0] ? "/" : "", name);
}

// The listing's slot budget is for photographs, not for whatever else the folder holds.
// This is build_plan()'s own extension test, called from board_smb_list() so that it runs
// before the cap rather than after it -- the same function, so the two cannot drift.
static bool accept_photo(const char *name)
{
    char path[SMB_MANIFEST_PATH_SIZE];
    char local[SMB_MANIFEST_NAME_SIZE];
    full_path(path, sizeof(path), name);
    return smb_manifest_local_name(path, local, sizeof(local));
}

static void local_path(char *out, size_t size, const char *local)
{
    snprintf(out, size, "%s/%s", BOARD_STORAGE_MOUNT, local);
}

// Is the mirrored copy actually on /data? Used by build_plan(), where the answer decides
// whether a file the manifest calls current has to be fetched again. See ticket 34.
// ONE readdir OF /data, NOT ONE stat() PER MIRRORED FILE, and the reason is measured rather
// than assumed. On 2026-09-06 the check run's plan phase was 8628 ms, of which the listing was
// 911 ms and 200 presence probes were 7663 ms -- 38.3 ms each. Taking the storage lock was
// 35 us of that, so the cost is inside stat() itself: FATFS resolves a name by scanning the
// directory, and doing that once per file over a 215-entry directory is quadratic. A single
// pass is linear and takes the lock once.
//
// The instrumentation that decided this is gone; `plan phases` now reports the replacement so
// the two can be compared in one log.
#define LOCAL_SET_MAX PHOTO_LIST_MAX

typedef struct {
    char (*name)[SMB_MANIFEST_NAME_SIZE];
    size_t count;
    // The directory held more than LOCAL_SET_MAX entries, so absence from this set is not
    // evidence of absence from the card -- the same distinction board_smb_list()'s
    // `truncated` makes about the share, and it matters for the same reason: treating an
    // incomplete listing as complete would re-fetch files that are already there. `name` is
    // NULL when the scan could not run at all, and then every probe falls back to stat().
    bool complete;
    // Ticket 27: of the entries above, how many are photographs the manifest does not own.
    // A by-product of a walk that was already happening, so it costs one strcmp sweep and no
    // extra I/O. What the number means is in app_smb_sync.h beside the status field -- read
    // that before drawing anything from it, because it is a baseline plus the leak and not
    // the leak.
    uint16_t unowned;
} local_set_t;

static bool local_set_has(const local_set_t *set, const char *name)
{
    for (size_t i = 0; i < set->count; i++) {
        if (strcmp(set->name[i], name) == 0) {
            return true;
        }
    }
    return false;
}

// Also removes any leftover `<name>.part`, which can only be an interrupted write from an
// earlier run: build_plan() runs before any fetch in the same window, and nothing else writes
// them. Collected and unlinked after closedir rather than during the walk, because deleting
// from under a FAT directory iterator is not something to find out about here.
static void local_set_build(local_set_t *set)
{
    memset(set, 0, sizeof(*set));
    set->name = heap_caps_malloc(sizeof(*set->name) * LOCAL_SET_MAX, MALLOC_CAP_SPIRAM);
    if (!set->name) {
        ESP_LOGW(TAG, "no PSRAM for the %s listing; falling back to stat() per file",
                 BOARD_STORAGE_MOUNT);
        return;
    }

    char stale[8][SMB_MANIFEST_NAME_SIZE];
    size_t stale_count = 0;

    board_storage_lock();
    board_storage_prepare_access();
    DIR *d = opendir(BOARD_STORAGE_MOUNT);
    if (d) {
        bool all_held = true;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_type == DT_DIR) {
                continue;
            }
            const size_t n = strlen(e->d_name);
            // A name this array cannot hold is NEVER truncated into it: a shortened name would
            // compare unequal to the manifest's and re-fetch a file that is already there, or
            // worse compare equal to a different one. It makes the set incomplete instead, and
            // the stat() fallback answers for it. d_name is up to 256 bytes here and these
            // slots are SMB_MANIFEST_NAME_SIZE, which is what every mirrored name fits in by
            // construction -- so this is the path for files the mirror does not own.
            if (n >= SMB_MANIFEST_NAME_SIZE) {
                all_held = false;
                continue;
            }
            // Sidecars are not photographs and must not consume a slot: the set is
            // LOCAL_SET_MAX entries, and one per photograph would halve what the
            // presence check can answer for and push the rest onto the 38.3 ms
            // stat() fallback ticket 36 removed.
            const size_t tsuf = sizeof(SMB_THUMB_SUFFIX) - 1;
            if (n >= tsuf && strcmp(e->d_name + n - tsuf, SMB_THUMB_SUFFIX) == 0) {
                continue;
            }
            const size_t msuf = sizeof(SMB_META_SUFFIX) - 1;
            if (n >= msuf && strcmp(e->d_name + n - msuf, SMB_META_SUFFIX) == 0) {
                continue;
            }
            if (n >= 5 && strcmp(e->d_name + n - 5, ".part") == 0) {
                if (stale_count < sizeof(stale) / sizeof(stale[0])) {
                    memcpy(stale[stale_count++], e->d_name, n + 1);
                }
                continue;
            }
            if (set->count >= LOCAL_SET_MAX) {
                all_held = false;
                continue;
            }
            // Ticket 27, counted here because the name is in hand and the alternative is a
            // second pass. photo_list_is_image() and NOT "everything in the set": the set also
            // holds .smbidx, .smbdir and whatever else a user has put on the card, and the
            // question is about PHOTOGRAPHS -- the population the grid and the slideshow see.
            if (photo_list_is_image(e->d_name) &&
                !smb_manifest_owns_local(&s_have, e->d_name) && set->unowned < UINT16_MAX) {
                set->unowned++;
            }
            memcpy(set->name[set->count++], e->d_name, n + 1);
        }
        set->complete = all_held;
        closedir(d);
        for (size_t i = 0; i < stale_count; i++) {
            char path[192];
            local_path(path, sizeof(path), stale[i]);
            if (unlink(path) == 0) {
                ESP_LOGW(TAG, "removed %s left by an interrupted write", stale[i]);
            }
        }
    } else {
        ESP_LOGW(TAG, "opendir(%s) failed; falling back to stat() per file",
                 BOARD_STORAGE_MOUNT);
        heap_caps_free(set->name);
        set->name = NULL;
    }
    board_storage_unlock();

    if (set->name && !set->complete) {
        ESP_LOGW(TAG, "the %s listing is incomplete at %u entries (over %u, or a name longer "
                      "than %u); presence falls back to stat() for anything not in it",
                 BOARD_STORAGE_MOUNT, (unsigned)set->count, (unsigned)LOCAL_SET_MAX,
                 (unsigned)(SMB_MANIFEST_NAME_SIZE - 1));
    }
}

// Ticket 27. `set->unowned` is only as good as the walk that produced it, so the two are
// published together and never apart: `exact` false means the directory listing was incomplete
// and the count is a LOWER BOUND. Called from build_plan() on the mirror path, where the walk was
// happening anyway, and from the on-demand window, where it is not -- see count_unowned().
static void publish_unowned(const local_set_t *set)
{
    lock();
    s_status.unowned_files = set->unowned;
    s_status.unowned_exact = (set->name != NULL) && set->complete;
    unlock();
}

static void local_set_free(local_set_t *set)
{
    heap_caps_free(set->name);
    memset(set, 0, sizeof(*set));
}

// The on-demand window has no directory walk of its own: build_plan() is THE MIRROR PATH ONLY
// (see compute_caps()'s header) and on-demand never lists. So this is a walk added for the count
// alone, and it is worth saying what it costs rather than asserting it is free -- `dirscan` has
// measured 82-98 ms across 20 to 247 entries, near enough flat because the cost is FATFS opening
// the directory rather than the entries in it. Against a window whose httpd_down is seconds that
// is under 2 %, and it buys the number on the medium the frame actually ships with on-demand.
static void count_unowned(void)
{
    local_set_t set;
    local_set_build(&set);
    publish_unowned(&set);
    local_set_free(&set);
}

// The slow path, kept for the two cases where the set cannot answer. `st_size > 0` is NOT part
// of it any more and does not need to be: write_local() now writes `<name>.part` and renames,
// so a zero-length mirrored file is unreachable rather than merely unlikely, and presence by
// name is the whole question.
static bool local_present_stat(const char *local)
{
    char path[192];
    local_path(path, sizeof(path), local);

    struct stat st;
    board_storage_lock();
    board_storage_prepare_access();
    const bool there = stat(path, &st) == 0;
    board_storage_unlock();
    return there;
}

static bool local_present(const local_set_t *set, const char *local, unsigned *fallbacks)
{
    if (set->name && local_set_has(set, local)) {
        return true;
    }
    if (set->name && set->complete) {
        return false;
    }
    if (fallbacks) {
        (*fallbacks)++;
    }
    return local_present_stat(local);
}

// WRITES `<name>.part` AND RENAMES, so the file at its real name is either absent or whole.
// That is what lets build_plan() answer "is it there" from one readdir instead of a stat() per
// file: before this, a power cut between fopen("wb") and fwrite left a zero-length file at the
// real name, which is why the presence check also tested st_size -- and testing st_size is what
// cost 38.3 ms a file (2026-09-06). Making the bad state unreachable is cheaper than detecting
// it 200 times an hour, and a leftover `.part` is cleaned up by the next local_set_build().
//
// The sequence itself is board_storage_write_atomic() since ticket 78, which is where the two
// record writers that lacked it went to get it. This function keeps the account above because it
// is where the reason was learned; the lock discipline is now the helper's.
static esp_err_t write_local(const char *local, const uint8_t *buf, size_t len)
{
    char path[192];
    local_path(path, sizeof(path), local);

    // The lock is taken inside the helper and not around the network read: on a microSD it takes
    // SPI2, which the panel is on, so holding it across a stalled NAS read would block a
    // refresh for as long as the stall lasts (ticket 03's 14.1 s figure is the SD one).
    const bool renamed = board_storage_write_atomic(path, buf, len);

    if (!renamed) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ------------------------------------------------------- import-time resize (ticket 49)

void app_smb_sync_sidecar_path(char *out, size_t size, const char *name, const char *suffix)
{
    const char *slash = strrchr(name, '/');
    snprintf(out, size, "%s/%s%s", SMB_SIDECAR_DIR, slash ? slash + 1 : name, suffix);
}

// Removes a sidecar if there is one -- either kind, the thumbnail or the metadata, by full path.
// Its own function rather than a call to delete_local(), which takes the photograph's name and
// would unlink the photograph as well.
static void delete_sidecar(const char *spath)
{
    board_storage_lock();
    board_storage_prepare_access();
    unlink(spath);
    board_storage_unlock();
}

static bool has_suffix(const char *name, const char *suffix)
{
    const size_t n = strlen(name);
    const size_t s = strlen(suffix);
    return n >= s && strcmp(name + n - s, suffix) == 0;
}

// Collects up to `max` names in `dir` that `want()` accepts, under the storage lock. Returns how
// many were collected; *more is set whenever the result is NOT the whole answer -- a match that
// did not fit, a name too long for a slot, or a directory that would not open. The last one
// matters most: the orphan sweep reads an empty root as "no photographs", and an unopenable
// root must not say that. Collected and acted on after closedir, for local_set_build()'s reason:
// deleting or renaming from under a FAT directory iterator is not something to find out about.
static size_t collect_names(const char *dir, bool (*want)(const char *), char (*names)[96],
                            size_t max, bool *more)
{
    size_t count = 0;
    *more = false;
    board_storage_lock();
    board_storage_prepare_access();
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_type == DT_DIR || !want(e->d_name)) {
                continue;
            }
            if (count >= max || strlen(e->d_name) >= sizeof(names[0])) {
                *more = true;
                continue;
            }
            strcpy(names[count++], e->d_name);
        }
        closedir(d);
    } else {
        *more = true;
    }
    board_storage_unlock();
    return count;
}

static bool is_sidecar(const char *name)
{
    return has_suffix(name, SMB_THUMB_SUFFIX) || has_suffix(name, SMB_META_SUFFIX);
}

static bool is_part(const char *name)
{
    return has_suffix(name, ".part");
}

static bool want_any(const char *name)
{
    (void)name;
    return true;
}

// Unlinks every sidecar in SMB_SIDECAR_DIR whose photograph is not in /data's root. Since the
// sidecars moved out of sight, a photograph deleted on a PC leaves its sidecars behind, and a
// later photograph given the same name -- an upload's imaged### -- would be served the old
// thumbnail and caption BY THE FRAME, which no browser-side remedy reaches.
//
// Safe only where nothing can be writing: store_photo() writes the `.mta` BEFORE the photograph,
// so a sweep during a mirror fetch would take a fresh sidecar for an orphan. Hence boot only,
// before httpd, the mirrors and the slideshow exist. The sidecars are listed BEFORE the root for
// the same reason, belt and braces: a sidecar is only ever written for a photograph being stored.
//
// Refuses (`skipped`) unless the root listing is the WHOLE root -- an absent name must mean an
// absent photograph, or this deletes live thumbnails.
static const char *sweep_orphan_sidecars(unsigned *orphans)
{
    enum { SIDECAR_MAX = 1024, ROOT_MAX = PHOTO_LIST_MAX + 64 };
    _Static_assert(sizeof(SMB_THUMB_SUFFIX) == sizeof(SMB_META_SUFFIX),
                   "the base name below is cut at one suffix length for both kinds");
    char (*side)[96] = heap_caps_malloc(SIDECAR_MAX * sizeof(*side), MALLOC_CAP_SPIRAM);
    char (*root)[96] = heap_caps_malloc(ROOT_MAX * sizeof(*root), MALLOC_CAP_SPIRAM);
    const char *verdict = "nomem";
    if (side && root) {
        bool side_more = false, root_more = false;
        const size_t ns = collect_names(SMB_SIDECAR_DIR, is_sidecar, side, SIDECAR_MAX, &side_more);
        const size_t nr = collect_names(BOARD_STORAGE_MOUNT, want_any, root, ROOT_MAX, &root_more);
        if (root_more) {
            verdict = "skipped";
        } else {
            // Partial sidecar list is fine: every orphan it holds is still an orphan.
            verdict = side_more ? "partial" : "ok";
            for (size_t i = 0; i < ns; i++) {
                const size_t base_len = strlen(side[i]) - (sizeof(SMB_THUMB_SUFFIX) - 1);
                bool found = false;
                for (size_t j = 0; j < nr && !found; j++) {
                    found = strlen(root[j]) == base_len && strncmp(root[j], side[i], base_len) == 0;
                }
                if (found) {
                    continue;
                }
                char path[BOARD_STORAGE_PATH_MAX];
                snprintf(path, sizeof(path), "%s/%s", SMB_SIDECAR_DIR, side[i]);
                board_storage_lock();
                board_storage_prepare_access();
                if (unlink(path) == 0) {
                    (*orphans)++;
                }
                board_storage_unlock();
            }
        }
    }
    heap_caps_free(side);
    heap_caps_free(root);
    return verdict;
}

void app_smb_sync_sidecars_prepare(bool sweep_orphans)
{
    board_storage_lock();
    board_storage_prepare_access();
    const bool dir_ok = mkdir(SMB_SIDECAR_DIR, 0755) == 0 || errno == EEXIST;
    board_storage_unlock();

    char (*names)[96] = heap_caps_malloc(32 * sizeof(*names), MALLOC_CAP_SPIRAM);
    unsigned moved = 0, failed = 0, parts = 0;
    if (dir_ok && names) {
        // In batches, because the batch is bounded and the root may hold hundreds. A pass that
        // moves nothing ends the loop even when more is set, so a name that cannot be renamed
        // cannot spin it.
        bool more = true;
        while (more) {
            const size_t n = collect_names(BOARD_STORAGE_MOUNT, is_sidecar, names, 32, &more);
            unsigned moved_now = 0;
            for (size_t i = 0; i < n; i++) {
                char from[BOARD_STORAGE_PATH_MAX];
                char to[BOARD_STORAGE_PATH_MAX];
                snprintf(from, sizeof(from), "%s/%s", BOARD_STORAGE_MOUNT, names[i]);
                app_smb_sync_sidecar_path(to, sizeof(to), names[i], "");
                board_storage_lock();
                board_storage_prepare_access();
                // FATFS f_rename refuses an existing destination (board_storage_write_atomic()).
                // A sidecar already in the folder is the newer one only if firmware wrote it
                // there, and firmware that did also cleared the root copy -- so the root one wins
                // only when no other exists, and otherwise it is the stale one.
                struct stat st;
                const bool exists = stat(to, &st) == 0;
                const bool ok = exists ? unlink(from) == 0 : rename(from, to) == 0;
                board_storage_unlock();
                if (ok) {
                    moved_now++;
                } else {
                    failed++;
                }
            }
            moved += moved_now;
            if (moved_now == 0) {
                break;
            }
        }
        bool part_more = false;
        const size_t n = collect_names(SMB_SIDECAR_DIR, is_part, names, 32, &part_more);
        for (size_t i = 0; i < n; i++) {
            char path[BOARD_STORAGE_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", SMB_SIDECAR_DIR, names[i]);
            board_storage_lock();
            board_storage_prepare_access();
            if (unlink(path) == 0) {
                parts++;
            }
            board_storage_unlock();
        }
    }
    heap_caps_free(names);

    const char *sweep = "off";
    unsigned orphans = 0;
    if (dir_ok && sweep_orphans) {
        sweep = sweep_orphan_sidecars(&orphans);
    }
    printf("# sidecars: dir=%s moved=%u failed=%u part=%u sweep=%s orphans=%u\n",
           dir_ok ? "ok" : "err", moved, failed, parts, sweep, orphans);
}

// ------------------------------------------------- import-time metadata (ticket 64)

// Writes `<local>.mta` -- two optional lines, each a one-letter key and a value:
//
//     C TOKYO JAPAN
//     D 19-08-14
//
// Nothing is written at all when neither line has content, so an EXIF-less source costs no file.
//
// **The coordinates are geocoded here and then discarded.** That is "derive before you retain"
// (owner, 2026-09-12) applied where it actually bites: a card holding four thousand
// photographs' GPS tags is a different object from one holding their city names, and the frame
// only ever needed the name. The table it looks the name up in is flash-resident and public
// domain (src/core/geo_city.h), so this costs no internal RAM and reaches no network.
static void write_meta(const char *mpath, const uint8_t *buf, size_t len)
{
    img_exif_meta_t meta;
    if (!img_exif_meta(buf, len, &meta)) {
        return; // no EXIF block: a re-encoded file, a PNG, or a BMP
    }

    char body[96];
    size_t n = 0;
    bool have_city = false;

    if (meta.has_gps) {
        const char *city = NULL;
        const char *country = NULL;
        if (geo_city_nearest(geo_city_table(), meta.lat_udeg, meta.lon_udeg, GEO_CITY_MAX_KM,
                             &city, &country) &&
            city != NULL) {
            // **Two lines, not one joined string** (2026-09-19). The band drops the country first
            // when the line is full and keeps the city, so it needs them apart -- and splitting a
            // joined `TOKYO JAPAN` back at the last space works for JAPAN and fails for UNITED
            // STATES. `geo_city_nearest()` hands them over separately; the only reason they were
            // ever joined here was that the band took one string.
            n += (size_t)snprintf(body + n, sizeof(body) - n, "C %s\n", city);
            if (country != NULL && n < sizeof(body)) {
                n += (size_t)snprintf(body + n, sizeof(body) - n, "N %s\n", country);
            }
            have_city = true;
        }
    }
    // **The FULL year is stored and the band shortens it at draw time.** The band's own format is
    // two digits -- a four-digit year plus a city name is over sixteen characters for any city of
    // more than five letters, and sixteen is what a 400 px band holds at a readable size -- but
    // storing two would make a 1985 photograph read as 2085 and "N YEARS AGO" come out negative.
    // The presentation is the band's business and the file keeps what it was told.
    if (meta.year > 0 && n < sizeof(body)) {
        n += (size_t)snprintf(body + n, sizeof(body) - n, "D %04d-%02d-%02d\n", (int)meta.year,
                              (int)meta.month, (int)meta.day);
    }
    if (n == 0 || n >= sizeof(body)) {
        return;
    }
    if (!board_storage_write_atomic(mpath, body, n)) {
        // A missing metadata sidecar is a band without a caption, not a failure -- exactly the
        // rule write_thumb() follows, and nothing above treats either as an error.
        ESP_LOGW(TAG, "meta %s: not written", mpath);
        return;
    }
    // Raw, because the arm that verifies this cannot see the card: which pieces the file got, and
    // the derived text itself. `city=0` with `gps=1` is the answer to a different question from
    // `gps=0` -- the first is an empty city table, the second a photograph with no position -- and
    // one line that said "no location" could not tell them apart.
    printf("# meta %s bytes=%u gps=%d city=%d date=%04d-%02d-%02d\n", mpath, (unsigned)n,
           meta.has_gps ? 1 : 0, have_city ? 1 : 0, (int)meta.year, (int)meta.month,
           (int)meta.day);
}

// Everything the fetched bytes have to satisfy before they are worth decoding. Sniffed
// rather than taken from the extension, because the bytes are already in hand and a file
// that lies about its type would otherwise be decoded on the strength of its name.
static bool resize_qualifies(const uint8_t *buf, size_t len)
{
    switch (epd_image_sniff(buf, len)) {
    case EPD_IMAGE_JPEG:
        return true;
    case EPD_IMAGE_PNG:
        // Small PNGs pass through: see SMB_RESIZE_PNG_MIN_BYTES.
        return len >= SMB_RESIZE_PNG_MIN_BYTES;
    default:
        // BMP and anything unrecognised. A BMP is rare enough that the storage argument is
        // not worth a second code path, and an unrecognised file is not ours to rewrite.
        return false;
    }
}

// Writes `<local>.thm` from an RGB888 buffer. Returns the bytes written, or 0 -- a missing
// sidecar is a slow /thumb/, not a failure, so nothing above this treats 0 as an error.
static size_t write_thumb(const char *tpath, const uint8_t *rgb, int32_t w, int32_t h)
{
    const int32_t big = w > h ? w : h;
    int32_t fac = (big + IMG_THUMB_MAX_EDGE - 1) / IMG_THUMB_MAX_EDGE;
    if (fac < 1) {
        fac = 1;
    }
    uint8_t *trgb = NULL;
    int32_t tw = 0, th = 0;
    if (img_resize_box(rgb, w, h, fac, &trgb, &tw, &th) != ESP_OK) {
        return 0;
    }
    uint8_t *tout = NULL;
    size_t tlen = 0;
    size_t wrote = 0;
    if (img_encode_jpeg(trgb, tw, th, IMG_THUMB_QUALITY, &tout, &tlen) == ESP_OK) {
        if (board_storage_write_atomic(tpath, tout, tlen)) {
            wrote = tlen;
        }
        free(tout);
    }
    heap_caps_free(trgb);
    return wrote;
}

// Stores a fetched photograph, and with app_settings_smb_resize() on also re-encodes it at
// the size the panel draws it and writes its thumbnail beside it.
//
// EVERY FAILURE FALLS BACK TO STORING THE ORIGINAL. A photograph that cannot be shrunk must
// still reach the card: this is an optimisation of what is stored, never a gate on whether
// anything is.
//
// **THE FACTOR IS TAKEN FROM THE SOURCE HEADER, BEFORE ANYTHING IS ALLOCATED**, and that is
// not a tidiness. Measured on hardware 2026-09-09: a 1024x1024 photograph is drawn at
// 600x600, so the integer box factor is 1 -- and the first version decoded it anyway, which
// is 3.15 MB, and then allocated another 3.15 MB for a reduction that was a straight copy.
// 6.29 MB does not fit in this board's ~6.4 MB of PSRAM beside a render, so every such file
// failed with `total_min=3009680` in the heartbeat and fell back to the original. That is a
// THIRD of this bench share (330 of 1032 files are 1024x1024). Asking the header first costs
// nothing and turns the whole case into "store the original, make a thumbnail cheaply".
static esp_err_t store_photo(const char *local, const uint8_t *buf, size_t len)
{
    // ANY re-fetch invalidates the sidecar, so it goes first and unconditionally -- before
    // the setting is even consulted. Without this, turning smb_resize off and re-fetching a
    // changed photograph leaves the OLD thumbnail in place and /thumb/ serves it forever:
    // the sidecar is not in .smbidx, so nothing else would ever notice. It is one FATFS
    // lookup that usually says ENOENT, which is what delete_local() already pays for the
    // same reason.
    char tpath[BOARD_STORAGE_PATH_MAX];
    app_smb_sync_sidecar_path(tpath, sizeof(tpath), local, SMB_THUMB_SUFFIX);
    delete_sidecar(tpath);

    // The metadata sidecar, for the same reason and in the same place -- but written HERE, before
    // any branch below, because `buf` is the only copy of the ORIGINAL bytes that will ever exist.
    // Every path out of this function either re-encodes them or stores them for the next re-fetch
    // to replace, and none of them can read EXIF afterwards.
    char mpath[BOARD_STORAGE_PATH_MAX];
    app_smb_sync_sidecar_path(mpath, sizeof(mpath), local, SMB_META_SUFFIX);
    delete_sidecar(mpath);
    write_meta(mpath, buf, len);

    if (!app_settings_smb_resize() || !resize_qualifies(buf, len)) {
        return write_local(local, buf, len);
    }

    int32_t sw = 0, sh = 0;
    if (!img_image_dims(buf, len, &sw, &sh)) {
        ESP_LOGW(TAG, "resize %s: no dimensions in the header; storing the original %u B",
                 local, (unsigned)len);
        return write_local(local, buf, len);
    }
    // NOT epd_fit_reduction(): that answers "which INTEGER factor", and the answer is 1 for
    // everything between 1x and 2x the fit -- a 1000x750 source would be kept whole when it
    // could be stored at 592x448. img_scale_fit_target() answers the question actually being
    // asked, which is "is this bigger than the box", and img_resize.c now reduces by the
    // ratio rather than by a factor (ticket 50 item A).
    int32_t fit_w = 0, fit_h = 0, tgt_w = 0, tgt_h = 0;
    const bool have_target = img_scale_fit_target(sw, sh, SMB_RESIZE_FIT_EDGE,
                                                  IMG_RESIZE_ALIGN, &fit_w, &fit_h,
                                                  &tgt_w, &tgt_h);
    // A picture the camera asked to be turned has to go through the re-encode even when it
    // is already inside the box, because the turn IS the re-encode -- the alternative leaves
    // the original bytes on the card and a thumbnail built by img_resize_to_edge(), which
    // does apply the orientation. The grid would then be upright and the panel sideways, and
    // the two disagreeing is worse than both being wrong: it looks like a thumbnail bug.
    const int orient = img_exif_orientation(buf, len);
    const bool nothing_to_reduce =
        (!have_target || (tgt_w >= sw && tgt_h >= sh)) && !img_orient_needed(orient);

    const int64_t t0 = now_ms();

    // Nothing to reduce: the panel draws this at or above the source's own size, so a
    // re-encode could only lose detail and add bytes. Store the original and spend the
    // decode on the thumbnail alone, which asks the decoder for a quarter of the pixels.
    if (nothing_to_reduce) {
        const esp_err_t err = write_local(local, buf, len);
        size_t thumb_len = 0;
        int64_t thumb_ms = 0;
        if (err == ESP_OK) {
            // Timed from HERE and not from t0. write_local() takes the storage lock, which
            // on a microSD is SPI2, which the panel is on -- so a refresh in flight makes it
            // wait the whole 15 s. Measured 2026-09-09: a `total=17393 ms` line sat directly
            // under `render ... panel_ms=15024.2`, and reading it as the cost of the
            // thumbnail would have been wrong by an order of magnitude.
            const int64_t tt0 = now_ms();
            uint8_t *rgb = NULL;
            int32_t w = 0, h = 0;
            const esp_err_t rerr = img_resize_to_edge(buf, len, IMG_THUMB_MAX_EDGE, &rgb,
                                                      &w, &h);
            if (rerr == ESP_OK) {
                thumb_len = write_thumb(tpath, rgb, w, h);
                heap_caps_free(rgb);
            } else {
                ESP_LOGW(TAG, "resize %s: thumbnail %s", local,
                         rerr == ESP_ERR_NO_MEM ? "out of memory" : "decode failed");
            }
            thumb_ms = now_ms() - tt0;
        }
        ESP_LOGI(TAG, "resize %s: %dx%d orient=%d inside the box, kept %u B thumb=%u B "
                      "thumb_ms=%lld store_ms=%lld", local, (int)sw, (int)sh, orient,
                 (unsigned)len,
                 (unsigned)thumb_len, (long long)thumb_ms,
                 (long long)(now_ms() - t0 - thumb_ms));
        return err;
    }

    uint8_t *rgb = NULL;
    int32_t w = 0, h = 0;
    const esp_err_t rerr = img_resize_to_fit(buf, len, SMB_RESIZE_FIT_EDGE,
                                             SMB_RESIZE_FIT_EDGE, &rgb, &w, &h);
    if (rerr != ESP_OK) {
        // The two are a 503 and a 415 in HTTP terms and they must not share a log line:
        // "out of memory" is a moment, "decode failed" is the file.
        ESP_LOGW(TAG, "resize %s: %s; storing the original %u B", local,
                 rerr == ESP_ERR_NO_MEM ? "out of memory" : "decode failed", (unsigned)len);
        return write_local(local, buf, len);
    }
    const int64_t t_decode = now_ms();

    uint8_t *out = NULL;
    size_t out_len = 0;
    if (img_encode_jpeg(rgb, w, h, SMB_RESIZE_QUALITY, &out, &out_len) != ESP_OK) {
        heap_caps_free(rgb);
        ESP_LOGW(TAG, "resize %s: encode failed; storing the original %u B", local,
                 (unsigned)len);
        return write_local(local, buf, len);
    }
    const int64_t t_encode = now_ms();

    // The name is unchanged even when a PNG has become JPEG bytes, and that is deliberate:
    // the local name is the manifest's identity, the eviction key and the slideshow's
    // current_name, so renaming it would touch every one of those. Nothing on the device is
    // misled -- epd_image_open_file() sniffs the magic and only falls back to the extension
    // (epd_image.c:555-557) -- and app_server.c's mime_for() is what a browser needs.
    //
    // The "no smaller" test stays as a safety net now that the factor gate above catches the
    // case it was really there for.
    const bool smaller = (out_len < len);
    const esp_err_t err = smaller ? write_local(local, out, out_len)
                                  : write_local(local, buf, len);
    free(out);

    // The thumbnail, from the buffer that is already decoded and already reduced. This is
    // the whole reason the two are one operation: /thumb/'s 1.2-1.4 s is a decode, and it has
    // just been paid.
    size_t thumb_len = 0;
    if (err == ESP_OK) {
        thumb_len = write_thumb(tpath, rgb, w, h);
    }
    heap_caps_free(rgb);

    // The TARGET is logged beside the size actually produced, because they can differ: the
    // resample is skipped when there is no PSRAM for its destination, and the picture is then
    // stored at the decode's own size rather than the fit's. Reading only `w`x`h` would show a
    // photograph that stored larger than expected with no reason given. `fit=` is also the
    // target for the file's OWN dimensions, so at orient 5-8 it is the transpose of the size
    // produced -- which is the observable that says the turn happened.
    ESP_LOGI(TAG, "resize %s: %dx%d orient=%d fit=%dx%d %ux%u %u -> %u B%s thumb=%u B "
                  "decode=%lld encode=%lld ms", local, (int)sw, (int)sh, orient, (int)tgt_w,
             (int)tgt_h,
             (unsigned)w, (unsigned)h, (unsigned)len, (unsigned)(smaller ? out_len : len),
             smaller ? "" : " (kept, no smaller)", (unsigned)thumb_len,
             (long long)(t_decode - t0), (long long)(t_encode - t_decode));
    return err;
}

static void delete_local(const char *local)
{
    char path[192];
    local_path(path, sizeof(path), local);
    // BOTH sidecars go with the photograph, and this is the only place they can: a sidecar is not
    // in .smbidx, so nothing else knows either exists. Both eviction and reconcile_deletions()
    // come through here, which is why one unlink each covers everything. Unconditional -- with
    // smb_resize off or no EXIF there is nothing to remove and unlink() says ENOENT, which costs
    // one FATFS lookup inside a lock that is already held.
    char tpath[BOARD_STORAGE_PATH_MAX];
    app_smb_sync_sidecar_path(tpath, sizeof(tpath), local, SMB_THUMB_SUFFIX);
    char mpath[BOARD_STORAGE_PATH_MAX];
    app_smb_sync_sidecar_path(mpath, sizeof(mpath), local, SMB_META_SUFFIX);
    board_storage_lock();
    board_storage_prepare_access();
    const int rc = unlink(path);
    unlink(tpath);
    unlink(mpath);
    board_storage_unlock();
    ESP_LOGI(TAG, "drop %s%s", local, rc == 0 ? "" : " (already gone)");
}

esp_err_t app_smb_sync_store_photo(const char *local, const uint8_t *buf, size_t len)
{
    return store_photo(local, buf, len);
}

void app_smb_sync_delete_photo(const char *local)
{
    delete_local(local);
}

static void manifest_load(void)
{
    char path[192];
    local_path(path, sizeof(path), SMB_MANIFEST_FILE);

    board_storage_lock();
    board_storage_prepare_access();
    FILE *f = fopen(path, "rb");
    long len = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
    }
    char *buf = NULL;
    if (f && len > 0 && len <= MANIFEST_FILE_MAX) {
        buf = heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM);
        if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) {
            heap_caps_free(buf);
            buf = NULL;
        }
    }
    if (f) {
        fclose(f);
    }
    board_storage_unlock();

    smb_manifest_reset(&s_have);
    if (buf) {
        // A parse failure is not repaired: the whole manifest is discarded and the next
        // run re-fetches. That is one code path for a damaged index instead of a repair
        // routine nobody exercises -- and, because the manifest is also the ownership
        // record, it means the mirror temporarily owns nothing rather than owning the
        // wrong thing.
        if (!smb_manifest_parse(&s_have, buf, (size_t)len)) {
            ESP_LOGW(TAG, "%s unparseable; a full re-fetch will rebuild it", path);
            smb_manifest_reset(&s_have);
        }
        heap_caps_free(buf);
    }
    ESP_LOGI(TAG, "manifest: %u mirrored files", (unsigned)smb_manifest_count(&s_have));
}

static void manifest_store(const smb_manifest_t *m)
{
    const size_t need = m->arena_used + smb_manifest_count(m) * 48 + 32;
    char *buf = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "no PSRAM for a %u byte manifest", (unsigned)need);
        return;
    }
    const size_t n = smb_manifest_serialise(m, buf, need);
    if (n == 0) {
        ESP_LOGE(TAG, "serialise into %u bytes failed", (unsigned)need);
        heap_caps_free(buf);
        return;
    }

    // TICKET 78: `<name>.part` and a rename, as the catalogue below and every photograph
    // already did. This file is the OWNERSHIP RECORD, so it was the worst of the four to be
    // writing at the real name: a cut inside a record line loses every file's ownership loudly,
    // and a cut exactly on a record boundary loses the tail of it SILENTLY -- an index that
    // parses is indistinguishable from an index of a smaller cache. The helper's comment has the
    // sweep that measures the two classes.
    char path[192];
    local_path(path, sizeof(path), SMB_MANIFEST_FILE);
    board_storage_write_atomic(path, buf, n);
    heap_caps_free(buf);
}

// TICKET 27's ORPHAN DEFECT, and this is the fix. The invariant the mirror needs is "every
// file on the card is named by the STORED manifest, at every instant" -- because a file the
// manifest does not name is a file the mirror will never touch again, and after a power cut the
// only manifest there is is the stored one.
//
// Neither manifest alone satisfies it mid-run:
//
//   * s_next alone omits a file that is on the card from an earlier run and was planned for
//     re-fetch, because build_plan() deliberately does not carry a changed record forward.
//   * s_have alone omits whatever this window has already written.
//
// So the union is stored, and only the union. Reachable, not theoretical: a session-level
// failure after one successful fetch in the same window took the `manifest_load()` path, which
// discarded s_next along with the record of the file just written.
//
// It appends to s_next and then TRUNCATES IT BACK rather than allocating a third manifest --
// one is ~24 KB of struct plus a 160 KB arena, and this runs on the window task's 8 KB stack in
// the middle of a run. The appended names stay in the arena past arena_used, where the next
// add() overwrites them.
static void manifest_store_union(void)
{
    const size_t count = s_next.count;
    const size_t used = s_next.arena_used;
    const bool was_truncated = s_next.truncated;
    const bool was_full = s_next.arena_full;

    for (size_t i = 0; i < smb_manifest_count(&s_have); i++) {
        const char *path = smb_manifest_path(&s_have, i);
        if (smb_manifest_find(&s_next, path) >= 0) {
            continue;
        }
        smb_manifest_add(&s_next, smb_manifest_local(&s_have, i), path,
                         smb_manifest_size(&s_have, i), smb_manifest_mtime(&s_have, i));
    }
    if (s_next.truncated || s_next.arena_full) {
        // The stored manifest is then missing records for files that are on the card, which is
        // the orphan this function exists to prevent. Said out loud rather than silently
        // half-done; it needs the share to hold more photographs than SMB_MANIFEST_MAX.
        ESP_LOGE(TAG, "the union of %u owned and %u new records does not fit %u; some files on "
                      "%s are left unowned",
                 (unsigned)smb_manifest_count(&s_have), (unsigned)count,
                 (unsigned)SMB_MANIFEST_MAX, BOARD_STORAGE_MOUNT);
    }
    manifest_store(&s_next);

    s_next.count = count;
    s_next.arena_used = used;
    s_next.truncated = was_truncated;
    s_next.arena_full = was_full;
}

// ------------------------------------------------------------------------- status

static void status_set_file(const char *name)
{
    lock();
    snprintf(s_status.last_file, sizeof(s_status.last_file), "%s", name ? name : "");
    unlock();
}

// ------------------------------------------------------------------ the catalogue

// Same shape as manifest_load(): read the whole file into PSRAM, parse, and on any malformation
// discard the lot. The recovery is different and much cheaper, though -- a damaged manifest
// means the mirror temporarily owns nothing, while a damaged catalogue just means the folders
// get listed again over the next few runs.
static void catalog_load(void)
{
    if (!s_catalog) {
        return;
    }
    char path[192];
    local_path(path, sizeof(path), SMB_CATALOG_FILE);

    board_storage_lock();
    board_storage_prepare_access();
    FILE *f = fopen(path, "rb");
    long len = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
    }
    char *buf = NULL;
    if (f && len > 0 && len <= CATALOG_FILE_MAX) {
        buf = heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM);
        if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) {
            heap_caps_free(buf);
            buf = NULL;
        }
    }
    if (f) {
        fclose(f);
    }
    board_storage_unlock();

    smb_catalog_reset(s_catalog);
    if (buf) {
        if (!smb_catalog_parse(s_catalog, buf, (size_t)len)) {
            ESP_LOGW(TAG, "%s unparseable; the folders will be listed again", path);
            smb_catalog_reset(s_catalog);
        }
        heap_caps_free(buf);
    }
    ESP_LOGI(TAG, "catalogue: %u photographs on the share",
             (unsigned)smb_catalog_count(s_catalog));
}

static void catalog_store(void)
{
    if (!s_catalog) {
        return;
    }
    const size_t need = s_catalog->arena_used + smb_catalog_count(s_catalog) * 48 + 32;
    char *buf = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "no PSRAM for a %u byte catalogue", (unsigned)need);
        return;
    }
    const size_t n = smb_catalog_serialise(s_catalog, buf, need);
    if (n == 0) {
        ESP_LOGE(TAG, "serialise the catalogue into %u bytes failed", (unsigned)need);
        heap_caps_free(buf);
        return;
    }

    // `<name>.part` and a rename, for write_local()'s reason: a power cut mid-write must leave
    // the old catalogue or none, never a truncated file that happens to parse.
    char path[192];
    local_path(path, sizeof(path), SMB_CATALOG_FILE);
    board_storage_write_atomic(path, buf, n);
    heap_caps_free(buf);
}

// One folder per run, round-robin. Called with a live session at the end of a run.
//
// WHY ONE, AND WHY NOT ALL OF THEM IN A LOOP: each listing is an smb2_opendir, whose internal
// RAM is 142.5 bytes per directory entry and is released again when the walk ends. Six in a row
// would be six times the wall clock inside a window that has httpd stopped, for a catalogue
// that is only allowed to be a few runs stale. The cursor spreads it out; at the one-hour
// period a six-folder share is fully catalogued within six hours of a boot and each folder is
// refreshed every six hours after that.
static void catalogue_next_folder(void)
{
    if (!s_catalog) {
        return;
    }
    const size_t folders = smb_catalog_folder_count(s_folder_list);
    if (folders == 0) {
        // An empty smb_path means the share root, and the mirror's own listing already covers
        // it. Cataloguing it again would be the same listing twice, so there is nothing to do
        // -- and on a root too big to list it is also the listing that cannot be afforded.
        return;
    }

    // A FOLDER THAT HAS LEFT smb_path KEEPS ITS RECORDS OTHERWISE, because the drop below only
    // ever runs for a folder the round-robin still lists. Ticket 51 found this by pointing the
    // mirror at one folder for an arm and putting the six real ones back: the eight strays were
    // then selected like anything else, failed to fetch, and cost EPOCH_MISS_MAX advances each.
    //
    // Here rather than in config_load(), which is where s_folder_list is actually written: this
    // function already owns folder-level maintenance of the catalogue, already holds the lock
    // for it, and already calls catalog_store() afterwards, so the pruning persists for free.
    // The cost is that a settings edit takes effect at the next catalogue refresh rather than at
    // the next window, which is the same staleness the catalogue is allowed everywhere else.
    lock();
    const size_t strays = smb_catalog_keep_folders(s_catalog, s_folder_list);
    unlock();
    if (strays > 0) {
        ESP_LOGI(TAG, "catalogue: dropped %u records of folders no longer in smb_path",
                 (unsigned)strays);
    }

    // The cursor comes off the catalogue, so it is whatever survived the last reboot. Taken
    // modulo the folder count on the way in as well as out: the list can shrink between runs
    // when somebody edits the setting, and a stale cursor past its end must wrap rather than
    // silently catalogue nothing.
    const size_t which = s_catalog->folder_cursor % folders;
    char folder[BOARD_SMB_PATH_SIZE];
    if (!smb_catalog_folder_at(s_folder_list, which, folder, sizeof(folder))) {
        return;
    }
    s_catalog->folder_cursor = (uint32_t)((which + 1) % folders);

    board_smb_entry_t *entries =
        heap_caps_malloc(sizeof(board_smb_entry_t) * SMB_SYNC_LIST_MAX, MALLOC_CAP_SPIRAM);
    if (!entries) {
        ESP_LOGW(TAG, "no PSRAM to catalogue %s this run", folder);
        return;
    }

    size_t count = 0;
    bool truncated = false;
    const int64_t t0 = now_ms();
    // accept_photo() is the mirror's own predicate and is reused deliberately so the catalogue
    // and the fetcher cannot disagree about what a photograph is. It prepends s_cfg.path to
    // build a full path, which is the WRONG folder here -- and harmless, because the predicate
    // reads only the leaf's extension and its leading character. Passing the real folder would
    // mean a second full_path() variant for no change in the answer.
    const board_smb_err_t err = board_smb_list_path(folder, BOARD_SMB_LIST_FILES, entries,
                                                    SMB_SYNC_LIST_MAX, &count, accept_photo,
                                                    &truncated);
    const int64_t list_ms = now_ms() - t0;
    if (err != BOARD_SMB_OK) {
        ESP_LOGW(TAG, "catalogue %s failed: %s", folder, board_smb_err_str(err));
        heap_caps_free(entries);
        return;
    }

    // Drop then re-add, so a folder whose contents changed is replaced rather than merged. A
    // merge would keep photographs that have been deleted from the share and, worse, would
    // double-weight the survivors in the shuffle.
    //
    // AND THEN PUT IT BACK WHERE IT WAS. smb_catalog_add() appends, so without the restore the
    // refreshed folder ends up at the tail and every record after its old position shifts --
    // 120 to 230 of them per hourly refresh on this share, with nothing on the share changed.
    // The epoch permutation, the want list and the fetch counter all hold indices across that,
    // so all three quietly came to mean something else: an epoch pass covered two thirds of the
    // share (modelled) and a photograph fetched within ~20 minutes of a refresh was never drawn
    // (measured, both runs, no exceptions). Tickets 44 and 45.
    //
    // UNDER THE LOCK, because since Phase 3 the selector reads this structure from another task
    // (app_smb_sync_catalog_local) and a drop shifts every record after it. The hold is pure RAM
    // work -- the listing above and the store below are both outside it -- and it is tens of
    // milliseconds at these sizes, against the 120 s a caller of board_smb.c's lock can wait.
    lock();
    size_t first = 0;
    const size_t dropped = smb_catalog_drop_folder(s_catalog, folder, &first);
    const size_t tail = smb_catalog_count(s_catalog);
    size_t added = 0;
    for (size_t i = 0; i < count; i++) {
        char path[SMB_CATALOG_PATH_SIZE];
        snprintf(path, sizeof(path), "%s%s%s", folder, folder[0] ? "/" : "", entries[i].name);
        if (smb_catalog_add(s_catalog, path, entries[i].size, entries[i].mtime)) {
            added++;
        }
    }
    smb_catalog_restore_at(s_catalog, tail, first);
    unlock();
    heap_caps_free(entries);

    ESP_LOGI(TAG,
             "catalogued %s [%u/%u]: %u photographs (%u dropped, %u listed%s) in %lld ms; %u on "
             "the share across %u folders%s; next %u",
             folder, (unsigned)(which + 1), (unsigned)folders, (unsigned)added,
             (unsigned)dropped, (unsigned)count, truncated ? ", TRUNCATED" : "",
             (long long)list_ms, (unsigned)smb_catalog_count(s_catalog), (unsigned)folders,
             s_catalog->truncated ? " (CATALOGUE FULL)" : "",
             (unsigned)s_catalog->folder_cursor);

    catalog_store();
    s_last_catalog_us = esp_timer_get_time();

    lock();
    s_status.catalog_count = (uint16_t)smb_catalog_count(s_catalog);
    s_status.catalog_folders = (uint8_t)folders;
    s_status.catalog_truncated = s_catalog->truncated || truncated;
    unlock();
}

// ------------------------------------------------------- on demand (ticket 37 Phase 3)

// Called with a live session on the window task. Fetches the wanted entries, oldest ask first,
// and takes each one off the list whether it landed or not: a file that failed is re-asked for by
// the selector on its next advance, which bounds the retry rate to the slideshow's interval
// instead of to the window rate.
static void ondemand_fetch(int64_t deadline, bool *landed)
{
    int fetched = 0;
    while (fetched < SMB_SYNC_FILES_PER_WINDOW && now_ms() < deadline) {
        lock();
        const bool any = s_want_count > 0;
        const uint16_t which = any ? s_want[0] : 0;
        if (any) {
            for (uint8_t i = 1; i < s_want_count; i++) {
                s_want[i - 1] = s_want[i];
            }
            s_want_count--;
            s_status.want_pending = s_want_count;
        }
        unlock();
        if (!any) {
            break;
        }
        // INSTRUMENT, NOT A FIX. 7-9 % of on-demand fetches are of a path already fetched in the
        // same run (8 of 90, then 4 of 54 after the duplicate-record fix, which stopped the extra
        // records and not the extra transfer). Five of those eight were two pops of the same index
        // inside ONE window, 3-4 s apart, with no eviction between -- so the want list must have
        // been replaced between the two pops. The obvious culprit is that declare_wants_locked()
        // calls a photograph absent while it is being fetched, and the timing does not fit: the
        // display line lands before the FIRST fetch of each pair, not between them.
        //
        // So this prints what is popped and what the list looked like, and app_smb_sync_want()
        // prints what it was handed. A `local_present()` skip here would remove the symptom and
        // leave the cause unknown, which is the mistake that ticket already recorded once.
        //
        // 2026-09-08: THE 7-9 % ABOVE WAS COUNTED BY INDEX AND IS TOO HIGH -- the hourly refresh
        // renumbers the catalogue, so a repeated index is usually a different photograph (ticket
        // 45). By share path it is 3.5 % over the same log, and the transfer actually being wasted
        // is elsewhere: 86 % of fetches were evicted before being drawn (ticket 44). The `want pop`
        // line stays because it is what makes either countable from a log at all.
        ESP_LOGI(TAG, "want pop %u, %u left", (unsigned)which, (unsigned)s_want_count);

        char path[SMB_CATALOG_PATH_SIZE];
        char local[SMB_MANIFEST_NAME_SIZE];
        uint64_t size = 0, mtime = 0;
        lock();
        const char *p = smb_catalog_path(s_catalog, which);
        if (p) {
            snprintf(path, sizeof(path), "%s", p);
            size = smb_catalog_size(s_catalog, which);
            mtime = smb_catalog_mtime(s_catalog, which);
        }
        unlock();
        // The index was resolved against a catalogue that may have been rebuilt since the ask.
        // Dropped rather than fetched: a shifted index names some OTHER photograph, and fetching
        // that would be a frame quietly showing the wrong thing.
        if (!p || !smb_manifest_local_name(path, local, sizeof(local))) {
            ESP_LOGW(TAG, "want %u no longer resolves; dropped", (unsigned)which);
            continue;
        }
        if (size == 0 || size > SMB_SYNC_FILE_MAX) {
            ESP_LOGW(TAG, "skip %s: %llu bytes", path, (unsigned long long)size);
            continue;
        }

        status_set_file(path);
        uint8_t *buf = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
        if (!buf) {
            ESP_LOGE(TAG, "no PSRAM for %llu bytes", (unsigned long long)size);
            break;
        }
        size_t len = 0;
        const int64_t t0 = now_ms();
        const board_smb_err_t err = board_smb_read_file_path(path, buf, (size_t)size, &len);
        const int64_t took = now_ms() - t0;
        bool ok = false;
        if (err == BOARD_SMB_OK && len == (size_t)size) {
            ok = (store_photo(local, buf, len) == ESP_OK);
        }
        heap_caps_free(buf);

        lock();
        s_status.last_error = err;
        unlock();
        fetched++;

        if (!ok) {
            ESP_LOGW(TAG, "fetch %s failed after %lld ms: %s", path, (long long)took,
                     board_smb_err_str(err));
            if (err == BOARD_SMB_ERR_CONNECT || err == BOARD_SMB_ERR_AUTH ||
                err == BOARD_SMB_ERR_SHARE || err == BOARD_SMB_ERR_DENIED) {
                break; // the session is gone; the rest of the list is not this file's fault
            }
            continue;
        }

        // REPLACE, NOT APPEND, and the difference is not tidiness. A photograph that was evicted
        // and later chosen again is fetched again, and appending would leave TWO records for one
        // path: `files_mirrored` inflates, and worse, evicting one of them unlinks the file while
        // the other still claims to own it -- so the next fetch adds a third. Found on hardware
        // 2026-09-06, as a manifest of 120 records against 113 owned files on the card.
        //
        // Removing first also keeps the invariant eviction depends on: the record goes on the END,
        // so record order stays fetch order.
        //
        // It is the healing path for the other direction too (ticket 34's): a record naming a file
        // that is no longer on the card is what the selector reports as "absent", and the fetch it
        // asks for lands here and rewrites the record. Under on-demand there is no build_plan() to
        // do that healing, so this is the only place it can happen.
        const int had = smb_manifest_find(&s_have, path);
        if (had >= 0) {
            smb_manifest_remove_at(&s_have, (size_t)had);
        }
        // Straight into s_have, and that is the whole reason eviction can be FIFO without storing
        // an access time: on-demand is the only thing that appends here, so the record order IS
        // the fetch order. The mirror path's s_next/swap does not run under this setting.
        if (!smb_manifest_add(&s_have, local, path, size, mtime)) {
            // The file is on the card and unowned, which is the orphan this module works hard to
            // avoid. It needs the cache to be over SMB_MANIFEST_MAX, which SMB_SYNC_CACHE_FILES
            // is five times under, so it is a reportable impossibility rather than a case.
            ESP_LOGE(TAG, "%s written but the manifest refused it (%u records)", local,
                     (unsigned)smb_manifest_count(&s_have));
        }
        *landed = true;
        // Counted on success only: a fetch that failed transferred nothing and is already
        // visible as last_error. The `want pop` lines above stay, so a log can be counted
        // independently of these two numbers -- which is how the counter itself is checked.
        lock();
        if (s_status.fetch_total < UINT16_MAX) {
            s_status.fetch_total++;
        }
        if (which < SMB_CATALOG_MAX && !(s_fetch_seen[which / 8] & (uint8_t)(1u << (which % 8)))) {
            s_fetch_seen[which / 8] |= (uint8_t)(1u << (which % 8));
            s_status.fetch_distinct++;
        }
        unlock();
        ESP_LOGI(TAG, "fetched %s -> %s (%llu bytes, %lld ms)", path, local,
                 (unsigned long long)size, (long long)took);
    }
}

// The cache's ceiling. Oldest fetch first, which under a shuffle epoch is also least-recently
// shown -- each photograph is drawn once per pass, so fetch order and show order agree and no
// per-file access time has to be kept.
//
// Three things are never evicted: what is on the panel, what the settle window is about to draw,
// and anything still wanted. The first two are asked of the slideshow rather than tracked here,
// because it is the only thing that knows.
//
// `need_space` is ticket 54's second target: the volume is under its reserve, so evict up to the
// window's budget whatever the count says. **It deliberately does NOT try to free a computed
// number of bytes.** The manifest's size field is the SHARE's size, which since ticket 49 is
// ~3.5x what is stored, and a stat() per record to get the real one is the 22-38 ms call ticket
// 36 removed from this module. So the space question is answered by re-reading the volume after
// the eviction rather than by predicting it, and a window that did not free enough simply does
// not fetch -- the next one evicts more.
//
// SMB_SYNC_CACHE_MIN_FILES is the floor that only this path needs. The count target never needed
// one, because eviction ran only ABOVE the ceiling; a space target would otherwise empty the
// cache on a volume that is full for reasons the cache did not cause -- uploads, the factory
// PNGs -- and the frame would show nothing while thrashing. Below the floor the window reports
// `capped` instead, which is a state to look at rather than a loop to run.
static size_t evict_to_cache_limit(bool need_space)
{
    const size_t have = smb_manifest_count(&s_have);
    if (need_space ? (have <= SMB_SYNC_CACHE_MIN_FILES) : (have <= SMB_SYNC_CACHE_FILES)) {
        return 0;
    }

    app_slideshow_state_t ss;
    app_slideshow_get_state(&ss);

    char protected_names[SMB_SYNC_WANT_MAX][SMB_MANIFEST_NAME_SIZE];
    size_t protected_count = 0;
    lock();
    for (uint8_t i = 0; i < s_want_count && protected_count < SMB_SYNC_WANT_MAX; i++) {
        const char *p = smb_catalog_path(s_catalog, s_want[i]);
        if (p && smb_manifest_local_name(p, protected_names[protected_count],
                                         SMB_MANIFEST_NAME_SIZE)) {
            protected_count++;
        }
    }
    unlock();

    size_t dropped = 0;
    size_t i = 0;
    while (smb_manifest_count(&s_have) >
               (need_space ? SMB_SYNC_CACHE_MIN_FILES : SMB_SYNC_CACHE_FILES) &&
           dropped < SMB_SYNC_EVICT_PER_WINDOW && i < smb_manifest_count(&s_have)) {
        const char *local = smb_manifest_local(&s_have, i);
        // The protected set is src/core/smb_evict.c since 2026-09-21. The walk stays here -- it
        // deletes files, mutates the manifest it is iterating and holds the lock -- but the rule it
        // must not get wrong is host-tested now: never the photograph on the glass, never the one
        // queued, never one the selector has asked for. smb_evict.h has what each mistake costs.
        if (!smb_evict_may_drop(local, ss.current_name, ss.pending_name, protected_names,
                                protected_count)) {
            // Said out loud, because a silent skip left ticket 77 §7.2 unable to tell "the rule
            // protected it" from "the rule never met a protected record". Rare by construction:
            // the candidate is the OLDEST record, and it has to be on the glass, queued or wanted.
            ESP_LOGI(TAG, "evict: kept %s (%s)", local,
                     strcmp(local, ss.current_name) == 0   ? "current"
                     : strcmp(local, ss.pending_name) == 0 ? "pending"
                                                            : "wanted");
            i++; // skipped, not removed, so the walk has to move on rather than re-test
            continue;
        }
        delete_local(local);
        smb_manifest_remove_at(&s_have, i); // the rest shift down, so i already points at the next
        dropped++;
    }

    if (dropped > 0) {
        lock();
        s_status.evicted = (uint16_t)(s_status.evicted + dropped);
        unlock();
        ESP_LOGI(TAG, "evicted %u; cache now %u of %u (need_space=%d)", (unsigned)dropped,
                 (unsigned)smb_manifest_count(&s_have), (unsigned)SMB_SYNC_CACHE_FILES,
                 need_space ? 1 : 0);
    }
    return dropped;
}

// ---------------------------------------------------------------------- the plan

// Caps are two-tier by medium and are checked in build_plan(), which is THE MIRROR PATH ONLY.
// On hitting one the run stops and reports; it NEVER evicts.
//
// **The header used to say "checked before every fetch" and that was never true of on-demand.**
// ondemand_fetch() consults neither s_cap_bytes nor s_cap_files -- ticket 37 Phase 3 built that
// window as "a different window, not a variant" and the caps were among the things it did not
// carry across. What bounds the on-demand path is SMB_SYNC_CACHE_FILES plus the volume's own
// free space, in cache_has_room() (ticket 54). Two paths, two rules, and this is the one for the
// mirror.
//
// NEITHER CAP CAN FIRE ON AN SD-BACKED /data, and it is arithmetic rather than bad luck.
// The file cap is PHOTO_LIST_MAX = 500 while a run can never plan more than
// SMB_SYNC_LIST_MAX = 200 entries, which ticket 27 already recorded; and the byte cap is
// 70 % of free, which on the fitted card (1,824,161,792 bytes free, read from the device on
// 2026-09-06) is 1.277 GB against a hard ceiling of 200 x SMB_SYNC_FILE_MAX = 800 MB. So on
// this card the effective control is listing_truncated, not `capped`. The byte cap becomes
// reachable on a card with under ~1.14 GB free, which is a real configuration and not this
// one. Ticket 33 has the numbers.
static void compute_caps(void)
{
    if (board_storage_get_media() == BOARD_STORAGE_MEDIA_SD) {
        uint64_t total = 0, freeb = 0;
        board_storage_usage(&total, &freeb);
        s_cap_bytes = freeb / 10u * 7u; // 70 % of what is free
        s_cap_files = PHOTO_LIST_MAX;
    } else {
        s_cap_bytes = 4ull * 1024 * 1024;
        s_cap_files = 200;
    }
#ifdef SMB_SYNC_CAP_FILES_OVERRIDE
    // A test hook, and it must be described as one: it exercises the cap BRANCH, which is
    // otherwise unreachable on this card, and says nothing about the real threshold. Not in
    // any committed build_flags; ticket 33 adds it to platformio_local.ini for one arm and
    // takes it out again.
    s_cap_files = SMB_SYNC_CAP_FILES_OVERRIDE;
    ESP_LOGW(TAG, "file cap overridden to %u by SMB_SYNC_CAP_FILES_OVERRIDE -- this is a "
                  "test build",
             (unsigned)s_cap_files);
#endif
    ESP_LOGI(TAG, "caps: %llu bytes, %u files", (unsigned long long)s_cap_bytes,
             (unsigned)s_cap_files);
}

// Lists the share and works out what has to be fetched. Everything already mirrored and
// unchanged goes straight into the next manifest.
static board_smb_err_t build_plan(void)
{
    s_entry_count = 0;
    s_plan_count = 0;
    s_plan_pos = 0;
    smb_manifest_reset(&s_next);

    s_listing_truncated = false;
    const int64_t t_list0 = esp_timer_get_time();
    const board_smb_err_t err = board_smb_list(s_entries, SMB_SYNC_LIST_MAX, &s_entry_count,
                                               accept_photo, &s_listing_truncated);
    const int64_t list_us = esp_timer_get_time() - t_list0;
    if (err != BOARD_SMB_OK) {
        return err;
    }
    // An empty directory is a success with zero files. Conflating that with a failure is
    // the bug ComittoNxA shipped, and treating a failure as empty is its equal and
    // opposite -- here it would delete every mirrored photograph.
    ESP_LOGI(TAG, "share lists %u entries%s", (unsigned)s_entry_count,
             s_listing_truncated ? " (TRUNCATED: the share holds more)" : "");

    const int64_t t_set0 = esp_timer_get_time();
    local_set_t local_set;
    local_set_build(&local_set);
    const int64_t set_us = esp_timer_get_time() - t_set0;
    unsigned probed = 0, fallbacks = 0;

    for (size_t i = 0; i < s_entry_count; i++) {
        char path[SMB_MANIFEST_PATH_SIZE];
        char local[SMB_MANIFEST_NAME_SIZE];
        full_path(path, sizeof(path), s_entries[i].name);
        if (!smb_manifest_local_name(path, local, sizeof(local))) {
            continue; // not one of the four image extensions
        }

        const int have = smb_manifest_find(&s_have, path);
        if (have >= 0 && smb_manifest_unchanged(&s_have, (size_t)have, s_entries[i].size,
                                                s_entries[i].mtime)) {
            // The manifest saying a file is current is not the same as the file being
            // there. Measured 2026-09-06 (ticket 34): the manifest named 13 files and the
            // card held 8 of them, and because this branch asked only the manifest, the
            // five missing photographs were reported "already current" and never
            // re-fetched -- permanently, since nothing else ever revisits them. A file can
            // leave the card without the manifest hearing about it: a web-UI delete, an
            // interrupted write, a card swapped for another card.
            //
            // So ask the filesystem. It costs one stat() per already-mirrored entry, on a
            // path that already takes the storage lock per file to write, and it is what
            // makes the mirror able to heal.
            probed++;
            if (local_present(&local_set, smb_manifest_local(&s_have, (size_t)have),
                              &fallbacks)) {
                smb_manifest_add(&s_next, smb_manifest_local(&s_have, (size_t)have), path,
                                 s_entries[i].size, s_entries[i].mtime);
                continue;
            }
            ESP_LOGW(TAG, "%s is in the manifest but not on %s; re-fetching",
                     smb_manifest_local(&s_have, (size_t)have), BOARD_STORAGE_MOUNT);
        }
        if (s_entries[i].size > SMB_SYNC_FILE_MAX) {
            ESP_LOGW(TAG, "skip %s: %llu bytes is over the per-file ceiling",
                     s_entries[i].name, (unsigned long long)s_entries[i].size);
            continue;
        }
        s_plan[s_plan_count++] = (uint16_t)i;
    }

    const unsigned dir_count = (unsigned)local_set.count;
    const bool dir_complete = local_set.complete;
    const unsigned dir_unowned = (unsigned)local_set.unowned;
    publish_unowned(&local_set);
    local_set_free(&local_set);

    ESP_LOGI(TAG, "plan: %u to fetch, %u already current", (unsigned)s_plan_count,
             (unsigned)smb_manifest_count(&s_next));
    // `stat_fallbacks` should be 0 on this card. It is printed because it is the number that
    // says whether the readdir replaced the per-file stat() or merely joined it -- a run where
    // it equals `probes` costs everything the old path cost plus the scan.
    ESP_LOGI(TAG,
             "plan phases: list=%lld ms dirscan=%lld ms (%u entries, complete=%d) probes=%u "
             "stat_fallbacks=%u unowned=%u",
             (long long)(list_us / 1000), (long long)(set_us / 1000), dir_count,
             dir_complete ? 1 : 0, probed, fallbacks, dir_unowned);
    return BOARD_SMB_OK;
}

// True if the file landed.
static bool fetch_one(size_t entry, bool *fatal)
{
    const board_smb_entry_t *e = &s_entries[entry];
    char path[SMB_MANIFEST_PATH_SIZE];
    char local[SMB_MANIFEST_NAME_SIZE];
    full_path(path, sizeof(path), e->name);
    if (!smb_manifest_local_name(path, local, sizeof(local))) {
        return false;
    }
    status_set_file(e->name);

    if (e->size == 0) {
        // A zero-byte file is not a photograph and heap_caps_malloc(0) is not a buffer.
        return false;
    }

    uint8_t *buf = heap_caps_malloc((size_t)e->size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "no PSRAM for %llu bytes", (unsigned long long)e->size);
        return false;
    }

    size_t len = 0;
    const int64_t t0 = now_ms();
    const board_smb_err_t err = board_smb_read_file(e->name, buf, (size_t)e->size, &len);
    const int64_t took = now_ms() - t0;

    bool ok = false;
    if (err == BOARD_SMB_OK && len == (size_t)e->size) {
        ok = (store_photo(local, buf, len) == ESP_OK);
    }
    heap_caps_free(buf);

    lock();
    s_status.last_error = err;
    unlock();

    if (!ok) {
        // BOARD_SMB_ERR_TIMEOUT is a per-file outcome, not a configuration problem: the
        // server sent a header and then stopped, board_smb.c spent its deadline and its
        // retries, and the right answer is to try this file again on the next run rather
        // than to tell the user to reconfigure anything.
        ESP_LOGW(TAG, "fetch %s failed after %lld ms: %s", e->name, (long long)took,
                 board_smb_err_str(err));
        // Only a session-level failure ends the window early; one bad file does not.
        *fatal = (err == BOARD_SMB_ERR_CONNECT || err == BOARD_SMB_ERR_AUTH ||
                  err == BOARD_SMB_ERR_SHARE || err == BOARD_SMB_ERR_DENIED);
        return false;
    }

    smb_manifest_add(&s_next, local, path, e->size, e->mtime);
    ESP_LOGI(TAG, "mirrored %s -> %s (%llu bytes, %lld ms)", e->name, local,
             (unsigned long long)e->size, (long long)took);
    return true;
}

// Everything the old manifest names and the new one does not. A file whose fetch failed
// keeps its old record, so the local copy stays and the next run retries it; a file that
// has gone from the share is deleted. Files named by NEITHER manifest are never touched
// -- that is what protects web-UI uploads and the factory imaged00N.png.
// Returns how many local copies it deleted, which finish_run() needs in order to know
// whether the stored manifest is still correct.
static size_t reconcile_deletions(void)
{
    size_t deleted = 0;
    for (size_t i = 0; i < smb_manifest_count(&s_have); i++) {
        const char *path = smb_manifest_path(&s_have, i);
        const char *local = smb_manifest_local(&s_have, i);
        if (smb_manifest_find(&s_next, path) >= 0) {
            continue;
        }

        // A truncated listing is a window onto the share, not the share, so a file
        // missing from it is not a file that has gone -- assume it is still there and
        // keep both the record and the local copy. Without this the mirror deletes
        // photographs the share still holds, which is the same mistake as reconciling
        // against a session that never came up, one paragraph down (ticket 27).
        bool on_share = s_listing_truncated;
        for (size_t j = 0; j < s_entry_count && !on_share; j++) {
            char p[SMB_MANIFEST_PATH_SIZE];
            full_path(p, sizeof(p), s_entries[j].name);
            on_share = (strcmp(p, path) == 0);
        }

        if (on_share) {
            // Still on the share, so this run simply did not manage to fetch it. Keep
            // the record and the local copy; the next run will see the same difference
            // and try again.
            smb_manifest_add(&s_next, local, path, smb_manifest_size(&s_have, i),
                             smb_manifest_mtime(&s_have, i));
        } else {
            delete_local(local);
            deleted++;
        }
    }
    return deleted;
}

// ------------------------------------------------------------------- the window

static void finish_run(bool landed)
{
    const size_t had = smb_manifest_count(&s_have);
    const size_t deleted = reconcile_deletions();

    // Swap: s_next becomes what we have. The arenas are swapped with them, because a
    // manifest is only meaningful alongside the arena its offsets point into.
    char *old_arena = s_have_arena;
    s_have = s_next;
    s_have_arena = s_next_arena;
    s_have.arena = s_have_arena;
    s_next_arena = old_arena;
    memset(&s_next, 0, sizeof(s_next));

    // Nothing fetched, nothing deleted and the same number of records: the file on the card
    // already says exactly this, so writing it again is a card write for no change. At the
    // one-hour period that is 24 pointless writes a day instead of 4. The three conditions
    // are deliberately conservative -- a record whose size or mtime moved without a fetch
    // cannot happen, because build_plan() only carries a record forward unchanged when it
    // matched the share on both.
    if (landed || deleted > 0 || smb_manifest_count(&s_have) != had) {
        manifest_store(&s_have);
    } else {
        ESP_LOGI(TAG, "manifest unchanged at %u files; not rewritten",
                 (unsigned)smb_manifest_count(&s_have));
    }

    uint64_t bytes = 0;
    for (size_t i = 0; i < smb_manifest_count(&s_have); i++) {
        bytes += smb_manifest_size(&s_have, i);
    }

    lock();
    s_status.files_mirrored = (uint16_t)smb_manifest_count(&s_have);
    s_status.bytes_mirrored = bytes;
    unlock();

    if (landed) {
        // The slideshow re-enumerates /data on every navigation, so a new file needs no
        // code above the mirror -- but a list it has already cached would be stale.
        app_slideshow_invalidate();
    }
    s_run_active = false;
    s_last_run_end_us = esp_timer_get_time();
    ESP_LOGI(TAG, "run complete: %u files, %llu bytes", (unsigned)s_status.files_mirrored,
             (unsigned long long)bytes);
}

static void restart_server(void)
{
    // Losing httpd permanently would leave a frame with no way to be configured, which
    // is worse than anything the mirror does. Nothing here can fix an out-of-memory
    // condition, but it can wait for the transient part of one to pass.
    for (int i = 0; i < 5; i++) {
        if (app_server_start() == ESP_OK) {
            return;
        }
        ESP_LOGE(TAG, "app_server_start failed, attempt %d", i + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "the web server did not come back up");
}

// Whether the on-demand cache may write to /data at all this window, and ticket 54's whole
// mechanism. Called BEFORE board_smb_connect(), which is not incidental: eviction takes the
// storage lock, which on a microSD is SPI2 -- the panel's bus -- and the window is careful never
// to hold that with a live SMB session waiting on it. Doing the check first also means a window
// with nowhere to put a photograph does not open a session to fetch one.
//
// **The instrument is the VOLUME's free space, not the sum of the manifest's sizes**, for two
// reasons. The manifest records the SHARE's size, which since ticket 49's import-time resize is
// ~3.5x what is stored. And free space also counts the .thm sidecars, web-UI uploads and the four
// factory PNGs -- every byte on the volume the manifest does not name. The failure mode is "the
// volume is full", so the volume is the thing to measure.
//
// Once per window rather than once per fetch: esp_vfs_fat_info() takes the storage lock, and its
// cost here is printed rather than assumed.
//
// An unreadable usage figure is treated as room. That is the pre-ticket-54 behaviour, and it is
// the right default: write_local() short-writes safely, so being wrong costs one failed fetch,
// while refusing on a read error would stop the frame fetching for ever.
static bool cache_has_room(size_t *evicted, int64_t *usage_ms, uint64_t *free_out,
                           uint64_t *reserve_out)
{
    *evicted = 0;
    *free_out = 0;
    *reserve_out = 0;

    uint64_t total = 0, freeb = 0;
    const int64_t t0 = now_ms();
    const bool ok = (board_storage_usage(&total, &freeb) == ESP_OK);
    *usage_ms = now_ms() - t0;
    if (!ok) {
        ESP_LOGW(TAG, "storage usage unreadable; fetching anyway");
        return true;
    }

    uint64_t reserve = smb_cache_reserve_bytes(total);
#ifdef SMB_SYNC_CACHE_RESERVE_OVERRIDE
    // A test hook, and it must be described as one. The condition this defends against needs the
    // CARD OUT (ticket 08 steps 4-8, hands on the slot), so with a card fitted the branch is
    // unreachable and an override is the only way to exercise it. Same shape and same warning as
    // SMB_SYNC_CAP_FILES_OVERRIDE: not in any committed build_flags, and it says nothing about
    // the real threshold.
    reserve = (uint64_t)SMB_SYNC_CACHE_RESERVE_OVERRIDE;
    ESP_LOGW(TAG, "cache reserve overridden to %llu -- this is a test build",
             (unsigned long long)reserve);
#endif
    *free_out = freeb;
    *reserve_out = reserve;
    if (freeb >= reserve) {
        return true;
    }

    ESP_LOGW(TAG, "cache: %llu free under a %llu reserve on a %llu volume; evicting first",
             (unsigned long long)freeb, (unsigned long long)reserve, (unsigned long long)total);
    *evicted = evict_to_cache_limit(true);
    if (board_storage_usage(&total, &freeb) == ESP_OK) {
        *free_out = freeb;
    }
    return freeb >= reserve;
}

static void window_task(void *arg)
{
    (void)arg;
    const int64_t t_start = now_ms();
    const int64_t deadline = t_start + SMB_SYNC_WINDOW_MS;
    bool landed = false;

    // Published rather than polled: the heap sampler runs at 20 ms from a priority-3 task and must
    // not block on this module's mutex to find out whether a window is open (ticket 47).
    //
    // **Before app_server_stop(), not after s_status.syncing**, and that is a measurement rather
    // than tidiness. With it below, two runs on 2026-09-11 recorded an internal-RAM low with NO
    // flags set about a second after a POST /api/smb/sync -- 13,303 B in one and 18,539 B in the
    // other -- because stopping and restarting httpd, and creating this task's own 8 KB stack, all
    // happen in the window's prologue while `syncing` is still false. An unflagged low reads as
    // "something nobody watches", which is the most expensive kind of wrong answer this instrument
    // can give.
    app_heapwatch_set_activity(HEAPWATCH_F_SYNC, true);

    app_server_stop();

    lock();
    s_status.syncing = true;
    s_status.capped = false;
    unlock();

    // Ticket 54, and before the connect for the lock reason cache_has_room() gives. The
    // catalogue refresh below still runs when there is no room: re-listing a folder costs no
    // space, and stopping it would stall the hourly clock over a condition it cannot fix.
    bool room = true;
    size_t pre_evicted = 0;
    int64_t usage_ms = 0;
    uint64_t free_bytes = 0, reserve_bytes = 0;
    if (s_on_demand) {
        room = cache_has_room(&pre_evicted, &usage_ms, &free_bytes, &reserve_bytes);
    }

    board_smb_err_t err = board_smb_connect(&s_cfg);
    const int64_t t_connected = now_ms();
    // Stays equal to t_connected when this window did not build a plan, which is every
    // window of a run after the first -- and every window under on-demand, which never lists.
    int64_t t_planned = t_connected;

    // ON-DEMAND IS A DIFFERENT WINDOW, not a variant of the one below (ticket 37 Phase 3): no
    // listing, no plan, no reconciliation, and a bounded cache instead of a mirror. It shares
    // the httpd stop, the connect, the catalogue pass and the tail because those are the same
    // work; everything between is not.
    if (s_on_demand) {
        lock();
        s_status.files_total = s_want_count;
        s_status.files_done = 0;
        unlock();

        if (err == BOARD_SMB_OK && room) {
            ondemand_fetch(deadline, &landed);
        } else if (err == BOARD_SMB_OK) {
            // Refused rather than attempted. write_local() would short-write, unlink the .part
            // and try again next window for ever, which is exactly the non-convergence ticket 40
            // found; `capped` is the same field the mirror path uses for the same meaning.
            ESP_LOGW(TAG, "cache: no room after evicting %u; skipping this window's fetches",
                     (unsigned)pre_evicted);
            lock();
            s_status.capped = true;
            unlock();
        } else {
            ESP_LOGE(TAG, "session failed: %s", board_smb_err_str(err));
            lock();
            s_status.last_error = err;
            unlock();
        }
        const int64_t t_fetched_od = now_ms();

        // The catalogue keeps the hourly clock; the fetches do not reset it. Skipped entirely
        // when the session is down -- a listing needs one, and s_last_catalog_us must not move
        // for a refresh that did not happen.
        const bool catalog_due =
            (s_last_catalog_us == INT64_MIN) ||
            ((esp_timer_get_time() - s_last_catalog_us) >= (int64_t)SMB_SYNC_PERIOD_MS * 1000);
        if (err == BOARD_SMB_OK && catalog_due) {
            catalogue_next_folder();
        }
        const int64_t t_catalogued_od = now_ms();

        // The session goes BEFORE the eviction, so the storage lock -- which on a microSD is
        // SPI2, the panel's bus -- is never held with a live SMB session waiting on it.
        board_smb_disconnect();
        const size_t evicted = pre_evicted + evict_to_cache_limit(false);

        // One manifest write per window rather than one per fetch, and it is safe in the same
        // way the mirror's mid-run store is: write_local() renames into place, so a file is
        // either absent or whole, and a power cut between a fetch and this store leaves a whole
        // file that no manifest names. THAT IS THE ORPHAN, and it is the one case Phase 3 does
        // not close -- bounded to at most SMB_SYNC_FILES_PER_WINDOW files, where the mirror's
        // was unbounded across a run. Recorded rather than hidden; closing it costs a card write
        // per photograph.
        if (landed || evicted > 0) {
            manifest_store(&s_have);
            uint64_t bytes = 0;
            for (size_t i = 0; i < smb_manifest_count(&s_have); i++) {
                bytes += smb_manifest_size(&s_have, i);
            }
            lock();
            s_status.files_mirrored = (uint16_t)smb_manifest_count(&s_have);
            s_status.bytes_mirrored = bytes;
            unlock();
            app_slideshow_invalidate();
        }

        // After the store, so the count is taken against the manifest this window leaves behind
        // rather than the one it started with -- an orphan created by a cut BEFORE that store is
        // exactly what this is for, and counting first would hide the window's own fetches in the
        // number for one window.
        count_unowned();

        s_run_active = false;
        s_last_run_end_us = esp_timer_get_time();

        restart_server();

        const uint32_t od_stack = uxTaskGetStackHighWaterMark(NULL);
        lock();
        s_status.syncing = false;
        s_status.stack_free = od_stack;
        unlock();
        app_heapwatch_set_activity(HEAPWATCH_F_SYNC, false);
        s_last_window_end_us = esp_timer_get_time();
        s_task_running = false;
        ESP_LOGI(TAG,
                 // fetch=/catalogue=/httpd_down= are DURATIONS IN MILLISECONDS; fetched= and
                 // distinct= are the counts (ticket 44 -- a plan once checked `fetch=` for a
                 // non-zero count and the check passed trivially either way).
                 // free=/reserve= are BYTES and usage= is a DURATION; room= is the only derived
                 // value on the line and the two bytes figures are beside it, so a wrong verdict
                 // can be recomputed from the log rather than re-run (ticket 54).
                 // unowned= is a COUNT, and it is NOT a count of orphans -- uploads and the
                 // factory images are in it, so read it moving rather than absolutely (ticket
                 // 27). The `!` marks the walk incomplete, which makes it a lower bound. It is
                 // on this line because on-demand is the path where the count is produced and
                 // the console is the instrument whenever httpd is the thing that is down.
                 "on-demand window: fetch=%lld catalogue=%lld httpd_down=%lld ms usage=%lld ms "
                 "free=%llu reserve=%llu room=%d evicted=%u want_left=%u cache=%u fetched=%u "
                 "distinct=%u unowned=%u%s stack %u",
                 (long long)(t_fetched_od - t_connected),
                 (long long)(t_catalogued_od - t_fetched_od),
                 (long long)(now_ms() - t_start), (long long)usage_ms,
                 (unsigned long long)free_bytes, (unsigned long long)reserve_bytes,
                 room ? 1 : 0, (unsigned)evicted, (unsigned)s_want_count,
                 (unsigned)smb_manifest_count(&s_have), (unsigned)s_status.fetch_total,
                 (unsigned)s_status.fetch_distinct, (unsigned)s_status.unowned_files,
                 s_status.unowned_exact ? "" : "!", (unsigned)od_stack);
        vTaskDelete(NULL);
        return;
    }

    if (err == BOARD_SMB_OK && s_plan_count == 0 && s_plan_pos == 0 && s_entry_count == 0) {
        err = build_plan();
        t_planned = now_ms();
        if (err == BOARD_SMB_OK) {
            compute_caps();
            lock();
            s_status.files_total = s_plan_count;
            s_status.files_done = 0;
            s_status.listing_truncated = s_listing_truncated;
            unlock();
        }
    }

    if (err != BOARD_SMB_OK) {
        ESP_LOGE(TAG, "session failed: %s", board_smb_err_str(err));
        lock();
        s_status.last_error = err;
        unlock();
    } else {
        uint64_t bytes = 0;
        for (size_t i = 0; i < smb_manifest_count(&s_next); i++) {
            bytes += smb_manifest_size(&s_next, i);
        }

        int fetched = 0;
        while (s_plan_pos < s_plan_count && fetched < SMB_SYNC_FILES_PER_WINDOW &&
               now_ms() < deadline) {
            const board_smb_entry_t *e = &s_entries[s_plan[s_plan_pos]];
            if (smb_manifest_count(&s_next) >= s_cap_files ||
                bytes + e->size > s_cap_bytes) {
                ESP_LOGW(TAG, "cap reached at %u files / %llu bytes; stopping, evicting "
                              "nothing",
                         (unsigned)smb_manifest_count(&s_next), (unsigned long long)bytes);
                lock();
                s_status.capped = true;
                unlock();
                s_plan_pos = s_plan_count; // ends the run, keeps everything already there
                break;
            }

            bool fatal = false;
            if (fetch_one(s_plan[s_plan_pos], &fatal)) {
                bytes += e->size;
                landed = true;
            }
            s_plan_pos++;
            fetched++;
            lock();
            s_status.files_done = s_plan_pos;
            unlock();
            if (fatal) {
                break;
            }
        }
    }

    const int64_t t_fetched = now_ms();

    // The catalogue pass, before the session goes: one folder of the list per run (ticket 37).
    // At the END of the run and not the start, deliberately -- the mirror's own plan and fetches
    // are what the user is waiting for, and a listing that fails or takes 900 ms must not delay
    // them or hold s_entries hostage. Its own buffer for the same reason: s_entries belongs to
    // the run until finish_run() has reconciled against it.
    const bool done = (err != BOARD_SMB_OK) || (s_plan_pos >= s_plan_count);
    if (done && err == BOARD_SMB_OK) {
        catalogue_next_folder();
    }
    const int64_t t_catalogued = now_ms();

    board_smb_disconnect();
    if (done) {
        if (err == BOARD_SMB_OK) {
            finish_run(landed);
        } else {
            // The session never came up, so the listing says nothing about the share and
            // reconciling against it would delete every mirrored photograph.
            //
            // An earlier window of this same run may have written files and stored a
            // partial manifest naming them, so re-read it: dropping s_next without that
            // would leave those files on the card with no ownership record in RAM, and
            // the mirror never touches a file it does not own.
            //
            // AND THIS WINDOW'S OWN FETCHES HAVE TO BE STORED FIRST (ticket 27's orphan
            // defect). Re-reading the card recovers what an earlier window stored and nothing
            // about what this one wrote after that, so a file fetched in the same window that
            // then hit a session failure was orphaned permanently.
            if (landed) {
                manifest_store_union();
            }
            if (landed || s_plan_pos > 0) {
                manifest_load();
            }
            s_run_active = false;
            s_last_run_end_us = esp_timer_get_time();
        }
        heap_caps_free(s_entries);
        s_entries = NULL;
        heap_caps_free(s_next_arena);
        s_next_arena = NULL;
        memset(&s_next, 0, sizeof(s_next));
        s_entry_count = 0;
        s_plan_count = 0;
        s_plan_pos = 0;
    } else if (landed) {
        // Partway through a run: the files already written are usable now, and the
        // manifest has to name them or a reboot would leave them unowned. The UNION, not
        // s_next -- s_next does not yet name a file that is on the card and waiting to be
        // re-fetched, and storing it alone orphaned exactly those (ticket 27).
        manifest_store_union();
        app_slideshow_invalidate();
    }

    restart_server();

    // Read before the task exits: after vTaskDelete there is nothing to ask.
    const uint32_t stack_free = uxTaskGetStackHighWaterMark(NULL);
    lock();
    s_status.syncing = false;
    s_status.stack_free = stack_free;
    unlock();
    app_heapwatch_set_activity(HEAPWATCH_F_SYNC, false);

    s_last_window_end_us = esp_timer_get_time();
    s_task_running = false;
    ESP_LOGI(TAG, "window done: %u/%u, stack high-water %u bytes", (unsigned)s_plan_pos,
             (unsigned)s_plan_count, (unsigned)stack_free);
    // The phases, separately, because the one-hour period rests on a claim about the cost of
    // a run that fetches nothing -- connect + list + one stat() per mirrored file -- and that
    // claim was arithmetic, not a measurement. A window with plan=0 prints its own answer
    // here. `httpd_down` spans the app_server_stop() to app_server_start() pair and is the
    // number the owner's decision was about.
    ESP_LOGI(TAG,
             "window timing: connect=%lld plan=%lld fetch=%lld catalogue=%lld finish=%lld "
             "httpd_down=%lld ms plan_count=%u",
             (long long)(t_connected - t_start), (long long)(t_planned - t_connected),
             (long long)(t_fetched - t_planned), (long long)(t_catalogued - t_fetched),
             (long long)(now_ms() - t_catalogued), (long long)(now_ms() - t_start),
             (unsigned)s_plan_count);
    vTaskDelete(NULL);
}

// ------------------------------------------------------------------- the settings

// `require_enabled` is false only for the connect test, which has to work on settings the
// user has not committed to yet -- the point of testing is to find out whether they are
// right before switching the mirror on.
static bool config_load(board_smb_config_t *out, bool require_enabled)
{
    app_settings_t s;
    app_settings_get(&s);
    if (s.smb_host[0] == '\0' || s.smb_share[0] == '\0') {
        return false;
    }
    if (require_enabled && !s.smb_enabled) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->host, sizeof(out->host), "%s", s.smb_host);
    snprintf(out->share, sizeof(out->share), "%s", s.smb_share);
    // smb_path is a LIST now (ticket 37). The transport takes ONE directory, and it is
    // element 0 -- the folder the mirror itself mirrors and the folder every existing device's
    // single-valued setting resolves to. The full list is kept for the catalogue, which walks
    // all of them. An empty list leaves path "", which has always meant the share root.
    snprintf(s_folder_list, sizeof(s_folder_list), "%s", s.smb_path);
    if (!smb_catalog_folder_at(s_folder_list, 0, out->path, sizeof(out->path))) {
        out->path[0] = '\0';
    }
    snprintf(out->user, sizeof(out->user), "%s", s.smb_user);
    snprintf(out->password, sizeof(out->password), "%s", s.smb_password);
    snprintf(out->domain, sizeof(out->domain), "%s", s.smb_domain);
    out->enabled = true;
    // Read here because this is the one place that already snapshots the settings, and the
    // window task must not see it change mid-window: the two window bodies free different
    // things, so a flip between the branch and the tail would leak or double-free.
    s_on_demand = s.smb_on_demand;
    lock();
    s_status.on_demand = s_on_demand;
    unlock();
    return true;
}

// ------------------------------------------------------------------ the connect test

// Connect, tree-connect, disconnect -- and no listing, so none of smb2_opendir's per-entry
// internal RAM. httpd stays up; the header says why that is the experiment and not an
// oversight.
static void test_task(void *arg)
{
    (void)arg;

    lock();
    s_status.test_state = APP_SMB_TEST_RUNNING;
    unlock();

    const size_t int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t dma_before = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

    const int64_t t0 = now_ms();
    const board_smb_err_t err = board_smb_connect(&s_test_cfg);
    const int64_t t_conn = now_ms();
    board_smb_disconnect();
    const int64_t t1 = now_ms();

    // RAW NUMBERS, NOT A VERDICT. Whether a session can coexist with httpd is what this
    // test informs, and a boolean computed here would be a second thing that can be wrong
    // (ticket 03's read_after_refresh). Note that int_min is the since-boot watermark, so it
    // is only meaningful against the heartbeat's own value from before the test -- which is
    // printed every 10 s and is therefore always available in the same log.
    ESP_LOGI(TAG,
             "test: %s connect=%lld ms total=%lld ms int_free %u -> %u int_min=%u "
             "int_largest=%u dma_largest %u -> %u",
             board_smb_err_str(err), (long long)(t_conn - t0), (long long)(t1 - t0),
             (unsigned)int_before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)dma_before,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    // Read before the task exits, as window_task does: after vTaskDelete there is nothing
    // left to ask.
    const uint32_t stack_free = uxTaskGetStackHighWaterMark(NULL);

    lock();
    s_status.test_state = APP_SMB_TEST_DONE;
    s_status.test_error = err;
    s_status.test_ms = (uint32_t)(t1 - t0);
    unlock();

    ESP_LOGI(TAG, "test task stack high-water %u bytes", (unsigned)stack_free);
    s_test_task_running = false;
    vTaskDelete(NULL);
}

static void service_test(void)
{
    if (!s_test_request || s_test_task_running || s_task_running) {
        return;
    }
    // A run owns the single session across all of its windows, so a test in one of its gaps
    // would be a second session inside one run. Stay QUEUED until the run ends; the status
    // says so, which is a better answer than a test that quietly does not happen.
    if (s_run_active) {
        return;
    }

    if (!config_load(&s_test_cfg, false)) {
        // The route validates host and share before queueing, so this is the race where the
        // settings were cleared in between. There is no BOARD_SMB_ERR_* for "nothing
        // configured" and inventing one would put a fictional SMB status in front of the
        // user, so the request is simply dropped back to idle.
        ESP_LOGW(TAG, "test cancelled: no host or share configured");
        s_test_request = false;
        lock();
        s_status.test_state = APP_SMB_TEST_IDLE;
        unlock();
        return;
    }

    const size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    if (int_free < SMB_SYNC_TEST_MIN_INT_FREE || dma_largest < SMB_SYNC_TEST_MIN_DMA_LARGEST) {
        ESP_LOGW(TAG, "test refused: int_free=%u (floor %u) dma_largest=%u (floor %u)",
                 (unsigned)int_free, (unsigned)SMB_SYNC_TEST_MIN_INT_FREE,
                 (unsigned)dma_largest, (unsigned)SMB_SYNC_TEST_MIN_DMA_LARGEST);
        s_test_request = false;
        lock();
        s_status.test_state = APP_SMB_TEST_REFUSED;
        unlock();
        return;
    }

    s_test_request = false;
    s_test_task_running = true;
    if (xTaskCreate(test_task, "smbtest", TEST_TASK_STACK, NULL, SYNC_TASK_PRIO, NULL) !=
        pdPASS) {
        ESP_LOGW(TAG, "no internal RAM for the test task");
        s_test_task_running = false;
        lock();
        s_status.test_state = APP_SMB_TEST_REFUSED;
        unlock();
    }
}

// -------------------------------------------------------------------- the schedule

static bool config_from_settings(void)
{
    return config_load(&s_cfg, true);
}

// The decision itself moved to src/core/smb_window.c on 2026-09-21, so that tickets 28, 42, 37 and
// 59 could be argued in test/test_smb_window instead of in a comment -- this rule was the most
// revised in the module and had never executed on the host, while a wrong branch reads from the
// console as a network or a storage problem. smb_window.h says why it is worth its own file.
//
// What is left here is the adapter: gather this module's state and hand it over. The statics are
// read without the lock, exactly as they were before -- run_is_due() never took it -- and the
// caller holds no lock at this point either.
//
// **One call is now unconditional where it used to be reached only on the last branch**:
// app_clock_schedule_active(). It costs a settings-mutex take per sync tick (once per 10 s) and
// introduces no new lock order, because the branch that already called it was reached with no lock
// held either. The alternative was to keep the term lazy, which means keeping it impure, which is
// the whole reason the predicate could not be tested where it was.
static bool run_is_due(int64_t now_us)
{
    const smb_window_state_t state = {
        .run_active = s_run_active,
        .last_window_end_us = s_last_window_end_us,
        .manual_request = s_manual_request,
        .last_run_end_us = s_last_run_end_us,
        .on_demand = s_on_demand,
        .want_count = s_want_count,
        .last_catalog_us = s_last_catalog_us,
        .schedule_active = app_clock_schedule_active(),
    };
    const smb_window_timing_t timing = {
        .window_gap_us = (int64_t)SMB_SYNC_WINDOW_GAP_MS * 1000,
        .first_delay_us = (int64_t)SMB_SYNC_FIRST_DELAY_MS * 1000,
        .period_us = (int64_t)SMB_SYNC_PERIOD_MS * 1000,
    };
    return smb_window_is_due(&state, &timing, now_us);
}

esp_err_t app_smb_sync_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.last_sync_ms = UINT32_MAX;

    s_have_arena = heap_caps_malloc(MANIFEST_ARENA, MALLOC_CAP_SPIRAM);
    if (!s_have_arena) {
        return ESP_ERR_NO_MEM;
    }
    smb_manifest_init(&s_have, s_have_arena, MANIFEST_ARENA);
    manifest_load();

    // The catalogue. Both allocations are PSRAM and neither is fatal: a frame with no catalogue
    // mirrors and shows exactly what it did before ticket 37, which is a worse frame rather
    // than a broken one, so this must not be allowed to fail app_smb_sync_init() and take the
    // mirror down with it.
    s_catalog = heap_caps_malloc(sizeof(*s_catalog), MALLOC_CAP_SPIRAM);
    s_cat_arena = heap_caps_malloc(CATALOG_ARENA, MALLOC_CAP_SPIRAM);
    if (s_catalog && s_cat_arena) {
        smb_catalog_init(s_catalog, s_cat_arena, CATALOG_ARENA);
        catalog_load();
        s_status.catalog_count = (uint16_t)smb_catalog_count(s_catalog);
        s_status.catalog_truncated = s_catalog->truncated;
    } else {
        ESP_LOGW(TAG, "no PSRAM for the share catalogue (%u + %u bytes); running without one",
                 (unsigned)sizeof(*s_catalog), (unsigned)CATALOG_ARENA);
        heap_caps_free(s_catalog);
        heap_caps_free(s_cat_arena);
        s_catalog = NULL;
        s_cat_arena = NULL;
    }

    uint64_t bytes = 0;
    for (size_t i = 0; i < smb_manifest_count(&s_have); i++) {
        bytes += smb_manifest_size(&s_have, i);
    }
    s_status.files_mirrored = (uint16_t)smb_manifest_count(&s_have);
    s_status.bytes_mirrored = bytes;
    // Otherwise GET /api/smb/status answers enabled:false for the first ten seconds
    // after a boot, which reads as "the mirror is off" rather than "nothing has ticked
    // yet" -- observed on hardware 2026-09-04.
    s_status.enabled = config_from_settings();
    s_status.catalog_folders = (uint8_t)smb_catalog_folder_count(s_folder_list);
    return ESP_OK;
}

void app_smb_sync_media_changed(void)
{
    if (!s_have_arena) {
        return; // init has not run; there is nothing loaded to be wrong
    }
    // A window may be mid-flight on the worker task, and both loads rewrite the structures it
    // is reading. Refused rather than queued: the next tick reloads, and the window it is
    // finishing was against the previous volume anyway.
    if (s_task_running) {
        ESP_LOGW(TAG, "media changed during a window; the reload waits for the next tick");
        s_media_reload_due = true;
        return;
    }
    ESP_LOGI(TAG, "media changed; reloading the manifest and the catalogue from %s",
             BOARD_STORAGE_MOUNT);
    manifest_load();
    catalog_load();
    lock();
    s_status.files_mirrored = (uint16_t)smb_manifest_count(&s_have);
    uint64_t bytes = 0;
    for (size_t i = 0; i < smb_manifest_count(&s_have); i++) {
        bytes += smb_manifest_size(&s_have, i);
    }
    s_status.bytes_mirrored = bytes;
    s_status.catalog_count = (uint16_t)smb_catalog_count(s_catalog);
    unlock();
}

void app_smb_sync_request(void)
{
    s_manual_request = true;
}

void app_smb_sync_test_request(void)
{
    s_test_request = true;
    lock();
    s_status.test_state = APP_SMB_TEST_QUEUED;
    s_status.test_error = BOARD_SMB_OK;
    s_status.test_ms = 0;
    unlock();
}

void app_smb_sync_tick(void)
{
    if (!s_have_arena) {
        return;
    }

    // Before the schedule, and outside the s_task_running guard, because a test is not a
    // sync: it is allowed while the mirror is disabled and it does not stop httpd.
    service_test();

    // A test holds the single libsmb2 session, so no window may open while one runs.
    if (s_task_running || s_test_task_running) {
        return;
    }

    // The deferred half of app_smb_sync_media_changed(): the window it could not interrupt has
    // finished, so the reload happens here, before anything reads the stale structures.
    if (s_media_reload_due) {
        s_media_reload_due = false;
        app_smb_sync_media_changed();
    }

    const bool enabled = config_from_settings();
    lock();
    s_status.enabled = enabled;
    unlock();
    if (!enabled) {
        s_manual_request = false;
        return;
    }

    // NOT WHILE /data IS THE INTERNAL VOLUME (ticket 08, 2026-09-08). With the card out the
    // frame falls back to the 6 MB flash volume, and under smb_on_demand the selector then
    // calls EVERY catalogue entry absent -- so a window would try to fetch the 100-file cache
    // into 6 MB and rewrite .smbidx there. It fails noisily rather than corrupting, but a run
    // that cannot converge by construction should not start. Logged once per transition: a
    // per-tick line would be six an hour of nothing happening.
    //
    // **THIS PAUSE IS NOW A BLUNTER INSTRUMENT THAN THE PROBLEM NEEDS, and it is left in place
    // deliberately** (ticket 54). Two of its premises have moved. Ticket 40's "~19 MB at this
    // share's mean" was measured before import-time resize shipped on by default; the card's
    // stored median is 42 kB (2026-09-09), so a hundred photographs is ~4.2 MB and the same
    // order as the volume rather than three times it. And the non-convergence it names is now
    // bounded on every medium by cache_has_room(): a window under the reserve evicts and then
    // refuses, which is what "converges" means here.
    //
    // So this could become "run, bounded" rather than "do not run", and the payoff is a frame
    // that still shows the share with no card in. **What it needs is the one thing an unattended
    // run cannot do**: the condition is the card OUT, ticket 08 steps 4-8, a hand on the slot. An
    // override can exercise the reserve branch with a card fitted -- and did -- but it cannot
    // exercise a 6 MB FATFS volume filling up. Lifting this without that run would be trading a
    // guard that has been verified for one that has not.
    if (board_storage_get_media() != BOARD_STORAGE_MEDIA_SD) {
        if (!s_media_warned) {
            s_media_warned = true;
            ESP_LOGW(TAG, "/data is the internal volume; the mirror is paused until a card is "
                          "back");
        }
        s_manual_request = false;
        return;
    }
    s_media_warned = false;

    // No station link means every window would spend its 12 s connect deadline with the
    // web server down, for nothing. The share is on the LAN by definition.
    board_wifi_status_t w;
    board_wifi_status(&w);
    if (!w.sta_connected) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    if (!run_is_due(now_us)) {
        s_wait_since_us = INT64_MIN;
        return;
    }

    // The quiet gate, and the deadline that stops it becoming a refusal (ticket 28).
    //
    // The intent is unchanged -- do not pull httpd out from under somebody using the page
    // -- but the gate as written could never open on a frame anybody was standing near,
    // and that is structural rather than bad luck. FR-2.2's captive portal answers every
    // DNS query with the softAP's address precisely so that an associated client probes
    // it; the client's operating system then fetches GET / on a timer to decide whether
    // the network is captive. Measured on 2026-09-05 with one client on the AP: a
    // successful request every ~30 s against a 30 s gate, and idle_ms sawtoothing
    // 7 -> 17 -> 27 -> 7 forever. With no client associated, nothing arrived at all in
    // 200 s. So the portal and the gate were in direct conflict by construction.
    //
    // Two changes, and the first is the important one:
    //
    //   * A manual request does not wait. Pressing Sync now IS the statement that the
    //     interruption is acceptable, and the request is itself HTTP activity from a page
    //     that is about to make more -- so the old code let the act of asking defer the
    //     thing asked for.
    //   * A scheduled run still prefers a quiet moment, but only for a bounded time.
    //     After SMB_SYNC_QUIET_DEADLINE_MS of waiting it goes anyway, which costs a
    //     watcher up to one window and is the only behaviour that cannot deadlock.
    //
    // The gate applies to STARTING a run, not to continuing one. Once the first window
    // has opened the interruption has already happened, and what spaces the remaining
    // windows out is SMB_SYNC_WINDOW_GAP_MS, which exists for exactly that. Re-applying
    // the quiet test per window would multiply the deadline by the window count -- a
    // 200-file run against a client on the access point would take its ten minutes fifty
    // times over and never finish.
    if (!s_manual_request && !s_run_active) {
        if (s_wait_since_us == INT64_MIN) {
            s_wait_since_us = now_us;
        }
        const bool quiet = app_server_ms_since_activity() >= SMB_SYNC_QUIET_MS;
        const int64_t waited_ms = (now_us - s_wait_since_us) / 1000;
        if (!quiet && waited_ms < (int64_t)SMB_SYNC_QUIET_DEADLINE_MS) {
            return;
        }
        if (!quiet) {
            // Said out loud: a window that interrupts somebody should be explicable from
            // the log rather than looking like the mirror ignoring its own rule.
            ESP_LOGI(TAG, "quiet deadline reached after %lld ms of activity; syncing anyway",
                     (long long)waited_ms);
        }
    }
    s_wait_since_us = INT64_MIN;

    // What the gate actually saw, at the moment it let a window through. Ticket 59 spent three
    // arms inferring this from where windows fell and got it wrong twice; the gate's own reading is
    // one number and it settles the question. Printed for a continuing run too (`run=1`), because
    // that case is NOT gated -- the distinction is the first thing to check when a window lands on
    // top of a client.
    ESP_LOGI(TAG, "window due: idle=%u ms run=%d manual=%d",
             (unsigned)app_server_ms_since_activity(), s_run_active ? 1 : 0,
             s_manual_request ? 1 : 0);

    // On-demand builds no plan and lists no folder in the fetch path, so it needs neither the
    // second manifest arena nor the listing buffer -- 160 KB and 136 KB of PSRAM that the old
    // path allocates per run. catalogue_next_folder() allocates its own.
    if (!s_run_active && s_on_demand) {
        s_run_active = true;
        s_manual_request = false;
        lock();
        s_status.last_error = BOARD_SMB_OK;
        unlock();
    } else if (!s_run_active) {
        s_next_arena = heap_caps_malloc(MANIFEST_ARENA, MALLOC_CAP_SPIRAM);
        s_entries = heap_caps_malloc(sizeof(board_smb_entry_t) * SMB_SYNC_LIST_MAX,
                                     MALLOC_CAP_SPIRAM);
        if (!s_next_arena || !s_entries) {
            ESP_LOGE(TAG, "no PSRAM to start a run");
            heap_caps_free(s_next_arena);
            heap_caps_free(s_entries);
            s_next_arena = NULL;
            s_entries = NULL;
            return;
        }
        smb_manifest_init(&s_next, s_next_arena, MANIFEST_ARENA);
        s_entry_count = 0;
        s_plan_count = 0;
        s_plan_pos = 0;
        s_run_active = true;
        s_manual_request = false;
        lock();
        s_status.last_error = BOARD_SMB_OK;
        s_status.files_total = 0;
        s_status.files_done = 0;
        unlock();
    }

    s_task_running = true;
    if (xTaskCreate(window_task, "smbsync", SYNC_TASK_STACK, NULL, SYNC_TASK_PRIO, NULL) !=
        pdPASS) {
        // Internal RAM was short. Reporting that and retrying on the next tick is a much
        // better failure than holding this stack permanently so it can never happen.
        ESP_LOGW(TAG, "no internal RAM for the sync task; retrying");
        s_task_running = false;
        s_last_window_end_us = esp_timer_get_time();
    }
}

size_t app_smb_sync_catalog_count(void)
{
    if (!s_catalog) {
        return 0;
    }
    lock();
    const size_t n = smb_catalog_count(s_catalog);
    unlock();
    return n;
}

bool app_smb_sync_catalog_local(size_t i, char *local, size_t local_size)
{
    if (!s_catalog || !local || local_size == 0) {
        return false;
    }
    // The path is COPIED OUT under the lock and the local name derived after it. Holding this
    // module's mutex through the hashing would buy nothing, and the copy is what makes the
    // returned pointer safe against a catalogue rebuild on the window task.
    char path[SMB_CATALOG_PATH_SIZE];
    bool got = false;
    lock();
    const char *p = smb_catalog_path(s_catalog, i);
    if (p) {
        snprintf(path, sizeof(path), "%s", p);
        got = true;
    }
    unlock();
    if (!got) {
        return false;
    }
    return smb_manifest_local_name(path, local, local_size);
}

void app_smb_sync_want(const uint16_t *index, size_t n)
{
    lock();
    if (n > SMB_SYNC_WANT_MAX) {
        n = SMB_SYNC_WANT_MAX;
    }
    for (size_t i = 0; i < n; i++) {
        s_want[i] = index ? index[i] : 0;
    }
    s_want_count = index ? (uint8_t)n : 0;
    s_status.want_pending = s_want_count;
    // The list AS HANDED OVER, with its timestamp -- the other half of the instrument in
    // ondemand_fetch(). The open question is whether this call lands between two pops of one
    // window, and only a log line on both sides can say.
    // +8 and `<=`, because at SMB_SYNC_WANT_MAX = 1 the old `list[MAX * 8]` with `at + 8 <
    // sizeof(list)` was false on the FIRST iteration and every declaration logged as `want set
    // []` while the want list was perfectly correct -- the `want pop` lines are what showed it.
    // A log line that empties itself when a constant is lowered is worse than no log line.
    char list[SMB_SYNC_WANT_MAX * 8 + 8];
    size_t at = 0;
    for (uint8_t i = 0; i < s_want_count && at + 8 <= sizeof(list); i++) {
        at += (size_t)snprintf(list + at, sizeof(list) - at, "%s%u", i ? "," : "", s_want[i]);
    }
    list[at] = '\0';
    unlock();
    ESP_LOGI(TAG, "want set [%s]", list);
}

void app_smb_sync_get_status(app_smb_sync_status_t *out)
{
    if (!out) {
        return;
    }
    lock();
    *out = s_status;
    unlock();
    if (s_last_run_end_us != INT64_MIN) {
        out->last_sync_ms = (uint32_t)((esp_timer_get_time() - s_last_run_end_us) / 1000);
    } else {
        out->last_sync_ms = UINT32_MAX;
    }
}
