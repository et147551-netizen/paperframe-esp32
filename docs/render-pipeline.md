# The render pipeline: palette, auto flow, quantiser, rotation, order

**Where this file assumes a 2:3 portrait panel, and it does so in three places.** The matte /
contiguous / strided region reasoning, the `rotation` defaults, and the 1.5 aspect threshold that
"protects exactly the" phone photograph are all derived for the M5Paper Color's 400x600. The project
builds for a 5:3 landscape panel since 2026-09-15 and **those three do not transfer** — ticket `62`
owns the re-derivation, and it is a decision about what "fit" should mean rather than a code change.
The palette, the quantiser and the auto flow themselves are panel-independent and travel unchanged;
only the palette's *calibration* is per panel, and it is already a user setting.

Moved out of the project index verbatim on 2026-09-09 so it is read when it bears on
the task rather than loaded every session. Nothing here was rewritten.

**Before you add a sixth render setting, read `src/app/app_apply.h`.** Five of the settings in this
file — `rotation`, `palette`, `auto_adjust`, `dither_diffuse`, `auto_rotate` — owe two things beyond
being persisted: the value has to be pushed into `app_display`'s own copy, and the picture on the
glass owes a re-render. `app_settings_set_*()` does neither, and **omitting either is silent in every
direction**: the value sticks, reads back correctly over HTTP, and nothing happens.
`app_settings.h` used to say so in prose at two of the setters, and `h_mode_cfg_set()` plus
`frame_main.c`'s TOP-button cycle each carried the protocol by hand. Since 2026-09-21 the fact is
`src/core/app_setting_effect.c`'s table — one row per key, host-tested in `test/test_setting_effect/`,
which fails if a key owes a live half and has no row or owes a redraw and pushes nothing — and
`src/app/app_apply.c` is the single module that acts on it. Adding a setting means a row there, a case
in `app_apply.c`'s three adjacent switches, and nothing at either call site.

Two things that did **not** change and must not be "tidied" into it: validation still happens at the
route *and* the setter, deliberately (ticket `12`, with tickets `68` and `71` giving the two reasons),
and `app_display`'s copies of these five are not a redundant cache — the display task reads them under
its own mutex, and `app_display.c:126-129` is the account of why the palette in particular is read once
per render rather than once per stage.

## ONE render path bypasses everything in this file, and it does so on purpose

**Ticket `68`'s maintenance course.** `render_flat()` in `src/app/app_display.c` writes a native panel
colour index straight into the packed frame with `epd_pack_solid()` (`src/core/epd_format.h`) and
**never touches `s_canvas` at all** — so no palette, no tone curve, no range compression, no auto flow,
no dither, no error diffusion and no fit arithmetic runs. Ten full-screen flats,
`K W R W Y W G W B W`, ordered in `src/core/epd_maint_course.c`.

That is the point rather than a shortcut: the owner asked for a service mode that "ignores the
palette" so that a person looking at the glass is looking at the ink rather than at a colour-reduction
decision. **So do not route it through anything described below, and do not "fix" it to use the palette
— a white flat here is `EPD_COLOR_WHITE` and not the current palette's nearest match to (255,255,255),
and the difference is the whole diagnostic value.** The order is a ghosting rule, not a panel-physics
one; `src/core/epd_maint_course.h` carries the argument and ticket `68` §2 the measurements behind it.

The **white standby** that the same ticket added is the opposite case and *does* go through this file:
it is `app_display_request_blank()`, i.e. `epd_canvas_fill(255,255,255)` plus a nearest quantise, which
is the existing FR-5.7 path unchanged.

## The palette is a user setting, and epdoptimize's calibration is the default

**The frame's own default palette is `boeber` — "Calibrated - brighter primaries" — since
2026-09-06, by owner instruction.** That is `apply_defaults()` in `src/app/app_settings.c`, and it is
**not** the same thing as `EPD_RENDER_DEFAULT`, which is still `EPD_PALETTE_EPDOPT_AITJCIZE` at full
tone compression as of 2026-09-05 (ticket 19 had chosen `EPD_PALETTE_MANUAL`). The two are now
deliberately different: `EPD_RENDER_DEFAULT` is the renderer's fallback for an out-of-range id and
what the non-frame envs draw with, while the setting is what `env:frame` starts from — so a colour
figure quoted from `env:photo` or the harness is no longer a figure about the shipping frame.
**The palette is a persisted setting the Web UI exposes** rather than a constant. Seven choices,
listed in
`epd_dither.h`'s `epd_palette_id_t`; `epd_render_for_palette()` is the only way to turn one
into a render config.

Why a setting: no palette suits every picture, and the difference is plainly visible. Every
*calibrated* palette sends 17-22 % of a photograph to green — this panel's green really is a
dark desaturated colour, and dark brown lands nearer to it than to black — while the
uncalibrated `original` sends 1.3 % there and 41.3 % to black instead, which crushes shadow
detail. `docs/measurements.md` has the scans and the histograms.

**Adding a palette is one entry in `PALETTE_TABLE` in `epd_dither.c`** and it then appears in
NVS, the API and the interface: `GET /api/mode/mode_1/config` returns the list as `palettes`,
and `index.html` builds its `<select>` from that rather than holding its own copy — **and since
2026-09-06 the route also serves each palette's own six colours and tone amount** (`match`,
`tone`), because the ink preview quantises with them, so a new palette needs no page edit at all.
The wire
names go into NVS and across HTTP, **so they cannot be renamed freely.** The `palette` NVS key
is an addition to FR-8's nine, not one of them — the stock firmware has no such setting.

**`src/core/epd_epdopt.c` is a port of `paperlesspaper/epdoptimize` (Apache-2.0,
`refs/epdoptimize/`) — three stages on a whole canvas, byte-for-byte verified against the
library, and it is host-only.** Only the *palette* was adopted; the frame quantises with
M5GFX's row-wise paths. Two measurements from 2026-09-05 are why, both in
`docs/measurements.md`:

- **Cost.** The ported pipeline is **24.0 s** a photograph against the row path's **1.62 s**,
  and it starves CPU 0 for the duration, so the task watchdog fires and httpd stops answering.
  Upstream's *own* default is worse still: its L\*a\*b\* range compressor ran **over 90 s
  without finishing**. Both figures are double-precision arithmetic on a single-precision FPU;
  the fast variant has no `pow()` at all and is still 15× the row path.
  **Both figures are from 160 MHz with a 32-byte cache line and are stale by about a third** —
  see the clock-and-cache section of `docs/build-system.md`; the ratio is what the
  argument rests on and the ratio holds.
- **Benefit.** The palette, and only the palette. The pipeline's own contribution was never
  isolated, and at 24 s it is not worth isolating.

**But its first two stages, rewritten in single precision, do run on the device.**
`src/core/epd_adjust.c` is the same tone mapping and range compression written to be affordable here:
**668.9 ms** against the ported version's 8 259 ms in the same build, a factor of 12. It is
deliberately **not** a parity port and must never be given one's acceptance test —
`test/test_adjust/` compares it to the double implementation within stated tolerances and
`test/test_epdopt/` keeps that one pinned to the library, and the chain is what makes "close to
upstream" checkable without claiming "identical". Two things came out of building it and both are
in `docs/measurements.md`: `-O2` made it *slower*, and `fmaxf`/`fminf` are external calls
that GCC cannot inline without `-ffast-math`, which was 40 % of one stage.
## The auto flow is wired in, ON by default since 2026-09-06, and has run on hardware in both envs

The demo site's *actual* default is not a preset: it classifies each picture and chooses that
picture's settings. All of it is now on the device behind the `auto_adjust` setting, which is
**on by default since 2026-09-06**: the owner looked at the result on the glass and decided
these settings are the ones to ship. That decision is the owner's own visual judgement rather
than the systematic flatbed sweep the project index used to ask for, so it settles *which default to ship*
without producing a per-picture-kind comparison — but the default is now the arm that has to be
argued **against**, not for.

| File | What | Test |
| --- | --- | --- |
| `src/core/epd_classify.c` | port of `image-style.ts` — what kind of picture is this | `test/test_classify/`, 26 metrics against the library |
| `src/core/epd_auto.c` | port of `buildLayeredSuggestion` at intent `natural` — kind → parameters | `test/test_auto/`, 26 fixtures × every field |
| `src/core/epd_adjust.c` | five per-pixel stages in single precision | `test/test_adjust/` |

**Five stages, not two, and the fifth is the one that gets missed.**
`enforceAutoWhitePreservation()` runs last on every layered suggestion and turns white
preservation on whenever the range mode is not `off` — so it applies to almost every photograph
rather than to one arm. The order is upstream's and it is load-bearing: the white plan is measured
on the **untouched source**, level compression lands **after** range compression rather than fused
into the tone LUT, and the white apply is last. `src/core/epd_adjust.h` carries the citations.

**The suggestion depends on the palette, and the two this frame ships disagree.**
`applyPaletteTuning()` forces display-mode compression at strength ≥ 0.8 for any palette whose
Rec. 709 luma range is ≤ 150. `aitjcize` spans 195.9 and escapes it; `manual` spans 130.6 and does
not. So changing the palette changes the tone curve, not only the colours — `test_auto` covers
both arms for exactly that reason.

**`epd_auto_row_tone()` is this project's own rule and getting it wrong ruins a picture
silently.** `epd_render_t.tone` *is* display-mode range compression in integers, and so is
`epd_adjust_range()`; running both squeezes the picture into the panel's range twice. It is not
simply "always off" either, because when the plan asks for no range compression (flat artwork,
pixel art) the row path's copy is the only one there is and ticket 19 measured that compression as
the win. Exactly one, always. A host test pins it rather than a comment.

**Speed is not being asked for at this stage** (owner, 2026-09-05). That does not erase the
plan's pre-registered 3 000 ms of added work per photograph — a pre-registration edited after the
fact is not one — it changes the consequence: exceeding it is a **finding to report**, not a
trigger to undo the wiring, and the import-time cache is not built on a timing figure alone. It
also means **do not add micro-optimisations to `src/core/epd_adjust.c`.** Two were tried and taken out
again: a hoisted divisor and a 256-entry reciprocal table in the level stage, and a division-free
saturation test in white preservation. Each bought nothing measurable in fidelity and each cost
something — a hoist gives up byte-identity with the library, and the table put 1 KB on a
display-task stack that has caused a reboot loop here once. Both sites name what would go back.

**It costs 1 917 ms a photograph, measured on hardware 2026-09-05**, ten runs agreeing to 0.1 ms,
with `# floor`, `adj-double` and `adj-float` all reproducing their recorded figures first. Per
photograph: decode 264 + auto 1 917 + row dither 984 = **3 165 ms against a 15 015 ms refresh**,
21 % where it was 8 %. Under the pre-registered 3 000 ms of added work.

**`epd_adjust_tone` has two branches and Stage 0 measured the cheap one: 708.7 ms against
38.1 ms.** `EPD_ADJUST_BALANCED`'s saturation is neutral so it takes the three-LUT path; the auto
flow asks for a non-neutral one for six of the seven kinds, including `photo` at `lumaStdDev ≤ 42`,
and those take the HSL round trip. That branch is 37 % of the whole flow. The arm's own control is
that `range_ms` moved 0.1 ms while tone moved 18.6×.

The rest, in order: classify 193.3, `epd_auto_suggest` **0.09**, white-preservation plan 372.0,
tone 708.9, range 628.8, white apply 13.8. Paper and level were not selected for this picture and
have no hardware figure. **No watchdog from either new arm** — the run's three triggers are all
`adj-double`'s, 8 870 ms apart, which is recorded behaviour. PSRAM 476 692 B, none of it internal.

**The matte and both region forms ran on 2026-09-06, and the shipping application ran with the
setting on.** `env:photo`'s `auto-matte` shot (rotation 1, no new asset) gives `region=400x266` —
matte, contiguous with an offset, 953.7 ms over ten runs — and a 240x600 crop uploaded to
`env:frame` gives `region=240x600`, the strided form, 726.3 ms over ten renders. **Every per-pixel
stage is exactly area-proportional and striding costs nothing**; classify is bound by its sample
count rather than the region's area, so it barely moves. White preservation reported a source white
of 244.0 where a p99 that had latched onto the matte would have said 255, and
`tools/render_preview.py` independently predicts 244.0 for the same bytes — that agreement is what
says the strided statistics are the right statistics. Eleven auto renders on `env:frame`: no
watchdog, no panic, the setting surviving a reboot, `epd_auto_row_tone()` giving `tone=0` every
time, and the 476 796 B scratch coming out of PSRAM with 104 bytes of internal bookkeeping and no
movement in `int_largest` or `dma_largest`.

**Before building any further region arm, read the geometry table in
`docs/measurements.md`.** A source *wider* than 2:3 is width-limited, so it produces a matte
and a **contiguous** region: it looks like it tests striding and does not. Only a source taller than
2:3 at rotation 0 reaches the strided form, and on `env:frame` that needs `orientation=landscape`
first — which **did not work before 2026-09-06**, because the API's `orientation` strings were
mapped to the opposite rotations (see the defect log).

**And a Web-UI upload used to defeat the region entirely**, by baking FR-5.3's matte into the file
so that `render_image()`'s region became the whole canvas. `exportUploadCanvas()` now uploads the
drawn rectangle when the composition is a single untouched layer. **The two contain fits are not
the same fit** — `fitImageToContainer()` caps its scale at `1` and `epd_fit_centre()` does not — so
it crops only when the drawn rectangle reaches exactly one edge of the canvas, which is where the
device's fit reproduces the browser's. Do not widen that condition without re-reading why it is
there; what the matte did to the classification is measured in `docs/measurements.md`.

**The Web-UI preview now quantises exactly as the device does, and the numbers it needs come from
the device**: `GET /api/mode/mode_1/config` serves `palettes[].match`, `palettes[].tone` and
`dither_strength`, and nothing is copied into the page — with no answer from the device it draws no
overlay rather than guessing. Two rules follow.

- **Run `python tools/ink_preview_parity.py <config.json> --self-check` after touching either
  quantiser.** It extracts the page's own functions and compares them index-for-index against
  `epd_dither.c`, so it covers the page, the firmware and the route at once. It had been silently
  wrong on 18 % of a photograph's pixels; `docs/measurements.md` has how, and the
  perturbations that prove the check can fail.
- **With `dither_diffuse` on (the default) the preview runs the device's own Floyd-Steinberg**
  (`quantizeToEpdPaletteDiffused()`, since 2026-09-22) — tone compression, then serpentine
  diffusion in integers — so it is what epdoptimize's demo calls "Dithered — simulates how it will
  look on the display". Before that it drew the ordered pair search whatever the setting, and said
  so in a note that is now gone. `ink_preview_parity.py` checks this mode too: identical to
  `render_preview quality … diffuse serpentine` on every palette.
- **It matches against the selected palette and draws in that palette's own colours**, as
  epdoptimize's demo draws its "Dithered" canvas. Until 2026-09-22 every palette was drawn in the
  one measured table (`preview_rgb`, `EPD_PALETTE_MEASURED`), the green-hair retraction as a rule;
  the owner judged that preview dark and muddy and asked for the demo's behaviour, and the
  `preview_rgb` field went with it. The consequence to keep in mind: comparing two palettes'
  previews now shows colour differences as well as index differences, so a preview comparison
  is not an index comparison — use `tools/ink_preview_parity.py` or the host renderer for that.

The auto flow is still not previewed, and the page says so when it is on. Since 2026-09-06 the same
is true of the error diffusion, with its own note and its own condition.
## The quantiser is a setting too, and error diffusion is the demo's

**`dither_diffuse` (2026-09-06, and **default ON** by the owner's decision the same day) replaces
M5GFX's row-wise pair search with
Floyd-Steinberg error diffusion**, which is what the epdoptimize demo quantises with. `src/core/epd_diffuse.c`
is that diffusion in integers; `src/core/epd_epdopt.c`'s `epd_epdopt_diffuse()` is the double-precision
port it is checked against, and stays host-only.

**Integer, and bit-identical for Floyd-Steinberg — that is the whole reason it is affordable.**
`epd_clamp_byte(np + e*k/16)` clamps then rounds with `floor(v+0.5)`, so with `N = 16*np + e*k` the
same value is `N<0 ? 0 : N>4080 ? 255 : (N+8)>>4`. No multiply, no `isfinite`, no `floor` — and
`measurements.md` had already named those twelve calls per pixel as the cost. **Narrowing
`epd_diffusion_tap_t.factor` to `float` buys nothing**: `double * float` promotes. Stucki's `k/42`
factors are not binary-exact, so `wanted_stucki` still falls back to Floyd-Steinberg and is logged.

**It is cheaper than the path it replaces, which nobody expected.** 602.7 ms diffusion + 217.6 ms
pack against the row path's 1076.3 ms, ten runs each; on the shipping application 737 ms with auto on
and 852 ms with it off. So there is no watchdog question and the pre-registered 3 000 ms is not
breached — `epd_diffuse_rect()` can run in bands and that is bit-exact and tested, but nothing uses
it, because 0.6 s did not need a yield point.

**Three things that are not tidiness**, each measured or tested rather than reasoned:

- **The region, not the canvas.** The matte is 255,255,255 and not a palette entry, so a whole-canvas
  diffusion bleeds the matte's own error into the picture's edge. `test/test_diffuse` asserts the
  surround is untouched **at both rotations**, because they fail in different directions.
- **Logical order, not physical**, so the dither does not turn with `orientation`. At rotation 1 that
  strides −1200 bytes and **costs nothing** — 265.8 against 267.2 ms, exactly area-proportional,
  because a logical row's working set fits in cache. `epd_canvas_logical_walk()` is the only place
  that mapping lives. **A rotation-0 arm cannot compare the two orders**: there they are the same
  walk, so such a measurement reports "identical" by construction.
- **The pack must be `EPD_TONE_NONE`.** After diffusion every pixel already is a palette entry, so
  nearest is exact — but `epd_dither_row_none_cfg()` applies tone *before* matching, so any
  compression here would move the palette's own white off itself and re-match it. Silent, and it
  ruins the picture. `epd_dither_tone_rect()` applies what `epd_auto_row_tone()` still owes,
  *before* the diffusion instead. **That function now has two application points and one rule**, and
  it is deliberately not two functions.

**Separate from `auto_adjust` on purpose, and they stay separate.** Both move the render towards the
demo's default, so folded together a visual difference could not be attributed; apart there are four
combinations. The owner's 2026-09-06 judgement picked one of those four to ship — it did not merge
the two settings, and the other three combinations are still reachable, which is the point. `tools/render_preview.py --diffuse` draws all four on the host.

The error-diffusion plan is where this came from, and two of its predictions were wrong in
the useful direction: the cost (it reasoned from a subtraction when `measurements.md` already carried
the measured 11.2 s) and the striding.

**Paper normalisation and level compression are still untimed, and the reason is a missing input
rather than a missing run.** `paper.enabled` is set in exactly one place — `poster_scan_tuning()`
gated on `warm_poster_scan()` — so it wants a warm faded scan, and all five images available here
report `paper=0 level=0` on the host. Deliberately not closed with a synthetic fixture.

**Do not generalise the 1 917 ms to the shipping application.** Both pictures `env:frame` actually
rendered took the *cheap* LUT tone branch (24.1 and 38.3 ms), so they cost 726 and 1 184 ms, not
1 917. Which branch a photograph gets depends on its `lumaStdDev`; both figures are real.

`tools/render_preview.py --mode auto` runs the whole thing on the host and prints the plan it
chose. The eyeball check the plan asked for passes: the two graphics in
`.scratch/digital-frame/fixtures/` come out `flatIllustration` with a photo score of 0, and the two
photographs come out `photo` and `highContrastPhoto`. The visible difference on a photograph is
that auto is **lighter** — it chose display compression at 0.7 where the row path uses full — and
whether that is better is a glass question, not a host one.

It is also where the palette dependence can be seen on a real picture rather than in a fixture:
`--mode auto --palette manual` on `imaged002.png` reports `range=display@0.8 row_tone=0` where
`--palette epdopt-aitjcize` reports `range=off row_tone=255`. Same image, same classification,
opposite halves of `epd_auto_row_tone()` — and exactly one range compression either way.

**The green-hair retraction, which is the useful part.** A flatbed scan showed
`EPD_RENDER_MANUAL` rendering dark brown hair green, and a host preview through epdoptimize's
palette appeared to fix it. **That was an artefact of the preview, not a quantisation
difference:** `render_preview.py --simulate` draws each palette in *its own* colours, so two
previews of the same indices look different. On the glass, `EPD_RENDER_EPDOPT` renders the
same green hair, and the two index maps differ in only 9.9 % of pixels. So the green is not
"the palette that was chosen"; it is what every calibrated palette does, and the default was
chosen on other grounds — `original`'s 41.3 % black loses too much shadow detail — with the
setting there so a picture that suits the other trade can have it.

**So: use `tools/render_preview.py` for colour questions, and compare like with like.**
`--mode epdopt` is the port, `--mode quality --palette epdopt-aitjcize` crosses the old
algorithm with the new palette, `--accurate` is upstream's own default. **Never compare two
`--simulate` outputs drawn with different palettes** — that is the mistake above.

`EPD_RENDER_MANUAL` and every other candidate are still in `epd_dither.c` and still
selectable; `photo_main.c`'s shot table puts them on the glass in one run. They are
the comparison arm, not dead code.

**Because it is a port, "better" is not the acceptance test — "identical" is.**
`tools/epdopt_reference.mjs` runs the library itself under Node and writes
`test/test_epdopt/epdopt_fixtures.h`; `pio test -e native` compares byte for byte across
seven images, two palettes, three cumulative stages and both scan orders. Four things in
that port look like mistakes and are not, so do not "fix" them: diffusion error accumulates
through clamped bytes rather than a float plane, `epd_clamp_byte()` is JavaScript's
`Math.round` (`floor(v + 0.5)`, not `lround`), the `EPD_PALETTE_EPDOPT_*` tables are in
epdoptimize's role order rather than this project's because that order breaks distance ties,
and the L\*a\*b\* constants are the pre-1998 CIE pivot. **A value in the fixture header must
never be edited to make a test pass.**

**What was deliberately not ported**, and it is the biggest of the four: **the demo site's
default is not a preset.** `processingPresetSelect.value = "auto"` — so
<https://paperlesspaper.github.io/epdoptimize> runs `auto-processing.ts` plus
`image-style.ts` (2,100 lines) classifying each image and choosing settings per picture. The
port uses the fixed `balanced` preset. Also out: edge preservation and antialiasing, paper
normalisation, clarity, and every dithering mode but error diffusion.

**The `dynamic` preset is not new material, and wiring it in is not one table entry** (read
2026-09-15, not rendered). `aitjcize/epaper-image-convert`'s `src/presets.js:44` — what
esp32-photoframe's "Processing Preset: Dynamic" uses — is epdoptimize's `processing.ts:148`:
S-curve at strength 0.9, shadow boost 0, midpoint 0.5, saturation ×1.3, **range compression off**.
Its `highlightCompress` is +1.5 against epdoptimize's −1.5 because its exponent is `1 + s·h` where
epdoptimize's is `1 − s·h`, so both give 2.35; epdoptimize's extra shadow factor and clamps do not
act at these values. `build_scurve_lookup()` already computes that curve. The trap is
`epd_auto_row_tone()`: a plan with `range_mode` off gets the row path's compression **instead**,
so a `dynamic` fed through today's plumbing is silently compressed once and stops being `dynamic`.
It needs a third state, deliberately zero compressions, which changes the "exactly one" rule above.
Its non-neutral saturation also takes `epd_adjust_tone`'s HSL branch.

**`tools/render_preview.py` renders a PNG through this exact code on the host**, so a colour
question does not need a 15.6 s refresh. `--mode quality --palette epdopt-aitjcize` is what the
frame ships; `--mode epdopt` is the port, which the device does not run; `--simulate` draws in
the palette's own calibrated colours; `--accurate` and `--serpentine` are the port's variants.
## A picture the frame is not hung the way up for gets turned

**To check on hardware whether a `rotation` change actually REACHED the canvas, read `# draw`, not
`region=`.** Every render prints

```
# draw src=404x600 base_rot=0 rot=0 auto_rotate=1
```

where **`base_rot` is the setting as `app_display` received it** and `rot` is what survived
`auto_rotate`. `region=` on the `# auto` line is the wrong instrument and fails silently: with
`auto_rotate` on, a portrait source asked to render at `rotation=1` is turned back, so the region is
**unchanged** and a working live half looks like a dead one. That cost two hardware arms on
2026-09-21 before a `grep '# draw '` over logs the same session had already written answered it —
`base_rot=1` then `base_rot=0`, following the POSTs exactly. Either read `base_rot`, or turn
`auto_rotate` off first and then `region=` means something.

**`auto_rotate` (2026-09-09, and **default ON** by owner decision) turns a picture 90° at
DRAW time when its orientation disagrees with `rotation` and it is far enough from square**
(ticket `51`). Measured on hardware the same day, eight fixtures, both arms: 4:3 goes **50 % →
89 %** of the panel, 3:2 **45 % → 99 %**, 16:9 **37 % → 83 %**, and 5:4, 1:1 and both portrait
fixtures are byte-identical between the arms. No watchdog, `int_min` 38,707 B. **Six scan arms
the same day settled the way up**, which the fill numbers cannot. (**5:4 stopped being one of the
identical pair on 2026-09-19**: the threshold moved to 1.25 and it turns now — see below.)

**`rotation` IS FOUR QUARTER TURNS since 2026-09-20 — ticket `69`, built and on the glass.** It was a
boolean; it is now a count, 0..3, and `EPD_CANVAS_ROTATION_MAX` in `epd_canvas.h` is the single place
the range lives. `epd_canvas.c`'s `offset_of()` holds all four mappings and everything else in that
file is derived from those four lines symbolically. Three things survived untouched, and all three
were free by accident:

- **The stored photograph size.** `SMB_RESIZE_FIT_EDGE` is a square of the long edge, and the logical
  dimensions still take only TWO values — 2 has 0's shape and 3 has 1's — so nothing in the mirror,
  the resize or the thumbnail path changes.
- **Ticket `64`'s band.** `epd_band_bottom_align()` fires two 1×1 probes and asks which end lands lower
  on the glass, so it is rotation-**count**-agnostic by construction. **Do not replace it with a table.**
  One consequence to know: the band therefore stays at the bottom of the glass at every rotation and
  does **not** turn with the picture, so a 180° render is a 180° of the *picture* and not of the glass.
  That is correct, it is why the text stays readable, and it is what made a hardware comparison of
  rotations 1 and 3 look like a refutation before the band was excluded from the crop.
- **Auto-rotate.** `base_rotation ^ 1u` at `app_display.c` maps 2↔3 as well as 0↔1, so it is a quarter
  turn to the other shape *from whichever base the frame is set to*. **Do not generalise it to
  `(base + 1) % 4`**, which turns 1 into 2 — the same shape, i.e. a flip and no turn.

**And it means an `auto_rotate=1` frame showing a source that matches the panel's own aspect renders
only TWO distinct images across the four settings** (`base_rot=1 rot=0`, `base_rot=3 rot=2` on the
console), which is the feature working and is a trap for anyone measuring rotation on hardware: turn
`auto_rotate` off, or the odd mappings never reach the panel.

What cost the work was **seven** guards rejecting `> 1`, in five files — not the three an earlier
version of this paragraph claimed. `.scratch/digital-frame/issues/69` §6 lists them; the two that
were missed are in `app_display.c`, and one of them *substitutes* the default rather than refusing,
so a build that widens NVS and the canvas alone comes up at rotation 1 at every boot with the setting
reading back correctly over HTTP.

**This setting is also what decides how much matte there is to draw in, and it was settled again on
that basis** (ticket `62` item 5, closed 2026-09-19: it stays ON). Turning a picture to fill the glass
is the same thing as destroying the band beside it — off, a 3:4 photograph on the E1002 leaves 55 % of
the glass as margin; on, the margin is 30 to 160 px. Ticket `64` designs against the small one. So a
change to this default is not a rendering tweak, it re-sizes everything drawn in the matte.

Six things not to re-derive:

- **The threshold is 1.25 since 2026-09-19, and was 1.3 before that. Neither is Android's 1.5.**
  What is taken from `PhotoRotation.kt` is the reason for having one — a near-square composition
  must not be overruled — and not the value: 4:3 is 1.333 and is what a phone shoots, so on a 2:3
  panel 1.5 protects exactly the photographs the feature exists for. `test/test_dither` pins both
  directions and **was made to fail on purpose at 150 and at 100** before being trusted.
  **1.3 moved to 1.25 because ticket `64`'s aspect-ratio arm put a 5:4 card on the glass at 53 %
  fill where a turn gives 83 %** — the largest gap the threshold was still costing, and the point
  where the payoff stops justifying the protection: 1.20 is 61 % against 73 %, 1.05 is 64 %
  against 70 %. Below 1.25 each step buys single-digit points and overrules a composition that
  reads as square, which is why the line is there and not lower.
- **`epd_fit_wants_rotate()` is one function with two callers and that is the whole design.**
  `app_display.c` turns the CANVAS rather than the pixels, so the matte, the auto flow's region
  and the diffusion's walk follow with no further change; `epd_image.c` needs the same answer
  because the JPEG scale is chosen at open, against the fit. Told the unswapped fit it reduces
  one power of two too far — 1024x768 becomes 512x384 for a draw at 533x400 — which is **soft
  rather than wrong and would not announce itself**. Measured both ways in the same run.
- **The hint is gated on the setting.** Called unconditionally it decodes one power of two too
  LARGE with the setting off. Found only because the arm that measures the hint cannot be built
  without noticing.
- **Draw time, not import time**, because `orientation` is persisted: baking a rotation in would
  invalidate the cache whenever it changes. `SMB_RESIZE_FIT_EDGE`'s square fit — chosen
  for a different reason — is what makes draw-time rotation exact. **That square is 600x600 on this
  panel and per-panel since 2026-09-17**: the definition was `EPD_HEIGHT`, which is the long edge
  only on a portrait-native panel, and is now `max(EPD_WIDTH, EPD_HEIGHT)` — 800x800 on the E1002,
  unchanged here.
- **It has now been seen on the glass, and the turn's DIRECTION depends on `orientation`**
  (2026-09-09, six scan arms, the fill numbers above being device-side only). It is a pure 90°
  in every arm — no mirror, no 180° — but `app_display.c` turns by `base_rotation ^ 1u`, and
  0→1 and 1→0 are opposite senses: measured **anticlockwise at `portrait`** and **clockwise at
  `landscape`**. "One fixed direction" holds only within one `orientation`. **Arm A is what makes
  any of it readable** — the device lies face-down on the bed, so a scan could itself be rotated
  or mirrored, and a 5:4 fixture pre-registered as *not* turned is what fixes the frame of
  reference. Do not run a rotation scan without its unturned control.
- **And that settles which EXIF tag suffers, the opposite way round from what was written here.**
  `ORIENT_OPS` makes tag 6 the **clockwise** rotation, so at `portrait` a landscape-shaped file
  carrying `Orientation=6` — the common phone tag — is turned anticlockwise and lands **upside
  down**, while tag 8 lands upright. At `landscape` such a file agrees with the panel and is not
  turned at all. Reachable only where nothing applied the tag first: pre-ticket-`53` imports,
  `smb_resize` off, and **an API upload** — `POST /api/photos/upload` does not read EXIF, which a
  tagged `curl` upload scanning identically to its control is what proves. The browser's canvas
  does; the route does not.
## The matte has text in it, and WHERE in the order it goes is the whole of the wiring

**Ticket `64`, built 2026-09-19, never seen on glass.** A `contain` fit leaves blank panel on one
axis; centred that is two thin strips and two thin strips hold nothing, so the photograph is pushed
to one edge (`epd_fit_align()`) and the leftover becomes one block with a line or two of text in it.
`src/core/epd_band.c` is the block, the layout and the draw; the ticket carries the geometry for
every aspect ratio and the matte-band plan the decisions.

Four things about it that are order, not layout, and each would be silent if wrong:

- **AFTER the auto flow.** The flow's statistics are measured over the region precisely to exclude
  the white matte, and white preservation's p99 latching onto a band's black would move the whole
  tone curve. Drawing the band first would be the matte problem this file already documents, in a
  new place.
- **BEFORE the pack.** Both quantise paths walk the full canvas, so pure black written on the
  surround comes back as the palette's own black exactly. `diffuse_and_pack()` touches only the
  region, so the band reaches the `EPD_TONE_NONE` pack untouched -- but `dither_canvas()` quantises
  every row including the band's, so with `dither_diffuse` OFF the text can pick up the pair
  search's dither. The default is on; this is a difference to look for, not a defect.
- **The fit is computed ONCE and passed.** `epd_image_draw_fit()` takes it, and `render_image()`
  reuses the same struct for the region. It used to be computed twice from the same arguments with a
  comment apologising for it, which was harmless only while the answer was "centred".
- **The band lies along the bottom of the GLASS, or there is none.** That is the rule after the
  owner looked at five samples on 2026-09-19 and rejected two of them: the band had gone wherever
  the leftover was, which put it down the panel's right edge for a 9:16 source and across its top for
  a turned 4:3 one. Both read as the picture having been pushed aside. So `epd_band_plan()` asks
  `epd_canvas_physical_rect()` which logical edge is the glass's bottom, aligns the photograph to the
  opposite one, and **when the leftover cannot lie along the bottom at all there is no band and the
  fit stays centred** -- a 9:16 photograph now draws exactly as it did before any of this existed.
  An earlier revision of this file recorded the varying edge as a decision to keep; that was reversed
  the same day, by the owner, after seeing it.
- **Logical is not physical, and that is the trap.** `epd_band` works in logical coordinates like the
  rest of the pipeline, so on a turned canvas the band along the glass's bottom is a logically
  VERTICAL strip whose text is drawn `rot90` -- and the canvas's own rotation brings it back upright
  for a viewer. `EPD_BAND_VERTICAL` does not mean vertical on the glass. The 4:3 and 3:4 cases now
  land as the same 400x67 strip at the foot of the panel by two different logical routes.
- **The text's size follows a thin band, down to scale 2.** The large scale is a ceiling, not a
  constant: `epd_band_large_scale()` gives 4 wherever it fits and 3 or 2 where it does not, so the
  reTerminal E1002's 30 px leftover now carries a line where it used to draw nothing. Scale 1 is not a
  candidate -- 7 px is about 1 mm here.
- **It is ONE line, filled ALONG the band, since 2026-09-19** — the owner's mockup. Content sits
  side by side in up to four segments with a divider rule between, justified space-between, and **a
  band thicker than one line keeps its white rather than growing the type.** So a 400x200 band and a
  400x67 band draw exactly the same thing, which is the decision and not an under-fill.
  **The band's long axis has only two values in this project: 400 px on the M5Paper Color at either
  rotation, 800 px on the E1002** — which is why the same content composes differently per board with
  no per-board code. **A band exists when the fit leaves its slack on the axis that runs down the
  glass, which on the E1002 means an effective aspect ratio past 800/480 = 1.667 in EITHER direction**
  — so a 16:9 landscape source bands there and so does a portrait one taller than 5:3, while a phone's
  3:4 portrait does not. One of the eight cards in `.scratch/scans/fx/` qualifies and that was briefly
  written up as "the band almost never appears on this board"; a real share photograph banding at
  `32x800` on 2026-09-20 is what corrected it. The test cards were never a sample of a library.
- **The stacked layout it replaced had a defect at 36-53 px and the one-line rule removes it by
  construction.** The old ladder chose a rung by whether text fitted a line's width and then spilled
  the remainder to a line below; at that thickness exactly one line fits, so the spill went nowhere
  and the capture date vanished — a 30 px band showed more than a 46 px one. With one line there is
  no "below". `EPD_BAND_LINE_GAP` went with it.
- **The country is a separate content field and is the LAST thing placed**, spliced back into the
  photograph's segment only when the whole line already fits. `TOKYO JAPAN` is 11 characters and
  `TOKYO` is 5, and on a 400 px band those six decide whether the room's reading appears. An ISO
  abbreviation (`JP`) was evaluated and rejected: it saves three where dropping saves six and changes
  no outcome in the two commonest bands. **Since 2026-09-22 the E1002's 800 px band shows the years
  (`7 YEARS AGO`) and drops the country**: the battery icon shrank to two thirds of the line height
  (`epd_band.c`'s `icon_thick()`, 40 px long at scale 4 against 63 before), the space it freed admits
  `ago`, and the country is only ever given what is left. The owner chose the years when asked.
  **The icon's fill is red at 33 % or less** (`EPD_BAND_BATTERY_LOW_PCT`, matching the web page); pure
  red lands on the red ink on every palette through the nearest passes, but with `dither_diffuse`
  off `spectra6` speckles it with yellow — `test_band` has the measurement. Ticket `64` has the arithmetic; `app_smb_sync.c` writes
  `C TOKYO` and `N JAPAN` as two sidecar lines because splitting a joined string at the last space
  fails for UNITED STATES.
- **`°` and `%` joined the 5x7 font the same day**, so the room reads `23°C 45%`. **The degree sign is
  byte 0xB0 and `EPD_TEXT_DEGREE` is how you write it** — a literal `°` in a UTF-8 source is two
  bytes and the first draws a hollow box.
- **The city table is real and INERT on this library, so a band with no place name is not a defect.**
  `tools/gen_cities.py` has built the table since 2026-09-20 — 6,860 cities, 221 countries, ~112 KB of
  `.rodata` — and the geocoder has named a photograph on the glass. But **0 of 1,057 files on the share
  carry a GPS position** (`.scratch/digital-frame/gps_scan.py`, run with a positive control), so no city
  can appear until a photograph that has one arrives. Check the source's EXIF before debugging the
  lookup. Ticket `64` has both the arm and the generator's own two traps.
- **A city too long for the band is CUT and the excess discarded** (owner, 2026-09-20), with no
  ellipsis. **The city is what is cut and the capture date never is** -- half of `19-08-14` is
  unreadable where half a place name is merely a shorter word -- so `RIO DE JANEIRO 19-08-14` becomes
  `RIO DE 19-08-14` on a 400 px band and stays whole on 800 px. A cut never lands on a space, because
  a trailing one draws as a gap and reads as a missing word. **This replaced refusing the segment**,
  which lost the city AND the date together to a fourteen-character name: the band fell through to the
  room and the photograph said nothing at all. The accepted consequence is that a truncated name is
  indistinguishable from a short one, so `SAN FR` reads as a place; `# band seg0` in the console is
  where the full string can be checked.
- **The photograph is never made smaller for the band**, which is the owner's first requirement:
  `epd_band_plan()` returns `epd_fit_centre()`'s own width and height in every case and only x and y
  move. `test_band` asserts that across seven sources at both rotations rather than arguing it once.
- **There WAS a rule that chose the side from the picture's brightness, and it is gone** -- forcing
  the band to the glass's bottom left it nothing to decide, so `epd_image_edge_luma()` was removed the
  same day it was verified. Ticket `64` keeps the account. One thing it taught survives and is true of
  `epd_image.c` generally: only a JPEG (and an interlaced PNG) is materialised whole at open, so
  anything that wants to measure a whole image before drawing it cannot do so for a plain PNG without
  decoding twice. The arm that found that had built its fixtures out of PNGs precisely because their
  bright side was known, which is the shape of mistake worth remembering -- a fixture in the one
  format the code under test cannot read.

## The playback ORDER is a setting too, and it is a permutation rather than a draw

**`slideshow_random` (2026-09-06, and **default ON** by the owner's decision the same day) walks a
shuffled permutation instead of filename order.** It was off because FR-5.4's order *is* filename
order, so shipping it on is a deliberate departure from that requirement rather than an oversight. `smb_catalog_shuffle()` is the
permutation and lives in the catalogue module so `test/test_catalog/` can pin its bijection on the
host. Four rules, each of which someone will be tempted to simplify away — the accounts, the
measurements and the verification are in ticket `37` Phases 2-3 and `docs/measurements.md`:

- **Not `esp_random() % count`.** Independent draws revisit and starve, which answers "the frame
  shows the same pictures" with "some pictures three times before others once".
- **The domain follows `smb_on_demand`**: `/data` when off, the share **catalogue** when on. An
  epoch over the catalogue without on-demand fetch would miss ~78 % of the time — a skip loop, not a
  permutation.
- **The position is DERIVED over `/data` and PERSISTED over the catalogue**, in RX8130 RAM slots
  2-3, because two catalogue entries in different folders can share one local name and the lookup is
  then ambiguous. Only a seed is stored (NVS `ss_seed`, once per pass); `.smbdir`'s own `seed` field
  is now redundant and left alone rather than removed. The permutation is in **PSRAM** —
  `SMB_CATALOG_MAX` is 4096, so 8 KB.
- **The cursor HOLDS ITS PLACE while a wanted photograph is in flight.** Walking forward to the next
  cached entry — the obvious reading of "don't block" — races the fetcher, so everything fetched has
  already been walked past: the frame would fetch 1,032 photographs to keep showing the same hundred.
  A miss draws a stand-in out of turn; `EPOCH_MISS_MAX` bounds a position that never arrives.

**`.scratch/digital-frame/shuffle_predict.py` reimplements the shuffle off-device, and the
reimplementation IS the check** rather than a shortcut. A full pass is hours of panel time, so the
wrap is reached by predicting `perm[N-1]` and jumping there with `POST /api/photos/display` — **an
arm that jumps to a name without a predicted position proves nothing**, because any name is at some
position. The console prints `display [i/n] … epoch p/N seed S` on every draw.

**`N` is the card's photo count and `files_mirrored` is what `.smbidx` OWNS — 245 against 230 when
this was written.** The card also carries uploads and the factory PNGs. Using the wrong one makes
every predicted position miss.
