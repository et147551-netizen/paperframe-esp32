// The catalogue of what is ON THE SHARE, as opposed to what is on the card.
//
// Ticket .scratch/digital-frame/issues/37. smb_manifest.c is the ownership record for /data
// and answers "did we put this file there"; this answers "what photographs exist to choose
// from". They are deliberately two files and two modules, because they have different
// lifetimes and different failure consequences: losing the manifest orphans files on the
// card, losing the catalogue costs one re-listing.
//
// WHY IT EXISTS AT ALL. Until 2026-09-06 the frame could show at most SMB_SYNC_LIST_MAX = 200
// photographs of a share, forever the same 200, because one listing was the whole of what it
// knew. The share on this bench holds ~2,100. This module is the record that outlives a single
// listing so that selection can be over the collection rather than over one window onto it.
//
// WHY IT IS BUILT ONE FOLDER AT A TIME, which is the part that looks arbitrary and is not.
// smb2_opendir() materialises an entire directory in internal RAM before the first
// smb2_readdir() returns, at a MEASURED 142.5 bytes per directory entry -- linear, no
// intercept, three points across two envs on one line (docs/agents/measurements.md,
// 2026-09-06). env:frame idles at ~71-74 KB internal free, so it can list roughly 315-420
// entries and no more; the share root's 1,080 entries would need 153,784 B and are simply out
// of reach. libsmb2 offers no batched listing and no allocator hook, so the cap cannot be
// imposed from outside -- but a DIRECTORY BOUNDARY imposes it for free. Each folder here is
// 123-245 entries; 202606's 34,868 B is measured. The share's own structure is the paging.
//
// So a catalogue is accumulated across several sync windows, one folder per window, and
// smb_catalog_drop_folder() is what makes a re-listing replace a folder rather than duplicate
// it.

#ifndef SMB_CATALOG_H
#define SMB_CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Room for the bench share's ~2,100 twice over. The records live in PSRAM, where 7.5 MB is
// free at all times, and the serialised form is ~64 bytes a line -- 4096 entries is ~262 KB
// on the card, against 1.8 GB free. Neither is the binding constraint; the listing's internal
// RAM is, and that is per FOLDER rather than per catalogue.
#define SMB_CATALOG_MAX 4096

// Matches SMB_MANIFEST_PATH_SIZE, and for the same reason: this holds the full path inside
// the share, which is what the manifest keys on and what the name hash covers. Not #included
// from there -- both files must stay linkable under env:native.
#define SMB_CATALOG_PATH_SIZE 400

// Inside BOARD_STORAGE_MOUNT. Not one of photo_list's four extensions, so it never appears in
// the slideshow -- the same rule .smbidx relies on.
#define SMB_CATALOG_FILE ".smbdir"

typedef struct {
    uint32_t path_off; // into the arena
    uint64_t size;
    uint64_t mtime;
} smb_catalog_rec_t;

// Names are packed into a caller-supplied arena, following photo_list_t and smb_manifest_t:
// the module allocates nothing, so a host test hands it a static buffer and the device hands
// it PSRAM.
typedef struct {
    char *arena;
    size_t arena_size;
    size_t arena_used;
    smb_catalog_rec_t rec[SMB_CATALOG_MAX];
    size_t count;
    bool truncated;  // hit SMB_CATALOG_MAX
    bool arena_full; // ran out of path storage

    // STATE THAT HAS TO SURVIVE A POWER CUT, AND IT LIVES HERE BECAUSE THERE IS NOWHERE ELSE.
    // The board's persistent state store is FOUR BYTES (board.h's BOARD_STATE_BYTES) and the
    // slideshow's own index already holds two of them, so the six bytes this needs do not fit;
    // NVS would mean a flash write per photograph, which at a five-minute interval is ~288 a
    // day. The catalogue file is already rewritten once per run, so carrying these in its
    // header costs nothing.
    //
    // That four is the RX8130's RTC RAM on the M5Paper Color and an NVS budget on a board
    // without any, but the arithmetic above does not depend on which: what it depends on is
    // that the store is small and that writing it is not free.
    //
    // folder_cursor: which folder of smb_path's list is catalogued next. Without persisting it,
    // a frame that restarts more often than once per (number of folders) runs never advances
    // past folder 0 -- seen twice on 2026-09-06, once in a test and once when the operator
    // rebooted the board.
    //
    // seed: the shuffle epoch's PRNG seed (see smb_catalog_shuffle). UNUSED UNTIL PHASE 2 and
    // carried now anyway, deliberately: the field costs four bytes, while adding it later would
    // mean a second format version and a second discarded catalogue.
    uint32_t folder_cursor;
    uint32_t seed;
} smb_catalog_t;

bool smb_catalog_init(smb_catalog_t *c, char *arena, size_t arena_size);
void smb_catalog_reset(smb_catalog_t *c);

// `path` is the full path inside the share, directory included, "/"-separated. Refuses a
// duplicate path rather than storing it twice: a folder listed twice without an intervening
// smb_catalog_drop_folder() would otherwise weight those photographs double in the shuffle,
// which is a bug that shows up as "some pictures come round more often" and would be very
// hard to see.
bool smb_catalog_add(smb_catalog_t *c, const char *path, uint64_t size, uint64_t mtime);

size_t smb_catalog_count(const smb_catalog_t *c);
const char *smb_catalog_path(const smb_catalog_t *c, size_t i);
uint64_t smb_catalog_size(const smb_catalog_t *c, size_t i);
uint64_t smb_catalog_mtime(const smb_catalog_t *c, size_t i);
int smb_catalog_find(const smb_catalog_t *c, const char *path);

// A record's folder is everything before its LAST '/', or "" when it has none. Exact match,
// not a prefix test: a prefix test would make dropping "2026" also drop "202606", and the
// bench share has folders whose names are prefixes of each other's. Returns how many records
// went.
//
// The arena is NOT compacted -- dropped paths keep their bytes until the whole catalogue is
// rebuilt. That is deliberate: compacting means rewriting every offset, and the arena is
// PSRAM where the waste is irrelevant. `arena_full` is the signal that a rebuild is due.
//
// `first_out`, when not NULL, receives the index the folder's first record occupied, so the
// caller can put the re-listed folder BACK THERE with smb_catalog_restore_at(). It is set to
// the record count when the folder had no records.
size_t smb_catalog_drop_folder(smb_catalog_t *c, const char *folder, size_t *first_out);

// Drops every record whose folder is not named by `list`, keeping the survivors' order. Returns
// how many went.
//
// WHY THIS EXISTS. smb_catalog_drop_folder() is only ever called for a folder the round-robin
// actually lists, so a folder REMOVED from smb_path is never listed again and its records stay
// for good. Found in ticket 51, which pointed the mirror at one folder for an arm and put the
// six real ones back: `catalog_count` stayed 1,040 rather than returning to 1,032, and the eight
// dead entries are then selected like any other, fail to fetch, and cost EPOCH_MISS_MAX advances
// each before the cursor steps past. Self-correcting within a pass and permanent across reboots.
//
// The obvious workarounds do not work and are worth not re-deriving: recreating the folder empty
// and listing it once would drop its records -- an empty listing is a success with zero files --
// but POST /api/smb/sync does not refresh the catalogue, and rebooting with a shortened list
// would drop the strays and leave the catalogue at ZERO, since one folder is listed per run.
//
// An EMPTY list drops nothing. See the implementation for why that is not laziness.
size_t smb_catalog_keep_folders(smb_catalog_t *c, const char *list);

// Moves the records at [from, count) so they begin at `at`, keeping the order of both blocks.
// A three-reversal rotate, so it allocates nothing and does not touch the arena -- a record's
// path_off does not depend on where the record sits.
//
// WHY THIS EXISTS. Re-listing a folder used to drop its records from the middle and append the
// fresh ones at the end, which RENUMBERED every record after that folder -- 120 to 230 of them
// per refresh on the bench share, hourly, even when nothing on the share had changed. Three
// things hold catalogue indices across a refresh (the epoch permutation, the want list, and
// ticket 44's counter) and all three then mean something different. Modelled cost before this:
// a 3.6-day epoch pass showed 66.5 % of the share and a third of it never; the measured cost was
// that a photograph fetched within ~20 minutes of a refresh was NEVER drawn. Tickets 44 and 45,
// and `smb_catalog_add()`'s own comment had already named the hazard: "reordering would silently
// change which photographs a stored cursor lands on".
void smb_catalog_restore_at(smb_catalog_t *c, size_t from, size_t at);

// Writes "#smbdir 2 <folder_cursor> <seed>\n" then one tab-separated record per line. Returns
// the byte count, or 0 if `out` was too small -- there is no partial write, because a
// half-written catalogue that parses is worse than none. Tab is illegal in a Windows and a FAT
// filename, so no field needs escaping.
size_t smb_catalog_serialise(const smb_catalog_t *c, char *out, size_t out_size);

// Replaces the contents of `c`. `text` need not be NUL-terminated. Any malformation fails the
// whole parse and leaves `c` empty: one code path for a damaged catalogue instead of a repair
// routine nobody exercises, and the recovery is a re-listing, which is cheap.
//
// VERSION 1 IS REJECTED RATHER THAN MIGRATED, and that is the same decision: a device upgrading
// across this change re-lists its folders over the next few runs. Accepting both would put a
// second parse path in the tree forever for one upgrade.
bool smb_catalog_parse(smb_catalog_t *c, const char *text, size_t len);

// ------------------------------------------------------------------- the shuffle epoch
//
// Selection is a PERMUTATION WALKED WITH A CURSOR, not an independent draw per advance.
// esp_random() % n revisits and starves: over 2,100 photographs the operator would see
// repeats long before they saw variety, which is the complaint this whole ticket exists to
// answer, arriving in a new form. A permutation shows every photograph exactly once per pass.
//
// It is regenerated from a 32-bit seed with a fixed PRNG rather than stored, so the state that
// has to survive a power cut is a seed plus a cursor -- six bytes, not a permutation of
// several thousand. The PRNG is xorshift32 and is part of the contract: change it and every
// frame in the field restarts its epoch in a different order, which is harmless, but a test
// pinning a known sequence would also break, which is the point of pinning it.
//
// `perm` must have room for `n` entries. n == 0 writes nothing.
void smb_catalog_shuffle(size_t n, uint32_t seed, uint16_t *perm);

// ------------------------------------------------------------------- the folder list
//
// `smb_path` becomes a COMMA-SEPARATED LIST of folders rather than one folder, because the
// frame cannot discover folder names for itself on a share whose root is too big to list --
// which is the share this exists for. A single folder is a one-item list, so every device
// already configured keeps working and no NVS key is added.
//
// Element 0 has a second job: it is the folder the mirror itself works in, so the existing
// single-folder behaviour is exactly "the list has one element".
//
// Splitting lives here rather than in app_smb_sync.c so it can be tested on the host: a
// user-typed list has more edge cases than it looks -- surrounding spaces, a trailing comma,
// an empty element, a leading or trailing '/' that would double a separator in a path.

// How many non-empty folders `list` names. An empty or NULL list is 0, and a caller should
// read that as "the share root", which is what an empty smb_path has always meant.
size_t smb_catalog_folder_count(const char *list);

// Copies folder `i` into `out`, trimmed of spaces and of any leading or trailing '/'.
// Returns false when `i` is out of range or the name does not fit.
bool smb_catalog_folder_at(const char *list, size_t i, char *out, size_t out_size);

#endif // SMB_CATALOG_H
