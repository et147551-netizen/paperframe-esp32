// The image-directory rules from FR-4.6, kept free of ESP-IDF so they can be tested
// on the host (NFR-3).
//
// The rules are fixed and come from the shipping firmware
// (refs/M5PaperColor-UserDemo/main/apps/local_photo_slideshow/
// local_photo_slideshow.cpp:525-557, local_photo_slideshow.h:137): /data only, no
// recursion, .jpg/.jpeg/.png/.bmp case-insensitively, sorted by filename ascending,
// capped at 500 entries.
//
// Two of those are easy to get subtly wrong and are pinned by tests rather than by
// this comment:
//
//   * The sort is BYTE ascending, matching std::sort over std::string in the
//     reference. So "10.jpg" precedes "9.jpg" and "Z.jpg" precedes "a.jpg". It is
//     not a natural sort, however much "sorted ascending" sounds like one.
//   * The 500 cap is applied while reading the directory, BEFORE sorting -- so a
//     directory of 600 files yields 500 arbitrary ones in directory order, not the
//     alphabetically first 500. That is what the reference does; changing it is a
//     product decision, not a bug fix.

#ifndef PHOTO_LIST_H
#define PHOTO_LIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PHOTO_LIST_MAX 500

// Names are packed into a caller-supplied arena, following the epd_canvas pattern:
// the module does no allocation, so a host test can hand it a static buffer and the
// device can hand it PSRAM.
typedef struct {
    char *arena;
    size_t arena_size;
    size_t arena_used;
    uint32_t offset[PHOTO_LIST_MAX];
    size_t count;
    bool truncated;   // hit PHOTO_LIST_MAX
    bool arena_full;  // ran out of name storage
} photo_list_t;

bool photo_list_init(photo_list_t *l, char *arena, size_t arena_size);
void photo_list_reset(photo_list_t *l);

// True if `name` ends in one of the four recognised extensions, case-insensitively.
// A name that is nothing but an extension (".jpg") is not an image: the reference
// matches on a dot found with rfind, and a leading dot leaves an empty stem, which
// on FAT is a dotfile rather than a photo.
bool photo_list_is_image(const char *name);

// Filters, then copies into the arena. Returns false when the name was rejected, the
// cap was reached, or the arena is full -- the flags say which. A false return never
// leaves the list corrupted.
bool photo_list_add(photo_list_t *l, const char *name);

void photo_list_sort(photo_list_t *l);

size_t photo_list_count(const photo_list_t *l);
const char *photo_list_at(const photo_list_t *l, size_t i);

#endif // PHOTO_LIST_H
