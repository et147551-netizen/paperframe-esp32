#include "smb_catalog.h"

#include <stdio.h>
#include <string.h>

// The header is a prefix plus two decimal numbers plus '\n' -- see smb_catalog_serialise().
// Version 2 added those numbers; version 1 is rejected, which the header explains.
#define CATALOG_HEADER_PREFIX "#smbdir 2 "

bool smb_catalog_init(smb_catalog_t *c, char *arena, size_t arena_size)
{
    if (c == NULL || arena == NULL || arena_size == 0) {
        return false;
    }
    c->arena = arena;
    c->arena_size = arena_size;
    smb_catalog_reset(c);
    return true;
}

void smb_catalog_reset(smb_catalog_t *c)
{
    if (c == NULL) {
        return;
    }
    c->arena_used = 0;
    c->count = 0;
    c->truncated = false;
    c->arena_full = false;
    // Cleared too: an uninitialised cursor would index a folder of smb_path's list that does
    // not exist, and a caller reading it back would have no way to tell.
    c->folder_cursor = 0;
    c->seed = 0;
}

size_t smb_catalog_count(const smb_catalog_t *c)
{
    return c ? c->count : 0;
}

const char *smb_catalog_path(const smb_catalog_t *c, size_t i)
{
    if (!c || i >= c->count) {
        return NULL;
    }
    return c->arena + c->rec[i].path_off;
}

uint64_t smb_catalog_size(const smb_catalog_t *c, size_t i)
{
    return (c && i < c->count) ? c->rec[i].size : 0;
}

uint64_t smb_catalog_mtime(const smb_catalog_t *c, size_t i)
{
    return (c && i < c->count) ? c->rec[i].mtime : 0;
}

int smb_catalog_find(const smb_catalog_t *c, const char *path)
{
    if (!c || !path) {
        return -1;
    }
    for (size_t i = 0; i < c->count; i++) {
        if (strcmp(c->arena + c->rec[i].path_off, path) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool smb_catalog_add(smb_catalog_t *c, const char *path, uint64_t size, uint64_t mtime)
{
    if (!c || !c->arena || !path || path[0] == '\0') {
        return false;
    }
    const size_t len = strlen(path);
    if (len >= SMB_CATALOG_PATH_SIZE) {
        return false;
    }
    if (c->count >= SMB_CATALOG_MAX) {
        c->truncated = true;
        return false;
    }
    // Refused rather than stored twice: a folder catalogued again without an intervening
    // drop would weight its photographs double in the shuffle, and "some pictures come
    // round more often than others" is close to unobservable in the field.
    //
    // This makes a build quadratic, and at SMB_CATALOG_MAX it is 8x the scale smb_manifest.c
    // waves through: a full parse of 2,100 records is ~2.2M string compares against PSRAM.
    // Left linear anyway, because it happens once per catalogue load and once per folder
    // listing, next to a listing that costs 917 ms and a fetch measured in seconds. If it ever
    // shows up in `plan phases`, the fix is a hash of the path rather than a sorted insert --
    // the record order is the server's and the shuffle does not care, but reordering would
    // silently change which photographs a stored cursor lands on.
    if (smb_catalog_find(c, path) >= 0) {
        return false;
    }
    if (c->arena_used + len + 1 > c->arena_size) {
        c->arena_full = true;
        return false;
    }

    memcpy(c->arena + c->arena_used, path, len + 1);
    c->rec[c->count].path_off = (uint32_t)c->arena_used;
    c->rec[c->count].size = size;
    c->rec[c->count].mtime = mtime;
    c->arena_used += len + 1;
    c->count++;
    return true;
}

// Everything before the last '/', or "" when there is none. Written out rather than using
// strrchr twice at each call site, because "the folder of a path" has to mean exactly one
// thing here for drop_folder to be safe.
static void folder_of(const char *path, char *out, size_t out_size)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        out[0] = '\0';
        return;
    }
    size_t len = (size_t)(slash - path);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, path, len);
    out[len] = '\0';
}

size_t smb_catalog_drop_folder(smb_catalog_t *c, const char *folder, size_t *first_out)
{
    if (first_out) {
        *first_out = c ? c->count : 0;
    }
    if (!c || !folder) {
        return 0;
    }
    size_t kept = 0, dropped = 0;
    for (size_t i = 0; i < c->count; i++) {
        char own[SMB_CATALOG_PATH_SIZE];
        folder_of(c->arena + c->rec[i].path_off, own, sizeof(own));
        // EXACT match on the folder, not a prefix on the path. A prefix test would make
        // dropping "2026" also drop "202606", and this bench's share really does have
        // folders whose names are prefixes of each other's (20260511, 202606).
        if (strcmp(own, folder) == 0) {
            if (first_out && dropped == 0) {
                *first_out = kept; // where the folder began, in post-compaction terms
            }
            dropped++;
            continue;
        }
        c->rec[kept++] = c->rec[i];
    }
    c->count = kept;
    // truncated is cleared: it meant "there were more entries than SMB_CATALOG_MAX", and
    // after making room that is no longer a description of the current contents. arena_full
    // is NOT cleared, because the arena is not compacted and the bytes really are still gone.
    if (dropped > 0) {
        c->truncated = false;
    }
    return dropped;
}

size_t smb_catalog_keep_folders(smb_catalog_t *c, const char *list)
{
    if (!c || !list) {
        return 0;
    }
    const size_t folders = smb_catalog_folder_count(list);
    if (folders == 0) {
        // An empty list means THE SHARE ROOT, which this function cannot name: a record's
        // folder is "" for a root file, and "" is also what an unparseable element gives. So
        // keeping everything is the only safe reading, and it matches catalogue_next_folder(),
        // which does nothing at all in that case. Dropping the lot on a momentarily blank
        // setting would cost a full re-listing per folder to get back.
        return 0;
    }

    size_t kept = 0, dropped = 0;
    for (size_t i = 0; i < c->count; i++) {
        char own[SMB_CATALOG_PATH_SIZE];
        folder_of(c->arena + c->rec[i].path_off, own, sizeof(own));
        bool listed = false;
        // Re-parsing the list per record is O(records x folders) string work -- ~6,000 short
        // scans for this bench's 1,040 records across six folders, once per catalogue refresh.
        // Deliberately not cached: the folder names are up to SMB_CATALOG_PATH_SIZE each, so an
        // array of them is the largest thing in this file, and the alternative is a
        // last-verdict cache that is only fast because records happen to be grouped by folder.
        for (size_t f = 0; f < folders && !listed; f++) {
            char name[SMB_CATALOG_PATH_SIZE];
            if (smb_catalog_folder_at(list, f, name, sizeof(name))) {
                // Exact match, for the same reason drop_folder uses one: 2026 must not match
                // 202606.
                listed = strcmp(own, name) == 0;
            }
        }
        if (!listed) {
            dropped++;
            continue;
        }
        c->rec[kept++] = c->rec[i];
    }
    c->count = kept;
    // Same reasoning as drop_folder: `truncated` described a count that no longer holds, and
    // `arena_full` still does because the arena is not compacted.
    if (dropped > 0) {
        c->truncated = false;
    }
    return dropped;
}

static void reverse_recs(smb_catalog_rec_t *r, size_t n)
{
    for (size_t i = 0; i + 1 < n; i++, n--) {
        const smb_catalog_rec_t t = r[i];
        r[i] = r[n - 1];
        r[n - 1] = t;
    }
}

void smb_catalog_restore_at(smb_catalog_t *c, size_t from, size_t at)
{
    if (!c || at >= from || from >= c->count) {
        return; // nothing appended, or it is already where it belongs
    }
    // Rotate [at, count) so that [from, count) leads it. Three reversals: the classic form,
    // in place and with no temporary the size of a folder -- 230 records is 5.5 KB and this
    // runs under the module lock on a task with an 8 KB stack.
    reverse_recs(c->rec + at, from - at);
    reverse_recs(c->rec + from, c->count - from);
    reverse_recs(c->rec + at, c->count - at);
}

size_t smb_catalog_serialise(const smb_catalog_t *c, char *out, size_t out_size)
{
    if (!c || !out) {
        return 0;
    }
    const int hn = snprintf(out, out_size, CATALOG_HEADER_PREFIX "%lu %lu\n",
                            (unsigned long)c->folder_cursor, (unsigned long)c->seed);
    if (hn < 0 || (size_t)hn >= out_size) {
        return 0;
    }
    size_t used = (size_t)hn;

    for (size_t i = 0; i < c->count; i++) {
        const int n = snprintf(out + used, out_size - used, "%llu\t%llu\t%s\n",
                               (unsigned long long)c->rec[i].size,
                               (unsigned long long)c->rec[i].mtime,
                               c->arena + c->rec[i].path_off);
        if (n < 0 || (size_t)n >= out_size - used) {
            return 0;
        }
        used += (size_t)n;
    }
    return used;
}

static bool take_field(const char **p, const char *end, char sep, char *dst, size_t dst_size)
{
    const char *start = *p;
    const char *q = start;
    while (q < end && *q != sep) {
        q++;
    }
    if (q >= end) {
        return false;
    }
    const size_t len = (size_t)(q - start);
    if (len == 0 || len >= dst_size) {
        return false;
    }
    memcpy(dst, start, len);
    dst[len] = '\0';
    *p = q + 1;
    return true;
}

static bool parse_u64(const char *s, uint64_t *out)
{
    if (*s == '\0') {
        return false;
    }
    uint64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        v = v * 10u + (uint64_t)(*s - '0');
    }
    *out = v;
    return true;
}

bool smb_catalog_parse(smb_catalog_t *c, const char *text, size_t len)
{
    if (!c || !text) {
        return false;
    }
    smb_catalog_reset(c);

    const size_t prefix_len = strlen(CATALOG_HEADER_PREFIX);
    if (len < prefix_len || memcmp(text, CATALOG_HEADER_PREFIX, prefix_len) != 0) {
        return false;
    }

    // The two header numbers, taken with the same field reader the records use so a missing or
    // non-numeric one fails here rather than being silently read as zero -- a cursor quietly
    // defaulting to 0 is exactly the bug this format change exists to fix.
    const char *p = text + prefix_len;
    const char *end = text + len;
    char cursor_s[16], seed_s[16];
    uint64_t cursor = 0, seed = 0;
    if (!take_field(&p, end, ' ', cursor_s, sizeof(cursor_s)) ||
        !take_field(&p, end, '\n', seed_s, sizeof(seed_s)) || !parse_u64(cursor_s, &cursor) ||
        !parse_u64(seed_s, &seed) || cursor > UINT32_MAX || seed > UINT32_MAX) {
        smb_catalog_reset(c);
        return false;
    }
    while (p < end) {
        char size_s[24], mtime_s[24];
        char path[SMB_CATALOG_PATH_SIZE];
        uint64_t size = 0, mtime = 0;

        // The path is last and terminated by '\n', so it is the one field that may contain a
        // '\t'-free arbitrary name. A duplicate path in the file fails the whole parse via
        // smb_catalog_add()'s refusal, which is the behaviour wanted: a catalogue that
        // already double-counts is damaged, and the recovery is a re-listing.
        if (!take_field(&p, end, '\t', size_s, sizeof(size_s)) ||
            !take_field(&p, end, '\t', mtime_s, sizeof(mtime_s)) ||
            !take_field(&p, end, '\n', path, sizeof(path)) || !parse_u64(size_s, &size) ||
            !parse_u64(mtime_s, &mtime) || !smb_catalog_add(c, path, size, mtime)) {
            smb_catalog_reset(c);
            return false;
        }
    }
    // Last, not at the top: smb_catalog_reset() is called on every failure below and would
    // wipe them again, so they are only meaningful once the whole file has parsed.
    c->folder_cursor = (uint32_t)cursor;
    c->seed = (uint32_t)seed;
    return true;
}

// ------------------------------------------------------------------- the folder list

// One element of `list`: [*start, *end) with spaces and '/' trimmed off both ends. Returns
// false when the element is empty after trimming, which is how a trailing comma and a "a,,b"
// gap are skipped rather than turned into a folder named "".
static bool element_at(const char *list, size_t i, const char **start, const char **end)
{
    size_t seen = 0;
    const char *p = list;
    for (;;) {
        const char *comma = strchr(p, ',');
        const char *e = comma ? comma : p + strlen(p);

        // Spaces and '/' come off both ends. Trimming '/' is what keeps "202606/" and
        // "/202606" from becoming a path with a doubled separator once a leaf is appended;
        // an INNER '/' survives, so a nested folder still works.
        const char *s = p;
        while (s < e && (*s == ' ' || *s == '\t' || *s == '/')) {
            s++;
        }
        const char *t = e;
        while (t > s && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '/')) {
            t--;
        }

        if (t > s) {
            if (seen == i) {
                *start = s;
                *end = t;
                return true;
            }
            seen++;
        }
        if (!comma) {
            return false;
        }
        p = comma + 1;
    }
}

size_t smb_catalog_folder_count(const char *list)
{
    if (!list) {
        return 0;
    }
    size_t n = 0;
    const char *s, *e;
    while (element_at(list, n, &s, &e)) {
        n++;
    }
    return n;
}

bool smb_catalog_folder_at(const char *list, size_t i, char *out, size_t out_size)
{
    if (!list || !out || out_size == 0) {
        return false;
    }
    const char *s, *e;
    if (!element_at(list, i, &s, &e)) {
        return false;
    }
    const size_t len = (size_t)(e - s);
    if (len >= out_size) {
        return false;
    }
    memcpy(out, s, len);
    out[len] = '\0';
    return true;
}

// ----------------------------------------------------------------- the shuffle epoch

// xorshift32. Deterministic and part of the contract -- see the header. A zero seed would
// make it produce zero forever, so it is folded away rather than trusted.
static uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void smb_catalog_shuffle(size_t n, uint32_t seed, uint16_t *perm)
{
    if (n == 0 || !perm) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        perm[i] = (uint16_t)i;
    }

    uint32_t state = seed ? seed : 0x9E3779B9u;
    // Fisher-Yates downwards, which is the form whose loop invariant is easy to state: after
    // step i, positions i..n-1 hold a uniformly chosen suffix and are never touched again.
    for (size_t i = n - 1; i > 0; i--) {
        const size_t j = (size_t)(next_random(&state) % (uint32_t)(i + 1));
        const uint16_t t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }
}
