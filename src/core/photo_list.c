#include "photo_list.h"

#include <stdlib.h>
#include <string.h>

static const char *const EXTENSIONS[] = {".jpg", ".jpeg", ".png", ".bmp"};
#define EXTENSION_COUNT (sizeof(EXTENSIONS) / sizeof(EXTENSIONS[0]))

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// Written out rather than calling strcasecmp: that is POSIX, and this file is meant
// to have no platform surface at all.
static bool ends_with_ci(const char *name, size_t name_len, const char *ext)
{
    const size_t ext_len = strlen(ext);
    if (name_len <= ext_len) {
        return false; // "<= " not "<": a bare ".jpg" has no stem, see the header
    }
    const char *tail = name + (name_len - ext_len);
    for (size_t i = 0; i < ext_len; i++) {
        if (lower(tail[i]) != ext[i]) {
            return false;
        }
    }
    return true;
}

bool photo_list_is_image(const char *name)
{
    if (name == NULL) {
        return false;
    }
    const size_t len = strlen(name);
    for (size_t i = 0; i < EXTENSION_COUNT; i++) {
        if (ends_with_ci(name, len, EXTENSIONS[i])) {
            return true;
        }
    }
    return false;
}

bool photo_list_init(photo_list_t *l, char *arena, size_t arena_size)
{
    if (l == NULL || arena == NULL || arena_size == 0) {
        return false;
    }
    l->arena = arena;
    l->arena_size = arena_size;
    photo_list_reset(l);
    return true;
}

void photo_list_reset(photo_list_t *l)
{
    if (l == NULL) {
        return;
    }
    l->arena_used = 0;
    l->count = 0;
    l->truncated = false;
    l->arena_full = false;
}

bool photo_list_add(photo_list_t *l, const char *name)
{
    if (l == NULL || l->arena == NULL || !photo_list_is_image(name)) {
        return false;
    }
    if (l->count >= PHOTO_LIST_MAX) {
        l->truncated = true;
        return false;
    }

    const size_t need = strlen(name) + 1;
    if (l->arena_used + need > l->arena_size) {
        l->arena_full = true;
        return false;
    }

    memcpy(l->arena + l->arena_used, name, need);
    l->offset[l->count] = (uint32_t)l->arena_used;
    l->arena_used += need;
    l->count++;
    return true;
}

// qsort's comparator sees the offsets, so it needs the arena. Passing it through a
// file-static is not thread-safe; qsort_r is not portable to the host toolchain here.
// The list is small and sorting is not on any concurrent path, but say so rather
// than leave it as a trap.
static const char *s_sort_arena;

static int compare_offsets(const void *a, const void *b)
{
    const char *sa = s_sort_arena + *(const uint32_t *)a;
    const char *sb = s_sort_arena + *(const uint32_t *)b;
    return strcmp(sa, sb); // byte ascending, deliberately not a natural sort
}

void photo_list_sort(photo_list_t *l)
{
    if (l == NULL || l->count < 2) {
        return;
    }
    s_sort_arena = l->arena;
    qsort(l->offset, l->count, sizeof(l->offset[0]), compare_offsets);
    s_sort_arena = NULL;
}

size_t photo_list_count(const photo_list_t *l)
{
    return (l == NULL) ? 0 : l->count;
}

const char *photo_list_at(const photo_list_t *l, size_t i)
{
    if (l == NULL || i >= l->count) {
        return NULL;
    }
    return l->arena + l->offset[i];
}
