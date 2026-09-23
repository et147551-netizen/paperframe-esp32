#include "epd_band.h"

#include <string.h>

// ------------------------------------------------------------------------ geometry

static int32_t band_long(const epd_band_t *band)
{
    return (band->kind == EPD_BAND_VERTICAL) ? band->height : band->width;
}

static int32_t band_short(const epd_band_t *band)
{
    return (band->kind == EPD_BAND_VERTICAL) ? band->width : band->height;
}

bool epd_band_bottom_align(const epd_canvas_t *c, bool slack_x, epd_align_t *out)
{
    if (c == NULL || out == NULL) {
        return false;
    }
    const int32_t lw = epd_canvas_logical_width(c);
    const int32_t lh = epd_canvas_logical_height(c);
    if (lw < 2 || lh < 2) {
        return false;
    }

    // Two 1x1 probes, at the two ends of the axis the fit left slack on. Where they land
    // PHYSICALLY is the whole answer, and asking is the point: equal physical y means this axis
    // runs across the glass rather than down it, so the leftover would sit beside the picture and
    // there must be no band.
    int32_t px = 0, pw = 0, ph = 0;
    int32_t lo_y = 0, hi_y = 0;
    if (!epd_canvas_physical_rect(c, 0, 0, 1, 1, &px, &lo_y, &pw, &ph)) {
        return false;
    }
    const int32_t far_x = slack_x ? (lw - 1) : 0;
    const int32_t far_y = slack_x ? 0 : (lh - 1);
    if (!epd_canvas_physical_rect(c, far_x, far_y, 1, 1, &px, &hi_y, &pw, &ph)) {
        return false;
    }
    if (lo_y == hi_y) {
        return false;
    }

    // The leftover belongs at whichever end is lower on the glass, and the photograph at the other.
    *out = (hi_y > lo_y) ? EPD_ALIGN_LOW : EPD_ALIGN_HIGH;
    return true;
}

// The band rect implied by an already-aligned fit. Private, because a fit whose alignment was not
// chosen by epd_band_plan() would put the band somewhere the operator rejected.
static epd_band_t band_from_fit(const epd_fit_t *fit, int32_t lw, int32_t lh, bool slack_x)
{
    epd_band_t band = {EPD_BAND_NONE, 0, 0, 0, 0, true};
    if (slack_x) {
        const int32_t before = fit->x;
        const int32_t after = lw - fit->x - fit->width;
        band.kind = EPD_BAND_VERTICAL;
        band.y = 0;
        band.height = lh;
        band.photo_at_low = (after >= before);
        band.x = band.photo_at_low ? fit->x + fit->width : 0;
        band.width = band.photo_at_low ? after : before;
    } else {
        const int32_t before = fit->y;
        const int32_t after = lh - fit->y - fit->height;
        band.kind = EPD_BAND_HORIZONTAL;
        band.x = 0;
        band.width = lw;
        band.photo_at_low = (after >= before);
        band.y = band.photo_at_low ? fit->y + fit->height : 0;
        band.height = band.photo_at_low ? after : before;
    }
    return band;
}

void epd_band_plan(const epd_canvas_t *c, int32_t img_w, int32_t img_h, epd_fit_t *out_fit,
                   epd_band_t *out_band)
{
    const epd_band_t no_band = {EPD_BAND_NONE, 0, 0, 0, 0, true};
    const epd_fit_t no_fit = {0.0f, 0, 0, 0, 0};
    if (out_fit == NULL || out_band == NULL) {
        return;
    }
    *out_band = no_band;
    *out_fit = no_fit;
    if (c == NULL) {
        return;
    }

    const int32_t lw = epd_canvas_logical_width(c);
    const int32_t lh = epd_canvas_logical_height(c);
    const epd_fit_t centred = epd_fit_centre(img_w, img_h, lw, lh);
    // **Centred is the answer unless a band earns the alignment.** The operator's first requirement
    // is that the photograph is never moved aside for the band, so every path that declines a band
    // leaves this fit in place rather than an aligned one with nothing drawn in the space.
    *out_fit = centred;
    if (centred.width <= 0 || centred.height <= 0) {
        return;
    }

    const int32_t slack_w = lw - centred.width;
    const int32_t slack_h = lh - centred.height;
    // A `contain` fit leaves slack on at most one axis, so the larger is the only candidate and
    // there is nothing to fall back to when it is refused.
    const bool slack_x = (slack_w >= slack_h);
    if ((slack_x ? slack_w : slack_h) < EPD_BAND_MIN_SHORT) {
        return;
    }

    epd_align_t align = EPD_ALIGN_CENTRE;
    if (!epd_band_bottom_align(c, slack_x, &align)) {
        return; // the leftover is beside the picture, not below it: photograph only
    }

    const epd_fit_t aligned = epd_fit_align(img_w, img_h, lw, lh,
                                            slack_x ? align : EPD_ALIGN_CENTRE,
                                            slack_x ? EPD_ALIGN_CENTRE : align);
    const epd_band_t band = band_from_fit(&aligned, lw, lh, slack_x);
    if (band_short(&band) < EPD_BAND_MIN_SHORT) {
        return; // rounding took it below the floor; keep the centred fit
    }
    *out_fit = aligned;
    *out_band = band;
}

int32_t epd_band_line_chars(const epd_band_t *band, int32_t scale)
{
    if (band == NULL || band->kind == EPD_BAND_NONE || scale <= 0) {
        return 0;
    }
    // Inverted from epd_text_width()'s rule -- n * ADVANCE * scale - scale <= len -- rather than
    // approximated, so the two agree AT the boundary instead of near it.
    int32_t n = (band_long(band) + scale) / (EPD_TEXT_ADVANCE * scale);
    if (n < 0) {
        n = 0;
    }
    if (n > EPD_BAND_MAX_CHARS - 1) {
        n = EPD_BAND_MAX_CHARS - 1;
    }
    return n;
}

int32_t epd_band_large_scale(const epd_band_t *band)
{
    if (band == NULL || band->kind == EPD_BAND_NONE) {
        return 0;
    }
    int32_t s = (band_short(band) - EPD_BAND_EDGE * 2) / EPD_TEXT_GLYPH_H;
    if (s > EPD_BAND_SCALE_LARGE) {
        s = EPD_BAND_SCALE_LARGE;
    }
    // EPD_BAND_MIN_SHORT already guarantees this, so the clamp is belt and braces rather than a
    // second rule -- but a band built by hand in a test could reach here short.
    if (s < EPD_BAND_SCALE_SMALL) {
        s = EPD_BAND_SCALE_SMALL;
    }
    return s;
}

// ------------------------------------------------------------------- the battery icon

// The icon is about two thirds of the glyph height and centred across the line, rather than the
// full height of the text beside it -- the operator asked for it at its minimum size (2026-09-22),
// the same change the web page's icon got.
static int32_t icon_thick(int32_t ink)
{
    const int32_t h = ink * 2 / 3;
    return h < 6 ? (ink < 6 ? ink : 6) : h;
}

static int32_t icon_stroke(int32_t h)
{
    const int32_t s = h / 8;
    return s < 1 ? 1 : s;
}

static int32_t icon_nub(int32_t h)
{
    const int32_t s = h / 4;
    return s < 2 ? 2 : s;
}

int32_t epd_band_battery_len(int32_t ink)
{
    if (ink <= 0) {
        return 0;
    }
    const int32_t h = icon_thick(ink);
    return h * 2 + icon_nub(h);
}

// ------------------------------------------------------------------------- layout

static bool present(const char *s)
{
    return s != NULL && s[0] != '\0';
}

// Bounded copy of at most `cap` characters plus a terminator. `cap` is a character count, not a
// buffer size, because every caller here has a character budget and not a buffer question.
static void copy_bounded(char *dst, int32_t cap, const char *src)
{
    if (cap > EPD_BAND_MAX_CHARS - 1) {
        cap = EPD_BAND_MAX_CHARS - 1;
    }
    int32_t i = 0;
    if (src != NULL) {
        for (; i < cap && src[i] != '\0'; i++) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

// `a b`, or whichever of the two is present, or "".
static void join(char *dst, const char *a, const char *b)
{
    dst[0] = '\0';
    if (!present(a)) {
        copy_bounded(dst, EPD_BAND_MAX_CHARS - 1, b);
        return;
    }
    copy_bounded(dst, EPD_BAND_MAX_CHARS - 1, a);
    if (!present(b)) {
        return;
    }
    int32_t n = (int32_t)strlen(dst);
    if (n + 1 >= EPD_BAND_MAX_CHARS - 1) {
        return;
    }
    dst[n++] = ' ';
    copy_bounded(dst + n, EPD_BAND_MAX_CHARS - 1 - n, b);
}

// The ink length of one segment along the band.
static int32_t seg_len(const epd_band_layout_t *out, int32_t i)
{
    const int32_t ink = EPD_TEXT_GLYPH_H * out->scale;
    return (out->segments[i].kind == EPD_BAND_SEG_BATTERY)
               ? epd_band_battery_len(ink)
               : epd_text_width(out->segments[i].text, out->scale);
}

// What the line occupies now, gaps included -- so `used(out) + GAP + len` is what one more segment
// would cost. Computed rather than accumulated because the country splice at the end changes a
// segment's length after the fact, and a running total would then be stale.
static int32_t used_len(const epd_band_layout_t *out)
{
    int32_t used = 0;
    for (int32_t i = 0; i < out->count; i++) {
        used += seg_len(out, i) + (i > 0 ? EPD_BAND_SEG_GAP(out->scale) : 0);
    }
    return used;
}

// Adds a text segment if it fits in what is left of `along`. Absent content is not a failure to
// fit -- it simply contributes no segment.
//
// **Over-long text is refused here, not cut** -- but the one segment that can BE over-long never
// arrives over-long, because `photo_segment()` has already cut the city to fit. So this is the
// backstop for the fixed-length pieces (the room, `ago`), where a cut would produce `09-19 27` and
// the two-rung degradation in the caller is the right answer instead.
static bool push_text(epd_band_layout_t *out, int32_t along, const char *text)
{
    if (!present(text) || out->count >= EPD_BAND_MAX_SEGMENTS) {
        return false;
    }
    const int32_t want = epd_text_width(text, out->scale) +
                         (out->count > 0 ? EPD_BAND_SEG_GAP(out->scale) : 0);
    if (used_len(out) + want > along) {
        return false;
    }
    out->segments[out->count].kind = EPD_BAND_SEG_TEXT;
    copy_bounded(out->segments[out->count].text, EPD_BAND_MAX_CHARS - 1, text);
    out->segments[out->count].battery_pct = -1;
    out->count++;
    return true;
}

static bool push_battery(epd_band_layout_t *out, int32_t along, int pct)
{
    if (pct < 0 || out->count >= EPD_BAND_MAX_SEGMENTS) {
        return false;
    }
    const int32_t want = epd_band_battery_len(EPD_TEXT_GLYPH_H * out->scale) +
                         (out->count > 0 ? EPD_BAND_SEG_GAP(out->scale) : 0);
    if (used_len(out) + want > along) {
        return false;
    }
    out->segments[out->count].kind = EPD_BAND_SEG_BATTERY;
    out->segments[out->count].text[0] = '\0';
    out->segments[out->count].battery_pct = pct > 100 ? 100 : pct;
    out->count++;
    return true;
}

// The photograph's segment, with the CITY cut down until the whole thing fits `avail` pixels.
//
// **The excess is discarded with no marker** (operator, 2026-09-20). The previous rule refused an
// over-long segment outright, which on a 400 px band lost the city AND the capture date to a name of
// fourteen characters -- `RIO DE JANEIRO 19-08-14` is 548 px at scale 4 against 392 available, so the
// band fell through to the room and the photograph said nothing at all. A cut name is wrong; no name
// is worse.
//
// **The CITY is what gets cut and the date is never touched.** The date is eight characters and
// fixed, and half of `19-08-14` is unreadable where half a place name is merely a shorter word. So
// `RIO DE JANEIRO 19-08-14` becomes `RIO DE 19-08-14` at 400 px and stays whole at 800 px.
//
// No ellipsis: the operator asked for the excess discarded unconditionally, and a marker would cost
// a character of the name on the band where every character is already contested. The consequence is
// accepted rather than hidden -- a truncated name is indistinguishable from a short one, so `SAN FR`
// reads as a place, and the console's `# band seg0` line is where the full string can be checked.
static void photo_segment(char *dst, const char *city, const char *taken, int32_t scale,
                          int32_t avail)
{
    join(dst, city, taken);
    if (!present(city) || epd_text_width(dst, scale) <= avail) {
        return;
    }
    // Longest prefix of the city that still fits. Walked down rather than solved, because the width
    // is epd_text_width()'s to define and a character count would have to reproduce its rule --
    // which is exactly the duplication epd_band_line_chars() exists to avoid.
    for (int32_t n = (int32_t)strlen(city) - 1; n > 0; n--) {
        char cut[EPD_BAND_MAX_CHARS];
        copy_bounded(cut, n, city);
        // A cut landing on a space would leave a trailing one, which draws as a gap before the date
        // and reads as a missing word rather than a shortened one.
        if (cut[n - 1] == ' ') {
            continue;
        }
        join(dst, cut, taken);
        if (epd_text_width(dst, scale) <= avail) {
            return;
        }
    }
    // Not one character of the city fits beside the date. The date alone is still the photograph
    // saying something, which is the whole point of cutting rather than refusing.
    join(dst, NULL, taken);
}

// `a b c`, skipping whichever are absent. The country splice needs three pieces in one string and
// join() only takes two.
static void join3(char *dst, const char *a, const char *b, const char *c)
{
    char head[EPD_BAND_MAX_CHARS];
    join(head, a, b);
    join(dst, head[0] != '\0' ? head : NULL, c);
}

void epd_band_layout(const epd_band_t *band, const epd_band_content_t *content,
                     epd_band_layout_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (band == NULL || band->kind == EPD_BAND_NONE || content == NULL) {
        return;
    }

    // One scale for the whole line, following the band's thickness and capped at 4 -- a thin band
    // gets smaller text rather than nothing (operator, 2026-09-19). A band THICKER than one line
    // keeps its white: the type does not grow to fill it, which is the other half of the same
    // decision and the reason there is still a cap.
    out->scale = epd_band_large_scale(band);
    const int32_t along = band_long(band) - EPD_BAND_EDGE * 2;

    // A fault owns the band while it lasts.
    if (present(content->fault)) {
        push_text(out, along, content->fault);
        return;
    }

    // 2. The photograph, WITHOUT the country. The country comes back at the end if the whole line
    //    still has room, which is what makes it the first thing dropped rather than the last.
    char photo[EPD_BAND_MAX_CHARS];
    photo_segment(photo, content->city, content->taken, out->scale, along);
    push_text(out, along, photo);

    // 3. The room, degrading to the date alone. No promotion: with one line there is no larger line
    //    to promote into, and a photograph with nothing to say simply leaves the room first.
    char room[EPD_BAND_MAX_CHARS];
    join(room, content->now, content->climate);
    if (!push_text(out, along, room)) {
        push_text(out, along, content->now);
    }

    // 4 and 5.
    push_battery(out, along, content->battery_pct);
    push_text(out, along, content->ago);

    // 6. The country, spliced into segment 0 -- and only if the line as a whole still has room for
    //    the longer string. `used_len()` is recomputed rather than adjusted because the splice
    //    changes a length that earlier gaps were measured against.
    if (present(content->country) && present(content->city) && out->count > 0 &&
        out->segments[0].kind == EPD_BAND_SEG_TEXT &&
        strcmp(out->segments[0].text, photo) == 0) {
        char with_country[EPD_BAND_MAX_CHARS];
        join3(with_country, content->city, content->country, content->taken);
        const int32_t grew = epd_text_width(with_country, out->scale) -
                             epd_text_width(out->segments[0].text, out->scale);
        if (used_len(out) + grew <= along) {
            copy_bounded(out->segments[0].text, EPD_BAND_MAX_CHARS - 1, with_country);
        }
    }
}

// --------------------------------------------------------------------------- draw

// A rectangle in BAND-LINE space -- `along` runs the line's reading direction from its start,
// `across` its thickness from the edge nearest the photograph -- mapped into logical canvas
// coordinates.
//
// **One mapping, and it matches epd_text.c's rot90 exactly**: turned, the line's own axis becomes
// logical y and its thickness runs towards -x, so `across` counts down from the box's far edge. A
// second copy of that reasoning is how the icon ends up upside down beside text that is not.
static void band_fill_rgb(epd_canvas_t *c, bool rot, int32_t bx, int32_t by, int32_t ink,
                          int32_t along, int32_t across, int32_t len, int32_t thick, uint8_t r,
                          uint8_t g, uint8_t b)
{
    if (len <= 0 || thick <= 0) {
        return;
    }
    if (rot) {
        epd_canvas_fill_rect(c, bx + (ink - across - thick), by + along, thick, len, r, g, b);
    } else {
        epd_canvas_fill_rect(c, bx + along, by + across, len, thick, r, g, b);
    }
}

static void band_fill(epd_canvas_t *c, bool rot, int32_t bx, int32_t by, int32_t ink, int32_t along,
                      int32_t across, int32_t len, int32_t thick)
{
    band_fill_rgb(c, rot, bx, by, ink, along, across, len, thick, 0, 0, 0);
}

static void draw_battery(epd_canvas_t *c, bool rot, int32_t bx, int32_t by, int32_t ink,
                         int32_t along0, int pct)
{
    const int32_t h = icon_thick(ink);
    const int32_t off = (ink - h) / 2; // centred across the line
    const int32_t stroke = icon_stroke(h);
    const int32_t body = h * 2;
    const int32_t nub = icon_nub(h);
    int32_t nub_thick = h / 2;
    if (nub_thick < 2) {
        nub_thick = 2;
    }

    // Outline, then the terminal nub: four sides rather than a filled rect with a white inner,
    // because the band's surround is white only until something else draws on it.
    band_fill(c, rot, bx, by, ink, along0, off, body, stroke);
    band_fill(c, rot, bx, by, ink, along0, off + h - stroke, body, stroke);
    band_fill(c, rot, bx, by, ink, along0, off, stroke, h);
    band_fill(c, rot, bx, by, ink, along0 + body - stroke, off, stroke, h);
    band_fill(c, rot, bx, by, ink, along0 + body, off + (h - nub_thick) / 2, nub, nub_thick);

    // The fill IS the reading -- there is no number and no '%' glyph (operator, 2026-09-19). One
    // stroke of clear air inside the outline so a full battery does not read as a solid slab.
    //
    // Red at a third or less, as on the web page (operator, 2026-09-22). The band is drawn before
    // the quantise, and pure red comes back as the red ink on every palette through both nearest
    // passes -- `dither_diffuse`'s pack (the default) and the Nearest row path. With
    // `dither_diffuse` off the ordered pair search can speckle it (`spectra6` pairs it with
    // yellow), as it does the black text; test_band has the measurement.
    const int32_t inner = body - stroke * 4;
    const int32_t inner_thick = h - stroke * 4;
    if (inner > 0 && inner_thick > 0 && pct > 0) {
        int32_t len = inner * pct / 100;
        if (len < 1) {
            len = 1; // 1 % must not read as 0 %
        }
        const uint8_t r = pct <= EPD_BAND_BATTERY_LOW_PCT ? 255 : 0;
        band_fill_rgb(c, rot, bx, by, ink, along0 + stroke * 2, off + stroke * 2, len, inner_thick,
                      r, 0, 0);
    }
}

void epd_band_draw(epd_canvas_t *c, const epd_band_t *band, const epd_band_layout_t *layout)
{
    if (c == NULL || band == NULL || layout == NULL || band->kind == EPD_BAND_NONE ||
        layout->count <= 0) {
        return;
    }

    const bool rot = (band->kind == EPD_BAND_VERTICAL);
    const int32_t long_len = band_long(band);
    const int32_t scale = layout->scale > 0 ? layout->scale : EPD_BAND_SCALE_SMALL;
    const int32_t ink = EPD_TEXT_GLYPH_H * scale;

    // EPD_BAND_EDGE from the photograph's own side of the band, not centred in it. On a 30 px band
    // the two are the same; on a 200 px one this is the difference between a caption under the
    // picture and a label adrift in the margin.
    const int32_t across = EPD_BAND_EDGE;
    int32_t bx;
    int32_t by;
    if (rot) {
        bx = band->photo_at_low ? band->x + across : band->x + band->width - across - ink;
        by = band->y;
    } else {
        bx = band->x;
        by = band->photo_at_low ? band->y + across : band->y + band->height - across - ink;
    }

    // Space-between. Fitting used the MINIMUM gap, so whatever is left over is surplus and gets
    // divided equally; one segment has no "between" and is centred instead.
    int32_t total = 0;
    for (int32_t i = 0; i < layout->count; i++) {
        total += seg_len(layout, i);
    }
    int32_t gap = EPD_BAND_SEG_GAP(scale);
    int32_t start = EPD_BAND_EDGE;
    if (layout->count == 1) {
        start = (long_len - total) / 2;
        if (start < 0) {
            start = 0;
        }
    } else {
        const int32_t free_len = long_len - EPD_BAND_EDGE * 2 - total;
        const int32_t even = free_len / (layout->count - 1);
        if (even > gap) {
            gap = even;
        }
    }

    int32_t along = start;
    for (int32_t i = 0; i < layout->count; i++) {
        const int32_t len = seg_len(layout, i);
        if (layout->segments[i].kind == EPD_BAND_SEG_BATTERY) {
            draw_battery(c, rot, bx, by, ink, along, layout->segments[i].battery_pct);
        } else if (rot) {
            epd_text_draw_rot90(c, bx, by + along, scale, layout->segments[i].text, 0, 0, 0);
        } else {
            epd_text_draw(c, bx + along, by, scale, layout->segments[i].text, 0, 0, 0);
        }

        // The divider rule, in the middle of the gap that follows. Drawn before `along` advances
        // past it so the arithmetic is the gap's own rather than the next segment's.
        if (i + 1 < layout->count) {
            const int32_t stroke = scale / 2 < 1 ? 1 : scale / 2;
            const int32_t rule = along + len + gap / 2 - stroke / 2;
            if (rot) {
                epd_canvas_fill_rect(c, bx, by + rule, ink, stroke, 0, 0, 0);
            } else {
                epd_canvas_fill_rect(c, bx + rule, by, stroke, ink, 0, 0, 0);
            }
        }
        along += len + gap;
    }
}
