// The mirror: photographs from one SMB share into /data, on a timer.
//
// Ticket .scratch/digital-frame/issues/25. board_smb.c is the transport, smb_manifest.c
// is the ownership record and the naming rules; this is the policy between them.
//
// THE SHAPE OF THIS MODULE IS DICTATED BY ONE MEASUREMENT. A live SMB session costs
// ~25.4 KB of internal RAM settled and takes int_min ~62.9 KB below the post-Wi-Fi level
// at its transient peak (ticket 23, re-measured on the async build). PSRAM does not help:
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384 keeps libsmb2's allocations internal, and that
// hypothesis is closed. So a sync cannot run alongside httpd, and this module stops it.
//
// THE IDLE FIGURE THIS USED TO CITE -- "env:frame idles at ~18 KB internal free" -- IS FROM
// TICKET 21 AND IS STALE. The 120 KB panel frame buffer moved to PSRAM on 2026-09-04, and
// env:frame has idled at int_free ~71-74 KB ever since (74,491 / 71,303 / 70,767 across one
// boot's heartbeat, .scratch/captures/frame-dirscan-20260906-172013.log). The conclusion
// above survives anyway, and now for a measured reason rather than that one: a 243-entry
// listing takes int_min to 13,207 B WITH httpd ALREADY STOPPED, against httpd's own 10,240 B
// task stack (docs/agents/measurements.md, 2026-09-06). It is the LISTING that cannot
// coexist; a connect alone was measured to coexist comfortably -- see the connect test below.
//
// Two consequences follow, and both are the design rather than a workaround:
//
//   * IT WORKS IN BOUNDED WINDOWS. A window mirrors at most SMB_SYNC_FILES_PER_WINDOW
//     files or SMB_SYNC_WINDOW_MS, then puts httpd back and sleeps. At the measured
//     84 KB/s (4 KB per SMB2 round trip at ~45 ms) a 2 MB photograph is ~24 s, so an
//     unbounded first sync of 200 files would leave the web UI down for tens of minutes.
//
//   * THE WORKER TASK EXISTS ONLY DURING A WINDOW. A resident task's 8 KB stack would
//     permanently hold nearly half the idle headroom to do nothing between syncs. It is
//     created at the start of a window and deletes itself at the end. This is NOT the
//     "kill the task from outside" idea ticket 25 rejected -- nothing is ever killed
//     while it is inside libsmb2; the task returns on its own, from a frame that is not.
//     An xTaskCreate that fails for want of internal RAM is a reportable "busy, retried
//     on the next tick" rather than a permanent hold.
//
// It also cannot run in app_main's own task: that stack is 3,584 bytes and libsmb2's
// connect path overflowed it into a 184-boot reset loop.

#ifndef APP_SMB_SYNC_H
#define APP_SMB_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_smb_classify.h"
#include "board_storage.h" // BOARD_STORAGE_MOUNT, for SMB_SIDECAR_DIR
#include "epd_geom.h" // EPD_WIDTH / EPD_HEIGHT, for SMB_RESIZE_FIT_EDGE
#include "esp_err.h"

// 4 files or 60 s per window, whichever comes first. Four is roughly one minute of web
// UI downtime at the measured throughput for typical phone photographs, and the deadline
// is what bounds it when they are not typical.
#define SMB_SYNC_FILES_PER_WINDOW 4
#define SMB_SYNC_WINDOW_MS 60000

// Between windows, so the UI is usable and a client has time to be served.
#define SMB_SYNC_WINDOW_GAP_MS 20000

// First run 30 s after boot, then every hour.
#define SMB_SYNC_FIRST_DELAY_MS 30000

// ONE HOUR, NOT SIX, BECAUSE A RUN WITH NOTHING TO DO IS ALREADY CHEAP. window_task()
// connects, build_plan() lists and diffs, and when the plan comes back empty the fetch loop
// does not execute at all -- so an unchanged share costs one connect, one smb2_opendir and
// one stat() per mirrored file, and no transfers and no second window. Six hours had been set
// as though every run were a full mirroring pass, which made the period's visible cost the
// arrival latency of a new photograph: up to six hours for a file already sitting on the
// share.
//
// This is the split the Android side gets by listing in SmbSyncWorker and fetching only a
// prefetch window -- reached here by noticing that the shipping code already separates the
// two and only the period was tuned as if it did not.
//
// THE COST IS FREQUENCY, NOT DEPTH, and it is worth stating because the two are easy to
// conflate. smb2_opendir materialises the whole directory in internal RAM at ~0.5 KB an
// entry (ticket 23 measured 129 entries as most of a ~60 KB transient peak), and that peak
// is the same height whether anything is fetched or not. This change meets it 6x as often
// and does not make it taller. It is also unmeasured above 129 entries, so a share near
// SMB_SYNC_LIST_MAX is where to look if something regresses.
//
// Overridable so that a soak can do in eight hours what the shipping period would take days
// to do. A leak of a few hundred bytes per sync is invisible across one run and obvious
// across fifty, and a soak that cannot produce the failing answer is not evidence
// (ticket 18). The code is identical either way -- only this constant differs -- and a pass
// at ten minutes implies the one-hour case rather than substituting for it. Never set in a
// committed build_flags.
#ifndef SMB_SYNC_PERIOD_MS
#define SMB_SYNC_PERIOD_MS (60 * 60 * 1000)
#endif

// A window never starts while someone is using the web UI: pulling httpd out from under
// a request is a broken page, and the mirror is never in that much of a hurry.
#define SMB_SYNC_QUIET_MS 30000

// ...and how long a scheduled run will defer to that before going regardless (ticket 28).
//
// Without a deadline the gate could never open on a frame anybody was standing near, and
// the reason is structural rather than bad luck: FR-2.2's captive portal answers every
// DNS query with the softAP's address precisely so that an associated client probes it,
// and the client's operating system then fetches GET / on a timer to decide whether the
// network is captive. Measured 2026-09-05 with one client on the AP -- a successful
// request every ~30 s against this 30 s window, forever. With nothing associated, no
// request arrived at all in 200 s.
//
// Ten minutes: long enough that a page genuinely in use has almost always finished, short
// enough that a phone sitting on the access point doing nothing but connectivity probes
// cannot starve the mirror indefinitely.
//
// IT IS A MUCH LARGER FRACTION OF THE PERIOD THAN IT WAS. Against six hours this deadline
// was 2.8 % and invisible; against one hour it is 17 %, so a client that is always active
// stretches the effective period to as much as 70 minutes. That is still hourly enough for
// what the period is for -- the arrival latency of a new photograph -- and it is left at ten
// minutes deliberately rather than scaled down with the period, because what the number has
// to cover is the length of a human interaction with the page, which did not change.
#define SMB_SYNC_QUIET_DEADLINE_MS (10 * 60 * 1000)

// 200, not PHOTO_LIST_MAX. smb2_opendir() materialises the whole directory before the
// first readdir returns, in INTERNAL RAM, and libsmb2's API offers no batched listing.
//
// THE PER-ENTRY COST IS 142.5 BYTES, MEASURED 2026-09-06, and it is a straight line through
// the origin: 129 dirents draw 18,252 B and 1,080 draw 153,784 B, two sizes 8.4x apart each
// predicting the other to 0.6 % (21 samples and 1; docs/agents/measurements.md). The "roughly
// 0.5 KB per entry" this comment used to quote was 3.6x too high -- it was one listing's share
// of a whole-window dip, never a slope.
//
// **AND THIS CAP DOES NOT CONTROL THAT COST.** The allocation is a function of what the
// DIRECTORY holds, not of `max`: at 142.5 B an entry the share root's 1,080 dirents cost
// 153,784 B whatever this number is, which is more than twice env:frame's entire internal free
// heap. So the cap bounds the plan, the fetches and the manifest -- worth having -- and bounds
// nothing about the listing's RAM. What env:frame can actually list is about 315-420 entries.
// Ticket 37 is where that leaves the idea of indexing a whole share.
//
// RAISED FROM 200 TO 500 ON 2026-09-06, and the measurement above is the whole justification:
// what this number buys is PSRAM (500 x sizeof(board_smb_entry_t) = 136 KB of 7.5 MB free) and
// it buys nothing in internal RAM, so the 200 was paying a cost in coverage for a saving that
// was never real. The bench share's largest folder holds 245 entries, which now fits with room
// rather than truncating at 200 and hiding 30 photographs.
#define SMB_SYNC_LIST_MAX 500

// Bounds the PSRAM buffer and stops one oversized image from stalling everything behind
// it. ~48 s at the measured 84 KB/s.
#define SMB_SYNC_FILE_MAX (4 * 1024 * 1024)

// The connect test (POST /api/smb/test). QUEUED survives until a run in progress finishes,
// because a test and a sync cannot share the single libsmb2 session; REFUSED means the
// pre-flight internal-RAM floor below was not met, which is a different answer from any SMB
// error and must not be reported as one.
typedef enum {
    APP_SMB_TEST_IDLE = 0,
    APP_SMB_TEST_QUEUED,
    APP_SMB_TEST_RUNNING,
    APP_SMB_TEST_DONE,
    APP_SMB_TEST_REFUSED,
} app_smb_test_state_t;

// The pre-flight floor for starting a connect test WITHOUT stopping httpd. CHOSEN, NOT
// MEASURED, and the arithmetic it is chosen from: a settled session costs ~13.8 KB of
// internal RAM on this build (ticket 23 measured ~21.8 KB of which 16 KB was the task stack;
// SYNC_TASK_STACK is 8192 here), and env:frame idles at int_free ~71 KB with dma_largest
// ~31 KB (ticket 33). 40 KB leaves ~26 KB over the settled need and 16 KB is half the idle
// dma_largest. They exist so that the experiment this test IS -- whether an SMB session can
// coexist with httpd at all -- cannot be run at the moment it is least likely to survive.
// Replace them with measurements once the test has produced some.
#define SMB_SYNC_TEST_MIN_INT_FREE 40960
#define SMB_SYNC_TEST_MIN_DMA_LARGEST 16384

// ----------------------------------------------------- on demand (ticket 37 Phase 3)
//
// WITH `smb_on_demand` ON, /data STOPS BEING A MIRROR AND BECOMES A CACHE OF WHAT IS BEING
// SHOWN. Three things move:
//
//   * The hourly run no longer lists element 0 of smb_path and fetches all of it. It only
//     refreshes one folder of the catalogue, which is what it was already doing at the end of
//     every run -- so the expensive half of a run goes away and the cheap half stays.
//   * Photographs are fetched because the selector asked for them. app_smb_sync_want() is that
//     ask, and a window with a non-empty want list does NO LISTING at all: it connects, reads
//     the named files by their full share path, and goes. A connect costs 156 ms and ~13.7 KB
//     of internal RAM; a listing over a 243-entry folder bottoms out at int_min 13,207 B with
//     httpd already stopped, which is why the two are separated at all.
//   * The cache is bounded at SMB_SYNC_CACHE_FILES **and by the volume's own free space**
//     (ticket 54), and older entries are evicted. The two bounds are checked in different
//     places for different reasons -- see SMB_SYNC_CACHE_FILES and cache_has_room().
//
// WHY THIS DOES NOT CONTRADICT THE "THE MIRROR NEVER EVICTS" RULE, which tickets 25 and 27 both
// state and which a reader is right to stop at. That rule was about a MIRROR: /data was the
// library, so a photograph leaving it was a photograph the user lost, and "a photograph
// vanishing from a frame is indistinguishable from a fault". Under on-demand /data is not the
// library -- the SHARE is, and the catalogue is the record of it. An evicted photograph is still
// on the share, is still selected in its turn, and comes back when it is drawn. The user-visible
// collection goes from 245 to 1,032 by evicting; the rule and this are the same intent under a
// changed meaning of /data, which is exactly why it is a setting and why off is the old
// behaviour byte for byte.
//
// WHAT IS GIVEN UP, said here rather than discovered: a photograph deleted from the share is no
// longer deleted from the card by reconciliation, because there is no listing to reconcile
// against. It leaves the CATALOGUE at that folder's next refresh, so it stops being selected,
// and eviction removes it in its turn. So the invariant weakens from "gone within an hour" to
// "stops being shown within a catalogue cycle, and leaves the card within a cache cycle".

// How many photographs ahead the selector declares. At a five-minute interval six is half an
// hour of lead against a fetch measured in seconds, and the queue exists to absorb a stall's
// ~3 s reconnect-and-resume rather than to run deep.
//
// IT IS 1 AS AN ARM OF TICKET 44, MEASURED AT 6 AND AT 2 FIRST, and the argument is one line of
// arithmetic: the selector declares this many ABSENT entries per advance and the cursor consumes
// ONE, so anything above 1 makes the fetch horizon grow without bound until eviction stops it.
// At 6 that horizon settled ~60 positions out and 85.2 % of everything fetched was evicted before
// it was drawn; at 2 it grew by one per advance (measured: gap 10 at cursor 10, 28 at 28) and was
// projected to reach the 100-file cache at ~8.3 h. At 1 declaration matches consumption.
//
// WHAT IS TRADED AWAY is the queue's original purpose -- absorbing a stall's ~3 s
// reconnect-and-resume, and a window that does not open. Windows run every ~2.4 min against an
// advance every 5, so there is one spare window per advance; `miss=` is the observable and it was
// 0 of 29 advances at 2 with the catalogue renumbering fixed (ticket 45).
#define SMB_SYNC_WANT_MAX 1

// The cache's ceiling in files. It sits under SMB_MANIFEST_MAX and PHOTO_LIST_MAX both, so
// neither 500-entry cap is approached and photo_list.h's ported rule needs no product argument.
//
// **THE "~2 MB A PHOTOGRAPH, SO THE COUNT IS THE BINDING CONTROL" ARGUMENT THAT USED TO BE HERE
// IS RETRACTED.** It was never measured: read from the device on 2026-09-09, the card's 61
// photographs have a MEDIAN of 42,090 B and a mean of 108,882 B, so the figure was wrong by
// ~50x. And the conclusion it supported was wrong in a way that mattered -- with no card fitted
// /data is the 6 MB internal partition (partitions.csv:25), where 100 photographs is the same
// order as the whole volume rather than a rounding error against 1.82 GB. Ticket 40 found it by
// reading; ticket 54 is the bound that answers it, and the free-space rule in cache_has_room()
// is what actually binds on a small volume.
//
// This count is deliberately MEDIUM-BLIND. One mechanism that measures the volume beats two that
// can disagree about it, and a count derived from free space would be a second estimate of the
// same quantity.
#define SMB_SYNC_CACHE_FILES 100

// The floor the SPACE-driven eviction stops at, and only that one -- the count-driven eviction
// never needed a floor because it ran only above the ceiling. Without it a volume that is full
// for reasons the cache did not cause (uploads, the four factory PNGs) would be answered by
// emptying the cache to nothing, and the frame would show one photograph while writing
// continuously. Below this the window reports `capped` instead: a state to look at rather than a
// loop to run. Two windows' worth of fetches, so the slideshow does not oscillate between two
// pictures while the volume is under pressure.
#define SMB_SYNC_CACHE_MIN_FILES 8

// Bounds one window's eviction work. Turning the setting on for the first time finds 245 files
// where 100 are wanted, and doing 145 unlinks in one window would hold the storage lock -- which
// on a microSD is SPI2, which the panel is on -- for far longer than any fetch does. It drains
// over the next few windows instead.
#define SMB_SYNC_EVICT_PER_WINDOW 8

// ------------------------------------------------------- import-time resize (ticket 49)
//
// With app_settings_smb_resize() on, a fetched photograph is decoded once and stored at the
// size the panel draws it, and its thumbnail is written beside it from the same decode.

// The fit target, and it is a SQUARE of the panel's long edge rather than 400x600 on
// purpose. `orientation` is a live setting: the canvas is 400x600 logical in portrait and
// 600x400 in landscape, and a photograph stored to fit one is too small for the other. The
// fit into a square of side max(w, h) is at least as large as the fit into either rectangle,
// so storing to it covers both and costs nothing at all for a source taller than it is wide
// -- the bench share's 768x1344 comes out at the same factor 2 either way.
//
// AND IT IS max(), NOT EPD_HEIGHT, WHICH IS WHAT THIS SAID UNTIL 2026-09-17. The two agree on
// a portrait-native panel and disagree on a landscape-native one: on the ED2208-GCA
// EPD_HEIGHT is 480, the SHORT edge, so every mirrored photograph was stored too small for the
// 800 px axis and then upscaled onto it. A no-op on the EL040EF1 (600 either way), so no figure
// measured there goes stale; ticket 63's E1002 resize figures were taken at 480 and are
// superseded. It also raises that board's resize PEAK -- a bigger box means a bigger output
// buffer, which is the shape of the failure ticket 56 was -- and no resize on that board has been
// measured at 800. Watch `decode=`/`encode=` and the heap line on its next mirror window.
#define SMB_RESIZE_FIT_EDGE (EPD_WIDTH > EPD_HEIGHT ? EPD_WIDTH : EPD_HEIGHT)

// Higher than the thumbnail's 65: this is the photograph, it is re-encoded once and then
// looked at for fifteen seconds at a time on a six-colour panel. It is not a measured
// number -- it is a judgement, and the thing to do with it is look at the glass.
#define SMB_RESIZE_QUALITY 80

// A PNG under this is stored untouched even when it is larger than the panel. JPEG at 4:2:0
// visibly damages flat art -- exactly what epd_classify calls `flatIllustration` -- and a
// small screenshot or diagram is not where the storage argument is won. Over it, the file is
// big enough that the argument outweighs the ringing.
#define SMB_RESIZE_PNG_MIN_BYTES (1024u * 1024u)

// Where both sidecars below live: `/data/a.jpg` has `/data/.thumbs/a.jpg.thm` and `.mta`. Moved
// out of the root on 2026-09-23 at the operator's request, so the card reads as photographs when
// it is opened on a PC. It also keeps them out of a FAT16 root's fixed 512 entries, which filled
// at 73 photographs when every photograph cost three names there (docs/agents/defect-log.md).
// Nothing that lists /data sees the folder: board_storage_scan() and local_set_build() both skip
// DT_DIR. Build every sidecar path with app_smb_sync_sidecar_path(), never by hand.
#define SMB_SIDECAR_DIR BOARD_STORAGE_MOUNT "/.thumbs"

// The thumbnail sidecar's suffix, appended to the whole local name so `a.jpg` gets
// `a.jpg.thm`.
//
// NOT `.jpg`, and that is the point rather than a preference: photo_list_is_image() accepts
// four image extensions and lists everything under /data that matches, so a thumbnail named
// like an image would appear in the photo grid AND be shown by the slideshow. This is the
// half of ticket 40's ruling against a /data thumbnail cache that a suffix answers; the other
// half -- that a sidecar sits outside .smbidx while FIFO eviction runs -- is answered by
// delete_local() unlinking it with the photograph.
#define SMB_THUMB_SUFFIX ".thm"

// The photograph's own metadata, derived at import: the city its GPS tag names and its capture
// date, for the matte band (ticket 64). Same lifecycle as the thumbnail above -- written by
// store_photo(), cleared before any re-fetch, and unlinked with the photograph by delete_local()
// and h_photos_delete().
//
// **Import is the only moment this can be read.** store_photo() re-encodes through esp_new_jpeg
// whenever smb_resize is on and the source is larger than the box, and the re-encode carries no
// EXIF at all -- so a photograph on the card usually has none and a draw-time read would find
// nothing for most of a library (`docs/agents/smb-mirror.md`).
//
// **What it costs, stated because it is not free**: a directory entry per photograph in
// SMB_SIDECAR_DIR (in /data's root, doubling what ticket 36's one-readdir scan walked, until
// 2026-09-23), and a FATFS cluster of slack for ~25 bytes of text. Only written when there IS something to write, so a source with no EXIF adds
// no file. If the entry count ever bites, the alternative is a catalogue field -- which was
// considered and declined (it would be a .smbidx format change, and ticket 45 owns that).
#define SMB_META_SUFFIX ".mta"

// Writes `SMB_SIDECAR_DIR/<base><suffix>` into `out`, where `base` is `name` after its last '/':
// a bare local name and a full /data path give the same answer.
void app_smb_sync_sidecar_path(char *out, size_t size, const char *name, const char *suffix);

// Creates SMB_SIDECAR_DIR and moves any sidecar still in /data's root into it -- written by
// firmware before 2026-09-23 -- then removes `.part` files an interrupted write left inside it.
// Every boot, before the slideshow's first draw reads a `.mta`. Never fatal: with no folder the
// sidecar writes fail, which is a slow /thumb/ and a band with no caption.
//
// `sweep_orphans` also unlinks every sidecar whose photograph is gone (deleted on a PC, where the
// folder is out of sight). ONLY at boot, before anything can be storing a photograph: see
// sweep_orphan_sidecars() for why a sweep beside a running mirror would delete a fresh `.mta`.
void app_smb_sync_sidecars_prepare(bool sweep_orphans);

// Stores a photograph fetched by ANOTHER source -- the Google Photos mirror (ticket 60) -- exactly
// the way this module stores its own: a .part file renamed into place, re-encoded at the panel's
// size and given a thumbnail sidecar when app_settings_smb_resize() is on. Exposed rather than
// copied, so the two sources cannot come to disagree about what a stored photograph is.
esp_err_t app_smb_sync_store_photo(const char *local, const uint8_t *buf, size_t len);

// Unlinks a photograph and its thumbnail sidecar. For the same caller, for the same reason.
void app_smb_sync_delete_photo(const char *local);

// How many photographs the catalogue holds: what there is to CHOOSE from, as against
// files_mirrored, which is what is on the card.
size_t app_smb_sync_catalog_count(void);

// The local file name that catalogue entry `i` has, or would have, on /data. False when `i` is
// out of range, when there is no catalogue, or when the entry has no usable local name.
//
// The selector needs this and not the share path: what it has to answer is "can I draw this
// now", and that is a question about /data.
bool app_smb_sync_catalog_local(size_t i, char *local, size_t local_size);

// The selector declares the catalogue entries it wants next, NEAREST FIRST, and the contract is
// that they are entries it has already determined are NOT on the card. That is deliberate: it
// keeps every filesystem probe on the side that already holds a listing of /data, so this call
// and the tick that acts on it need no I/O to know whether there is work.
//
// Never blocks. Replaces the previous list wholesale -- a stale want is worse than none, because
// the selection has moved on.
void app_smb_sync_want(const uint16_t *index, size_t n);

typedef struct {
    bool enabled;
    bool syncing;
    uint16_t files_total;    // in the current run's plan
    uint16_t files_done;     // of that plan, this run
    uint16_t files_mirrored; // records in the manifest
    uint64_t bytes_mirrored;
    uint32_t last_sync_ms; // since the last completed run, UINT32_MAX before the first
    board_smb_err_t last_error;
    // A cap was reached, so the run stopped early. It is NOT an error and it never
    // evicts: a photograph vanishing from a frame is, from where the user is standing,
    // indistinguishable from a fault.
    bool capped;
    // The share holds more photographs than one listing can carry, so the mirror is
    // working from a window onto it. Not an error either -- but the page has to be able
    // to say so, because the alternative is a user whose older photographs never appear
    // and nothing anywhere mentioning it (ticket 27).
    bool listing_truncated;
    char last_file[64];
    uint32_t stack_free; // high-water mark of the last window's task, bytes

    // The share catalogue (ticket 37): how many photographs EXIST to choose from, as against
    // files_mirrored, which is how many are on the card. Reported separately because the whole
    // point of the catalogue is that the two numbers stop being the same -- a frame showing 200
    // of 1,032 is working correctly, and there was previously no way for the page to say so.
    uint16_t catalog_count;
    uint8_t catalog_folders; // non-empty folders named by smb_path
    // Either the catalogue is full (SMB_CATALOG_MAX) or a folder held more photographs than one
    // listing could carry. Not an error, and the same standing as listing_truncated: the page
    // has to be able to say the collection is bigger than the record of it.
    bool catalog_truncated;

    // On-demand (Phase 3). `on_demand` is the setting as the module last read it, so the page can
    // say which of two quite different behaviours it is looking at; `want_pending` is how many
    // photographs the selector has asked for and not yet been given, which is the number that
    // says whether the fetcher is keeping up.
    bool on_demand;
    uint8_t want_pending;
    // Evictions since boot. Not a rate and not an error. It was put here to distinguish "the
    // cache is working" from "the cache is thrashing" and IT CANNOT: ticket 44 measured 202
    // evictions against 204 fetches of 90 distinct photographs, so a thrashing cache and a
    // healthy one both report eviction 1:1 with fetching. The pair below is what says it.
    uint16_t evicted;

    // Successful on-demand fetches since boot, and how many of them were of a catalogue INDEX
    // not fetched before. THE RAW PAIR, deliberately: the ratio, and any verdict drawn from it,
    // are the reader's to compute.
    //
    // fetch_distinct IS NOT A COUNT OF DISTINCT PHOTOGRAPHS and the difference is not small.
    // The hourly catalogue refresh renumbers the share -- smb_catalog_drop_folder() compacts and
    // the refreshed folder is appended, so 120-185 records move per refresh even when nothing on
    // the share changed -- so an index fetched before a refresh names a different photograph
    // after it, and this counts that as a repeat. Measured 2026-09-08: 28 of 28 index repeats
    // resolved to DIFFERENT paths, and by path the same run had no duplicate fetches at all.
    // It is a lower bound on distinct photographs and drifts further under the longer the frame
    // runs; ticket 45 has what a path-keyed count would take. READ THE PATHS IN THE LOG when the
    // question is how much transfer was wasted -- and note that catalog_count staying put says
    // nothing, because a refresh reorders without changing the total.
    uint16_t fetch_total;
    uint16_t fetch_distinct;

    // Ticket 27: photographs on /data that the manifest does not own, and whether the walk that
    // counted them saw the whole directory.
    //
    // IT IS NOT A COUNT OF ORPHANS, and the difference is the whole reason this comment exists.
    // smb_manifest_local_name() keeps a share leaf name VERBATIM whenever it is FAT-safe printable
    // ASCII, so a mirrored photograph is in general indistinguishable BY NAME from a Web-UI upload
    // -- which is also why the obvious fix for the leak, "delete what the manifest does not own",
    // is unsafe rather than merely expensive. So this counts the four factory images, every upload
    // and every orphan alike. It is A BASELINE PLUS THE LEAK.
    //
    // THE READING IS THE NUMBER MOVING, NOT THE NUMBER. It changes by design when a photograph is
    // uploaded or deleted, and it changes by defect when a power cut lands between a fetch and the
    // end-of-window manifest store -- at most SMB_SYNC_FILES_PER_WINDOW files in mirror mode and
    // SMB_SYNC_WANT_MAX on demand. A step up with no upload behind it is the thing to look at.
    // Nobody has yet observed that leak outside the 2026-09-05 accident that found it, and the
    // first thing worth knowing is the rate, which is what this is for.
    //
    // It also matters because since ticket 54 an orphan is not merely inert: evict_to_cache_limit()
    // walks the MANIFEST, so an orphan is never a candidate for eviction and permanently displaces
    // a cached photograph. Accumulated orphans present as `capped` on a card with room, which
    // reads as a full volume and not as a leak.
    //
    // unowned_exact is false when the directory walk could not see everything -- over
    // PHOTO_LIST_MAX entries, a name too long for the set, or the scan failing outright -- and then
    // unowned_files is a LOWER BOUND. Do not compare two readings across a change in this flag.
    uint16_t unowned_files;
    bool unowned_exact;

    // The connect test, reported separately from the mirror's own last_error: a test that
    // failed says nothing about whether the mirror is working, and a mirror that failed
    // says nothing about the settings currently in the form.
    app_smb_test_state_t test_state;
    board_smb_err_t test_error;
    uint32_t test_ms; // wall clock of the last completed test
} app_smb_sync_status_t;

// Reads the manifest off /data. Safe to call before the network is up.
esp_err_t app_smb_sync_init(void);

// Decides whether to open a window, and spawns the worker if so. Never blocks; call it
// from an existing loop. A tick while a window is running does nothing.
void app_smb_sync_tick(void);

// /data changed medium: reload the manifest and the catalogue from the volume that is there
// now. Both are FILES ON THE VOLUME (.smbidx and .smbdir), so after a card is pulled or put
// back the in-RAM copies describe the wrong one -- and under smb_on_demand the manifest is what
// eviction unlinks by. Not app_smb_sync_init(): that also creates the mutex and the arenas.
//
// Called from the FR-4.2 media poll (frame_main.c). A change arriving mid-window is deferred to
// the next tick rather than applied under the worker task.
void app_smb_sync_media_changed(void);

// Asks for a run at the next tick. The manual trigger is not a convenience: without it
// there is no way to tell "the share has no new photographs" from "the mirror is
// broken", which is exactly the ambiguity that cost the WSL side a logcat session.
void app_smb_sync_request(void);

// Asks for a connect test at the next tick: connect, tree-connect, disconnect. No listing,
// so it does NOT pay smb2_opendir's ~0.5 KB an entry -- which is the whole point, and is why
// this is the one SMB operation cheap enough to attempt while httpd is up.
//
// IT DELIBERATELY DOES NOT STOP httpd, and that makes it the smallest safe form of an open
// question: docs/agents/defect-log.md records that whether a sync still needs to stop httpd is
// an unresolved measurement rather than a closed decision. Testing it here costs a
// connect, cannot corrupt the mirror, has no listing peak, and refuses outright below the
// floors above. A window is the wrong place to ask it for the first time.
//
// It also works while the mirror is DISABLED, because the reason to test is to find out
// whether the settings are right before turning it on.
void app_smb_sync_test_request(void);

void app_smb_sync_get_status(app_smb_sync_status_t *out);

#endif // APP_SMB_SYNC_H
