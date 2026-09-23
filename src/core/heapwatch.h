#pragma once

// A low-water record that remembers WHEN it was reached and WHAT was running.
//
// Ticket 47: the lowest internal-RAM figure this project has recorded is int_min=2899, and
// nothing in any capture can say what took the device there. heap_caps_get_minimum_free_size()
// is a since-boot watermark with no moment attached, so the heartbeat prints a number that is
// already history by the time it is read. Three single-cause candidates were tested against it
// and all three excluded; what is left needs the moment, not another candidate.
//
// This translation unit is the decision alone -- no esp_* anywhere -- for the reason img_dims.c
// and img_scale.c are separate: the arithmetic is what can be wrong quietly, and it is testable
// on the host. The sampling, the clock and the flags themselves are app_heapwatch.c's.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What was running at the instant of the low. A bitmask because the whole question is whether
// more than one of them was true at once -- ticket 47's remaining hypothesis is a three-way
// overlap, and a single "cause" field could not express it.
#define HEAPWATCH_F_SYNC (1u << 0)     // an SMB window is open
#define HEAPWATCH_F_DISPLAY (1u << 1)  // a panel refresh is in flight
#define HEAPWATCH_F_HTTP (1u << 2)     // an HTTP request began recently (see HEAPWATCH_HTTP_RECENT_MS)
// Ticket 60: an outbound HTTPS session is open. Set around each TLS session by the Google Photos
// mirror (app_gphotos_sync.c) and by the bench probe (app_gphotos.c, -DFRAME_GPHOTOS_PROBE). What
// matters is whether it is ever set AT THE SAME INSTANT as s, d or h: a session cost ~45-47 KB of
// internal RAM at mbedTLS's default allocation and ~10-14 KB at CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC.
#define HEAPWATCH_F_HTTPS (1u << 3)    // an outbound TLS fetch is in flight (bench only)

typedef struct {
    bool seen;            // false until the first sample; 0 is a legitimate `value`
    uint32_t value;       // bytes free in the pool this record watches, at the low
    uint32_t companion;   // the OTHER pool's bytes at that same instant, for context only
    int64_t at_ms;        // when, on the same clock the heartbeat prints
    uint32_t flags;       // HEAPWATCH_F_*, as sampled at that instant
    int64_t http_age_ms;  // ms since the last request began; -1 when there has never been one
} heapwatch_low_t;

// Replaces the record if `value` is a new low, and replaces EVERY field together when it does.
// Returns true when it replaced.
//
// Two rules that are decisions rather than details:
//
// - **An equal value does not displace.** The first arrival at a floor is the attributable one;
//   a later sample at the same depth may be the device sitting idle at a plateau, and taking it
//   would move `at_ms` and `flags` away from the event that caused the depth.
// - **All fields move or none do.** A value updated without its flags reads as an attributed low
//   and is a lie. This is the failure the host suite is built to catch.
bool heapwatch_note(heapwatch_low_t *low, uint32_t value, uint32_t companion, int64_t at_ms,
                    uint32_t flags, int64_t http_age_ms);

// "sdhg" for all four, "-" for none, in that fixed order so two lines can be compared by eye.
// Needs at least HEAPWATCH_FLAGS_STR_MIN bytes and returns `buf`.
//
// A caller-supplied buffer rather than a static one, because the heartbeat formats BOTH records in
// a single printf: a function returning its own static buffer would print one record's flags twice
// and the log would be quietly wrong in the field that carries the whole attribution.
#define HEAPWATCH_FLAGS_STR_MIN 5
char *heapwatch_flags_str(uint32_t flags, char *buf, size_t n);
