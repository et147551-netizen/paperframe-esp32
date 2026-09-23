#include "smb_manifest.h"

#include <stdio.h>
#include <string.h>

// The four extensions photo_list_is_image() accepts, lowercased. Kept as a table rather
// than a chain of strcasecmp so the list is one thing in one place.
static const char *const k_extensions[] = {"jpg", "jpeg", "png", "bmp"};

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// FNV-1a, 32-bit. Chosen because it is four lines and needs no table: the hash's job is
// to separate names, not to resist an adversary.
static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (uint32_t)(unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

// FAT cannot store these, and a name carrying one would fail the fopen rather than the
// transfer, which is a much harder failure to read from a log.
static bool fat_hostile(char c)
{
    return strchr("\\/:*?\"<>|", c) != NULL;
}

bool smb_manifest_init(smb_manifest_t *m, char *arena, size_t arena_size)
{
    if (!m || !arena || arena_size == 0) {
        return false;
    }
    m->arena = arena;
    m->arena_size = arena_size;
    smb_manifest_reset(m);
    return true;
}

void smb_manifest_reset(smb_manifest_t *m)
{
    if (!m) {
        return;
    }
    m->arena_used = 0;
    m->count = 0;
    m->truncated = false;
    m->arena_full = false;
}

bool smb_manifest_local_name(const char *full_path, char *out, size_t out_size)
{
    if (!full_path || !out || out_size == 0) {
        return false;
    }

    const char *slash = strrchr(full_path, '/');
    const char *leaf = slash ? slash + 1 : full_path;
    if (leaf[0] == '\0') {
        return false;
    }

    // A LEADING DOT IS REJECTED OUTRIGHT, AND THAT IS A DELIBERATE DEPARTURE FROM FR-4.6
    // (ticket 38). The extension test below is the shipping firmware's rule
    // (local_photo_slideshow.cpp:525-557) and it has no notion of a hidden file, so this is
    // a superset applied where the mirror decides what to pull off a share rather than in
    // photo_list, which keeps the ported rule verbatim for /data.
    //
    // The case that forced it: \\192.168.1.10\photos\202606 holds twelve
    // .trashed-<epoch>-album_v27_<id>.jpg -- Android MediaStore's soft-deleted photographs,
    // pictures the user deleted on their phone. Every one of them passed the test below,
    // because strrchr finds the LAST dot and `dot == leaf` therefore only ever caught a name
    // that is nothing but an extension (".jpg"). They stayed off the card only because 230
    // visible .jpg already overflow SMB_SYNC_LIST_MAX and '.' (0x2E) sorts after 'a' (0x61)
    // in the share's descending-by-name order -- an accident of the cap, not a rule, and
    // ticket 37 removes that cap.
    if (leaf[0] == '.') {
        return false;
    }

    // rfind on the dot, matching photo_list_is_image(): a leading dot leaves an empty
    // stem, which on FAT is a dotfile rather than a photograph.
    const char *dot = strrchr(leaf, '.');
    if (!dot || dot == leaf || dot[1] == '\0') {
        return false;
    }

    char ext[8];
    const size_t ext_len = strlen(dot + 1);
    if (ext_len >= sizeof(ext)) {
        return false;
    }
    for (size_t i = 0; i <= ext_len; i++) {
        ext[i] = lower(dot[1 + i]);
    }

    bool known = false;
    for (size_t i = 0; i < sizeof(k_extensions) / sizeof(k_extensions[0]); i++) {
        if (strcmp(ext, k_extensions[i]) == 0) {
            known = true;
            break;
        }
    }
    if (!known) {
        return false;
    }

    const size_t leaf_len = strlen(leaf);
    bool verbatim = leaf_len < out_size;
    for (size_t i = 0; verbatim && i < leaf_len; i++) {
        const unsigned char c = (unsigned char)leaf[i];
        if (c < 0x20 || c > 0x7E || fat_hostile((char)c)) {
            verbatim = false;
        }
    }

    if (verbatim) {
        memcpy(out, leaf, leaf_len + 1);
        return true;
    }

    // The hash covers the whole path, so the same leaf under two directories gets two
    // names. A share that already holds a literal smb_<hex>.<ext> could in principle
    // collide with a hashed one; at 2^-32 per pair that is not worth a second mechanism.
    const int n = snprintf(out, out_size, "smb_%08x.%s", (unsigned)fnv1a(full_path), ext);
    return n > 0 && (size_t)n < out_size;
}

static bool arena_put(smb_manifest_t *m, const char *s, uint32_t *off)
{
    const size_t len = strlen(s) + 1;
    if (m->arena_used + len > m->arena_size) {
        m->arena_full = true;
        return false;
    }
    *off = (uint32_t)m->arena_used;
    memcpy(m->arena + m->arena_used, s, len);
    m->arena_used += len;
    return true;
}

bool smb_manifest_add(smb_manifest_t *m, const char *local, const char *path,
                      uint64_t size, uint64_t mtime)
{
    if (!m || !local || !path || local[0] == '\0' || path[0] == '\0') {
        return false;
    }
    if (strlen(local) >= SMB_MANIFEST_NAME_SIZE || strlen(path) >= SMB_MANIFEST_PATH_SIZE) {
        return false;
    }
    if (m->count >= SMB_MANIFEST_MAX) {
        m->truncated = true;
        return false;
    }

    // Both strings or neither: a half-added record would leave count and the arena
    // disagreeing, and the caller is told false either way.
    const size_t used_before = m->arena_used;
    uint32_t local_off = 0, path_off = 0;
    if (!arena_put(m, local, &local_off) || !arena_put(m, path, &path_off)) {
        m->arena_used = used_before;
        return false;
    }

    m->rec[m->count].local_off = local_off;
    m->rec[m->count].path_off = path_off;
    m->rec[m->count].size = size;
    m->rec[m->count].mtime = mtime;
    m->count++;
    return true;
}

bool smb_manifest_remove_at(smb_manifest_t *m, size_t i)
{
    if (!m || i >= m->count) {
        return false;
    }
    // Order-preserving, because under the on-demand cache the record order IS the fetch order
    // and that is what the eviction policy is (app_smb_sync.h). A swap-with-last would be one
    // memcpy instead of a shift and would silently destroy it.
    for (size_t j = i + 1; j < m->count; j++) {
        m->rec[j - 1] = m->rec[j];
    }
    m->count--;
    // The arena is NOT compacted: compacting means rewriting every offset, and the arena is
    // PSRAM where the waste is irrelevant. `arena_full` therefore stays set, and it is the
    // signal that the manifest wants rebuilding from a fresh arena rather than trimming.
    // `truncated` IS cleared, because it meant "there were more records than SMB_MANIFEST_MAX"
    // and after making room that is no longer a description of the contents.
    m->truncated = false;
    return true;
}

size_t smb_manifest_count(const smb_manifest_t *m)
{
    return m ? m->count : 0;
}

const char *smb_manifest_local(const smb_manifest_t *m, size_t i)
{
    return (m && i < m->count) ? m->arena + m->rec[i].local_off : NULL;
}

const char *smb_manifest_path(const smb_manifest_t *m, size_t i)
{
    return (m && i < m->count) ? m->arena + m->rec[i].path_off : NULL;
}

uint64_t smb_manifest_size(const smb_manifest_t *m, size_t i)
{
    return (m && i < m->count) ? m->rec[i].size : 0;
}

uint64_t smb_manifest_mtime(const smb_manifest_t *m, size_t i)
{
    return (m && i < m->count) ? m->rec[i].mtime : 0;
}

int smb_manifest_find(const smb_manifest_t *m, const char *path)
{
    if (!m || !path) {
        return -1;
    }
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->arena + m->rec[i].path_off, path) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool smb_manifest_owns_local(const smb_manifest_t *m, const char *local)
{
    if (!m || !local) {
        return false;
    }
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->arena + m->rec[i].local_off, local) == 0) {
            return true;
        }
    }
    return false;
}

bool smb_manifest_unchanged(const smb_manifest_t *m, size_t i, uint64_t size,
                            uint64_t mtime)
{
    return m && i < m->count && m->rec[i].size == size && m->rec[i].mtime == mtime;
}

// ------------------------------------------------------------------ serialisation

#define MANIFEST_HEADER "#smbidx 1\n"

size_t smb_manifest_serialise(const smb_manifest_t *m, char *out, size_t out_size)
{
    if (!m || !out) {
        return 0;
    }
    size_t used = 0;
    const size_t header_len = strlen(MANIFEST_HEADER);
    if (header_len >= out_size) {
        return 0;
    }
    memcpy(out, MANIFEST_HEADER, header_len);
    used = header_len;

    for (size_t i = 0; i < m->count; i++) {
        const int n = snprintf(out + used, out_size - used, "%s\t%llu\t%llu\t%s\n",
                               m->arena + m->rec[i].local_off,
                               (unsigned long long)m->rec[i].size,
                               (unsigned long long)m->rec[i].mtime,
                               m->arena + m->rec[i].path_off);
        if (n < 0 || (size_t)n >= out_size - used) {
            return 0;
        }
        used += (size_t)n;
    }
    return used;
}

// Copies up to the next `sep` into `dst`, advancing `*p`. False when the separator is
// missing before `end`, or the field does not fit.
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

bool smb_manifest_parse(smb_manifest_t *m, const char *text, size_t len)
{
    if (!m || !text) {
        return false;
    }
    smb_manifest_reset(m);

    const size_t header_len = strlen(MANIFEST_HEADER);
    if (len < header_len || memcmp(text, MANIFEST_HEADER, header_len) != 0) {
        return false;
    }

    const char *p = text + header_len;
    const char *end = text + len;
    while (p < end) {
        char local[SMB_MANIFEST_NAME_SIZE];
        char size_s[24], mtime_s[24];
        char path[SMB_MANIFEST_PATH_SIZE];
        uint64_t size = 0, mtime = 0;

        if (!take_field(&p, end, '\t', local, sizeof(local)) ||
            !take_field(&p, end, '\t', size_s, sizeof(size_s)) ||
            !take_field(&p, end, '\t', mtime_s, sizeof(mtime_s)) ||
            !take_field(&p, end, '\n', path, sizeof(path)) || !parse_u64(size_s, &size) ||
            !parse_u64(mtime_s, &mtime) || !smb_manifest_add(m, local, path, size, mtime)) {
            smb_manifest_reset(m);
            return false;
        }
    }
    return true;
}

uint64_t smb_cache_reserve_bytes(uint64_t total_bytes)
{
    // A volume so small that a sixteenth is under the floor still gets the floor: the
    // reserve is what a fetch needs room inside, not a proportion for its own sake. The
    // caller then finds free < reserve on an almost-full tiny volume and refuses to fetch,
    // which is the right answer rather than a division that quietly permits it.
    const uint64_t lo = 1ull * 1024 * 1024;
    const uint64_t hi = 32ull * 1024 * 1024;
    const uint64_t want = total_bytes / 16u;
    if (want < lo) {
        return lo;
    }
    if (want > hi) {
        return hi;
    }
    return want;
}
