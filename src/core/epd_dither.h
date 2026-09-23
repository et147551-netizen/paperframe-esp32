// RGB888 -> Spectra 6 palette indices, packed two pixels per byte.
//
// This is where colour correctness is decided, and colour is the only thing a viewer
// actually sees. Every photograph the frame ever displays passes through here: the web
// UI uploads full-colour PNGs and does its quantisation only for the on-screen preview
// (docs/requirements/digital-frame.md, "Where colour reduction actually happens"), so
// there is no path where the device receives an already-reduced image.
//
// Nothing here may depend on ESP-IDF. Like epd_format.c, this builds under env:native so
// the arithmetic is tested on the host rather than judged by eye on a 12-second refresh.
//
// The algorithms are ports of M5GFX's, not new inventions -- see
// refs/M5GFX/src/lgfx/v1/panel/Panel_ED2208.cpp:72-230, which carries LovyanGFX's FreeBSD
// (BSD-2-Clause) header, (c) lovyan03, inside the MIT-licensed M5GFX repository, (c) 2021
// M5Stack. Ticket 65 item 3 owns replacing it. That is deliberate: it is what
// this panel's own library ships, it is what a user comparing against the stock firmware
// will see, and matching it means any difference in the output is attributable to a bug
// in this code rather than to a different design.

#ifndef EPD_DITHER_H
#define EPD_DITHER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The six colours this panel renders, as ideal RGB. Copied verbatim from
// Panel_ED2208.cpp:45-52 so output matches the stock firmware.
//
// Index 4 is orange -- valid on the 7.3" part, NOT on this one -- and index 7 is unused.
// Neither appears in this table, so neither can ever be emitted.
#define EPD_PALETTE_COUNT 6

typedef struct {
    uint8_t r, g, b;
    uint8_t index;
} epd_palette_entry_t;

extern const epd_palette_entry_t EPD_PALETTE[EPD_PALETTE_COUNT];

// The same six colours as the panel actually SHOWS them, which is not what it is sent.
// Measured on this unit in ticket 20; see epd_dither.c for how the numbers were made and
// what they are worth. Entry 0 must be black and entry 1 white in any palette used here
// -- tone compression reads the reachable range from those two.
extern const epd_palette_entry_t EPD_PALETTE_MEASURED[EPD_PALETTE_COUNT];

// The module manual's own optical figures for all six, L*a*b* converted to sRGB. Same
// white and black as EPD_PALETTE_MEASURED -- they are where that table's anchors came
// from -- and the vendor's numbers for the four chromatic entries instead of this
// camera's. Trying both is what separates "compress into the panel's range" from "our
// camera's estimate of four colours".
extern const epd_palette_entry_t EPD_PALETTE_MANUAL[EPD_PALETTE_COUNT];

// ------------------------------------------------- epdoptimize's calibrated palettes
//
// Five independent calibrations of the same six Spectra 6 colours, from
// refs/epdoptimize/src/dither/data/default-palettes.json (Apache-2.0). Ported rather than
// measured again: see docs/measurements.md for what this project's own colour
// instruments could and could not say.
//
// **The order of these tables is load-bearing and is not the order of EPD_PALETTE.**
// epdoptimize sorts every palette into a canonical role order before matching
// (palette-order.ts:1-27) -- black, white, blue, green, red, yellow -- and its matcher
// keeps the first entry on a distance tie, exactly as epd_nearest_index_cfg() does. Two
// palettes that hold the same six colours in different orders therefore quantise a
// tie-breaking pixel to different colours. Reordering these is a behaviour change, not
// tidying.
//
// Worth knowing before choosing between them: every one of these whites is lighter than
// the EL040EF1 module manual's own figure for this panel (154,164,162), so they assume a
// wider reachable range than EPD_PALETTE_MANUAL does. That is the substance of the
// difference, not the four chromatic entries.

// The palette epdoptimize's own demo selects by default, and so the one behind the
// pictures at https://paperlesspaper.github.io/epdoptimize.
extern const epd_palette_entry_t EPD_PALETTE_EPDOPT_AITJCIZE[EPD_PALETTE_COUNT];
// The library's current general-purpose Spectra 6 calibration.
extern const epd_palette_entry_t EPD_PALETTE_EPDOPT_SPECTRA6[EPD_PALETTE_COUNT];
// Its predecessor, kept upstream under the name "spectra6legacy".
extern const epd_palette_entry_t EPD_PALETTE_EPDOPT_LEGACY[EPD_PALETTE_COUNT];
// A third party's calibration, upstream as "spectra6-boeber".
extern const epd_palette_entry_t EPD_PALETTE_EPDOPT_BOEBER[EPD_PALETTE_COUNT];
// No calibration at all: the six ideal primaries, in the canonical role order. Upstream's
// "spectra6-original", and the control arm -- it is what the other four are corrections to.
extern const epd_palette_entry_t EPD_PALETTE_EPDOPT_ORIGINAL[EPD_PALETTE_COUNT];

// How to turn a pixel into an index. Two decisions, and they belong together: matching
// against measured appearance while feeding the matcher an uncompressed 0-255 range
// would compare colours in one space against colours in another.
typedef struct {
    const epd_palette_entry_t *palette;
    // How far to map the input range onto what the panel can reach: 0 leaves it alone,
    // 255 maps input black exactly onto the panel's black and input white onto its
    // white, and anything between is a blend of the two.
    //
    // The panel's white is a mid grey and its black is not black, so at 0 the top and
    // bottom of every photograph clip into the ends and their gradation is lost. At 255
    // all of it comes back, and saturated areas give up some punch because they now sit
    // nearer the boundaries between palette entries and start to dither. The middle is
    // a real choice between those, not a hedge.
    uint8_t tone;
} epd_render_t;

#define EPD_TONE_NONE 0
#define EPD_TONE_HALF 128
#define EPD_TONE_FULL 255

// What every render did before ticket 19: device colours, no compression.
extern const epd_render_t EPD_RENDER_STOCK;
// Measured appearance, and three amounts of compression into it.
extern const epd_render_t EPD_RENDER_MEASURED_NONE;
extern const epd_render_t EPD_RENDER_MEASURED_HALF;
extern const epd_render_t EPD_RENDER_MEASURED;
// The manual's colours, compressed the same way.
extern const epd_render_t EPD_RENDER_MANUAL;

// epdoptimize's aitjcize calibration at full compression, and what the frame ships.
extern const epd_render_t EPD_RENDER_EPDOPT;

// What the frame renders photographs with when the user has not chosen otherwise. It is
// EPD_RENDER_EPDOPT as of 2026-09-05; ticket 19 had chosen EPD_RENDER_MANUAL.
//
// Ticket 19's finding stands as far as it went: the win was the tone compression, not the
// palette measurement -- compression depends only on white and black, and the camera's
// estimates of the four chromatic colours made the picture worse. What it lacked was a
// candidate calibrated by somebody else.
//
// **Why this one, and what is still unresolved.** On the glass, four of the five palettes
// send 17-22 % of a photograph's pixels to green, because every *calibrated* green is a dark
// desaturated colour -- which is what this panel's green actually is -- and dark brown lands
// nearer to it than to black. EPD_PALETTE_EPDOPT_ORIGINAL is the only one that does not
// (1.3 % green), and it buys that by matching against a pure (0,255,0) the panel cannot
// produce: it pushes 41.3 % of the image to black and loses shadow detail with it. So the
// choice between them is a trade, it depends on the picture, and it is the user's --
// hence the palette is a **setting** (app_settings.h) rather than a constant, and this is
// only the default. docs/measurements.md has the scans and the index histograms.
//
// New code should pass a cfg. The two plain wrappers below still mean EPD_RENDER_STOCK,
// so nothing renders differently by accident; they are the old spelling, not the default.
extern const epd_render_t EPD_RENDER_DEFAULT;

// ---------------------------------------------------------------- selectable palettes
//
// FR-8 has no palette setting -- this is an addition, because no single palette is right for
// every photograph and the difference is plainly visible. The identifiers are the wire
// format: they go into NVS and across the HTTP API, so **they are not free to rename**.
//
// Order is the order the Web UI offers them, best-general-purpose first.
typedef enum {
    EPD_PALETTE_ID_AITJCIZE = 0,
    EPD_PALETTE_ID_ORIGINAL,     // no calibration: cleanest neutrals, crushed shadows
    EPD_PALETTE_ID_MANUAL,       // ticket 19's choice, the module manual's own figures
    EPD_PALETTE_ID_SPECTRA6,
    EPD_PALETTE_ID_LEGACY,
    EPD_PALETTE_ID_BOEBER,       // the default app_settings starts from
    EPD_PALETTE_ID_STOCK,        // device primaries, and the only one with no compression
    EPD_PALETTE_ID_COUNT,
} epd_palette_id_t;

// The wire name, or NULL for an out-of-range id.
const char *epd_palette_id_name(epd_palette_id_t id);

// A one-line description, for the Web UI to show beside the name. The UI does not hold
// copies of these: a palette added here appears in the interface without touching the HTML.
const char *epd_palette_id_label(epd_palette_id_t id);

// Wire name to id. False leaves `out` untouched, which is what turns an unknown value from
// an HTTP body into a 400 rather than a silent fallback to the default.
bool epd_palette_id_from_name(const char *name, epd_palette_id_t *out);

// The render config for an id. An out-of-range id yields EPD_RENDER_DEFAULT, because a
// corrupt NVS entry should render photographs rather than refuse to.
epd_render_t epd_render_for_palette(epd_palette_id_t id);

// Strength of the ordered bias in the dithered path. 140 is what M5GFX uses for
// epd_quality (Panel_ED2208.cpp:438), which is the mode the stock firmware displays
// photographs in.
#define EPD_DITHER_STRENGTH_QUALITY 140

// Nearest palette entry to one colour, by squared RGB distance. Ties go to the earlier
// palette entry, matching M5GFX's strict `<` comparison.
uint8_t epd_nearest_index(int32_t r, int32_t g, int32_t b);

// Best pair of palette entries for two adjacent pixels, returned already packed as
// (hi << 4) | lo.
//
// This is not two independent nearest lookups. It searches all 36 combinations and
// scores each by the error of the *pair summed together* plus each pixel's own squared
// error. The summed term lets two pixels average out to a colour neither can reach
// alone, which is what produces the texture on a Spectra 6 panel; the individual term
// keeps hue information when pairs tie on the summed term, without which a dusty orange
// collapses to black-and-white (the comment at Panel_ED2208.cpp:105-107 records that
// this was a real failure).
uint8_t epd_pair_index(int32_t r0, int32_t g0, int32_t b0,
                       int32_t r1, int32_t g1, int32_t b1);

// ------------------------------------------------------------------ row conversion
//
// Both take one row of RGB888 (3 bytes per pixel, r first) and write ceil(width/2)
// packed bytes. `width` is the pixel count; an odd width pads the final low nibble with
// white, as M5GFX does.

// No dithering: each pixel independently mapped to its nearest palette entry. This is
// the `imageN` path -- what the user picked "Nearest" for. Sharper, and it loses
// gradation.
void epd_dither_row_none(const uint8_t *src_rgb, uint8_t *dst, size_t width);

// Ordered bias, then the pair search. This is the `imaged` path and the default: better
// detail, at the cost of visible noise.
//
// `y` is the row index -- the bias pattern advances with it, so passing a constant
// produces banding. `strength` is EPD_DITHER_STRENGTH_QUALITY unless you are
// experimenting.
void epd_dither_row_quality(const uint8_t *src_rgb, uint8_t *dst, size_t width, size_t y,
                            uint8_t strength);

// The same two, against a chosen palette and compression. The pair above are these with
// EPD_RENDER_STOCK, kept so that every existing caller and test keeps its old behaviour
// until it deliberately asks for the new one.
void epd_dither_row_none_cfg(const uint8_t *src_rgb, uint8_t *dst, size_t width,
                             const epd_render_t *cfg);
void epd_dither_row_quality_cfg(const uint8_t *src_rgb, uint8_t *dst, size_t width,
                                size_t y, uint8_t strength, const epd_render_t *cfg);

// Nearest entry within a given palette. `epd_nearest_index` is this with EPD_PALETTE.
uint8_t epd_nearest_index_cfg(const epd_palette_entry_t *palette, int32_t r, int32_t g,
                              int32_t b);

// Applies `cfg->tone` to a rectangle in place, and nothing else -- no matching, no packing.
//
// The row conversions above fold this into their own loop, which is right when the quantiser is
// the row path. The error diffusion (epd_diffuse.h) cannot: it leaves the canvas holding exact
// palette colours, so the pack after it has to run at EPD_TONE_NONE, and any integer compression
// the plan still owes has to happen *before* the diffusion instead. epd_auto_row_tone() decides
// whether it is owed; this applies it. EPD_TONE_NONE does nothing at all.
//
// Addressing is the same affine form epd_diffuse_rect() takes, so a rotated region works: pixel
// (x, y) is `origin + x*step_x + y*step_y`. Use epd_canvas_logical_walk() to get the three values.
void epd_dither_tone_rect(uint8_t *origin, int32_t w, int32_t h, ptrdiff_t step_x,
                          ptrdiff_t step_y, const epd_render_t *cfg);

// The same search, returning the entry rather than its index -- the error-diffusion stage
// (epd_diffuse.h) needs the chosen colour's RGB in order to compute the error it introduced,
// and this is the one place the tie-break rule lives. Never NULL for a non-NULL palette.
const epd_palette_entry_t *epd_nearest_entry_cfg(const epd_palette_entry_t *palette, int32_t r,
                                                 int32_t g, int32_t b);

// ------------------------------------------------------------------ fit and centre
//
// Scale an image to fit inside the screen preserving aspect ratio, then centre it
// (FR-5.3). Lives here rather than in the canvas because it is ESP-IDF-free and is the
// single most common source of off-by-one framing errors -- which makes it worth having
// under test rather than worth writing twice.

typedef struct {
    float scale;   // multiply source dimensions by this
    int32_t x;     // left edge of the drawn image, in screen coordinates
    int32_t y;     // top edge
    int32_t width; // drawn size after scaling
    int32_t height;
} epd_fit_t;

// Which end of an axis the drawn image is pushed to, on whichever axis the fit leaves slack.
// The fit is `contain`, so at most one axis has slack and the other's alignment cannot act.
//
// Why this exists: centring splits the leftover into TWO bands, and two thin bands hold
// nothing. A 4:3 photograph on 600x400 leaves 33 px either side, and the font cannot be read
// at 33 px. Pushed to one edge the same leftover is one 67x400 block, which holds a line of
// text. Ticket 64; the matte-band plan has the geometry for every aspect ratio.
typedef enum {
    EPD_ALIGN_CENTRE = 0, // slack split in two -- what every caller did before 2026-09-19
    EPD_ALIGN_LOW,        // image to x=0 / y=0, so the band is on the right or at the bottom
    EPD_ALIGN_HIGH,       // image to the far edge, so the band is on the left or at the top
} epd_align_t;

// `epd_fit_centre()` with the slack pushed to one end. **The scale, width and height are
// identical to epd_fit_centre()'s for the same arguments** -- only x and y move. That is
// load-bearing: `choose_jpeg_scale()` and `epd_fit_reduction()` are defined against the fit's
// SIZE, and ticket 56 is the account of what changing a fit's size costs. An offset region is
// already a measured form (`region=400x266`, 953.7 ms, 2026-09-06), so nothing downstream --
// the matte, the auto flow's statistics, the diffusion's walk -- needs re-measuring.
//
// An alignment is a statement about a LOGICAL axis. Which side of the glass that is depends on
// the canvas rotation, and rotation 0 is portrait on one board and landscape on the other, so a
// caller that wants "always the same side of the glass" has to derive the argument rather than
// hard-code it. Ticket 62 item 4 is what that mistake looks like when it ships.
epd_fit_t epd_fit_align(int32_t img_w, int32_t img_h, int32_t screen_w, int32_t screen_h,
                        epd_align_t x_align, epd_align_t y_align);

// Returns a zeroed struct if any dimension is non-positive, so a corrupt header cannot
// turn into a negative-width blit.
epd_fit_t epd_fit_centre(int32_t img_w, int32_t img_h, int32_t screen_w, int32_t screen_h);

// The largest integer reduction that still leaves an image AT LEAST as large as
// epd_fit_centre() will draw it on `screen_w` x `screen_h`. Returns 1 or more, always.
//
// This is the rule for storing a photograph at the size it will be looked at (the SMB
// import path, app_smb_sync.c). It is deliberately NOT the long-edge rule that
// app_server.c's thumbnails use: the fit is `contain`, so a 768x1344 source into 400x600
// is drawn at 342x600, and the long edge would say ceil(1344/600) = 3 -- storing 256x448
// and making the panel UPSCALE it. The ratio that binds is the larger one, and floor
// rather than ceil, because a stored image may be bigger than it is drawn and must never
// be smaller.
//
// `align` is the multiple the reduced dimensions get cropped to before use (16 for a
// 4:2:0 JPEG encoder, 1 for nobody in particular). The crop happens after the division
// and can therefore take the result back below the fit on its own, so it is applied here
// and the factor stepped down until the guarantee holds rather than left to the caller to
// get right. That guarantee -- `(img/f aligned down) >= fit` -- is what test_dither
// asserts.
int32_t epd_fit_reduction(int32_t img_w, int32_t img_h, int32_t screen_w, int32_t screen_h,
                          int32_t align);

// How far from square a picture has to be before the frame is allowed to turn it, times
// 100. 125 rather than the 1.5 the Android frame uses (PhotoRotation.kt), and the
// difference is the panel: 4:3 is 1.333 and it is what a phone shoots, so 1.5 would leave
// every phone photograph at ~50 % fill on a 2:3 screen where a turn gives ~89 %. What IS
// taken from that side is the reason for having a threshold at all, which its comment
// states -- "rotating them would betray the photographer's intended composition" -- so
// 1:1 and anything under 1.25 stay as they were framed. The value is the owner's taste,
// not an optimisation: turning a disagreeing picture ALWAYS increases the drawn area, by
// symmetry, so there is no maximum to find.
//
// **It was 130 from 2026-09-09 to 2026-09-19, and 5:4 is why it moved.** Ticket 64's band
// put a 1280x1024 card on the glass and it filled 53 % where a turn gives 83 % -- the
// largest gap the threshold was still costing, because the payoff falls away fast below
// 1.25 (1.10 is 61 % against 73 %, 1.05 is 64 % against 70 %) while the composition reads
// more and more square. So 125 takes nearly all of the remaining area and still protects
// the pictures the Android comment is about. Lowering it further buys single-digit points
// per step and starts turning frames that were deliberately near-square.
#define EPD_ROTATE_RATIO_X100 125

// True when the picture's orientation disagrees with the screen's AND it is far enough
// from square that turning it is not overruling the composition.
//
// The single source of truth for that decision, called from two places that must agree:
// app_display.c, which turns the canvas before drawing, and epd_image.c, which has to
// pick the JPEG decode scale against the fit the draw will actually use. If those two
// disagree the picture is decoded at half the resolution it is then drawn at, which is
// soft rather than wrong and so would not announce itself.
//
// Non-positive dimensions are false, as epd_fit_centre() returns a zeroed fit for them.
bool epd_fit_wants_rotate(int32_t img_w, int32_t img_h, int32_t screen_w, int32_t screen_h);

#endif // EPD_DITHER_H
