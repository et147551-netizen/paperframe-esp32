// Image decoding: JPEG, PNG and BMP into the canvas.
//
// The interface is a **row stream**, not "decode into a buffer", and that is the whole
// design decision here. A 400 x 600 RGB888 image is 720 KB, which is fine; a photograph
// straight off a phone at 4000 x 3000 is 36 MB, which is not, and files arriving on the
// microSD or over USB MSC (FR-4.4) have not been through the browser's resize. Streaming
// rows costs one row of memory instead of a whole image, and the scaling in
// epd_image_draw() consumes rows as they arrive.
//
// The shipping firmware sidesteps this by only ever seeing browser-resized uploads. We
// cannot, so the decoder is where the difference is absorbed.
//
// Uses ESP-IDF and the managed components libpng and esp_jpeg, so it is not part of the
// env:native build. What can be tested on the host -- the fit arithmetic, the dither --
// already lives in epd_dither.c.

#ifndef EPD_IMAGE_H
#define EPD_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "epd_canvas.h"
#include "epd_dither.h" // epd_fit_t, for epd_image_draw_fit()

typedef enum {
    EPD_IMAGE_UNKNOWN = 0,
    EPD_IMAGE_JPEG,
    EPD_IMAGE_PNG,
    EPD_IMAGE_BMP,
} epd_image_format_t;

// Format from a filename extension, case-insensitively: .jpg/.jpeg, .png, .bmp
// (FR-4.6). Extension only -- the content is validated when the decoder opens it, and a
// file that lies about its type fails there rather than here.
epd_image_format_t epd_image_format_from_name(const char *name);

// Format from the first bytes of the data. Used in preference to the extension when the
// bytes are already in hand.
epd_image_format_t epd_image_sniff(const uint8_t *data, size_t len);

// Ask the JPEG decoder whether it can decode this file, without decoding it (ticket 72). Three
// outcomes and the last two are NOT interchangeable:
//
//   ESP_OK                 drawable
//   ESP_ERR_NOT_SUPPORTED  the decoder refused it -- progressive, or a sampling factor it does
//                          not implement. A caller may reject the file on this.
//   anything else          the question could not be asked (no PSRAM, unreadable, over the
//                          JPEG ceiling). A caller must NOT reject the file on this.
//
// Costs one read of the whole file into PSRAM plus a header parse -- ~80 ms for a 338 KB file,
// against 15,000+ ms for the render it saves. Call it only on files that sniff as JPEG.
esp_err_t epd_image_jpeg_probe_file(const char *path);

// Opaque decoder state. Sized here so callers can put it on the stack; the decoders'
// own working buffers are heap/PSRAM.
typedef struct epd_image_reader epd_image_reader_t;

// Big enough for the largest of the three decoder states. Checked with a static assert
// in epd_image.c, so growing a decoder past this is a build error rather than a stack
// smash.
#define EPD_IMAGE_READER_SIZE 512

typedef struct {
    _Alignas(8) uint8_t opaque[EPD_IMAGE_READER_SIZE];
} epd_image_reader_storage_t;

// The largest JPEG this decoder will look at. esp_jpeg's API takes a pointer rather than a
// stream, so the whole file has to be resident in PSRAM before the header is parsed, and a
// file over this is refused rather than allowed to exhaust the heap.
//
// In the header rather than in epd_image.c because a caller that reads a file itself has to
// agree with it: app_server.c's /thumb/ handler slurps the file and hands the bytes to
// epd_image_open_mem_fit(), which never sees a path and so cannot apply this. Two numbers
// there meant a file the panel could draw had no thumbnail (ticket 46's A0).
//
// **It is a JPEG ceiling.** A non-interlaced PNG streams rows and has no file limit at all on
// the display path, so a 6-12 MB PNG remains displayable while /thumb/ refuses it. Left as it
// is: a per-format ceiling would be a second rule for a case nothing here has produced.
#define EPD_IMAGE_MAX_FILE_BYTES (6u * 1024u * 1024u)

// The alignment of any whole-image buffer this module allocates, and therefore of anything
// epd_image_take_rgb() hands out. 16 because esp_new_jpeg demands it of an encoder input and
// img_resize.c takes ownership of exactly this buffer; it costs nothing to promise it here.
#define EPD_IMAGE_RGB_ALIGN 16u

// ------------------------------------------------------------------------ the API
//
// Typical use is epd_image_draw(), which wires a reader to a canvas. The lower-level
// calls are exposed because ticket 07 wants to time decode separately from dither, and
// because the QR/boot screens draw from memory without a file behind them.

// Opens an image held in memory. `data` must stay valid until epd_image_close().
esp_err_t epd_image_open_mem(epd_image_reader_storage_t *storage, const uint8_t *data,
                             size_t len, epd_image_reader_t **out);

// As above, but says how large the result will actually be drawn. A JPEG is reduced by the
// largest power of two that still covers `fit_w` x `fit_h`, and that reduction is not a
// detail: the decode happens whole at open time, and it measured 900 ms of a 1.35 s
// thumbnail on 2026-09-07. Asking for a quarter of the panel costs a quarter of it.
//
// epd_image_open_mem() is this at the panel's own size, so every caller that draws on the
// panel keeps the behaviour it had before this existed. PNG and BMP ignore the hint --
// neither has a cheap reduced decode -- so it changes the cost of JPEG only.
esp_err_t epd_image_open_mem_fit(epd_image_reader_storage_t *storage, const uint8_t *data,
                                 size_t len, int32_t fit_w, int32_t fit_h,
                                 epd_image_reader_t **out);

// Opens an image from the filesystem. The file stays open until epd_image_close().
esp_err_t epd_image_open_file(epd_image_reader_storage_t *storage, const char *path,
                              epd_image_reader_t **out);

// The same, for the path that draws on the panel. Two things come with it, and they are one
// entry point because the display path needs BOTH and `rotatable` is a runtime setting:
//
//   - `rotatable` true says the caller will turn its canvas 90 degrees for a picture whose
//     orientation disagrees with it (app_display.c, the `auto_rotate` setting), so
//     epd_fit_wants_rotate() is applied to the picture's own dimensions and the JPEG decode
//     scale is picked against the fit the draw will actually use. Gated on the setting: with
//     it off the canvas is not turned, and a reader that chose its scale against the turned
//     fit would decode one power of two LARGER than the draw needs.
//   - A JPEG whose output buffer cannot be allocated at that scale is decoded one power of
//     two SMALLER rather than refused, and the panel upscales the result. Soft beats blank,
//     and each step is logged. THIS is why the parameter exists rather than a second pair of
//     entry points: a flag set on only the rotatable variant would have missed every draw
//     with `auto_rotate` off (ticket 56).
//
// Separate from epd_image_open_file() because the callers that must do NEITHER --
// storage_main.c, which has no such setting, and img_resize.c through the _mem variants,
// whose img_resize_to_fit() promises a result that is never smaller than the draw size --
// should keep reading as they did. The rest of the reader is identical.
esp_err_t epd_image_open_file_display(epd_image_reader_storage_t *storage, const char *path,
                                      bool rotatable, epd_image_reader_t **out);

// Dimensions, valid once open. Reading these does not decode pixels -- FR-5.2 needs the
// size before it can compute a scale.
int32_t epd_image_width(const epd_image_reader_t *r);
int32_t epd_image_height(const epd_image_reader_t *r);

// Hands the whole decoded image to the caller, who then owns it and frees it with free().
// The reader keeps nothing: a later epd_image_next_row() reports the stream finished rather
// than reading memory it no longer owns.
//
// Only a reader that MATERIALISED the whole image has one to give -- JPEG always, interlaced
// PNG -- and anything else returns ESP_ERR_NOT_SUPPORTED, which is not an error but a "you
// still have to copy the rows yourself".
//
// It exists because for JPEG a box reduction is ALWAYS a no-op: choose_jpeg_scale() leaves
// the decode between 1x and 2x the fit, so an integer factor of 2 would undershoot and the
// factor is always 1. img_resize.c was then allocating a second full-size buffer and copying
// into it, which is 8.06 MB against 6.44 MB of PSRAM for a 12 MP photograph -- the whole
// difference between a phone photograph resizing and not (ticket 49's retraction).
//
// The buffer is EPD_IMAGE_RGB_ALIGN-aligned, which is what lets the caller hand it straight
// to the JPEG encoder.
esp_err_t epd_image_take_rgb(epd_image_reader_t *r, uint8_t **rgb, int32_t *w, int32_t *h);
epd_image_format_t epd_image_format(const epd_image_reader_t *r);

// Yields the next row, top to bottom, as `width * 3` bytes of RGB888. The pointer is
// owned by the reader and is valid until the next call.
//
// Returns ESP_ERR_NOT_FOUND once every row has been delivered. A truncated or corrupt
// file returns an error here rather than silently producing garbage rows.
esp_err_t epd_image_next_row(epd_image_reader_t *r, const uint8_t **rgb);

void epd_image_close(epd_image_reader_t *r);

// ------------------------------------------------------------------ draw to canvas

// Decodes `r` straight onto `c`, scaled to fit and centred (FR-5.2, FR-5.3), clearing to
// white first so an image that does not fill the screen is matted.
//
// One pass, one row of memory. Vertical scaling works by emitting the destination rows
// that map to each source row as it arrives, so the source is never held whole.
esp_err_t epd_image_draw(epd_image_reader_t *r, epd_canvas_t *c);

// The same draw with a fit the CALLER computed, which is how the matte band gets a photograph
// pushed to one edge instead of centred (ticket 64, `epd_fit_align()` in epd_dither.h).
//
// It also removes a duplication that had a comment apologising for it in app_display.c: the auto
// flow needs the drawn rectangle in order to exclude the white matte, and used to recompute it
// with the same arguments and hope the two agreed. Now there is one rectangle and it is passed.
//
// The fit's WIDTH and HEIGHT must be the ones the reader's decode scale was chosen against --
// only x and y may differ from `epd_fit_centre()`'s. A fit of a different size decodes at one
// power of two too far and the picture is drawn soft, which does not announce itself (ticket 56).
esp_err_t epd_image_draw_fit(epd_image_reader_t *r, epd_canvas_t *c, const epd_fit_t *fit);

// **`epd_image_edge_luma()` was here and was removed on 2026-09-19.** It measured the decoded
// image's left and right edge brightness so the matte band could sit beside the brighter one. The
// operator then looked at five samples on the glass and fixed the band to the bottom of the glass
// instead, which leaves no side to choose and left this without a caller.
//
// Recorded rather than silently dropped because it was verified first, and the verification is the
// part worth keeping: two images differing only in which half was bright gave `edge=31,224` with the
// band on the right and `edge=224,31` with it on the left. Ticket 64 has that account; `git log` has
// the code. One thing it taught is still true of this module and is documented at
// `epd_image_next_row()`: only a JPEG (and an interlaced PNG) is materialised whole at open, so a
// plain PNG cannot be measured without decoding it twice.

// Convenience: open, draw, close.
esp_err_t epd_image_draw_mem(const uint8_t *data, size_t len, epd_canvas_t *c);
esp_err_t epd_image_draw_file(const char *path, epd_canvas_t *c);

#endif // EPD_IMAGE_H
