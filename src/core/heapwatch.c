#include "heapwatch.h"

bool heapwatch_note(heapwatch_low_t *low, uint32_t value, uint32_t companion, int64_t at_ms,
                    uint32_t flags, int64_t http_age_ms)
{
    if (!low) {
        return false;
    }
    if (low->seen && value >= low->value) {
        return false;
    }
    low->seen = true;
    low->value = value;
    low->companion = companion;
    low->at_ms = at_ms;
    low->flags = flags;
    low->http_age_ms = http_age_ms;
    return true;
}

char *heapwatch_flags_str(uint32_t flags, char *buf, size_t n)
{
    if (!buf || n == 0) {
        return buf;
    }
    size_t i = 0;
    if ((flags & HEAPWATCH_F_SYNC) && i + 1 < n) {
        buf[i++] = 's';
    }
    if ((flags & HEAPWATCH_F_DISPLAY) && i + 1 < n) {
        buf[i++] = 'd';
    }
    if ((flags & HEAPWATCH_F_HTTP) && i + 1 < n) {
        buf[i++] = 'h';
    }
    // 'g' for the outbound TLS fetch (ticket 60), last so the existing three keep their columns
    // and a capture from before this flag existed still reads the same way.
    if ((flags & HEAPWATCH_F_HTTPS) && i + 1 < n) {
        buf[i++] = 'g';
    }
    if (i == 0 && n > 1) {
        buf[i++] = '-';
    }
    buf[i] = '\0';
    return buf;
}
