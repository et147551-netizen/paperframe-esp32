// The mirror's record of what it put in /data, and the naming rules that get an SMB
// file there. Kept free of ESP-IDF so it can be tested on the host (NFR-3), the same
// way photo_list.c and board_smb_classify.c are.
//
// Ticket .scratch/digital-frame/issues/25.
//
// Two rules live here, and both are easy to get subtly wrong:
//
//   * THE MANIFEST IS THE OWNERSHIP RECORD. A file in /data that this manifest does not
//     name is never touched by the mirror. That is what protects photographs uploaded
//     through the web UI and the factory firmware's imaged001.png-imaged004.png, which
//     have survived every erase and reflash because an app at 0x10000 cannot reach
//     0xA00000 (ticket 08). Deleting a photograph the user did not expect to lose is,
//     from where they are standing, indistinguishable from a fault.
//
//   * /data CANNOT HOLD A NON-ASCII FILENAME. It is FAT with
//     CONFIG_FATFS_API_ENCODING_ANSI_OEM=y and CONFIG_FATFS_CODEPAGE=437, while SMB2
//     carries names as UTF-16LE. So a Japanese-named photograph has nowhere to land as
//     itself and becomes smb_<8 hex>.<ext>. The hash is taken over the FULL SMB PATH and
//     not the leaf, so two identically-named files in different directories cannot
//     collide.
//
// A parse failure discards the whole manifest rather than repairing it. The recovery is
// a full re-fetch, which at the 4 MB cap is cheap, and it means there is exactly one
// code path for a damaged index instead of a repair routine nobody exercises.

#ifndef SMB_MANIFEST_H
#define SMB_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 500 to match PHOTO_LIST_MAX -- more records than the slideshow can show would be a
// mirror filling a directory the frame cannot enumerate.
#define SMB_MANIFEST_MAX 500

// The file the manifest is serialised to, inside BOARD_STORAGE_MOUNT. It is not one of
// photo_list's four extensions, so it never appears in the slideshow.
#define SMB_MANIFEST_FILE ".smbidx"

// "smb_" + 8 hex + "." + 4 + NUL is 18. 64 leaves room for a verbatim ASCII name that
// FAT can hold while staying well inside the 255-byte LFN limit.
#define SMB_MANIFEST_NAME_SIZE 64

// BOARD_SMB_PATH_SIZE (128, the directory) + '/' + BOARD_SMB_NAME_SIZE (256, the leaf),
// rounded up. Not taken from board_smb.h by #include: this file must stay linkable under
// env:native, which cannot see the smb2/ headers that header's neighbours pull in.
#define SMB_MANIFEST_PATH_SIZE 400

typedef struct {
    uint32_t local_off; // into the arena
    uint32_t path_off;  // into the arena
    uint64_t size;
    uint64_t mtime;
} smb_manifest_rec_t;

// Names and paths are packed into a caller-supplied arena, following photo_list_t: the
// module allocates nothing, so a host test hands it a static buffer and the device hands
// it PSRAM.
typedef struct {
    char *arena;
    size_t arena_size;
    size_t arena_used;
    smb_manifest_rec_t rec[SMB_MANIFEST_MAX];
    size_t count;
    bool truncated;  // hit SMB_MANIFEST_MAX
    bool arena_full; // ran out of name storage
} smb_manifest_t;

bool smb_manifest_init(smb_manifest_t *m, char *arena, size_t arena_size);
void smb_manifest_reset(smb_manifest_t *m);

// Derives the local name for a file at `full_path` (the whole SMB path, directory
// included; the leaf is whatever follows the last '/').
//
// Returns false -- and writes nothing -- when the leaf has no usable image extension, so a
// non-image is never transferred at all. A leaf that is nothing but an extension (".jpg") is
// a dotfile, not a photograph, and is rejected here as it is in photo_list_is_image().
//
// IT IS A SUPERSET OF photo_list_is_image(), NOT THE SAME FILTER, and it stopped being the
// same one deliberately (ticket 38): ANY leading dot is rejected here, while photo_list keeps
// FR-4.6's ported rule verbatim for /data. The mirror is the layer that decides what to pull
// off somebody else's share, and on a share fed by an Android phone a leading dot means
// "deleted" -- .trashed-<epoch>-<name>.jpg, twelve of them on this bench. The extension test
// alone accepted every one.
//
// An ASCII leaf that FAT can store is kept verbatim. Anything else -- a byte outside
// 0x20-0x7E, one of \ / : * ? " < > |, or a name too long for `out` -- becomes
// smb_<8 hex>.<ext> with the extension lowercased.
bool smb_manifest_local_name(const char *full_path, char *out, size_t out_size);

bool smb_manifest_add(smb_manifest_t *m, const char *local, const char *path,
                      uint64_t size, uint64_t mtime);

// Drops record `i`, shifting the rest down so the ORDER IS PRESERVED. That matters rather than
// being tidy: under the on-demand cache (ticket 37 Phase 3) records are appended in fetch order
// and nothing else reorders them, so the record order *is* the fetch order and the eviction
// policy reads it directly. A swap-with-last would destroy it silently.
//
// The arena is not compacted -- the dropped strings keep their bytes. `arena_full` therefore
// stays set and means "rebuild from a fresh arena"; `truncated` is cleared.
bool smb_manifest_remove_at(smb_manifest_t *m, size_t i);

size_t smb_manifest_count(const smb_manifest_t *m);
const char *smb_manifest_local(const smb_manifest_t *m, size_t i);
const char *smb_manifest_path(const smb_manifest_t *m, size_t i);
uint64_t smb_manifest_size(const smb_manifest_t *m, size_t i);
uint64_t smb_manifest_mtime(const smb_manifest_t *m, size_t i);

// Index of the record for `path`, or -1. Linear: at 500 records and one lookup per
// listing entry this is 250,000 string compares in the worst case, which is microseconds
// against a transfer measured in seconds.
int smb_manifest_find(const smb_manifest_t *m, const char *path);

// Does the manifest own the file `local` ON THE CARD? The key is the LOCAL NAME, which is a
// different question from smb_manifest_find()'s: that one asks whether a file on the SHARE has a
// record, and this one asks whether a file already on /data came from the mirror. Ticket 27 needs
// the second and there was no way to ask it.
//
// The compare is exact and deliberately not case-insensitive. Every name on both sides of it has
// the same origin -- smb_manifest_local_name() produced the record's, and the mirror wrote the file
// under that same string -- so a mirrored file matches byte for byte. FAT being case-insensitive
// means a name differing only in case cannot sit beside a mirrored one, so a loose compare could
// never resolve a real collision and could only claim a file the mirror never wrote.
bool smb_manifest_owns_local(const smb_manifest_t *m, const char *local);

// Change detection is (path, mtime, size). A server whose mtime resolution is coarse
// will therefore miss an edit that preserves the size -- accepted: the alternative is
// hashing every file over a 84 KB/s link.
bool smb_manifest_unchanged(const smb_manifest_t *m, size_t i, uint64_t size,
                            uint64_t mtime);

// Writes "#smbidx 1\n" then one tab-separated record per line. Returns the byte count,
// or 0 if `out` was too small -- there is no partial write, because a half-written index
// that parses is worse than none.
//
// Tab is illegal in a Windows filename and in a FAT one, so no field needs escaping.
size_t smb_manifest_serialise(const smb_manifest_t *m, char *out, size_t out_size);

// Replaces the contents of `m`. `text` need not be NUL-terminated. Any malformation --
// a wrong version line, a missing field, a non-numeric size, a name too long, more than
// SMB_MANIFEST_MAX records -- fails the whole parse and leaves `m` empty.
bool smb_manifest_parse(smb_manifest_t *m, const char *text, size_t len);

// ------------------------------------------------------- the on-demand cache's floor

// Bytes to keep free on the volume `/data` is on, given its total size. It is here, and
// pure, because it is the one piece of ticket 54 a host test can reach: everything else in
// that change is FATFS and a card.
//
// `total / 16`, clamped to [1 MB, 32 MB]. The 6 MB internal partition gives 1 MB, leaving
// ~4.2 MB -- about a hundred resized photographs at the 42 kB median measured on
// 2026-09-09, so the file count and the space bind at roughly the same place and neither
// thrashes against the other. The 2 GB card gives 32 MB out of 1.82 GB free, which is
// invisible. Same shape as compute_caps()'s "70 % of free" and app_server.c's
// UPLOAD_FREE_MARGIN.
//
// **IT IS A POLICY FLOOR AND NOT A GUARANTEE.** One window can fetch
// SMB_SYNC_FILES_PER_WINDOW files of up to SMB_SYNC_FILE_MAX each, so no reserve that fits
// a 6 MB volume covers the worst case; write_local()'s short write stays the backstop. What
// this stops is the STEADY STATE being a full volume, which is what ticket 40 found: with
// no card fitted, an SD-sized cache never converges and rewrites the internal partition
// for ever.
uint64_t smb_cache_reserve_bytes(uint64_t total_bytes);

#endif // SMB_MANIFEST_H
