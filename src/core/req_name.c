#include "req_name.h"

#include <string.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool req_url_decode(const char *in, char *out, size_t size)
{
    if (!in || !out || size == 0) {
        return false;
    }
    size_t w = 0;
    for (const char *p = in; *p; p++) {
        if (w + 1 >= size) {
            return false;
        }
        if (*p == '%') {
            // p[2] is read ONLY when p[1] was a hex digit, so a string ending in '%' cannot be
            // read past its terminator: hex_value('\0') is -1 and the ternary short-circuits.
            // Pinned by a test, because the safety is in the evaluation order rather than in a
            // length check and a later tidy-up could lose it.
            const int hi = hex_value(p[1]);
            const int lo = hi < 0 ? -1 : hex_value(p[2]);
            if (lo < 0) {
                return false;
            }
            out[w++] = (char)((hi << 4) | lo);
            p += 2;
        } else if (*p == '+') {
            out[w++] = ' ';
        } else {
            out[w++] = *p;
        }
    }
    out[w] = '\0';
    return true;
}

bool req_url_encode(const char *in, char *out, size_t size)
{
    static const char *HEX = "0123456789ABCDEF";
    if (!in || !out || size == 0) {
        return false;
    }
    size_t w = 0;
    for (const char *p = in; *p; p++) {
        const unsigned char c = (unsigned char)*p;
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            if (w + 1 >= size) return false;
            out[w++] = (char)c;
        } else {
            if (w + 3 >= size) return false;
            out[w++] = '%';
            out[w++] = HEX[c >> 4];
            out[w++] = HEX[c & 0x0F];
        }
    }
    out[w] = '\0';
    return true;
}

bool req_name_is_safe(const char *name)
{
    return name && name[0] && !strstr(name, "..") && !strchr(name, '/') && !strchr(name, '\\');
}
