#include "pair_code.h"

#include <string.h>

static const char ALPHABET[] = PAIR_CODE_ALPHABET;

// -1 for a symbol that is not in the alphabet after folding.
static int symbol_value(char ch)
{
    for (int i = 0; i < 32; i++) {
        if (ALPHABET[i] == ch) {
            return i;
        }
    }
    return -1;
}

// One typed byte to the symbol it was meant to be, or 0 for "drop this" and -1 for "refuse the
// whole string".
//
// The folding is Crockford's, and each mapping is a mistake a reader of this panel actually
// makes: O for zero, I or l for one. U is NOT folded -- it is excluded from the alphabet, so a
// typed U is a typo in something else and refusing says so rather than silently pairing on a
// different code.
static int fold(char ch)
{
    if (ch == '-' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
        return 0;  // separator, dropped
    }
    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }
    if (ch == 'O') {
        return '0';
    }
    if (ch == 'I' || ch == 'L') {
        return '1';
    }
    return symbol_value(ch) >= 0 ? ch : -1;
}

bool pair_code_from_bytes(const uint8_t *bytes, size_t n, char *out, size_t size)
{
    if (bytes == NULL || out == NULL || size < PAIR_CODE_SIZE || n != PAIR_CODE_BYTES) {
        if (out != NULL && size > 0) {
            out[0] = '\0';
        }
        return false;
    }

    // 40 bits, most significant symbol first. A whole number of symbols fits a whole number of
    // bytes here, which is why the byte count is fixed rather than a parameter with slack.
    uint64_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc = (acc << 8) | bytes[i];
    }
    for (int i = PAIR_CODE_LEN - 1; i >= 0; i--) {
        out[i] = ALPHABET[acc & 0x1F];
        acc >>= 5;
    }
    out[PAIR_CODE_LEN] = '\0';
    return true;
}

bool pair_code_format(const char *code, char *out, size_t size)
{
    if (code == NULL || out == NULL || size < PAIR_CODE_DISPLAY_SIZE) {
        if (out != NULL && size > 0) {
            out[0] = '\0';
        }
        return false;
    }
    if (strlen(code) != PAIR_CODE_LEN) {
        out[0] = '\0';
        return false;
    }
    memcpy(out, code, 4);
    out[4] = '-';
    memcpy(out + 5, code + 4, 4);
    out[PAIR_CODE_DISPLAY_SIZE - 1] = '\0';
    return true;
}

bool pair_code_normalise(const char *in, char *out, size_t size)
{
    if (out == NULL || size < PAIR_CODE_SIZE) {
        if (out != NULL && size > 0) {
            out[0] = '\0';
        }
        return false;
    }
    out[0] = '\0';
    if (in == NULL) {
        return false;
    }

    size_t used = 0;
    for (const char *p = in; *p; p++) {
        const int folded = fold(*p);
        if (folded < 0) {
            out[0] = '\0';
            return false;
        }
        if (folded == 0) {
            continue;
        }
        if (used >= PAIR_CODE_LEN) {
            // Too long, refused rather than truncated: a truncating normaliser would compare
            // the first eight symbols of a paste and call a longer string right.
            out[0] = '\0';
            return false;
        }
        out[used++] = (char)folded;
    }
    out[used] = '\0';

    if (used != PAIR_CODE_LEN) {
        out[0] = '\0';
        return false;
    }
    return true;
}
