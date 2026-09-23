// What settings does this kind of picture want? A port of epdoptimize's
// buildLayeredSuggestion() (src/auto-processing.ts:312-405) at intent "natural".
//
// This is the second half of what <https://paperlesspaper.github.io/epdoptimize> runs.
// epd_classify.h is the first half; this turns its answer into parameters. Between them they
// are the "auto" the demo site's processing select defaults to, and the reason a photograph on
// that page looks different from the same photograph through a fixed preset.
//
// **Pure struct to struct.** No image, no allocation, no ESP-IDF -- which is what lets
// test/test_auto/ feed it epdoptimize's *own* metrics rather than this project's, so a
// disagreement localises here instead of to epd_classify.c's stated tolerances.
//
// Four things upstream decides that this struct does not carry, each dropped deliberately:
//
//   * `colorMatching: "lab"`. There is no L*a*b* matcher on the device -- epd_dither.c is
//     squared RGB throughout -- so `wanted_lab` records the request for the log line and
//     nothing acts on it. It affects the textOrUi, lineArt and faded-scan arms.
//   * `errorDiffusionMatrix`. src/epd_diffuse.c runs Floyd-Steinberg and only that, because it
//     is the one kernel whose integer form is bit-identical to upstream's; a `stucki` request
//     (highContrastPhoto) is recorded in `wanted_stucki` on the same terms as `wanted_lab` and
//     falls back to Floyd-Steinberg. **`serpentine` is no longer dropped** -- it is a field of
//     the plan, because upstream sets it on every errorDiffusion arm and this device now has a
//     diffuser to honour it with.
//   * `processingPreset` as a name. Layered auto reads the preset for its *values* and then
//     overwrites most of them; the name survives only into upstream's reasons strings.
//   * The preset score table. `getPresetScores()` is computed by buildLayeredSuggestion and
//     never read by it -- the layered base comes from the kind alone (:320) -- so porting it
//     would be forty lines that cannot change an output.
//
// And one thing it does carry that upstream would not recognise: see `epd_auto_row_tone()`.

#ifndef EPD_AUTO_H
#define EPD_AUTO_H

#include <stdbool.h>
#include <stdint.h>

#include "epd_adjust.h"
#include "epd_classify.h"
#include "epd_dither.h"

// ------------------------------------------------------------------------- the plan
//
// The three extra stages' parameter structs -- epd_level_t, epd_paper_t and epd_white_t -- are
// declared in epd_adjust.h with the stages that read them. Two of the three are set by one arm
// each; **epd_white_t is not**, and that is the easiest thing here to miss.
// enforceAutoWhitePreservation() (auto-processing.ts:428-448) runs last on every layered
// suggestion and turns white preservation on whenever the range mode is not "off", so it applies
// to almost every photograph rather than to one arm.

typedef struct {
    epd_image_kind_t kind;

    // Tone mapping and range compression, ready for epd_adjust_tone()/epd_adjust_range().
    // `adjust.palette` is the palette argument; range compression needs its endpoints.
    epd_adjust_t adjust;

    // In the order they run, which is NOT the order the fields are listed in upstream's
    // option bag -- see epd_auto.c's header comment.
    epd_paper_t paper;
    epd_level_t level;
    epd_white_t white;

    // ditheringType == "quantizationOnly": the nearest-match path rather than the pair
    // search. FR-3.3's two colour-reduction modes, chosen by the picture instead of by the
    // filename.
    bool nearest;

    // Serpentine row scanning for the diffuser (epd_diffuse.h). Upstream sets it on every
    // errorDiffusion arm and omits it for quantizationOnly (auto-processing.ts:388-391), so it is
    // the inverse of `nearest` today -- carried as its own field rather than derived, because
    // that relation is upstream's and a fixture column is what would notice it changing.
    bool serpentine;

    // Requests with no device path. Logged, never acted on.
    bool wanted_lab;
    bool wanted_stucki;
} epd_auto_plan_t;

// The whole of buildLayeredSuggestion at intent "natural". `palette` is what the picture will
// be quantised against, and it changes the answer: applyPaletteTuning() (:1145-1174) forces
// display-mode range compression at strength >= 0.8 for any palette whose Rec. 709 luma range
// is <= 150, overriding whatever the kind chose. Of the palettes this frame ships, `aitjcize`
// spans 195.9 and does not trip it while `manual` spans 130.6 and does.
//
// Never fails: every kind including EPD_KIND_UNKNOWN has a defined answer. A NULL argument
// leaves `out` untouched.
void epd_auto_suggest(const epd_classification_t *c, const epd_palette_entry_t *palette,
                      epd_auto_plan_t *out);

// **The one deliberate addition to upstream's flow, and getting it wrong ruins the picture
// silently.** `epd_render_t.tone` IS display-mode range compression, per channel, in integers
// (epd_dither.c's tone_compress()), and every calibrated palette in PALETTE_TABLE ships
// EPD_TONE_FULL. So when epd_adjust_range() runs, the row path's copy has to be off or the
// picture is squeezed into the panel's range twice and both ends collapse.
//
// It is not simply "always off", because ticket 19 measured the tone compression rather than
// the palette as the win, and upstream has no equivalent stage: when auto asks for no range
// compression at all (pixelArt, flatIllustration) the row path's copy is the only one there
// is, and turning it off would give those kinds no compression whatsoever. So the rule is that
// range compression happens **exactly once** on every path, and this function is that rule --
// a function rather than a comment so test_auto can reach it.
//
// **It has two application points and only one rule.** On the row path the value goes into
// `epd_render_t.tone` and the quantiser applies it per pixel as it matches. On the diffusion path
// it has to be applied to the region *before* epd_diffuse_rect() runs, and the pack that follows
// is then EPD_TONE_NONE unconditionally -- after diffusion every pixel already is a palette entry,
// so compressing at the pack would move the palette's own white off itself and re-match it. The
// rule is the same either way (compress here exactly when the plan did not), which is why this is
// one function and not two that could drift apart.
uint8_t epd_auto_row_tone(const epd_auto_plan_t *plan, uint8_t palette_tone);

#endif // EPD_AUTO_H
