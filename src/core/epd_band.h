// The band a matte leaves beside a photograph, and what gets written in it.
//
// A `contain` fit leaves blank panel on one axis. Centred, that blank is TWO thin strips and two
// thin strips hold nothing -- a 4:3 photograph on 600x400 leaves 33 px either side and the 5x7
// font is not readable at 33 px. Pushed to one edge (`epd_fit_align`, epd_dither.h) the same
// leftover is one block, which holds a line of text. This module is the block and the text: where
// it is, what fits ALONG it, which content wins that length, and the draw.
//
// **It is ONE line since 2026-09-19** and the content sits side by side in segments. It used
// to be up to four lines stacked across the band's short axis; `epd_band_layout_t`'s comment
// has the defect that change removed.
//
// Ticket 64. **The rule changed on 2026-09-19 after the operator looked at five samples on the
// glass**, and the change is the whole of what makes this module's geometry non-obvious:
//
//   **The band sits along the BOTTOM OF THE GLASS, or there is none and the photograph is centred.**
//
// Before that it went wherever the leftover was, which put it down the right edge for a 9:16 source
// and across the top for a turned 4:3 one. Both were rejected: the point is to use space the
// photograph did not want, never to look as though the photograph was moved aside for it.
//
// **Logical is not physical, and conflating them is the trap here.** Everything below is in LOGICAL
// canvas coordinates, because that is how the rest of the render pipeline works (see
// `docs/agents/render-pipeline.md` on the diffusion's walk). On a canvas that `auto_rotate` has
// turned, the band that lies along the bottom of the glass is a logically VERTICAL strip, and its
// text is drawn turned 90° precisely so that the canvas's own rotation brings it back upright for a
// viewer. So `EPD_BAND_VERTICAL` does not mean "vertical on the glass" -- it means the band runs
// along the logical Y axis, and what that looks like depends on the rotation.
//
// **Everything here is pure**: canvas geometry in, glyph rectangles out, no ESP-IDF and no clock.
// The content arrives as already-formatted pieces, so the part that can be wrong silently -- which
// segments survive the band's length, what happens when a name is too long -- is host-testable
// without a panel, an RTC or a sensor. `.scratch/digital-frame/band_patterns.c` links against this
// module and prints every pattern it can produce, which is how the 36-53 px defect was found.
//
// Two things deliberately NOT here. The band is never drawn on its own schedule: every piece of
// content changes no more often than the photograph does, so the band rides the photograph's own
// refresh (one is 15,014.6 ms; a per-minute redraw would have the frame flashing a quarter of the
// time). And nothing here reads a sensor or a clock -- `app_display.c` gathers the content.

#ifndef EPD_BAND_H
#define EPD_BAND_H

#include <stdbool.h>
#include <stdint.h>

#include "epd_canvas.h"
#include "epd_dither.h"
#include "epd_text.h"

// The readable ceiling and floor, in `epd_text` scale units. A 4.0" 400x600 panel is about 180 ppi,
// so scale 4 puts 28 px of ink at roughly 3.9 mm of cap height and scale 2 puts 14 px at 2.0 mm.
//
// **The large scale is a CEILING, not a constant** (operator, 2026-09-19: size the text to the
// leftover). A band too thin for scale 4 gets scale 3 or 2 rather than nothing, which is what lets
// the reTerminal E1002's 30 px band carry a line at all. Scale 1 is not a candidate: 7 px is about
// 1 mm here and nobody can read it, so a band too thin for scale 2 draws nothing.
#define EPD_BAND_SCALE_LARGE 4
#define EPD_BAND_SCALE_SMALL 2

// Between the ink and each edge of the band. **EPD_BAND_LINE_GAP went with the stacked layout**
// (2026-09-19): with one line there is nothing to put a gap between, and leaving the constant would
// have been a number nothing read.
#define EPD_BAND_EDGE 4

// The shortest band that can carry anything, which follows from the floor above rather than being a
// number of its own: 4 + 14 + 4.
#define EPD_BAND_MIN_SHORT (EPD_BAND_EDGE * 2 + EPD_TEXT_GLYPH_H * EPD_BAND_SCALE_SMALL)

// Four candidates -- the photograph, the room, the battery, the elapsed years -- so four segments
// is the most one line can hold however long the band is.
#define EPD_BAND_MAX_SEGMENTS 4
// The longest segment ever composed is a city name, a country and a date, and the longest ASCII
// place name in the table is around 25 characters. 48 is that with room, not a capacity: what a
// segment may actually hold is the BAND's, computed by epd_band_line_chars().
#define EPD_BAND_MAX_CHARS 48

// The smallest space between two segments, and what the fit is computed against. One glyph pitch,
// so it reads as a word space with the divider rule standing in it. The DRAW then spreads whatever
// length is left over across the same gaps, so a band with room to spare looks deliberate rather
// than left-aligned with a tail of white.
#define EPD_BAND_SEG_GAP(scale) (EPD_TEXT_ADVANCE * (scale))

typedef enum {
    EPD_BAND_NONE = 0,   // the photograph fills the glass, or the leftover cannot be used
    EPD_BAND_VERTICAL,   // runs along the LOGICAL y axis; text turned 90°
    EPD_BAND_HORIZONTAL, // runs along the LOGICAL x axis; text upright
} epd_band_kind_t;

typedef struct {
    epd_band_kind_t kind;
    int32_t x, y, width, height; // logical canvas coordinates
    // Which side of the band the photograph is on, along the band's SHORT axis. The line is placed
    // EPD_BAND_EDGE from that side rather than centred in the band, so that on a generous band it
    // reads as a caption under the picture instead of a label floating in the margin. This field is
    // the whole reason that is possible, and it is the only thing that still reads it.
    bool photo_at_low;
} epd_band_t;

// Which end of a logical axis the leftover has to sit at for the band to lie along the BOTTOM OF
// THE GLASS -- or false when that axis does not run along the glass's vertical axis at all, in
// which case the leftover would be beside the picture and there must be no band.
//
// **Asked of the canvas, never tabulated per rotation.** `epd_canvas.h` says outright that a second
// copy of the rotation mapping in a second file is what it exists to prevent, and
// `epd_canvas_physical_rect()` IS that mapping. A probe also cannot be wrong per board the way a
// constant can -- rotation 0 is portrait on one of this project's panels and landscape on the other,
// and ticket 62 item 4 is the account of what believing otherwise costs.
//
// `slack_x` says which logical axis the fit left slack on.
bool epd_band_bottom_align(const epd_canvas_t *c, bool slack_x, epd_align_t *out);

// **The fit and the band decided together, which is the point of this function.** The band's
// existence decides how the photograph is aligned, and the photograph's fit decides where the band
// is; computing them separately is what put the band on the wrong edge of the glass before.
//
// `*out_fit`'s WIDTH and HEIGHT are always `epd_fit_centre()`'s for the same arguments -- only x and
// y move, and with no band they are `epd_fit_centre()`'s too. The photograph is never made smaller
// to make room, which is the operator's first requirement and the one thing here that must not
// change: `choose_jpeg_scale()` and `epd_fit_reduction()` are defined against the fit's size and
// ticket 56 is what changing it costs.
void epd_band_plan(const epd_canvas_t *c, int32_t img_w, int32_t img_h, epd_fit_t *out_fit,
                   epd_band_t *out_band);

// How many glyphs of `scale` fit along the band's long axis. Derived from epd_text_width()'s own
// rule -- the trailing inter-glyph gap is not ink -- so the two cannot disagree: 400 px at scale
// 4 is 16 characters, and at scale 2 it is 33.
int32_t epd_band_line_chars(const epd_band_t *band, int32_t scale);

// The largest scale whose one line fits the band's short side, clamped to
// [EPD_BAND_SCALE_SMALL, EPD_BAND_SCALE_LARGE]. 0 when there is no band.
int32_t epd_band_large_scale(const epd_band_t *band);

// The length along the line that a battery icon of this line thickness occupies, so the layout can
// centre it without knowing how the icon is drawn.
int32_t epd_band_battery_len(int32_t ink);

// At or below this charge the icon's fill is red rather than black, matching the web page.
#define EPD_BAND_BATTERY_LOW_PCT 33

// The pieces, already formatted by whoever owns each producer. Every string may be NULL or "".
//
// They are pieces rather than finished lines because the overflow ladder has to be able to take
// them apart: `city` and `taken` share a line when they fit and are separated when they do not,
// and a rule that could only accept or reject a finished string would drop one of them.
typedef struct {
    // Something is persistently wrong. Takes the large line ALONE and suppresses every other
    // layer -- the band stops being about the photograph while the frame needs attention.
    // Empty in normal operation, which is the same rule as ticket 55's fault LED: a status line
    // that is always present is one nobody reads.
    const char *fault;

    // From the photograph itself, via the import-time sidecar. Absent for anything imported
    // before that existed, for uploads, and wherever the source carried no EXIF.
    //
    // **`city` and `country` are separate because the country is the FIRST thing dropped**
    // (operator, 2026-09-19). It is the least informative piece and the only one whose removal can
    // rescue a whole later segment: `TOKYO JAPAN` is 11 characters and `TOKYO` is 5, and on the
    // M5Paper Color's 400 px band those six characters are the difference between the room's line
    // appearing and not. So the layout composes WITHOUT the country and splices it back in at the
    // end if everything else already fits -- which is why they cannot arrive pre-joined.
    //
    // An abbreviation (`JP`, ISO 3166-1) was evaluated the same day and NOT taken: it saves three
    // characters where dropping saves six, and it changes no outcome at all in the two commonest
    // bands -- 400 px at scale 4, where `TOKYO JP 19-08-14` is still 17 against a capacity of 16,
    // and 800 px at scale 3, where the full country already fits. `JAP` is a racial slur and was
    // never a candidate. Ticket 64 carries the arithmetic.
    const char *city;    // "TOKYO"
    const char *country; // "JAPAN" -- dropped first
    const char *taken;   // "19-08-14"
    const char *ago;     // "7 YEARS AGO"

    // From the room. `climate` uses EPD_TEXT_DEGREE and '%', both drawable since 2026-09-19.
    const char *now;     // "09-19"
    const char *climate; // "23\xB0C 45%"

    // Charge, 0-100, or **negative for unknown**, which is not the same as 0: a board that cannot
    // read its cell must draw no icon rather than an empty one. Drawn as an icon and nothing else
    // (operator, 2026-09-19) -- there is no '%' glyph in the 5x7 set and the fill IS the reading.
    int battery_pct;
} epd_band_content_t;

typedef enum {
    EPD_BAND_SEG_TEXT = 0,
    EPD_BAND_SEG_BATTERY,
} epd_band_seg_kind_t;

// **ONE line, filled ALONG the band, since 2026-09-19.** It used to be up to four lines stacked
// across the band's short axis; the operator replaced that with a single line whose content sits
// side by side, and a band thicker than one line keeps its white rather than growing the type.
//
// That change is not cosmetic -- it removes a defect by construction. The stacked version chose a
// rung of its overflow ladder by whether text fitted a line's WIDTH, then spilled the remainder
// onto a line below; in a band 36-53 px thick exactly one line fits, so the spill went nowhere and
// the capture date vanished. A 30 px band showed the city AND the date while a 46 px band showed
// only the city. With one line there is no "below", so there is nothing to spill into and nothing
// to lose silently.
typedef struct {
    int32_t count;
    // One scale for the whole line -- it follows the band's thickness (epd_band_large_scale) and
    // every segment shares it, so the line reads as one caption rather than a ransom note.
    int32_t scale;
    struct {
        epd_band_seg_kind_t kind;
        char text[EPD_BAND_MAX_CHARS]; // empty for the battery segment
        int32_t battery_pct;
    } segments[EPD_BAND_MAX_SEGMENTS];
} epd_band_layout_t;

// Composes ONE line of segments for `band` from `content`, in priority order, stopping when the
// band's LONG axis is full. `out->count` is 0 for a band of EPD_BAND_NONE or content with nothing
// in it.
//
// The order, and it is the decision rather than an implementation detail:
//
//  1. A fault, alone, the whole line. The band stops being about the photograph while the frame
//     needs attention.
//  2. The photograph: `city` and `taken`, **without the country**. **A city too long for the band is
//     CUT and the excess discarded, with no marker** (operator, 2026-09-20) -- and the CITY is what
//     is cut, never the date. Refusing it instead lost the city and the capture date together on a
//     400 px band, which is what a fourteen-character name did before.
//  3. The room: `now` and `climate`, degrading to `now` alone when the pair will not fit. Nothing
//     is promoted any more -- with one line there is no large line to promote INTO, and the room
//     simply becomes the first segment when the photograph has nothing.
//  4. The battery icon.
//  5. `ago`. **It does not fit any band this project currently produces** -- 800 px at scale 3
//     leaves about four characters once the three above are placed and `7 YEARS AGO` is twelve --
//     so it is last, cheap, and honest about being aspirational rather than deleted.
//  6. **The country, spliced back into segment 2 if the whole line still has room.** Last because
//     it is the least informative piece; see `epd_band_content_t`.
//
// Fitting uses EPD_BAND_SEG_GAP between segments, which is the minimum; epd_band_draw() spreads
// the surplus.
void epd_band_layout(const epd_band_t *band, const epd_band_content_t *content,
                     epd_band_layout_t *out);

// Draws `layout` into `band`, turned or upright according to `band->kind`. Pure black, because
// after the region's diffusion the surround goes straight to the pack's nearest match and a
// palette entry comes back exactly; it is the caller's job to draw BEFORE that pack and to know
// that the row-quality path (dither_diffuse off) quantises the band along with everything else.
//
// **Justification is space-between along the band**: the first segment sits EPD_BAND_EDGE from the
// low end, the last the same distance from the high end, and the surplus is divided equally between
// them. A single segment is centred instead, because space-between has no meaning for one. A
// divider rule stands in the middle of each gap, which is what makes three segments read as three
// fields rather than one run-on line.
void epd_band_draw(epd_canvas_t *c, const epd_band_t *band, const epd_band_layout_t *layout);

#endif // EPD_BAND_H
