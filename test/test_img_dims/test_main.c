// img_image_dims(): dimensions from an image header, without decoding it.
//
// Worth its own suite because it is a byte parser whose wrong answer is SILENT -- it hands
// back a plausible width and height and the caller resizes to the wrong size, or refuses to
// resize at all. On hardware (ticket 49) the whole feature turns on the number it returns.
//
// The fixtures are hand-built rather than real files: a real JPEG cannot be checked into a
// test directory usefully, and the cases that matter are structural (a segment walked over,
// a fill byte, a truncation) rather than pictorial.

#include <stdbool.h>
#include <string.h>

#include "unity.h"

#include "img_dims.h"

void setUp(void) {}
void tearDown(void) {}

// A JPEG with `pre` bytes of segments before the frame header, so the marker walk has to
// step over something to reach it.
static size_t build_jpeg(uint8_t *out, uint8_t sof_marker, uint16_t w, uint16_t h,
                         const uint8_t *pre, size_t pre_len)
{
    size_t n = 0;
    out[n++] = 0xFF;
    out[n++] = 0xD8; // SOI
    if (pre_len > 0) {
        memcpy(out + n, pre, pre_len);
        n += pre_len;
    }
    out[n++] = 0xFF;
    out[n++] = sof_marker;
    out[n++] = 0x00;
    out[n++] = 0x11; // length 17
    out[n++] = 0x08; // sample precision
    out[n++] = (uint8_t)(h >> 8);
    out[n++] = (uint8_t)(h & 0xFF);
    out[n++] = (uint8_t)(w >> 8);
    out[n++] = (uint8_t)(w & 0xFF);
    out[n++] = 0x03; // components
    memset(out + n, 0, 9);
    n += 9;
    return n;
}

static void test_baseline_jpeg(void)
{
    uint8_t buf[128];
    const size_t n = build_jpeg(buf, 0xC0, 1024, 1024, NULL, 0);
    int32_t w = 0, h = 0;
    TEST_ASSERT_TRUE(img_image_dims(buf, n, &w, &h));
    TEST_ASSERT_EQUAL_INT32(1024, w);
    TEST_ASSERT_EQUAL_INT32(1024, h);
}

// The dimensions are not square and not equal, so a swapped pair fails the test rather
// than passing it by symmetry -- which a 1024x1024 fixture alone would not catch.
static void test_width_and_height_are_not_swapped(void)
{
    uint8_t buf[128];
    const size_t n = build_jpeg(buf, 0xC0, 768, 1344, NULL, 0);
    int32_t w = 0, h = 0;
    TEST_ASSERT_TRUE(img_image_dims(buf, n, &w, &h));
    TEST_ASSERT_EQUAL_INT32(768, w);
    TEST_ASSERT_EQUAL_INT32(1344, h);
}

// An APP0 and an APP1 in front, as every camera JPEG has. The walk must skip them by their
// declared length rather than scanning for the next 0xFF, or an EXIF payload containing
// 0xFFC0 would be read as a frame header.
static void test_skips_leading_segments(void)
{
    uint8_t pre[64];
    size_t p = 0;
    pre[p++] = 0xFF;
    pre[p++] = 0xE0;
    pre[p++] = 0x00;
    pre[p++] = 0x10; // APP0, length 16
    memset(pre + p, 0, 14);
    p += 14;
    pre[p++] = 0xFF;
    pre[p++] = 0xE1;
    pre[p++] = 0x00;
    pre[p++] = 0x0C; // APP1, length 12 -- so 10 payload bytes follow the length field
    // A byte pair inside the payload that LOOKS like a frame header.
    pre[p++] = 0xFF;
    pre[p++] = 0xC0;
    pre[p++] = 0x00;
    pre[p++] = 0x11;
    memset(pre + p, 0x42, 6);
    p += 6;

    uint8_t buf[256];
    const size_t n = build_jpeg(buf, 0xC0, 640, 480, pre, p);
    int32_t w = 0, h = 0;
    TEST_ASSERT_TRUE(img_image_dims(buf, n, &w, &h));
    TEST_ASSERT_EQUAL_INT32(640, w);
    TEST_ASSERT_EQUAL_INT32(480, h);
}

// A progressive JPEG carries its size in exactly the same place. This function must answer
// it: whether the DECODER can read the file is a separate question, asked separately, and
// conflating the two sent one investigation down a blind alley on 2026-09-09.
static void test_progressive_jpeg_still_reports_its_size(void)
{
    uint8_t buf[128];
    const size_t n = build_jpeg(buf, 0xC2, 800, 600, NULL, 0);
    int32_t w = 0, h = 0;
    TEST_ASSERT_TRUE(img_image_dims(buf, n, &w, &h));
    TEST_ASSERT_EQUAL_INT32(800, w);
    TEST_ASSERT_EQUAL_INT32(600, h);
}

// 0xC4 is a Huffman table, not a frame header, even though it is inside the SOF range.
static void test_dht_is_not_mistaken_for_a_frame_header(void)
{
    uint8_t pre[32];
    size_t p = 0;
    pre[p++] = 0xFF;
    pre[p++] = 0xC4;
    pre[p++] = 0x00;
    pre[p++] = 0x0E; // DHT, length 14
    memset(pre + p, 0x7F, 12);
    p += 12;

    uint8_t buf[128];
    const size_t n = build_jpeg(buf, 0xC1, 320, 200, pre, p);
    int32_t w = 0, h = 0;
    TEST_ASSERT_TRUE(img_image_dims(buf, n, &w, &h));
    TEST_ASSERT_EQUAL_INT32(320, w);
    TEST_ASSERT_EQUAL_INT32(200, h);
}

// Fill bytes: any number of 0xFF may precede a marker.
static void test_fill_bytes_before_a_marker(void)
{
    uint8_t pre[8];
    size_t p = 0;
    pre[p++] = 0xFF;
    pre[p++] = 0xFF;
    pre[p++] = 0xFF;

    uint8_t buf[128];
    const size_t n = build_jpeg(buf, 0xC0, 256, 384, pre, p);
    int32_t w = 0, h = 0;
    TEST_ASSERT_TRUE(img_image_dims(buf, n, &w, &h));
    TEST_ASSERT_EQUAL_INT32(256, w);
    TEST_ASSERT_EQUAL_INT32(384, h);
}

// Scan data before any frame header is a malformed file. It must fail rather than walk into
// entropy-coded bytes looking for a marker.
static void test_sos_before_sof_fails(void)
{
    uint8_t buf[64];
    size_t n = 0;
    buf[n++] = 0xFF;
    buf[n++] = 0xD8;
    buf[n++] = 0xFF;
    buf[n++] = 0xDA;
    buf[n++] = 0x00;
    buf[n++] = 0x0C;
    memset(buf + n, 0x33, 32);
    n += 32;
    int32_t w = -1, h = -1;
    TEST_ASSERT_FALSE(img_image_dims(buf, n, &w, &h));
}

static void test_png(void)
{
    uint8_t buf[64];
    const uint8_t magic[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    memset(buf, 0, sizeof(buf));
    memcpy(buf, magic, sizeof(magic));
    // IHDR length + type, then width and height big-endian at 16 and 20.
    buf[16] = 0x00; buf[17] = 0x00; buf[18] = 0x04; buf[19] = 0x00; // 1024
    buf[20] = 0x00; buf[21] = 0x00; buf[22] = 0x02; buf[23] = 0x58; // 600
    int32_t w = 0, h = 0;
    TEST_ASSERT_TRUE(img_image_dims(buf, sizeof(buf), &w, &h));
    TEST_ASSERT_EQUAL_INT32(1024, w);
    TEST_ASSERT_EQUAL_INT32(600, h);
}

static void test_rejects_what_it_cannot_read(void)
{
    int32_t w = -1, h = -1;
    uint8_t buf[128];

    // Too short to hold any header.
    memset(buf, 0xFF, sizeof(buf));
    TEST_ASSERT_FALSE(img_image_dims(buf, 8, &w, &h));

    // Not an image at all.
    memcpy(buf, "not an image at all, just text ok", 33);
    TEST_ASSERT_FALSE(img_image_dims(buf, 33, &w, &h));

    // A JPEG truncated inside the frame header.
    const size_t n = build_jpeg(buf, 0xC0, 1024, 1024, NULL, 0);
    TEST_ASSERT_FALSE(img_image_dims(buf, n - 8, &w, &h));

    // A segment claiming a length that runs off the end.
    size_t k = 0;
    buf[k++] = 0xFF; buf[k++] = 0xD8;
    buf[k++] = 0xFF; buf[k++] = 0xE0; buf[k++] = 0x7F; buf[k++] = 0xFF;
    TEST_ASSERT_FALSE(img_image_dims(buf, k, &w, &h));

    // Nulls.
    TEST_ASSERT_FALSE(img_image_dims(NULL, 100, &w, &h));
    TEST_ASSERT_FALSE(img_image_dims(buf, 100, NULL, &h));
    TEST_ASSERT_FALSE(img_image_dims(buf, 100, &w, NULL));
}

// A zero dimension is a corrupt header, and it must not become a reduction factor.
static void test_zero_dimensions_are_rejected(void)
{
    uint8_t buf[128];
    int32_t w = -1, h = -1;
    size_t n = build_jpeg(buf, 0xC0, 0, 600, NULL, 0);
    TEST_ASSERT_FALSE(img_image_dims(buf, n, &w, &h));
    n = build_jpeg(buf, 0xC0, 400, 0, NULL, 0);
    TEST_ASSERT_FALSE(img_image_dims(buf, n, &w, &h));
}


// ------------------------------------------------------------------ EXIF orientation
//
// The fixtures are hand-built for the reason the ones above are: what matters is
// structural. A real camera JPEG would exercise one byte order, one tag layout and one
// well-formed block, and every case that can put a picture on its side is one of the others.

typedef struct {
    bool little;         // TIFF byte order: II rather than MM
    uint16_t tag;        // 0x0112 is Orientation
    uint16_t type;       // 3 = SHORT
    uint32_t value;
    uint32_t ifd_off;    // 8 in every real file; an override tests a bad one
    uint16_t entries;    // 1; an override makes the table overrun the block
    bool exif_magic;     // false writes an XMP-shaped APP1 instead
    size_t truncate_to;  // 0 = whole; otherwise the payload is cut to this many bytes
} exif_spec_t;

static void put16(uint8_t *p, bool little, uint16_t v)
{
    if (little) {
        p[0] = (uint8_t)(v & 0xFF);
        p[1] = (uint8_t)(v >> 8);
    } else {
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)(v & 0xFF);
    }
}

static void put32(uint8_t *p, bool little, uint32_t v)
{
    if (little) {
        p[0] = (uint8_t)(v & 0xFF);
        p[1] = (uint8_t)((v >> 8) & 0xFF);
        p[2] = (uint8_t)((v >> 16) & 0xFF);
        p[3] = (uint8_t)(v >> 24);
    } else {
        p[0] = (uint8_t)(v >> 24);
        p[1] = (uint8_t)((v >> 16) & 0xFF);
        p[2] = (uint8_t)((v >> 8) & 0xFF);
        p[3] = (uint8_t)(v & 0xFF);
    }
}

// A whole APP1 segment -- marker, length, magic, TIFF header, IFD0 -- ready to be handed to
// build_jpeg() as `pre`.
static size_t build_app1(uint8_t *out, const exif_spec_t *sp)
{
    uint8_t pay[128];
    size_t n = 0;
    if (sp->exif_magic) {
        memcpy(pay + n, "Exif\0\0", 6);
        n += 6;
    } else {
        memcpy(pay + n, "http:/", 6); // an XMP packet starts with its namespace URI
        n += 6;
    }
    const size_t tiff = n;
    pay[n++] = sp->little ? 0x49 : 0x4D;
    pay[n++] = sp->little ? 0x49 : 0x4D;
    put16(pay + n, sp->little, 42);
    n += 2;
    put32(pay + n, sp->little, sp->ifd_off);
    n += 4;
    // IFD0 sits at `ifd_off` from the start of the TIFF block, which for a real file is
    // immediately after the 8-byte header.
    n = tiff + 8;
    put16(pay + n, sp->little, sp->entries);
    n += 2;
    put16(pay + n, sp->little, sp->tag);
    n += 2;
    put16(pay + n, sp->little, sp->type);
    n += 2;
    put32(pay + n, sp->little, 1); // count
    n += 4;
    // A SHORT occupies the first two bytes of the 4-byte value field in file order, under
    // either byte order. A LONG fills all four.
    if (sp->type == 3) {
        put16(pay + n, sp->little, (uint16_t)sp->value);
        pay[n + 2] = 0;
        pay[n + 3] = 0;
    } else {
        put32(pay + n, sp->little, sp->value);
    }
    n += 4;
    put32(pay + n, sp->little, 0); // next IFD: none
    n += 4;

    if (sp->truncate_to > 0 && sp->truncate_to < n) {
        n = sp->truncate_to;
    }

    size_t m = 0;
    out[m++] = 0xFF;
    out[m++] = 0xE1;
    out[m++] = (uint8_t)((n + 2) >> 8);
    out[m++] = (uint8_t)((n + 2) & 0xFF);
    memcpy(out + m, pay, n);
    return m + n;
}

static exif_spec_t exif_defaults(bool little, uint16_t orientation)
{
    exif_spec_t sp;
    sp.little = little;
    sp.tag = 0x0112;
    sp.type = 3;
    sp.value = orientation;
    sp.ifd_off = 8;
    sp.entries = 1;
    sp.exif_magic = true;
    sp.truncate_to = 0;
    return sp;
}

static int orientation_of(const exif_spec_t *sp)
{
    uint8_t app1[192];
    uint8_t buf[512];
    const size_t plen = build_app1(app1, sp);
    const size_t n = build_jpeg(buf, 0xC0, 768, 1344, app1, plen);
    return img_exif_orientation(buf, n);
}

// Both byte orders, because a reader that handles one and not the other looks correct on
// every file from one manufacturer.
static void test_orientation_in_both_byte_orders(void)
{
    exif_spec_t sp = exif_defaults(true, 6);
    TEST_ASSERT_EQUAL_INT(6, orientation_of(&sp));
    sp = exif_defaults(false, 6);
    TEST_ASSERT_EQUAL_INT(6, orientation_of(&sp));
}

static void test_every_defined_value_is_reported(void)
{
    for (uint16_t v = 1; v <= 8; v++) {
        exif_spec_t sp = exif_defaults(true, v);
        TEST_ASSERT_EQUAL_INT((int)v, orientation_of(&sp));
        sp = exif_defaults(false, v);
        TEST_ASSERT_EQUAL_INT((int)v, orientation_of(&sp));
    }
}

// A LONG is out of spec for this tag and some cameras write one anyway. Refusing it would
// show the picture sideways, which is the failure this exists to prevent.
static void test_a_long_typed_value_is_accepted(void)
{
    exif_spec_t sp = exif_defaults(true, 8);
    sp.type = 4;
    TEST_ASSERT_EQUAL_INT(8, orientation_of(&sp));
}

// Everything unreadable is 1 -- the identity -- and never an error code, because the
// caller's question is "how do I turn this".
static void test_unreadable_is_the_identity(void)
{
    exif_spec_t sp = exif_defaults(true, 6);
    sp.exif_magic = false; // an XMP APP1, not an Exif one
    TEST_ASSERT_EQUAL_INT(1, orientation_of(&sp));

    sp = exif_defaults(true, 6);
    sp.tag = 0x0110; // Model, not Orientation
    TEST_ASSERT_EQUAL_INT(1, orientation_of(&sp));

    sp = exif_defaults(true, 6);
    sp.type = 5; // RATIONAL: the value is an offset, not a number
    TEST_ASSERT_EQUAL_INT(1, orientation_of(&sp));

    sp = exif_defaults(true, 0); // out of range, low
    TEST_ASSERT_EQUAL_INT(1, orientation_of(&sp));

    sp = exif_defaults(true, 9); // out of range, high
    TEST_ASSERT_EQUAL_INT(1, orientation_of(&sp));
}

// A file from someone else's camera chooses these numbers. None of them may read past the
// block, and all of them are the identity.
static void test_malformed_blocks_are_the_identity(void)
{
    exif_spec_t sp = exif_defaults(true, 6);
    sp.ifd_off = 4096; // IFD0 past the end of the block
    TEST_ASSERT_EQUAL_INT(1, orientation_of(&sp));

    sp = exif_defaults(true, 6);
    sp.ifd_off = 2; // inside the TIFF header, where the magic is
    TEST_ASSERT_EQUAL_INT(1, orientation_of(&sp));

    sp = exif_defaults(true, 6);
    sp.entries = 4000; // a table 48 kB long in a 40-byte block
    TEST_ASSERT_EQUAL_INT(1, orientation_of(&sp));

    sp = exif_defaults(true, 6);
    sp.truncate_to = 12; // the TIFF header, cut off mid-IFD
    TEST_ASSERT_EQUAL_INT(1, orientation_of(&sp));

    sp = exif_defaults(true, 6);
    sp.truncate_to = 4; // not even a magic
    TEST_ASSERT_EQUAL_INT(1, orientation_of(&sp));
}

static void test_a_bad_byte_order_mark_is_the_identity(void)
{
    uint8_t app1[192];
    uint8_t buf[512];
    exif_spec_t sp = exif_defaults(true, 6);
    const size_t plen = build_app1(app1, &sp);
    app1[4 + 6] = 'X'; // the II
    app1[4 + 7] = 'X';
    const size_t n = build_jpeg(buf, 0xC0, 768, 1344, app1, plen);
    TEST_ASSERT_EQUAL_INT(1, img_exif_orientation(buf, n));

    sp = exif_defaults(true, 6);
    const size_t plen2 = build_app1(app1, &sp);
    app1[4 + 8] = 0xFF; // the 42
    app1[4 + 9] = 0xFF;
    const size_t n2 = build_jpeg(buf, 0xC0, 768, 1344, app1, plen2);
    TEST_ASSERT_EQUAL_INT(1, img_exif_orientation(buf, n2));
}

// Every camera JPEG has an APP0 in front of its APP1, so the walk has to reach the second
// segment rather than only the first.
static void test_exif_behind_an_app0(void)
{
    static const uint8_t app0[] = {0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0, 1, 1,
                                   0, 0, 1, 0, 1, 0, 0};
    uint8_t pre[256];
    uint8_t buf[512];
    memcpy(pre, app0, sizeof(app0));
    exif_spec_t sp = exif_defaults(false, 6);
    const size_t plen = sizeof(app0) + build_app1(pre + sizeof(app0), &sp);
    const size_t n = build_jpeg(buf, 0xC0, 768, 1344, pre, plen);
    TEST_ASSERT_EQUAL_INT(6, img_exif_orientation(buf, n));
}

// A JPEG with no APP1 at all, a PNG, and something that is neither. None of these has an
// orientation and all of them are the identity rather than a failure.
static void test_files_without_an_orientation(void)
{
    uint8_t buf[128];
    size_t n = build_jpeg(buf, 0xC0, 768, 1344, NULL, 0);
    TEST_ASSERT_EQUAL_INT(1, img_exif_orientation(buf, n));

    memset(buf, 0, sizeof(buf));
    static const uint8_t png_magic[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    memcpy(buf, png_magic, sizeof(png_magic));
    buf[19] = 100;
    buf[23] = 100;
    TEST_ASSERT_EQUAL_INT(1, img_exif_orientation(buf, 64));

    TEST_ASSERT_EQUAL_INT(1, img_exif_orientation(NULL, 100));
    TEST_ASSERT_EQUAL_INT(1, img_exif_orientation(buf, 0));
}

// The two readers share one marker walk, so a fixture that carries an EXIF block must still
// report its dimensions -- and the APP1 payload contains 0xFF bytes, which a walk that
// scanned for the next marker instead of stepping by the declared length would trip on.
static void test_dimensions_are_still_read_past_an_exif_block(void)
{
    uint8_t app1[192];
    uint8_t buf[512];
    exif_spec_t sp = exif_defaults(true, 6);
    sp.entries = 1;
    const size_t plen = build_app1(app1, &sp);
    const size_t n = build_jpeg(buf, 0xC0, 768, 1344, app1, plen);
    int32_t w = 0, h = 0;
    TEST_ASSERT_TRUE(img_image_dims(buf, n, &w, &h));
    TEST_ASSERT_EQUAL_INT32(768, w);
    TEST_ASSERT_EQUAL_INT32(1344, h);
}

// ------------------------------------------------- EXIF date and position (ticket 64)
//
// A second builder, because the meta reader follows two SUB-IFDs and the orientation builder above
// has no way to express one. Fixed offsets inside the TIFF block, laid out with gaps, so each
// value's own out-of-line placement is real rather than incidental.
//
// Hand-built for the same reason as everything above: a real camera JPEG is one byte order, one
// hemisphere and one encoding of an angle, and every case that can put the wrong PLACE on the
// panel is one of the others.

#define M_IFD0 8
#define M_EXIF_IFD 64
#define M_DT 96
#define M_GPS_IFD 128
#define M_LAT 200
#define M_LON 224
#define M_END 248

typedef struct {
    bool little;
    bool with_exif_ifd;
    const char *datetime; // NULL omits the tag; 19 characters plus a terminator when present
    bool with_gps_ifd;
    bool gps_complete; // false omits the LONGITUDE, leaving the block half-written
    char lat_ref;
    char lon_ref;
    uint32_t lat[6]; // num, den for degrees, minutes, seconds
    uint32_t lon[6];
    size_t truncate_to; // 0 = whole
} meta_spec_t;

static void put_entry(uint8_t *p, bool little, uint16_t tag, uint16_t type, uint32_t count,
                      uint32_t value)
{
    put16(p, little, tag);
    put16(p + 2, little, type);
    put32(p + 4, little, count);
    if (type == 3) {
        put16(p + 8, little, (uint16_t)value);
        p[10] = 0;
        p[11] = 0;
    } else if (type == 2 && count <= 4) {
        // ASCII short enough to live in the entry: characters in file order, not byte-swapped.
        p[8] = (uint8_t)value;
        p[9] = 0;
        p[10] = 0;
        p[11] = 0;
    } else {
        put32(p + 8, little, value);
    }
}

static size_t build_app1_meta(uint8_t *out, const meta_spec_t *sp)
{
    uint8_t pay[6 + M_END];
    memset(pay, 0, sizeof(pay));
    memcpy(pay, "Exif\0\0", 6);
    uint8_t *t = pay + 6;

    t[0] = sp->little ? 0x49 : 0x4D;
    t[1] = sp->little ? 0x49 : 0x4D;
    put16(t + 2, sp->little, 42);
    put32(t + 4, sp->little, M_IFD0);

    uint16_t n0 = 1; // the orientation is always there, as it is in every camera file
    if (sp->with_exif_ifd) {
        n0++;
    }
    if (sp->with_gps_ifd) {
        n0++;
    }
    put16(t + M_IFD0, sp->little, n0);
    uint8_t *e = t + M_IFD0 + 2;
    put_entry(e, sp->little, 0x0112, 3, 1, 1);
    e += 12;
    if (sp->with_exif_ifd) {
        put_entry(e, sp->little, 0x8769, 4, 1, M_EXIF_IFD);
        e += 12;
    }
    if (sp->with_gps_ifd) {
        put_entry(e, sp->little, 0x8825, 4, 1, M_GPS_IFD);
        e += 12;
    }
    put32(e, sp->little, 0); // no next IFD

    if (sp->with_exif_ifd) {
        const uint16_t n = (sp->datetime != NULL) ? 1 : 0;
        put16(t + M_EXIF_IFD, sp->little, n);
        uint8_t *ee = t + M_EXIF_IFD + 2;
        if (sp->datetime != NULL) {
            put_entry(ee, sp->little, 0x9003, 2, 20, M_DT);
            ee += 12;
            size_t len = strlen(sp->datetime);
            if (len > 19) {
                len = 19;
            }
            memcpy(t + M_DT, sp->datetime, len);
        }
        put32(ee, sp->little, 0);
    }

    if (sp->with_gps_ifd) {
        const uint16_t n = sp->gps_complete ? 4 : 3;
        put16(t + M_GPS_IFD, sp->little, n);
        uint8_t *ge = t + M_GPS_IFD + 2;
        put_entry(ge, sp->little, 0x0001, 2, 2, (uint32_t)(uint8_t)sp->lat_ref);
        ge += 12;
        put_entry(ge, sp->little, 0x0002, 5, 3, M_LAT);
        ge += 12;
        put_entry(ge, sp->little, 0x0003, 2, 2, (uint32_t)(uint8_t)sp->lon_ref);
        ge += 12;
        if (sp->gps_complete) {
            put_entry(ge, sp->little, 0x0004, 5, 3, M_LON);
            ge += 12;
        }
        put32(ge, sp->little, 0);
        for (int i = 0; i < 6; i++) {
            put32(t + M_LAT + i * 4, sp->little, sp->lat[i]);
            put32(t + M_LON + i * 4, sp->little, sp->lon[i]);
        }
    }

    size_t n = 6 + M_END;
    if (sp->truncate_to > 0 && sp->truncate_to < n) {
        n = sp->truncate_to;
    }

    size_t m = 0;
    out[m++] = 0xFF;
    out[m++] = 0xE1;
    out[m++] = (uint8_t)((n + 2) >> 8);
    out[m++] = (uint8_t)((n + 2) & 0xFF);
    memcpy(out + m, pay, n);
    return m + n;
}

// Tokyo: 35 deg 41 min 22.20 sec N, 139 deg 41 min 30.12 sec E -- with the seconds written as
// hundredths, which is how phones write them.
static meta_spec_t meta_defaults(bool little)
{
    meta_spec_t sp;
    memset(&sp, 0, sizeof(sp));
    sp.little = little;
    sp.with_exif_ifd = true;
    sp.datetime = "2019:08:14 10:30:00";
    sp.with_gps_ifd = true;
    sp.gps_complete = true;
    sp.lat_ref = 'N';
    sp.lon_ref = 'E';
    const uint32_t lat[6] = {35, 1, 41, 1, 2220, 100};
    const uint32_t lon[6] = {139, 1, 41, 1, 3012, 100};
    memcpy(sp.lat, lat, sizeof(lat));
    memcpy(sp.lon, lon, sizeof(lon));
    return sp;
}

static bool meta_of(const meta_spec_t *sp, img_exif_meta_t *out)
{
    uint8_t app1[512];
    uint8_t buf[1024];
    const size_t plen = build_app1_meta(app1, sp);
    const size_t n = build_jpeg(buf, 0xC0, 768, 1344, app1, plen);
    return img_exif_meta(buf, n, out);
}

// The expected microdegrees are integer arithmetic done by hand, not a tolerance: 35 + 41/60 +
// 22.20/3600 in millionths, with the truncation each division actually does.
static void test_the_date_and_the_place_in_both_byte_orders(void)
{
    for (int pass = 0; pass < 2; pass++) {
        const meta_spec_t sp = meta_defaults(pass == 0);
        img_exif_meta_t m;
        TEST_ASSERT_TRUE(meta_of(&sp, &m));
        TEST_ASSERT_EQUAL_INT32(2019, m.year);
        TEST_ASSERT_EQUAL_INT32(8, m.month);
        TEST_ASSERT_EQUAL_INT32(14, m.day);
        TEST_ASSERT_TRUE(m.has_gps);
        TEST_ASSERT_EQUAL_INT32(35689499, m.lat_udeg);
        TEST_ASSERT_EQUAL_INT32(139691699, m.lon_udeg);
    }
}

// The same angle written the way an encoder that scales everything writes it. This is the case
// that overflows a 32-bit `num * 1000000`, so it is the reason the arithmetic is 64-bit.
static void test_a_scaled_rational_gives_the_same_angle(void)
{
    meta_spec_t sp = meta_defaults(true);
    const uint32_t lat[6] = {350000, 10000, 410000, 10000, 222000, 10000};
    memcpy(sp.lat, lat, sizeof(lat));
    img_exif_meta_t m;
    TEST_ASSERT_TRUE(meta_of(&sp, &m));
    TEST_ASSERT_TRUE(m.has_gps);
    TEST_ASSERT_EQUAL_INT32(35689499, m.lat_udeg);
}

static void test_the_southern_and_western_hemispheres_are_negative(void)
{
    meta_spec_t sp = meta_defaults(true);
    sp.lat_ref = 'S';
    sp.lon_ref = 'W';
    img_exif_meta_t m;
    TEST_ASSERT_TRUE(meta_of(&sp, &m));
    TEST_ASSERT_TRUE(m.has_gps);
    TEST_ASSERT_EQUAL_INT32(-35689499, m.lat_udeg);
    TEST_ASSERT_EQUAL_INT32(-139691699, m.lon_udeg);
}

// A hemisphere letter this does not recognise is a refusal rather than an assumption, because the
// sign is the difference between Tokyo and a point off the coast of Chile.
static void test_an_unknown_hemisphere_is_refused(void)
{
    meta_spec_t sp = meta_defaults(true);
    sp.lat_ref = 'X';
    img_exif_meta_t m;
    TEST_ASSERT_TRUE(meta_of(&sp, &m));
    TEST_ASSERT_FALSE(m.has_gps);
    // And the date is untouched by the position's failure.
    TEST_ASSERT_EQUAL_INT32(2019, m.year);
}

static void test_each_half_survives_the_other_being_absent(void)
{
    meta_spec_t no_gps = meta_defaults(true);
    no_gps.with_gps_ifd = false;
    img_exif_meta_t m;
    TEST_ASSERT_TRUE(meta_of(&no_gps, &m));
    TEST_ASSERT_FALSE(m.has_gps);
    TEST_ASSERT_EQUAL_INT32(2019, m.year);
    TEST_ASSERT_EQUAL_INT32(0, m.lat_udeg);

    meta_spec_t no_date = meta_defaults(true);
    no_date.with_exif_ifd = false;
    TEST_ASSERT_TRUE(meta_of(&no_date, &m));
    TEST_ASSERT_EQUAL_INT32(0, m.year);
    TEST_ASSERT_TRUE(m.has_gps);

    meta_spec_t no_tag = meta_defaults(true);
    no_tag.datetime = NULL;
    TEST_ASSERT_TRUE(meta_of(&no_tag, &m));
    TEST_ASSERT_EQUAL_INT32(0, m.year);
    TEST_ASSERT_TRUE(m.has_gps);
}

// Half a GPS block is not a position. A frame that took the latitude and invented a longitude
// would name a city on the wrong side of the world with no way to tell.
static void test_an_incomplete_position_is_no_position(void)
{
    meta_spec_t sp = meta_defaults(true);
    sp.gps_complete = false;
    img_exif_meta_t m;
    TEST_ASSERT_TRUE(meta_of(&sp, &m));
    TEST_ASSERT_FALSE(m.has_gps);
}

static void test_nonsense_positions_are_refused(void)
{
    meta_spec_t zero_den = meta_defaults(true);
    zero_den.lat[1] = 0;
    img_exif_meta_t m;
    TEST_ASSERT_TRUE(meta_of(&zero_den, &m));
    TEST_ASSERT_FALSE(m.has_gps);

    meta_spec_t too_far = meta_defaults(true);
    too_far.lat[0] = 200;
    TEST_ASSERT_TRUE(meta_of(&too_far, &m));
    TEST_ASSERT_FALSE(m.has_gps);

    meta_spec_t too_far_east = meta_defaults(true);
    too_far_east.lon[0] = 400;
    TEST_ASSERT_TRUE(meta_of(&too_far_east, &m));
    TEST_ASSERT_FALSE(m.has_gps);
}

// A camera with an unset clock writes zeroes, and a frame that believed it would say "199 YEARS
// AGO" -- which reads as a fault in the frame rather than in the file.
static void test_an_unset_clock_is_no_date(void)
{
    meta_spec_t sp = meta_defaults(true);
    sp.datetime = "0000:00:00 00:00:00";
    img_exif_meta_t m;
    TEST_ASSERT_TRUE(meta_of(&sp, &m));
    TEST_ASSERT_EQUAL_INT32(0, m.year);
    TEST_ASSERT_EQUAL_INT32(0, m.month);
    TEST_ASSERT_TRUE(m.has_gps);

    sp.datetime = "not a date at all!!";
    TEST_ASSERT_TRUE(meta_of(&sp, &m));
    TEST_ASSERT_EQUAL_INT32(0, m.year);
}

// The case the whole import-time read exists for: a re-encoded photograph has no EXIF block, so
// this is what the mirror's own output looks like to the reader.
static void test_a_file_with_no_exif_says_so(void)
{
    uint8_t buf[256];
    const size_t n = build_jpeg(buf, 0xC0, 640, 480, NULL, 0);
    img_exif_meta_t m;
    TEST_ASSERT_FALSE(img_exif_meta(buf, n, &m));
    TEST_ASSERT_EQUAL_INT32(0, m.year);
    TEST_ASSERT_FALSE(m.has_gps);

    TEST_ASSERT_FALSE(img_exif_meta(NULL, 0, &m));
    TEST_ASSERT_FALSE(img_exif_meta(buf, n, NULL));
}

// A truncated APP1 is ordinary -- a segment cut by a short read -- and every offset in it points
// past the end. Nothing may be believed and nothing may be read past.
static void test_a_truncated_block_yields_nothing(void)
{
    for (size_t cut = 8; cut < 6 + M_END; cut += 7) {
        meta_spec_t sp = meta_defaults(true);
        sp.truncate_to = cut;
        img_exif_meta_t m;
        (void)meta_of(&sp, &m);
        // Whatever it manages to read, it must not claim a position whose bytes were cut off.
        if (cut < 6 + M_LON + 24) {
            TEST_ASSERT_FALSE(m.has_gps);
        }
    }
}

// The orientation still reads out of a block that now also carries two sub-IFDs -- the shared
// walker did not break the thing it was factored out of.
static void test_the_orientation_still_reads_beside_the_new_tags(void)
{
    uint8_t app1[512];
    uint8_t buf[1024];
    const meta_spec_t sp = meta_defaults(true);
    const size_t plen = build_app1_meta(app1, &sp);
    const size_t n = build_jpeg(buf, 0xC0, 768, 1344, app1, plen);
    TEST_ASSERT_EQUAL_INT(1, img_exif_orientation(buf, n));

    int32_t w = 0, h = 0;
    TEST_ASSERT_TRUE(img_image_dims(buf, n, &w, &h));
    TEST_ASSERT_EQUAL_INT32(768, w);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_baseline_jpeg);
    RUN_TEST(test_width_and_height_are_not_swapped);
    RUN_TEST(test_skips_leading_segments);
    RUN_TEST(test_progressive_jpeg_still_reports_its_size);
    RUN_TEST(test_dht_is_not_mistaken_for_a_frame_header);
    RUN_TEST(test_fill_bytes_before_a_marker);
    RUN_TEST(test_sos_before_sof_fails);
    RUN_TEST(test_png);
    RUN_TEST(test_rejects_what_it_cannot_read);
    RUN_TEST(test_zero_dimensions_are_rejected);

    RUN_TEST(test_orientation_in_both_byte_orders);
    RUN_TEST(test_every_defined_value_is_reported);
    RUN_TEST(test_a_long_typed_value_is_accepted);
    RUN_TEST(test_unreadable_is_the_identity);
    RUN_TEST(test_malformed_blocks_are_the_identity);
    RUN_TEST(test_a_bad_byte_order_mark_is_the_identity);
    RUN_TEST(test_exif_behind_an_app0);
    RUN_TEST(test_files_without_an_orientation);
    RUN_TEST(test_dimensions_are_still_read_past_an_exif_block);

    RUN_TEST(test_the_date_and_the_place_in_both_byte_orders);
    RUN_TEST(test_a_scaled_rational_gives_the_same_angle);
    RUN_TEST(test_the_southern_and_western_hemispheres_are_negative);
    RUN_TEST(test_an_unknown_hemisphere_is_refused);
    RUN_TEST(test_each_half_survives_the_other_being_absent);
    RUN_TEST(test_an_incomplete_position_is_no_position);
    RUN_TEST(test_nonsense_positions_are_refused);
    RUN_TEST(test_an_unset_clock_is_no_date);
    RUN_TEST(test_a_file_with_no_exif_says_so);
    RUN_TEST(test_a_truncated_block_yields_nothing);
    RUN_TEST(test_the_orientation_still_reads_beside_the_new_tags);
    return UNITY_END();
}
