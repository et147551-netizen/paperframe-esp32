// Finding a Google Photos shared album's photographs in its server-rendered HTML.
//
// Ticket .scratch/digital-frame/issues/60, FR-10.1. Kept free of ESP-IDF so it runs under
// env:native (NFR-3), the same way smb_manifest.c does.
//
// WHAT IT LOOKS FOR IS A RULE SET (gphotos_rules.h), not code, since
// the scrape-rules plan. The built-in set is the rule the arms measured, and nothing
// wider. Each media item in the payload is
//
//     googleusercontent.com/pw/<key>",<w>,<h>
//
// -- a photo base URL immediately followed by its pixel dimensions. That is scrape_probe.py's
// GEOMETRY expression transcribed, and it found 167 of 167 photographs of a link-shared album
// (.scratch/digital-frame/gphotos_prereg.md, gate A). Avatars (/a/, /ogw/) and Google's own
// preview references (`=w96-h72-no`) never match, because neither is followed by `",w,h`.
//
// STREAMING, NOT WINDOWED. The source memo read 512-byte chunks with a 128-byte overlap, and every
// base URL in the measured album is 157 characters -- so a URL straddling a chunk boundary was lost,
// depending on where the boundaries happened to fall. This parser carries its state across calls,
// so a boundary anywhere, one byte at a time included, yields the same items. Each pattern has its
// own state, so two patterns never disturb each other either.

#ifndef GPHOTOS_PARSE_H
#define GPHOTOS_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gphotos_rules.h"

// `pattern` indexes the rule set the parser was started with.
typedef void (*gphotos_item_cb)(const char *key, size_t key_len, uint32_t w, uint32_t h,
                                size_t pattern, void *ctx);

typedef struct {
    uint8_t phase;
    uint8_t lit;       // how much of the anchor is matched
    uint8_t fail[GPHOTOS_ANCHOR_MAX];  // KMP failure table for the anchor
    uint8_t tok;       // the after token being matched
    uint8_t tok_pos;   // bytes of a literal token matched
    uint8_t digits;
    uint8_t n_nums;
    uint32_t num;
    uint32_t nums[2];
    size_t key_len;
    char key[GPHOTOS_KEY_MAX];
} gphotos_match_t;

typedef struct {
    const gphotos_rules_t *rules;  // must outlive the parse
    gphotos_match_t m[GPHOTOS_RULES_MAX_PATTERNS];
    gphotos_item_cb cb;
    void *ctx;
    size_t items;      // items emitted
    size_t overlong;   // keys skipped for exceeding their pattern's key max
} gphotos_parse_t;

void gphotos_parse_init(gphotos_parse_t *p, const gphotos_rules_t *rules, gphotos_item_cb cb,
                        void *ctx);
void gphotos_parse_feed(gphotos_parse_t *p, const char *buf, size_t len);

// End of the document: emits an item whose last number ran up to the very last byte.
void gphotos_parse_finish(gphotos_parse_t *p);

// The name a photograph has on /data: "gp_<8 hex>.jpg", the hex being FNV-1a 32 over the key.
//
// PINNED BY A TEST, because it is the identity: a firmware that computed it differently would find
// none of its photographs on the card and fetch the whole album again. Not the key itself, which is
// 120 characters of base64 against FAT's names, and not smb_manifest_local_name()'s "smb_" prefix,
// so the two mirrors' names cannot meet. Not the pattern either, so a rule update that keeps a key
// keeps its photograph.
bool gphotos_local_name(const char *key, size_t key_len, char *out, size_t out_size);

// The URL to fetch one photograph at, from its pattern's template. The built-in pattern gives
// "https://lh3.googleusercontent.com/pw/<key>=w<edge>-h<edge>", which asks the CDN to fit the
// photograph inside a square without cropping. False when `out` is too small.
bool gphotos_photo_url(const gphotos_pattern_t *pattern, const char *key, size_t key_len,
                       unsigned edge, char *out, size_t out_size);

// One line of a list file, without its line break. Version 1 lines are "<key>" and are read under
// GPHOTOS_RULES_DEFAULT_ID; version 2 lines are "<id> <key>". False for a malformed line: an id
// that is not [a-z0-9]{1,8}, or a key that is empty, too long, or holds a character that is not
// allowed. `id` is GPHOTOS_ID_SIZE bytes.
bool gphotos_dir_line(const char *line, size_t n, unsigned version, char *id, size_t *key_off,
                      size_t *key_len);

#endif // GPHOTOS_PARSE_H
