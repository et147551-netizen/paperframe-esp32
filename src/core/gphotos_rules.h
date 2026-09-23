// The rules that find a Google Photos shared album's photographs, as DATA rather than code.
//
// Designed in the scrape-rules plan. Google promises nothing about the album page's markup
// (FR-10.4), and the frame sits where nobody can flash it, so what the scrape looks for travels as a
// signed rule file instead of a rebuild. This module is the rule set itself and its validator: pure,
// no ESP-IDF, host-tested under env:native. JSON and the signature are the device's side
// (app_gphotos_rules.c) -- env:native has no cJSON and no mbedtls.
//
// A PATTERN is what one media item looks like in the stream:
//
//     <anchor><key>[after tokens...]
//
// -- a literal, then a run of key characters, then a fixed sequence of literals and numbers. The
// built-in default is today's measured rule, `googleusercontent.com/pw/<key>",<w>,<h>`
// (gphotos_parse.h). The first two numbers of a match are reported as its width and height.
//
// THE VALIDATOR IS THE SECURITY BOUNDARY a signature cannot be: it is what stops a rule set, however
// it arrived, from putting a key where a host goes or a line break into a list file.

#ifndef GPHOTOS_RULES_H
#define GPHOTOS_RULES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Room for the key, NUL included. Measured keys are 120 characters; a longer one is counted in
// `overlong` and skipped rather than truncated, because a truncated key fetches nothing.
#define GPHOTOS_KEY_MAX 256

#define GPHOTOS_RULES_MAX_PATTERNS 4
#define GPHOTOS_ANCHOR_MAX 32          // bytes; the matcher's KMP table is this long
#define GPHOTOS_ID_SIZE 9              // 1-8 of [a-z0-9], NUL included
#define GPHOTOS_AFTER_MAX 8            // tokens after the key
#define GPHOTOS_LIT_MAX 8              // bytes in one literal token
#define GPHOTOS_URL_TEMPLATE_SIZE 160  // NUL included
#define GPHOTOS_UA_SIZE 192            // NUL included
#define GPHOTOS_RULES_FILE_MAX 4096    // a signed rule file, signature line included

// The id a `#gpdir 1` list's keys are read under: the built-in pattern's.
#define GPHOTOS_RULES_DEFAULT_ID "lh3"

typedef enum { GPHOTOS_TOK_LIT = 0, GPHOTOS_TOK_NUM } gphotos_tok_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t len;  // GPHOTOS_TOK_LIT only
    char lit[GPHOTOS_LIT_MAX + 1];
} gphotos_token_t;

typedef struct {
    char id[GPHOTOS_ID_SIZE];
    char anchor[GPHOTOS_ANCHOR_MAX + 1];
    uint8_t anchor_len;
    uint8_t key_chars[16];  // bitmap over ASCII 0-127
    uint16_t key_max;       // characters, < GPHOTOS_KEY_MAX
    gphotos_token_t after[GPHOTOS_AFTER_MAX];
    uint8_t n_after;
    // "https://<host>/...{key}...{edge}...": {key} exactly once, {edge} any number of times, and
    // neither in the host.
    char url[GPHOTOS_URL_TEMPLATE_SIZE];
} gphotos_pattern_t;

// The built-in set's seq. Raise it past the highest published rule file whenever a firmware changes
// gphotos_rules_default(): a set kept in NVS is in force only while its seq is ABOVE this, so without
// the raise a newer firmware keeps scraping with an older downloaded set (ticket 61).
#define GPHOTOS_RULES_BUILTIN_SEQ 0u

typedef struct {
    uint32_t seq;  // GPHOTOS_RULES_BUILTIN_SEQ is the built-in set; a rule file is above it
    char ua[GPHOTOS_UA_SIZE];
    gphotos_pattern_t patterns[GPHOTOS_RULES_MAX_PATTERNS];
    uint8_t n;
} gphotos_rules_t;

// Every function that can refuse sets *why to a string literal saying what was wrong.

// The characters a key may ever hold, whatever a rule says: [A-Za-z0-9._~%/-]. A list file is one
// entry per line and a key goes into a URL's path, so a space, a quote, a line break, '?', '#' or
// '@' must never be one.
bool gphotos_key_char_allowed(char c);

// Appends one pattern to `r`. `chars` is a set like "A-Za-z0-9_-" (ranges and single characters; a
// '-' that is first or last is itself), every member of which must be allowed. `after` holds
// "num" and "lit:<1-8 printable bytes>" tokens.
bool gphotos_rules_add_pattern(gphotos_rules_t *r, const char *id, const char *anchor,
                               const char *chars, unsigned key_max, const char *const *after,
                               size_t n_after, const char *url, const char **why);

// Printable ASCII, 1-191 characters: it goes into a request header.
bool gphotos_rules_set_ua(gphotos_rules_t *r, const char *ua, const char **why);

// What a whole rule set needs beyond each part: at least one pattern, distinct ids, a User-Agent.
bool gphotos_rules_check(const gphotos_rules_t *r, const char **why);

// The built-in set: today's measured lh3 rule and a browser User-Agent, GPHOTOS_RULES_BUILTIN_SEQ.
void gphotos_rules_default(gphotos_rules_t *r);

// The index of the pattern called `id`, or -1.
int gphotos_rules_find(const gphotos_rules_t *r, const char *id);

// A photograph's URL from its pattern's template. False when `key` holds a character that is not
// allowed or `out` is too small.
bool gphotos_rules_url(const gphotos_pattern_t *p, const char *key, size_t key_len, unsigned edge,
                       char *out, size_t out_size);

#endif // GPHOTOS_RULES_H
