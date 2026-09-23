// Pure logic shared by the harness and the host test suite.
//
// Nothing here may depend on ESP-IDF. This file and epd_format.c are the only
// sources built under env:native (see build_src_filter in platformio.ini), which is
// what makes the blind-written driver in issues/03 verifiable before the board exists.

#ifndef EPD_FORMAT_H
#define EPD_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------- frame packing

// True for the six colour indices this panel actually renders. Index 4 is orange,
// valid on the 7.3" part only, and returns false here.
bool epd_color_valid(uint8_t index);

// Pack two pixels into one byte, high nibble first.
uint8_t epd_pack_pair(uint8_t hi, uint8_t lo);

// Fill buf with a solid frame of `color`. Returns bytes written, or 0 if `color` is
// not valid on this panel or buf_len is smaller than EPD_FRAME_BYTES.
size_t epd_pack_solid(uint8_t *buf, size_t buf_len, uint8_t color);

// Fill buf with a cols x rows grid of solid patches tiling the whole panel, `patches`
// given row-major in physical portrait orientation. Returns bytes written, or 0 if any
// index is invalid on this panel, the grid is empty or larger than the panel, or
// buf_len is smaller than EPD_FRAME_BYTES.
//
// The cells meet edge to edge with no margin between them. A margin would have to be
// drawn in some colour, and every colour this panel has is also one of the patches
// under measurement -- so the only edges in the frame are cell boundaries, and the
// analysis reads the inner part of each cell to stay clear of them.
//
// Built for issues/20: a colour chart must not go through the dither, or the run
// measures the dither rather than the ink.
size_t epd_pack_chart(uint8_t *buf, size_t buf_len, const uint8_t *patches, int cols,
                      int rows);

// ------------------------------------------------------------ temperature codec

// Decode a TSC (0x40) reading to whole degrees C. The field is plain signed 8-bit:
// 0x00 -> 0, 0x19 -> 25, 0x3C -> 60, 0xE7 -> -25.
//
// Measured on hardware (ticket 18): TSC actually replies with TWO bytes, and the
// second is a fraction -- 0x21,0x80 is 33.5 C. This decoder takes the integer byte
// only, which is what every CSV column and log line here wants; the half degree is
// dropped, not rounded. If a caller ever needs it, read two bytes and treat the
// pair as signed Q8.8 rather than changing this.
int epd_tsc_decode(uint8_t raw);

// Build a TSE (0x41) byte selecting the internal sensor (bit 7 = 0) with a signed
// offset in TO[3:0]. Valid codes are -8..+7; returns false and writes nothing
// outside that. On this board TO[3:0] is the entire temperature design space,
// because the external sensor pins are unconnected.
//
// `offset_steps` is a COUNT OF CODES, not degrees, and one code is 0.5 C -- so the
// real range is -4.0..+3.5 C. Measured across all thirteen codes on hardware
// (ticket 18): +1 moved the reading 0x2200 -> 0x2280, +7 reached 0x2580, -8 reached
// 0x1E00, and returning to 0 came back to 0x2200 exactly. The parameter was named
// offset_c and documented as degrees, which was transcribed from the datasheet and
// is off by a factor of two. Every caller passes 0.
bool epd_tse_encode(int offset_steps, uint8_t *out);

// ---------------------------------------------------------------- the FRS model

// H4 written as code, so that issues/08 tests a prediction instead of fitting one.
//
// Two readings disagree and the sweep at 0x0F discriminates between them:
//
//   _datasheet: the SPD1656 family table read literally. A[5:0], rate = 12.5*(A+1)
//               across 0x00-0x0F, so 0x0F is 200 Hz. Explicit encodings 0x39 = 200,
//               0x3A = 100, 0x3C = 50, everything else 50.
//
//   _observed:  the same, except 0x08-0x0F fall through to "other" -> 50 Hz. This is
//               what the circulating EL040EF1 table implies, since it shows 0x03 and
//               0x08-0x0F producing an identical 19,083 ms.
//
// Both are hypotheses about a controller whose command set is entirely reverse
// engineered. Returns Hz.
float epd_frs_rate_hz_datasheet(uint8_t a);
float epd_frs_rate_hz_observed(uint8_t a);

// Predicted refresh time in ms under t = C + N/rate, the fit from research §11.2.
// C ~= 0.55 s fixed overhead, N ~= 926k frame-seconds. Re-fit properly in
// phase0-preflight/issues/04 before this is used to judge anything.
float epd_frs_predicted_ms(float rate_hz);

// ------------------------------------------------------------------- CSV output

// Phase durations in microseconds, as captured by esp_timer. Kept in us rather than
// ms so no precision is lost before the median is taken.
typedef struct {
    uint32_t run;
    uint8_t frs;
    // Which host-side sequence produced this row: 0 = stock (fixed delays), 1 = busy
    // (BUSY waits only). epd_el040ef1.h owns the enum; this stays a plain byte so
    // epd_format.c keeps building under env:native with no ESP-IDF in sight.
    uint8_t seq;
    int temp_c;
    int64_t reset_us;
    int64_t init_us;
    int64_t xfer_us;
    int64_t pon_us;
    int64_t drf_us;
    int64_t pof_us;
    int64_t total_us;
    // PON command to DRF command. The evidence for the >80 ms floor, emitted as the
    // raw interval rather than as a "floor respected" boolean -- issues/03 cost a
    // re-run because a derived boolean in first-cut instrumentation was measuring
    // something other than its name.
    int64_t pon_to_drf_us;
} epd_timings_t;

// The column list is fixed now so the same harness serves the FRS sweep (issues/08)
// and the temperature work (Phase 3) without modification.
//
// issues/09 appends `seq` and `t_pon2drf_ms` at the END, so a reader that takes
// columns by name still loads every log written before it. tools/analyse.py accepts
// both headers for exactly that reason.
#define EPD_CSV_HEADER                                                                 \
    "run,frs,temp_c,t_reset_ms,t_init_ms,t_xfer_ms,t_pon_ms,t_drf_ms,t_pof_ms,"        \
    "t_total_ms,seq,t_pon2drf_ms"

// "stock" / "busy", and "?" for anything else. Pure, so the host suite pins it.
const char *epd_seq_name(uint8_t seq);

// Format one row. Returns the number of characters that would have been written,
// snprintf-style. Durations are printed in ms to 3 decimal places.
int epd_csv_row(char *buf, size_t buf_len, const epd_timings_t *t);

#endif // EPD_FORMAT_H
