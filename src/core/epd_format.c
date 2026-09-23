#include "epd_format.h"

#include <stdio.h>

#include "epd_cmds.h"
#include "epd_geom.h"

bool epd_color_valid(uint8_t index)
{
    switch (index) {
    case EPD_COLOR_BLACK:
    case EPD_COLOR_WHITE:
    case EPD_COLOR_YELLOW:
    case EPD_COLOR_RED:
    case EPD_COLOR_BLUE:
    case EPD_COLOR_GREEN:
        return true;
    default:
        // Catches EPD_COLOR_ORANGE_UNUSED (4) and anything above 6.
        return false;
    }
}

uint8_t epd_pack_pair(uint8_t hi, uint8_t lo)
{
    return (uint8_t)(((hi & 0x0F) << 4) | (lo & 0x0F));
}

size_t epd_pack_solid(uint8_t *buf, size_t buf_len, uint8_t color)
{
    if (buf == NULL || buf_len < EPD_FRAME_BYTES || !epd_color_valid(color)) {
        return 0;
    }

    const uint8_t packed = epd_pack_pair(color, color);
    for (size_t i = 0; i < EPD_FRAME_BYTES; i++) {
        buf[i] = packed;
    }
    return EPD_FRAME_BYTES;
}

size_t epd_pack_chart(uint8_t *buf, size_t buf_len, const uint8_t *patches, int cols,
                      int rows)
{
    if (buf == NULL || patches == NULL || buf_len < EPD_FRAME_BYTES || cols <= 0
        || rows <= 0 || cols > EPD_WIDTH || rows > EPD_HEIGHT) {
        return 0;
    }
    // Refuse the whole chart rather than drawing the valid part of it. A frame with one
    // wrong patch in it looks like a measurement and is not one.
    for (int i = 0; i < cols * rows; i++) {
        if (!epd_color_valid(patches[i])) {
            return 0;
        }
    }

    const size_t row_bytes = EPD_WIDTH / 2;
    for (int y = 0; y < EPD_HEIGHT; y++) {
        const uint8_t *cell_row = &patches[(size_t)((y * rows) / EPD_HEIGHT) * cols];
        uint8_t *dst = &buf[(size_t)y * row_bytes];
        for (size_t i = 0; i < row_bytes; i++) {
            // Each pixel of the pair is placed independently, so a cell boundary that
            // falls in the middle of a byte lands on the right pixel.
            const int x = (int)(i * 2u);
            dst[i] = epd_pack_pair(cell_row[(x * cols) / EPD_WIDTH],
                                   cell_row[((x + 1) * cols) / EPD_WIDTH]);
        }
    }
    return EPD_FRAME_BYTES;
}

int epd_tsc_decode(uint8_t raw)
{
    return (int)(int8_t)raw;
}

bool epd_tse_encode(int offset_steps, uint8_t *out)
{
    // Codes, not degrees: one step is 0.5 C, so this is -4.0..+3.5 C. Measured on
    // hardware across every code, ticket 18.
    if (out == NULL || offset_steps < -8 || offset_steps > 7) {
        return false;
    }
    // Bit 7 = 0 selects the internal sensor. There is no external sensor on this
    // board -- J5 pins 24/25 are unconnected -- so bit 7 is never set.
    *out = (uint8_t)(offset_steps & 0x0F);
    return true;
}

// Shared by both readings: the explicit encodings above the linear range.
// Returns true and sets *hz if `a` is one of them.
static bool frs_explicit(uint8_t a, float *hz)
{
    switch (a) {
    case 0x39:
        *hz = 200.0f;
        return true;
    case 0x3A:
        *hz = 100.0f;
        return true;
    case 0x3C:
        *hz = 50.0f; // power-on default
        return true;
    default:
        return false;
    }
}

float epd_frs_rate_hz_datasheet(uint8_t a)
{
    float hz;
    if (frs_explicit(a, &hz)) {
        return hz;
    }
    if (a <= 0x0F) {
        return 12.5f * (float)(a + 1);
    }
    return 50.0f; // "other"
}

float epd_frs_rate_hz_observed(uint8_t a)
{
    // NOT frs_explicit(). Measured 2026-09-04 (.scratch/epd-refresh-optimization/
    // issues/08): 0x39 and 0x3A are NOT decoded on this panel. Their DRF is the stock
    // 0x08's to within the arms' own spread and their chart scans differ from the 0x08
    // chart by 0.9 and 0.5 LSB, which is the instrument floor. The related family's
    // datasheet calls them 200 Hz and 100 Hz; epd_frs_rate_hz_datasheet() still says so,
    // and this function is what the panel does.
    if (a == 0x3C) {
        return 50.0f; // power-on default, and 50 Hz either way
    }

    // The circulating table shows 0x03 and 0x08-0x0F producing identical times, which
    // reads as 0x08 and above falling through to "other" rather than continuing the
    // linear ramp. The 0x0F measurement settled that: it is 50 Hz.
    //
    // 0x03-0x07 is exact -- 0x03, 0x05 and 0x07 land on this line within 0.3 %. BELOW
    // 0x03 IT IS NOT: the measured rates are 25.0 Hz at 0x00 (where this returns 12.5),
    // 32.1 Hz at 0x01 (25.0) and 43.0 Hz at 0x02 (37.5). 0x00 is exactly twice the
    // model, and no single-parameter law fits all three, so no formula is offered here --
    // the numbers are in the ticket and this function is knowingly wrong below 0x03.
    if (a <= 0x07) {
        return 12.5f * (float)(a + 1);
    }
    return 50.0f;
}

float epd_frs_predicted_ms(float rate_hz)
{
    if (rate_hz <= 0.0f) {
        return 0.0f;
    }
    const float fixed_overhead_ms = 550.0f;
    const float waveform_frames = 926000.0f;
    return fixed_overhead_ms + (waveform_frames / rate_hz);
}

const char *epd_seq_name(uint8_t seq)
{
    switch (seq) {
    case 0:
        return "stock";
    case 1:
        return "busy";
    default:
        return "?";
    }
}

int epd_csv_row(char *buf, size_t buf_len, const epd_timings_t *t)
{
    if (t == NULL) {
        return -1;
    }
    return snprintf(buf, buf_len,
                    "%u,0x%02X,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%s,%.3f",
                    (unsigned)t->run, (unsigned)t->frs, t->temp_c,
                    (double)t->reset_us / 1000.0, (double)t->init_us / 1000.0,
                    (double)t->xfer_us / 1000.0, (double)t->pon_us / 1000.0,
                    (double)t->drf_us / 1000.0, (double)t->pof_us / 1000.0,
                    (double)t->total_us / 1000.0, epd_seq_name(t->seq),
                    (double)t->pon_to_drf_us / 1000.0);
}
