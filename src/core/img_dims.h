// Image dimensions from the header alone -- no decode, no allocation.
//
// Its own translation unit, and the reason is testability: img_resize.c pulls in esp_jpeg_enc
// and epd_image, so it cannot be in the env:native build, and this is a byte parser over
// attacker-shaped input that silently returns the wrong answer when it is wrong. Nothing else
// here has ESP-IDF in it, so test/test_img_dims can have it.
//
// It exists because the SMB import has to know the reduction factor BEFORE committing to a
// decode. epd_image_open_mem_fit() decodes the whole image at open (epd_image.c:463), so
// asking IT costs the very allocation the answer is needed to avoid: a 1024x1024 source is
// 3.15 MB decoded and another 3.15 MB reduced, which does not fit in this board's ~6.4 MB of
// PSRAM beside a render. Measured on hardware 2026-09-09, ticket 49 -- every 1024x1024
// photograph on the bench share failed until this existed.

#ifndef IMG_DIMS_H
#define IMG_DIMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// JPEG (any SOFn) and PNG only. Returns false for anything else, including a truncated or
// malformed header -- the caller then has no factor and must not resize.
//
// Deliberately indifferent to whether a JPEG is baseline or progressive: every SOFn carries
// the dimensions in the same place. Whether the DECODER can read the file is the decoder's
// answer to give, and it gives it separately.
bool img_image_dims(const uint8_t *src, size_t src_len, int32_t *w, int32_t *h);

// The EXIF orientation a JPEG declares: 1-8 as TIFF tag 0x0112 defines them, and **1 for
// everything it cannot read** -- not an error code. A file with no APP1, an XMP APP1, a
// truncated one, a byte order it does not recognise, a value out of range, and a PNG or a
// BMP all come back 1, which is the identity transform.
//
// Returning 1 rather than failing is the whole interface decision: the caller's question is
// "how do I turn this", and the answer for anything unreadable is "not at all". A failure
// code would have every call site write that same fallback, and one of them would get it
// wrong.
//
// It exists because a phone stores a portrait photograph as a LANDSCAPE frame plus
// Orientation=6, so a frame that ignores the tag draws it on its side. Worse since
// auto_rotate (ticket 51): the frame then turns the landscape-shaped picture to fill the
// panel, which is upright for one of 6/8 and upside down for the other, with no observable
// saying which. Ticket 53.
//
// Bounds are checked against `src_len` at every step -- this is a byte parser over a file
// from someone else's camera, and the IFD entry count is a number the file chooses.
int img_exif_orientation(const uint8_t *src, size_t src_len);

// What a photograph says about ITSELF, for the band a matte leaves beside it (ticket 64): when it
// was taken and where. Every field is optional; an absent one reads as 0 or false.
//
// **Read this while the ORIGINAL bytes are still in hand.** The mirror's import re-encodes through
// esp_new_jpeg and the re-encode carries no EXIF at all, so a photograph sitting on the card
// usually has none -- `docs/agents/smb-mirror.md` has the account. `app_smb_sync.c` calls this
// beside `img_exif_orientation()`, on the same buffer, for that reason. A draw-time read would
// return nothing for most of a library and look like a feature that is merely rare.
//
// Same parser and the same bounds discipline as the orientation above: one pass over IFD0 which
// follows two sub-IFD pointers, every offset and count checked against the block it came from.
typedef struct {
    // From DateTimeOriginal (0x9003). All three are 0 together when absent or unparseable; the
    // time of day is deliberately not kept, because nothing displays it.
    int32_t year;  // 1826-2100, or 0
    int32_t month; // 1-12, or 0
    int32_t day;   // 1-31, or 0

    // Millionths of a degree, north and east positive. `has_gps` is separate because 0,0 is a
    // real place in the Gulf of Guinea and a photograph tagged with it is indistinguishable from
    // one tagged with nothing.
    bool has_gps;
    int32_t lat_udeg; // ±90,000,000
    int32_t lon_udeg; // ±180,000,000
} img_exif_meta_t;

// False when the file has no readable EXIF block at all. `*out` is zeroed first either way, so a
// caller that ignores the return still sees absent fields rather than stale ones.
bool img_exif_meta(const uint8_t *src, size_t src_len, img_exif_meta_t *out);

#endif // IMG_DIMS_H
