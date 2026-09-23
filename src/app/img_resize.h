// Decode an image, reduce it, and re-encode it as JPEG.
//
// This is the pipeline /thumb/ has run since 2026-09-07, lifted out of app_server.c so the
// SMB import path can run the same one. Nothing here is new work: the decoder's own
// reduced-scale open, an integer box filter over the row stream, and esp_new_jpeg at 4:2:0.
// What IS new is that there are now two reduction rules and they must not be confused --
// see the two entry points below.
//
// Pulls in esp_jpeg_enc and epd_image, so it is not part of the env:native build. The one
// piece that is testable on the host -- epd_fit_reduction(), the arithmetic that decides
// how far an image may be shrunk -- deliberately lives in epd_dither.c instead, next to the
// epd_fit_centre() it is defined in terms of, and is covered by test/test_dither.

#ifndef IMG_RESIZE_H
#define IMG_RESIZE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "img_dims.h"

// Reduced dimensions are cropped to a multiple of this. esp_new_jpeg at
// JPEG_SUBSAMPLE_420 works in 2x2 chroma blocks inside 8x8 DCT blocks, and its FAQ warns
// that a misaligned input wraps a few columns to the opposite side of the image on the S3,
// silently. Cropped rather than padded: padding invents pixels at an edge the panel
// letterboxes anyway.
#define IMG_RESIZE_ALIGN 16

// The photo grid's thumbnail, and its home is here because TWO paths produce one: /thumb/
// on demand (app_server.c) and the SMB import as a sidecar (app_smb_sync.c). They must
// agree, so the number lives once.
//
// The long edge is a judgement about how soft a thumbnail may look rather than a derived
// number -- raise it after looking at a real phone. The reduction factor is an integer, so
// it does not land where you first expect: the bench share is uniformly 768x1344 and
// epd_image already halves that to 384x672, so 384 gives factor 2 and a 192x336 thumbnail,
// while 256 would give factor 3 and 128x224 -- visibly soft once a phone at
// devicePixelRatio 3 stretches a ~100x175 CSS box over it.
#define IMG_THUMB_MAX_EDGE 384

// Lower than the quality the import path stores a photograph at: a thumbnail is drawn into
// that ~100x175 box and is never the thing being looked at.
#define IMG_THUMB_QUALITY 65

// Decode `src` and box-reduce it so that the LONG EDGE is at most `max_edge`.
//
// The thumbnail rule. A thumbnail may be soft, so this rounds the reduction UP and may
// come out smaller than `max_edge` asks for.
//
// On success `*out_rgb` is a 16-byte-aligned RGB888 buffer of `*out_w * *out_h * 3` bytes,
// preferentially in PSRAM; free it with heap_caps_free(). Returns ESP_ERR_NO_MEM
// distinctly from ESP_FAIL, because a caller has to be able to tell "no memory right now"
// from "this file is not an image" -- they are a 503 and a 415 and saying the wrong one
// tells a user their photograph is corrupt when it is not.
esp_err_t img_resize_to_edge(const uint8_t *src, size_t src_len, int32_t max_edge,
                             uint8_t **out_rgb, int32_t *out_w, int32_t *out_h);

// Decode `src` and box-reduce it as far as possible WITHOUT going below the size
// epd_fit_centre() will draw it at on a `screen_w` x `screen_h` panel.
//
// The storage rule, and the opposite rounding to the one above: the result may be larger
// than it is drawn and must never be smaller, or the panel upscales. epd_fit_reduction()
// is the factor and epd_dither.h explains why it is not the long edge.
//
// Same ownership and same error contract as img_resize_to_edge().
esp_err_t img_resize_to_fit(const uint8_t *src, size_t src_len, int32_t screen_w,
                            int32_t screen_h, uint8_t **out_rgb, int32_t *out_w,
                            int32_t *out_h);

// Box-reduce an RGB888 buffer already in memory by an integer factor, cropping to
// IMG_RESIZE_ALIGN. `fac` of 1 still copies, so the caller always owns two buffers and
// never has to reason about aliasing. Same ownership as above.
esp_err_t img_resize_box(const uint8_t *rgb, int32_t w, int32_t h, int32_t fac,
                         uint8_t **out_rgb, int32_t *out_w, int32_t *out_h);

// Encode RGB888 as a 4:2:0 baseline JPEG. `w` and `h` must be multiples of
// IMG_RESIZE_ALIGN, which everything above guarantees.
//
// On success `*out` holds `*out_len` bytes and is freed with free().
esp_err_t img_encode_jpeg(const uint8_t *rgb, int32_t w, int32_t h, int quality,
                          uint8_t **out, size_t *out_len);

#endif // IMG_RESIZE_H
