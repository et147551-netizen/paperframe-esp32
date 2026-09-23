#include "img_dims.h"

#include <string.h>

// A walk over a JPEG's marker segments, shared by the two readers below. It is one walk
// rather than two because the second reader was written by copying the first, and a byte
// parser duplicated is a byte parser that gets fixed in one place.
typedef struct {
    const uint8_t *src;
    size_t len;
    size_t i;
} jpeg_walk_t;

static bool jpeg_walk_begin(jpeg_walk_t *w, const uint8_t *src, size_t len)
{
    if (len < 4 || src[0] != 0xFF || src[1] != 0xD8) {
        return false;
    }
    w->src = src;
    w->len = len;
    w->i = 2;
    return true;
}

// The next segment that HAS a payload. `payload` points past the two length bytes and
// `plen` excludes them, so a segment's own fields are indexed from zero.
//
// Returns false at the end of the markers, at SOS -- scan data is not walkable, and every
// caller here wants a header -- and on any length that does not fit, which is a truncated
// or malformed file.
static bool jpeg_walk_next(jpeg_walk_t *w, uint8_t *marker, const uint8_t **payload,
                           size_t *plen)
{
    while (w->i + 3 < w->len) {
        if (w->src[w->i] != 0xFF) {
            w->i++;
            continue;
        }
        const uint8_t m = w->src[w->i + 1];
        if (m == 0xFF) {
            w->i++; // fill byte
            continue;
        }
        if (m == 0xD8 || m == 0xD9 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
            w->i += 2; // no payload
            continue;
        }
        const size_t len = ((size_t)w->src[w->i + 2] << 8) | w->src[w->i + 3];
        if (len < 2 || w->i + 2 + len > w->len) {
            return false;
        }
        if (m == 0xDA) {
            return false; // scan data
        }
        *marker = m;
        *payload = w->src + w->i + 4;
        *plen = len - 2;
        w->i += 2 + len;
        return true;
    }
    return false;
}

bool img_image_dims(const uint8_t *src, size_t src_len, int32_t *w, int32_t *h)
{
    if (src == NULL || w == NULL || h == NULL || src_len < 4) {
        return false;
    }

    // PNG: the IHDR is always the first chunk, at a fixed offset. The 24-byte minimum is
    // THIS branch's, not the function's -- it used to guard the whole thing, which rejected
    // any JPEG under 24 bytes. Harmless on a real photograph and wrong on principle, and
    // test_img_dims is what found it.
    static const uint8_t png_magic[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (src_len >= 24 && memcmp(src, png_magic, sizeof(png_magic)) == 0) {
        *w = (int32_t)((uint32_t)src[16] << 24 | (uint32_t)src[17] << 16 |
                       (uint32_t)src[18] << 8 | src[19]);
        *h = (int32_t)((uint32_t)src[20] << 24 | (uint32_t)src[21] << 16 |
                       (uint32_t)src[22] << 8 | src[23]);
        return *w > 0 && *h > 0;
    }

    // Walk the marker segments to the frame header. Every SOFn carries the dimensions in
    // the same place, so this does not care whether the file is baseline or progressive --
    // the decoder does, and it says so in its own way.
    jpeg_walk_t walk;
    if (!jpeg_walk_begin(&walk, src, src_len)) {
        return false;
    }
    uint8_t m = 0;
    const uint8_t *p = NULL;
    size_t plen = 0;
    while (jpeg_walk_next(&walk, &m, &p, &plen)) {
        const bool is_sof = (m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC;
        if (!is_sof) {
            continue;
        }
        if (plen < 5) {
            return false;
        }
        // payload[0] is the sample precision; the dimensions follow, height first.
        *h = (int32_t)(((uint32_t)p[1] << 8) | p[2]);
        *w = (int32_t)(((uint32_t)p[3] << 8) | p[4]);
        return *w > 0 && *h > 0;
    }
    return false;
}

// ------------------------------------------------------------------- EXIF orientation

static uint16_t rd16(const uint8_t *p, bool little)
{
    return little ? (uint16_t)((uint16_t)p[1] << 8 | p[0])
                  : (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t rd32(const uint8_t *p, bool little)
{
    return little ? ((uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0])
                  : ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
}

// Byte order and IFD0's offset. Every offset in a TIFF is relative to the block's own start,
// which is why the block is passed around rather than the file.
static bool tiff_header(const uint8_t *t, size_t tlen, bool *little, uint32_t *ifd0)
{
    if (tlen < 8) {
        return false;
    }
    if (t[0] == 0x49 && t[1] == 0x49) {
        *little = true;
    } else if (t[0] == 0x4D && t[1] == 0x4D) {
        *little = false;
    } else {
        return false;
    }
    if (rd16(t + 2, *little) != 42) {
        return false;
    }
    const uint32_t off = rd32(t + 4, *little);
    if (off < 8 || (size_t)off + 2 > tlen) {
        return false;
    }
    *ifd0 = off;
    return true;
}

// The first entry for `tag` in the IFD at `off`, or NULL. **One walker for every IFD this file
// reads** -- IFD0, the Exif sub-IFD and the GPS sub-IFD have identical structure, and three
// copies of the bounds arithmetic would be three chances for one of them to be wrong.
static const uint8_t *ifd_find(const uint8_t *t, size_t tlen, uint32_t off, bool little,
                               uint16_t tag)
{
    if (off < 8 || (size_t)off + 2 > tlen) {
        return NULL;
    }
    const uint32_t count = rd16(t + off, little);
    // 12 bytes an entry, and the count is a file-controlled number: check the whole table
    // fits before indexing into any of it.
    if ((size_t)off + 2 + (size_t)count * 12u > tlen) {
        return NULL;
    }
    for (uint32_t k = 0; k < count; k++) {
        const uint8_t *e = t + off + 2 + (size_t)k * 12u;
        if (rd16(e, little) == tag) {
            return e;
        }
    }
    return NULL;
}

static size_t tiff_type_size(uint16_t type)
{
    switch (type) {
    case 1:
    case 2:
    case 6:
    case 7:
        return 1; // BYTE, ASCII, SBYTE, UNDEFINED
    case 3:
    case 8:
        return 2; // SHORT, SSHORT
    case 4:
    case 9:
    case 11:
        return 4; // LONG, SLONG, FLOAT
    case 5:
    case 10:
    case 12:
        return 8; // RATIONAL, SRATIONAL, DOUBLE
    default:
        return 0;
    }
}

// The value bytes of an entry that must be `type` and must hold at least `min_count` of them.
// NULL for a type mismatch, a short count, or a value that falls outside the block -- a truncated
// APP1 segment is ordinary and must not be read past.
//
// Four bytes or fewer live INSIDE the entry; anything larger is an offset from the block's start.
// The arithmetic is 64-bit because both the count and the offset are numbers the file chooses.
static const uint8_t *entry_value(const uint8_t *t, size_t tlen, const uint8_t *e, bool little,
                                  uint16_t type, uint32_t min_count)
{
    if (rd16(e + 2, little) != type) {
        return NULL;
    }
    const size_t unit = tiff_type_size(type);
    if (unit == 0) {
        return NULL;
    }
    const uint32_t count = rd32(e + 4, little);
    if (count < min_count) {
        return NULL;
    }
    const uint64_t bytes = (uint64_t)count * (uint64_t)unit;
    if (bytes <= 4u) {
        return e + 8;
    }
    const uint32_t off = rd32(e + 8, little);
    if ((uint64_t)off + bytes > (uint64_t)tlen) {
        return NULL;
    }
    return t + off;
}

// A sub-IFD pointer: one LONG whose value is an offset into the same block. 0 when absent, which
// is also an invalid IFD offset, so one return value covers both.
static uint32_t ifd_pointer(const uint8_t *t, size_t tlen, uint32_t ifd0, bool little,
                            uint16_t tag)
{
    const uint8_t *e = ifd_find(t, tlen, ifd0, little, tag);
    if (e == NULL || entry_value(t, tlen, e, little, 4, 1) == NULL) {
        return 0;
    }
    return rd32(e + 8, little);
}

static int img_exif_ifd0_orientation(const uint8_t *t, size_t tlen)
{
    bool little = false;
    uint32_t ifd0 = 0;
    if (!tiff_header(t, tlen, &little, &ifd0)) {
        return 1;
    }
    const uint8_t *e = ifd_find(t, tlen, ifd0, little, 0x0112);
    if (e == NULL) {
        return 1;
    }
    const uint16_t type = rd16(e + 2, little);
    // The value sits INSIDE the entry because it is at most 4 bytes, and a SHORT
    // occupies the first two of that field in file order under either byte order.
    // LONG is out of spec for this tag and accepted anyway: a reader that refuses it
    // shows the picture sideways, which is the failure this exists to prevent.
    uint32_t v;
    if (type == 3) {
        v = rd16(e + 8, little);
    } else if (type == 4) {
        v = rd32(e + 8, little);
    } else {
        return 1;
    }
    return (v >= 1 && v <= 8) ? (int)v : 1;
}

// The TIFF block inside APP1, or NULL. Both readers want it, and walking the JPEG separately in
// each would be two chances to disagree about which APP1 is the Exif one.
static const uint8_t *exif_tiff_block(const uint8_t *src, size_t src_len, size_t *out_len)
{
    *out_len = 0;
    if (src == NULL || src_len < 4) {
        return NULL;
    }
    jpeg_walk_t walk;
    if (!jpeg_walk_begin(&walk, src, src_len)) {
        return NULL; // PNG and BMP have no EXIF, and neither does a malformed file
    }
    static const uint8_t exif_magic[6] = {'E', 'x', 'i', 'f', 0x00, 0x00};
    uint8_t m = 0;
    const uint8_t *p = NULL;
    size_t plen = 0;
    while (jpeg_walk_next(&walk, &m, &p, &plen)) {
        // APP1 is also where XMP lives, so the magic decides rather than the marker.
        if (m != 0xE1 || plen < sizeof(exif_magic) ||
            memcmp(p, exif_magic, sizeof(exif_magic)) != 0) {
            continue;
        }
        *out_len = plen - sizeof(exif_magic);
        return p + sizeof(exif_magic);
    }
    return NULL;
}

int img_exif_orientation(const uint8_t *src, size_t src_len)
{
    size_t tlen = 0;
    const uint8_t *t = exif_tiff_block(src, src_len, &tlen);
    return (t != NULL) ? img_exif_ifd0_orientation(t, tlen) : 1;
}

// ------------------------------------------------------------- EXIF date and position

static void read_taken(const uint8_t *t, size_t tlen, uint32_t exif_ifd, bool little,
                       img_exif_meta_t *out)
{
    const uint8_t *e = ifd_find(t, tlen, exif_ifd, little, 0x9003);
    if (e == NULL) {
        return;
    }
    // "YYYY:MM:DD HH:MM:SS" and a terminator, so 20 ASCII. Only the first ten are read: nothing
    // displays a time of day, and a band that could would be a clock, which this frame must not
    // draw at 15,014.6 ms a refresh.
    const uint8_t *v = entry_value(t, tlen, e, little, 2, 10);
    if (v == NULL) {
        return;
    }
    for (int i = 0; i < 10; i++) {
        const bool want_digit = (i != 4 && i != 7);
        if (want_digit ? (v[i] < '0' || v[i] > '9') : (v[i] != ':')) {
            return;
        }
    }
    const int32_t y = (v[0] - '0') * 1000 + (v[1] - '0') * 100 + (v[2] - '0') * 10 + (v[3] - '0');
    const int32_t mo = (v[5] - '0') * 10 + (v[6] - '0');
    const int32_t d = (v[8] - '0') * 10 + (v[9] - '0');
    // 1826 is the first photograph. The lower bound matters because cameras write "0000:00:00"
    // for an unset clock, and a frame saying "197 YEARS AGO" reads as a bug in the frame.
    if (y < 1826 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31) {
        return;
    }
    out->year = y;
    out->month = mo;
    out->day = d;
}

// degrees + minutes/60 + seconds/3600, in millionths of a degree.
//
// **64-bit throughout, and that is not caution.** Encoders write the same angle many ways --
// 35/1 or 350000/10000 for the degrees, 0/1 seconds with the minutes as 4102/100 -- and a
// 32-bit `num * 1000000` overflows on the second form at 3.5e11.
static bool rational_dms_udeg(const uint8_t *v, bool little, int32_t *out)
{
    static const int64_t scale[3] = {1, 60, 3600};
    int64_t total = 0;
    for (int i = 0; i < 3; i++) {
        const uint32_t num = rd32(v + i * 8, little);
        const uint32_t den = rd32(v + i * 8 + 4, little);
        if (den == 0) {
            return false;
        }
        total += ((int64_t)num * 1000000) / ((int64_t)den * scale[i]);
    }
    *out = (int32_t)total;
    return true;
}

static void read_gps(const uint8_t *t, size_t tlen, uint32_t gps_ifd, bool little,
                     img_exif_meta_t *out)
{
    const uint8_t *lat_e = ifd_find(t, tlen, gps_ifd, little, 0x0002);
    const uint8_t *lon_e = ifd_find(t, tlen, gps_ifd, little, 0x0004);
    const uint8_t *lat_ref_e = ifd_find(t, tlen, gps_ifd, little, 0x0001);
    const uint8_t *lon_ref_e = ifd_find(t, tlen, gps_ifd, little, 0x0003);
    if (lat_e == NULL || lon_e == NULL || lat_ref_e == NULL || lon_ref_e == NULL) {
        return;
    }

    const uint8_t *lat_v = entry_value(t, tlen, lat_e, little, 5, 3);
    const uint8_t *lon_v = entry_value(t, tlen, lon_e, little, 5, 3);
    const uint8_t *lat_ref = entry_value(t, tlen, lat_ref_e, little, 2, 1);
    const uint8_t *lon_ref = entry_value(t, tlen, lon_ref_e, little, 2, 1);
    if (lat_v == NULL || lon_v == NULL || lat_ref == NULL || lon_ref == NULL) {
        return;
    }

    int32_t lat = 0;
    int32_t lon = 0;
    if (!rational_dms_udeg(lat_v, little, &lat) || !rational_dms_udeg(lon_v, little, &lon)) {
        return;
    }

    // A hemisphere this does not recognise is a refusal, not a guess: the sign is the difference
    // between Tokyo and a point off the coast of Chile.
    if (lat_ref[0] == 'S' || lat_ref[0] == 's') {
        lat = -lat;
    } else if (lat_ref[0] != 'N' && lat_ref[0] != 'n') {
        return;
    }
    if (lon_ref[0] == 'W' || lon_ref[0] == 'w') {
        lon = -lon;
    } else if (lon_ref[0] != 'E' && lon_ref[0] != 'e') {
        return;
    }

    if (lat < -90000000 || lat > 90000000 || lon < -180000000 || lon > 180000000) {
        return;
    }
    out->has_gps = true;
    out->lat_udeg = lat;
    out->lon_udeg = lon;
}

bool img_exif_meta(const uint8_t *src, size_t src_len, img_exif_meta_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    size_t tlen = 0;
    const uint8_t *t = exif_tiff_block(src, src_len, &tlen);
    if (t == NULL) {
        return false;
    }
    bool little = false;
    uint32_t ifd0 = 0;
    if (!tiff_header(t, tlen, &little, &ifd0)) {
        return false;
    }

    const uint32_t exif_ifd = ifd_pointer(t, tlen, ifd0, little, 0x8769);
    const uint32_t gps_ifd = ifd_pointer(t, tlen, ifd0, little, 0x8825);
    if (exif_ifd != 0) {
        read_taken(t, tlen, exif_ifd, little, out);
    }
    if (gps_ifd != 0) {
        read_gps(t, tlen, gps_ifd, little, out);
    }
    // True means "there was an EXIF block", not "it had anything in it" -- a JPEG straight out of
    // a re-encode has no block at all, and that is the case worth distinguishing.
    return true;
}
