#include "app_slideshow.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app_catalog.h"
#include "app_clock.h"
#include "app_display.h"
#include "app_maint.h" // app_maint_course_running() -- the only thing taken from it, see below
#include "app_settings.h"
#include "app_smb_sync.h"
#include "board.h"
#include "board_storage.h"
#include "photo_list.h"
#include "smb_manifest.h"
// For smb_catalog_shuffle() only. The permutation is generic and lives there because that
// module is ESP-IDF-free and so its bijection is pinned by test/test_catalog/ on the host --
// see the epoch section below. The name is the catalogue's; the function is not.
#include "smb_catalog.h"

static const char *TAG = "slideshow";

// local_photo_slideshow.h:138. Long enough that a held button coalesces, short enough
// that a single press does not feel ignored.
#define SETTLE_MS 1000

// board_state_*() slots 0 and 1, little-endian, the shipping firmware's own layout
// (local_photo_slideshow.cpp:25, 245-246). On the M5Paper Color those are RX8130 RTC RAM
// bytes; this file no longer knows that, which is ticket 43's seam 4 closed.
#define RAM_INDEX_CURRENT 0

// Slots 2 and 3, the last two BOARD_STATE_BYTES has. The on-demand shuffle epoch's cursor;
// see the epoch section below for why it is persisted rather than derived.
#define RAM_EPOCH_POS 2

#define ARENA_BYTES 16384

static SemaphoreHandle_t s_mutex;
static char s_arena[ARENA_BYTES];
static photo_list_t s_list;

static uint16_t s_current = APP_SLIDESHOW_NO_PHOTO;
static uint16_t s_pending = APP_SLIDESHOW_NO_PHOTO;
static bool s_needs_refresh;
static bool s_invalidated;
static uint32_t s_settle_start_ms;
static uint32_t s_last_refresh_ms;

// ------------------------------------------------------------------- the shuffle epoch
//
// Ticket .scratch/digital-frame/issues/37, Phases 2 and 3. With `slideshow_random` on, an advance
// takes the next position of a PERMUTATION rather than the next index.
//
// WHY A PERMUTATION AND NOT esp_random() % count. Independent draws revisit and starve: the
// owner's complaint is "the frame shows the same pictures", and a random draw answers it
// with "the frame shows some pictures three times before others once", which is the same
// complaint in a new form. A permutation shows every photograph exactly once per pass.
//
// THE DOMAIN DEPENDS ON `smb_on_demand`, and so does how the position is kept:
//
//   * OFF -- the domain is the photo list. The position is DERIVED, by looking up the
//     already-persisted current index in the regenerated permutation, so a resumed pass needs no
//     new persisted state and no write per advance.
//   * ON -- the domain is the share CATALOGUE, which is the whole point of ticket 37: 1,032
//     photographs to choose from rather than the 245 on the card. The position cannot be derived
//     there, because a catalogue index maps to a local name and two entries in different folders
//     can share one (`202606/a.jpg` and `250416/a.jpg` both land as `a.jpg`), so the lookup is
//     ambiguous. It is persisted instead, in RX8130 RAM slots 2-3 -- the two bytes the
//     slideshow's own index leaves free, and the last two there are.
//
// Either way the seed changes once per PASS, which is where its one NVS write goes.
//
// IN PSRAM AND NOT BSS, which Phase 2 argued the other way round and was right to at the time:
// the photo list caps at PHOTO_LIST_MAX = 500, so 1 000 B of internal BSS bought no allocation
// failure path. The catalogue caps at SMB_CATALOG_MAX = 4096, which is 8 KB, and 8 KB of internal
// RAM against a ~70 KB idle free heap is not available. The failure path that came with the move
// is a fallback to filename order, which is a worse frame rather than a broken one.
static uint16_t *s_perm;
static uint16_t s_perm_n;     // 0 = no epoch generated yet
static uint32_t s_perm_seed;  // the seed s_perm was generated from
static bool s_perm_catalog;   // true when s_perm is over the catalogue rather than the photo list

// Only meaningful while s_perm_catalog. The cursor into the epoch, persisted in RTC RAM.
static uint16_t s_epoch_pos;
// Consecutive advances that found the next epoch position's file still absent. The cursor holds
// its place while a wanted photograph is being fetched -- that is what keeps the fetcher from
// being asked for pictures the cursor has already walked past -- but a position that can NEVER
// be fetched (deleted from the share between catalogue refreshes, or failing every time) would
// stick the slideshow on it forever. After this many it is given up on and skipped.
#define EPOCH_MISS_MAX 3
static uint8_t s_epoch_miss;

// How far ahead of the cursor to look for something drawable when the wanted photograph has not
// arrived. With a 100-file cache in a 1,032-entry catalogue the expected gap is ~10, so this is
// margin rather than a budget; each step costs a mutex, a hash and a scan of the photo list, and
// the walk stops at the first hit.
#define EPOCH_STANDIN_SCAN 200

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

// Two bytes, written on every navigation. A failure here is logged and otherwise
// ignored: an index that does not survive a power cut is a worse frame, not a broken
// one, and refusing to advance because the RTC did not answer would be worse still.
static void store_index(uint16_t index)
{
    const esp_err_t a = board_state_write(RAM_INDEX_CURRENT, (uint8_t)(index & 0xFF));
    const esp_err_t b = board_state_write(RAM_INDEX_CURRENT + 1, (uint8_t)(index >> 8));
    if (a != ESP_OK || b != ESP_OK) {
        ESP_LOGW(TAG, "index not persisted: %s / %s", esp_err_to_name(a), esp_err_to_name(b));
    }
}

static uint16_t load_index(void)
{
    uint8_t b0 = 0, b1 = 0;
    if (board_state_read(RAM_INDEX_CURRENT, &b0) != ESP_OK ||
        board_state_read(RAM_INDEX_CURRENT + 1, &b1) != ESP_OK) {
        ESP_LOGW(TAG, "stored index unreadable; starting at the beginning");
        return APP_SLIDESHOW_NO_PHOTO;
    }
    return (uint16_t)((uint16_t)b1 << 8 | b0);
}

// The on-demand epoch cursor, slots 2-3. Same judgement as store_index(): a failure is logged and
// otherwise ignored, because a cursor that does not survive a power cut restarts the pass rather
// than breaking the frame.
static void store_epoch_pos(uint16_t pos)
{
    const esp_err_t a = board_state_write(RAM_EPOCH_POS, (uint8_t)(pos & 0xFF));
    const esp_err_t b = board_state_write(RAM_EPOCH_POS + 1, (uint8_t)(pos >> 8));
    if (a != ESP_OK || b != ESP_OK) {
        ESP_LOGW(TAG, "epoch cursor not persisted: %s / %s", esp_err_to_name(a),
                 esp_err_to_name(b));
    }
}

static uint16_t load_epoch_pos(void)
{
    uint8_t b0 = 0, b1 = 0;
    if (board_state_read(RAM_EPOCH_POS, &b0) != ESP_OK ||
        board_state_read(RAM_EPOCH_POS + 1, &b1) != ESP_OK) {
        return 0;
    }
    return (uint16_t)((uint16_t)b1 << 8 | b0);
}

// Called with the lock held.
static void clamp_locked(void)
{
    const uint16_t count = (uint16_t)photo_list_count(&s_list);
    if (count == 0) {
        return;  // keep NO_PHOTO rather than inventing an index for an empty directory
    }
    if (s_current != APP_SLIDESHOW_NO_PHOTO && s_current >= count) {
        s_current %= count;
    }
    if (s_pending != APP_SLIDESHOW_NO_PHOTO && s_pending >= count) {
        s_pending %= count;
    }
}

static esp_err_t rescan_locked(void)
{
    photo_list_init(&s_list, s_arena, sizeof(s_arena));
    const esp_err_t err = board_storage_scan(&s_list);
    clamp_locked();
    return err;
}

// Called with the lock held. Regenerates the permutation for `count` entries from `seed`,
// minting and persisting a seed when there is none. Returns false if it could not, in which
// case the caller must fall back to filename order rather than to an unusable epoch.
static bool epoch_build_locked(uint16_t count, uint32_t seed, bool over_catalog)
{
    if (count == 0 || count > SMB_CATALOG_MAX) {
        s_perm_n = 0;
        return false;
    }
    if (!s_perm) {
        s_perm = heap_caps_malloc(SMB_CATALOG_MAX * sizeof(*s_perm), MALLOC_CAP_SPIRAM);
        if (!s_perm) {
            ESP_LOGW(TAG, "no PSRAM for a %u-entry shuffle epoch; using filename order",
                     (unsigned)SMB_CATALOG_MAX);
            s_perm_n = 0;
            return false;
        }
    }
    if (seed == 0) {
        // esp_random() is only a true RNG once RF is up, and this can run before that. It does
        // not matter here and would in app_auth.c: a predictable slideshow ORDER is not a
        // weakness, so the boot-order guarantee that the token needs is not needed for a seed.
        // Folded away from 0 because 0 means "not yet minted" and xorshift32 sticks at it.
        seed = esp_random();
        if (seed == 0) {
            seed = 0x9E3779B9u;
        }
        const esp_err_t err = app_settings_set_slideshow_seed(seed);
        if (err != ESP_OK) {
            // Kept in RAM anyway: an epoch that does not survive a reboot is a worse frame,
            // not a broken one -- the same judgement store_index() makes.
            ESP_LOGW(TAG, "shuffle seed not persisted: %s", esp_err_to_name(err));
        }
    }
    smb_catalog_shuffle(count, seed, s_perm);
    s_perm_n = count;
    s_perm_seed = seed;
    s_perm_catalog = over_catalog;
    return true;
}

// Called with the lock held. True when s_perm holds a usable permutation of `count` entries.
//
// A count CHANGE regenerates. A count that is the same while the names underneath it changed
// does not, and cannot be detected without keying the epoch on names: the consequence is that
// the order jumps once, not that anything is skipped or repeated within the new pass.
static bool epoch_ready_locked(uint16_t count, bool over_catalog)
{
    const uint32_t seed = app_settings_slideshow_seed();
    if (s_perm_n == count && s_perm_seed == seed && s_perm_catalog == over_catalog && seed != 0) {
        return true;
    }
    return epoch_build_locked(count, seed, over_catalog);
}

// Called with the lock held. Where `index` sits in the current epoch, or -1.
static int epoch_position_locked(uint16_t index)
{
    for (uint16_t i = 0; i < s_perm_n; i++) {
        if (s_perm[i] == index) {
            return (int)i;
        }
    }
    return -1;
}

// Called with the lock held. The photo-list index `delta` positions along the epoch from
// `base`, or APP_SLIDESHOW_NO_PHOTO when there is no usable epoch and the caller should walk
// filename order instead.
static uint16_t epoch_step_locked(uint16_t base, int delta, uint16_t count)
{
    if (!epoch_ready_locked(count, false)) {
        return APP_SLIDESHOW_NO_PHOTO;
    }

    const int pos = base == APP_SLIDESHOW_NO_PHOTO ? -1 : epoch_position_locked(base);
    if (pos < 0) {
        // Nothing displayed yet, or the list changed under the epoch. Enter at the end the
        // direction implies rather than at 0, so a `prev` from a cold start still goes back.
        return s_perm[delta > 0 ? 0 : (uint16_t)(count - 1)];
    }

    if (delta > 0) {
        if ((uint16_t)(pos + 1) < s_perm_n) {
            return s_perm[pos + 1];
        }
        // The pass is over. A NEW seed, or every pass would be the same order forever and the
        // setting would deliver one shuffle rather than a shuffled slideshow.
        if (!epoch_build_locked(count, 0, false)) {
            return APP_SLIDESHOW_NO_PHOTO;
        }
        const esp_err_t err = app_settings_set_slideshow_seed(s_perm_seed);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "shuffle seed not persisted: %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "shuffle epoch complete, reseeded to %lu over %u photos",
                 (unsigned long)s_perm_seed, (unsigned)count);
        // The new permutation may open on the picture already displayed, which
        // request_locked() would cancel -- costing the frame a whole interval on a 1-in-count
        // chance. Step past it instead.
        if (count > 1 && s_perm[0] == base) {
            return s_perm[1];
        }
        return s_perm[0];
    }

    if (pos == 0) {
        // `prev` at the head wraps to the tail of the SAME epoch and does NOT reseed: walking
        // backwards must not destroy the order being walked back through.
        return s_perm[count - 1];
    }
    return s_perm[pos - 1];
}

// --------------------------------------------- the catalogue epoch (ticket 37 Phase 3)

// Called with the lock held. Where `local` sits in the current /data listing, or -1.
static int local_index_of(const char *local)
{
    const size_t n = photo_list_count(&s_list);
    for (size_t i = 0; i < n; i++) {
        const char *have = photo_list_at(&s_list, i);
        if (have && strcmp(have, local) == 0) {
            return (int)i;
        }
    }
    return -1;
}

// Called with the lock held. The photo-list index of the photograph at catalogue epoch position
// `pos`, or -1 when it is not on the card (or the position does not resolve at all).
static int at_epoch_pos(uint16_t pos)
{
    if (pos >= s_perm_n) {
        return -1;
    }
    char local[SMB_MANIFEST_NAME_SIZE];
    if (!app_catalog_local(s_perm[pos], local, sizeof(local))) {
        return -1;
    }
    return local_index_of(local);
}

// Called with the lock held. Tells the mirror what is coming: the next SMB_SYNC_WANT_MAX
// catalogue entries at or after `from`, in epoch order, that are NOT already on the card.
//
// Absent ones only, which is the contract app_smb_sync_want() states -- it keeps every filesystem
// probe on this side, where a listing of /data is already in hand, so the mirror's tick can tell
// whether there is work without touching the card.
static void declare_wants_locked(uint16_t from)
{
    uint16_t want[SMB_SYNC_WANT_MAX];
    size_t n = 0;
    for (uint16_t k = 0; k < s_perm_n && n < SMB_SYNC_WANT_MAX; k++) {
        const uint16_t pos = (uint16_t)((from + k) % s_perm_n);
        if (at_epoch_pos(pos) < 0) {
            want[n++] = s_perm[pos];
        }
    }
    app_catalog_want(want, n);
}

// Called with the lock held. One position along the epoch, reseeding when a forward step runs off
// the end -- the same rule as the photo-list epoch, and for the same reason: without a new seed
// every pass would be the same order forever.
static uint16_t epoch_advance_locked(int delta, uint16_t count)
{
    if (delta > 0) {
        if ((uint16_t)(s_epoch_pos + 1) < s_perm_n) {
            return (uint16_t)(s_epoch_pos + 1);
        }
        if (!epoch_build_locked(count, 0, true)) {
            return 0;
        }
        const esp_err_t err = app_settings_set_slideshow_seed(s_perm_seed);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "shuffle seed not persisted: %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "catalogue epoch complete, reseeded to %lu over %u photographs",
                 (unsigned long)s_perm_seed, (unsigned)count);
        return 0;
    }
    // `prev` at the head wraps to the tail of the SAME epoch and does not reseed: walking
    // backwards must not destroy the order being walked back through.
    return s_epoch_pos == 0 ? (uint16_t)(s_perm_n - 1) : (uint16_t)(s_epoch_pos - 1);
}

// Called with the lock held. Returns the photo-list index to draw, or APP_SLIDESHOW_NO_PHOTO when
// the catalogue epoch cannot answer and the caller should fall back.
//
// THE CURSOR HOLDS ITS PLACE WHILE A WANTED PHOTOGRAPH IS IN FLIGHT, and that is the part worth
// not simplifying. The obvious loop -- walk forward to the next entry that IS on the card -- makes
// the cursor race ahead of the fetcher, so every photograph the fetcher brings in has already been
// walked past and is never shown: it would fetch 1,032 photographs to display the same cached
// hundred. Holding the cursor and drawing a STAND-IN instead means the wanted photograph is still
// next when it lands.
static uint16_t catalog_step_locked(int delta, uint16_t count)
{
    if (!epoch_ready_locked(count, true)) {
        return APP_SLIDESHOW_NO_PHOTO;
    }
    if (s_epoch_pos >= s_perm_n) {
        s_epoch_pos = 0; // a catalogue that shrank, or nothing stored yet
    }

    // A miss in progress means the cursor is still waiting on s_epoch_pos, so retry it rather
    // than stepping over the photograph that is being fetched for it.
    const uint16_t target = s_epoch_miss > 0 ? s_epoch_pos : epoch_advance_locked(delta, count);
    declare_wants_locked(target);

    s_epoch_pos = target;
    store_epoch_pos(target);

    const int chosen = at_epoch_pos(target);
    if (chosen >= 0) {
        s_epoch_miss = 0;
        return (uint16_t)chosen;
    }

    s_epoch_miss++;
    if (s_epoch_miss > EPOCH_MISS_MAX) {
        // Given up on: it may have been deleted from the share since the catalogue was refreshed,
        // or fail every fetch. Clearing the counter is what lets the next advance step past it.
        ESP_LOGW(TAG, "epoch position %u never arrived after %u tries; skipping it",
                 (unsigned)target, (unsigned)EPOCH_MISS_MAX);
        s_epoch_miss = 0;
    }

    // The stand-in: the nearest entry ahead that IS on the card, so the frame shows a photograph
    // rather than holding the previous one while the fetch runs. It is drawn out of turn and will
    // be drawn again when the cursor reaches it, which is the cost of not going blank.
    for (uint16_t k = 1; k <= EPOCH_STANDIN_SCAN && k < s_perm_n; k++) {
        const uint16_t pos = (uint16_t)((target + k) % s_perm_n);
        const int idx = at_epoch_pos(pos);
        if (idx >= 0) {
            return (uint16_t)idx;
        }
    }
    return APP_SLIDESHOW_NO_PHOTO;
}

static void display_locked(uint16_t index)
{
    const char *name = photo_list_at(&s_list, index);
    if (!name) {
        return;
    }
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", BOARD_STORAGE_MOUNT, name);
    app_display_request(path);
    // Under the catalogue epoch the position is the CURSOR, not a lookup: the permutation is over
    // the catalogue, so searching it for a photo-list index would find nothing, and a stand-in is
    // being drawn out of turn anyway. `miss` is what says which of the two this line is.
    const int pos = s_perm_n == 0 ? -1
                    : s_perm_catalog ? (int)s_epoch_pos
                                     : epoch_position_locked(index);
    if (pos >= 0 && s_perm_catalog) {
        ESP_LOGI(TAG, "display [%u/%u] catalogue epoch %d/%u seed %lu miss=%u %s",
                 (unsigned)index + 1, (unsigned)photo_list_count(&s_list), pos + 1,
                 (unsigned)s_perm_n, (unsigned long)s_perm_seed, (unsigned)s_epoch_miss, name);
    } else if (pos >= 0) {
        ESP_LOGI(TAG, "display [%u/%u] epoch %d/%u seed %lu %s", (unsigned)index + 1,
                 (unsigned)photo_list_count(&s_list), pos + 1, (unsigned)s_perm_n,
                 (unsigned long)s_perm_seed, name);
    } else {
        ESP_LOGI(TAG, "display [%u/%u] %s", (unsigned)index + 1,
                 (unsigned)photo_list_count(&s_list), name);
    }
}

// FR-5.5: set the pending index and start the settle window. If the settled index is
// what is already displayed, nothing refreshes at all.
static void request_locked(uint16_t index)
{
    s_pending = index;
    if (s_pending == s_current && s_current != APP_SLIDESHOW_NO_PHOTO) {
        s_needs_refresh = false;
        // The interval restarts even though nothing was drawn. Without this the automatic
        // advance retries on every 200 ms tick for as long as the condition holds, because
        // only a real refresh used to move s_last_refresh_ms -- and with a single photo on
        // /data the condition holds forever. Observed 2026-09-04 with one file on the card:
        // "back to the displayed image, refresh cancelled" five times a second, each one
        // preceded by a full board_storage_scan(), which on a microSD takes the panel's SPI
        // bus. The web server stopped answering while it spun.
        s_last_refresh_ms = now_ms();
        ESP_LOGI(TAG, "back to the displayed image, refresh cancelled");
        return;
    }
    s_needs_refresh = true;
    s_settle_start_ms = now_ms();
    store_index(s_pending);
}

esp_err_t app_slideshow_start(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    lock();
    // Before the first advance can consult it. Clamped where it is used, because the catalogue's
    // size is not known here and may not be known for another few runs.
    s_epoch_pos = load_epoch_pos();
    s_epoch_miss = 0;
    const esp_err_t err = rescan_locked();
    const size_t count = photo_list_count(&s_list);

    if (count == 0) {
        // FR-5.7. Not an error: the frame runs, shows nothing, and picks up whatever
        // the web UI uploads next.
        s_current = APP_SLIDESHOW_NO_PHOTO;
        s_pending = APP_SLIDESHOW_NO_PHOTO;
        unlock();
        ESP_LOGW(TAG, "no photos in %s; running with an empty directory", BOARD_STORAGE_MOUNT);
        return err;
    }

    uint16_t stored = load_index();
    if (stored == APP_SLIDESHOW_NO_PHOTO || stored >= count) {
        stored = 0;
    }
    s_pending = stored;
    s_current = APP_SLIDESHOW_NO_PHOTO;  // nothing has been drawn by this boot yet
    s_needs_refresh = false;
    s_last_refresh_ms = now_ms();
    display_locked(stored);
    s_current = stored;
    store_index(stored);
    const char *name = photo_list_at(&s_list, stored);
    unlock();

    printf("# slideshow start index=%u of %u name=%s\n", (unsigned)stored, (unsigned)count,
           name ? name : "(none)");
    return err;
}

static void step(int delta)
{
    lock();
    rescan_locked();
    const uint16_t count = (uint16_t)photo_list_count(&s_list);
    if (count == 0) {
        s_current = APP_SLIDESHOW_NO_PHOTO;
        s_pending = APP_SLIDESHOW_NO_PHOTO;
        s_needs_refresh = false;
        // THE SAME FIX AS THE BRANCH ABOVE, ON THE CASE THAT WAS MISSED. Without this the
        // advance condition in app_slideshow_update() stays true -- only a real refresh moved
        // s_last_refresh_ms -- so an empty /data re-enters here on every 200 ms tick, and each
        // pass runs a full board_storage_scan(). Measured 2026-09-08 with the card pulled: a
        // permanent 5 Hz of `sdmmc_read_sectors_dma ... 0x107`, ten log lines a second, holding
        // SPI2 -- the panel's own bus -- five times a second, forever. An empty /data is a
        // supported state (FR-5.7), so this is not only about a missing card.
        s_last_refresh_ms = now_ms();
        // AN EMPTY /data IS ALSO WHERE AN ON-DEMAND CACHE STARTS, and returning before the
        // catalogue epoch meant nothing was ever declared wanted, so nothing was ever fetched and
        // the frame stayed blank for good with a whole catalogue listed (ticket 81). There is
        // nothing to draw this time either way; the step is taken for its wants, and the fetch's
        // app_slideshow_invalidate() plus the next advance draws what lands.
        const size_t cat = app_settings_slideshow_random() ? app_catalog_count() : 0;
        if (cat > 0) {
            (void)catalog_step_locked(delta, (uint16_t)cat);
        }
        unlock();
        return;
    }

    // The base for the step is the pending index, not the current one, so a held
    // button walks forward while the settle window is still open.
    uint16_t base = s_pending != APP_SLIDESHOW_NO_PHOTO ? s_pending : s_current;
    uint16_t next;

    // FR-5.4's order is filename order, so random is a departure and is off by default. The
    // epoch is consulted first and falls through to that order whenever it cannot answer.
    if (app_settings_slideshow_random()) {
        // WITH ON-DEMAND ON THE DOMAIN IS THE CATALOGUE, which is the whole of ticket 37: the
        // selection is over what the SHARE holds rather than over what happens to be cached. A
        // catalogue of zero -- no share configured, or nothing listed yet -- falls through to the
        // photo-list epoch, which is Phase 2's behaviour and always available.
        //
        // Since ticket 60 the catalogue is app_catalog's: the share's under smb_on_demand, plus
        // the Google Photos album's list, both fetched one photograph at a time.
        const size_t cat = app_catalog_count();
        next = cat > 0 ? catalog_step_locked(delta, (uint16_t)cat)
                       : epoch_step_locked(base, delta, count);
        if (next != APP_SLIDESHOW_NO_PHOTO) {
            request_locked(next);
            unlock();
            return;
        }
        // Two ways here. Under on-demand it is the real and expected one: nothing within
        // EPOCH_STANDIN_SCAN of the cursor is on the card, which is what a cold cache looks like,
        // and filename order over whatever /data does hold is the right answer. Without
        // on-demand it means the permutation could not be built at all -- PSRAM, since count is
        // capped by photo_list and count == 0 was handled above.
        ESP_LOGW(TAG, "the epoch has nothing drawable (%u on the card, %u catalogued); using "
                      "filename order",
                 (unsigned)count, (unsigned)cat);
    } else if (s_perm_n != 0) {
        // The setting was turned off. Drop the epoch so display_locked() stops printing a
        // position that nothing is walking, and so turning it back on regenerates.
        s_perm_n = 0;
    }

    if (base == APP_SLIDESHOW_NO_PHOTO) {
        next = delta > 0 ? 0 : (uint16_t)(count - 1);
    } else if (delta > 0) {
        next = (uint16_t)((base + 1) % count);
    } else {
        next = (uint16_t)((base + count - 1) % count);
    }
    request_locked(next);
    unlock();
}

void app_slideshow_next(void)
{
    step(+1);
}

void app_slideshow_prev(void)
{
    step(-1);
}

void app_slideshow_invalidate(void)
{
    lock();
    s_invalidated = true;
    unlock();
}

esp_err_t app_slideshow_show_name(const char *name)
{
    if (!name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    rescan_locked();
    const size_t count = photo_list_count(&s_list);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(photo_list_at(&s_list, i), name) == 0) {
            s_pending = (uint16_t)i;
            s_current = (uint16_t)i;
            s_needs_refresh = false;
            s_last_refresh_ms = now_ms();
            display_locked((uint16_t)i);
            store_index((uint16_t)i);
            unlock();
            return ESP_OK;
        }
    }
    unlock();
    return ESP_ERR_NOT_FOUND;
}

void app_slideshow_update(void)
{
    if (!s_mutex) {
        return;
    }

    lock();

    if (s_invalidated) {
        s_invalidated = false;
        rescan_locked();
        if (photo_list_count(&s_list) == 0) {
            s_current = APP_SLIDESHOW_NO_PHOTO;
            s_pending = APP_SLIDESHOW_NO_PHOTO;
            s_needs_refresh = false;
        } else if (s_current == APP_SLIDESHOW_NO_PHOTO && !s_needs_refresh &&
                   app_clock_schedule_active() && !app_maint_course_running()) {
            // The first photograph on an empty /data -- a mirror's fetch or copy landing -- is
            // drawn now rather than at the next advance, which is an interval away and an hour at
            // the default (ticket 81 §7). Under the catalogue epoch the step retries the held
            // cursor, so it is the wanted photograph that is drawn. The same two gates as the
            // automatic advance below, so a sync that lands at night does not undo a white park.
            unlock();
            step(+1);
            return;
        }
    }

    const uint32_t now = now_ms();

    // The settle window has closed: draw the pending image, unless the panel is still
    // busy with the previous one -- in which case wait rather than stacking requests.
    if (s_needs_refresh && (now - s_settle_start_ms) >= SETTLE_MS) {
        if (!app_display_busy()) {
            if (s_pending != APP_SLIDESHOW_NO_PHOTO &&
                s_pending < (uint16_t)photo_list_count(&s_list)) {
                display_locked(s_pending);
                s_current = s_pending;
                s_last_refresh_ms = now;
            }
            s_needs_refresh = false;
        }
        unlock();
        return;
    }

    // FR-5.4: automatic advance. Interval 0 would mean manual only; app_settings keeps
    // the value at 1 or more (NFR-6), so the check is on auto_slideshow alone.
    //
    // Ticket 42's hold goes here and NOWHERE ELSE in this file: an automatic advance is the
    // only thing the schedule suppresses. A button press, an API call and a manual
    // app_slideshow_next() all still work while the window is closed, because the frame going
    // unresponsive at 23:00 is a frame the owner cannot fix at 23:01. `s_last_refresh_ms`
    // is deliberately not touched, so the first advance after the window reopens is due
    // immediately rather than one interval later.
    // **And ticket 68's colour course goes here too, which ticket 68 §7 argued against and a
    // measurement overturned.** That section said both halves of the maintenance feature are
    // one-shot actions at window edges and so need no gate here — true of the SCHEDULED course,
    // which runs inside a window that has already suppressed this branch, and not of the manual
    // one. Observed on the E1002, 2026-09-20: `interval_minutes` is 5 there and a back-to-back
    // course is ~5.7 minutes, so an advance landed between the yellow flat and the next white and
    // put a photograph in the middle of a diagnostic sequence. No flat was lost — `course_step()`
    // waits on `app_display_busy()`, so the next one queued behind the photograph rather than being
    // overwritten — but a person watching ten flats to judge the ink saw a photograph, which is the
    // one thing the mode must not show them.
    //
    // Same shape as the hold above and the same limits: it suppresses **only** the automatic
    // advance. A button press, an API call and a manual app_slideshow_next() all still work during a
    // course, because a frame that goes unresponsive for five minutes is worse than a spoiled
    // course.
    if (!s_needs_refresh && app_settings_auto_slideshow() && !app_display_busy() &&
        app_clock_schedule_active() && !app_maint_course_running()) {
        const uint32_t interval_ms = (uint32_t)app_settings_interval_minutes() * 60u * 1000u;
        if (interval_ms > 0 && (now - s_last_refresh_ms) >= interval_ms) {
            unlock();
            app_slideshow_next();
            return;
        }
    }

    unlock();
}

void app_slideshow_get_state(app_slideshow_state_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_mutex) {
        out->current_index = APP_SLIDESHOW_NO_PHOTO;
        out->pending_index = APP_SLIDESHOW_NO_PHOTO;
        return;
    }
    lock();
    out->current_index = s_current;
    out->pending_index = s_pending;
    out->count = photo_list_count(&s_list);
    out->refresh_pending = s_needs_refresh;
    if (s_current != APP_SLIDESHOW_NO_PHOTO && s_current < photo_list_count(&s_list)) {
        const char *name = photo_list_at(&s_list, s_current);
        if (name) {
            strncpy(out->current_name, name, sizeof(out->current_name) - 1);
        }
    }
    if (s_pending != APP_SLIDESHOW_NO_PHOTO && s_pending < photo_list_count(&s_list)) {
        const char *name = photo_list_at(&s_list, s_pending);
        if (name) {
            strncpy(out->pending_name, name, sizeof(out->pending_name) - 1);
        }
    }
    unlock();
}
