#include "gphotos_rules.h"

#include <stdio.h>
#include <string.h>

#define UA_DEFAULT                                                                          \
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) " \
    "Chrome/120.0.0.0 Safari/537.36"

bool gphotos_key_char_allowed(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '~' || c == '%' || c == '/' || c == '-';
}

static bool printable(char c)
{
    return c >= 0x20 && c <= 0x7e;
}

static bool refuse(const char **why, const char *msg)
{
    if (why) {
        *why = msg;
    }
    return false;
}

static void set_bit(uint8_t *bits, char c)
{
    bits[(uint8_t)c >> 3] |= (uint8_t)(1u << ((uint8_t)c & 7));
}

static bool parse_chars(uint8_t *bits, const char *spec, const char **why)
{
    memset(bits, 0, 16);
    if (!spec || spec[0] == '\0') {
        return refuse(why, "key chars are empty");
    }
    for (size_t i = 0; spec[i] != '\0';) {
        char lo = spec[i];
        char hi = lo;
        if (spec[i + 1] == '-' && spec[i + 2] != '\0') {
            hi = spec[i + 2];
            i += 3;
        } else {
            i += 1;
        }
        if (lo > hi) {
            return refuse(why, "key chars hold a backwards range");
        }
        for (int c = (unsigned char)lo; c <= (unsigned char)hi; c++) {
            if (!gphotos_key_char_allowed((char)c)) {
                return refuse(why, "key chars go beyond [A-Za-z0-9._~%/-]");
            }
            set_bit(bits, (char)c);
        }
    }
    return true;
}

static bool parse_token(gphotos_token_t *t, const char *s, const char **why)
{
    memset(t, 0, sizeof(*t));
    if (!s) {
        return refuse(why, "an after token is not a string");
    }
    if (strcmp(s, "num") == 0) {
        t->kind = GPHOTOS_TOK_NUM;
        return true;
    }
    if (strncmp(s, "lit:", 4) != 0) {
        return refuse(why, "an after token is neither \"num\" nor \"lit:...\"");
    }
    const size_t n = strlen(s + 4);
    if (n == 0 || n > GPHOTOS_LIT_MAX) {
        return refuse(why, "a literal token is not 1-8 bytes");
    }
    for (size_t i = 0; i < n; i++) {
        if (!printable(s[4 + i])) {
            return refuse(why, "a literal token holds an unprintable byte");
        }
    }
    t->kind = GPHOTOS_TOK_LIT;
    t->len = (uint8_t)n;
    memcpy(t->lit, s + 4, n);
    return true;
}

static bool host_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '-';
}

static bool check_url(const char *url, const char **why)
{
    static const char HTTPS[] = "https://";
    if (!url || strncmp(url, HTTPS, sizeof(HTTPS) - 1) != 0) {
        return refuse(why, "url does not start with https://");
    }
    if (strlen(url) >= GPHOTOS_URL_TEMPLATE_SIZE) {
        return refuse(why, "url is too long");
    }
    const char *p = url + sizeof(HTTPS) - 1;
    size_t host = 0;
    while (p[host] != '\0' && p[host] != '/') {
        if (!host_char(p[host])) {
            return refuse(why, "url host holds a character a host cannot");
        }
        host++;
    }
    if (host == 0 || p[host] != '/') {
        return refuse(why, "url has no host and path");
    }
    unsigned keys = 0;
    for (const char *q = p + host; *q != '\0';) {
        if (*q == '{') {
            if (strncmp(q, "{key}", 5) == 0) {
                keys++;
                q += 5;
            } else if (strncmp(q, "{edge}", 6) == 0) {
                q += 6;
            } else {
                return refuse(why, "url holds a placeholder other than {key} and {edge}");
            }
            continue;
        }
        if (*q == '}' || !printable(*q) || *q == ' ') {
            return refuse(why, "url holds a stray brace, a space or an unprintable byte");
        }
        q++;
    }
    if (keys != 1) {
        return refuse(why, "url must hold {key} exactly once");
    }
    return true;
}

static bool check_id(const char *id, const char **why)
{
    const size_t n = id ? strlen(id) : 0;
    if (n == 0 || n >= GPHOTOS_ID_SIZE) {
        return refuse(why, "a pattern id is not 1-8 characters");
    }
    for (size_t i = 0; i < n; i++) {
        if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9'))) {
            return refuse(why, "a pattern id is not [a-z0-9]");
        }
    }
    return true;
}

bool gphotos_rules_add_pattern(gphotos_rules_t *r, const char *id, const char *anchor,
                               const char *chars, unsigned key_max, const char *const *after,
                               size_t n_after, const char *url, const char **why)
{
    if (r->n >= GPHOTOS_RULES_MAX_PATTERNS) {
        return refuse(why, "more than 4 patterns");
    }
    gphotos_pattern_t p;
    memset(&p, 0, sizeof(p));
    if (!check_id(id, why)) {
        return false;
    }
    memcpy(p.id, id, strlen(id));

    const size_t an = anchor ? strlen(anchor) : 0;
    if (an == 0 || an > GPHOTOS_ANCHOR_MAX) {
        return refuse(why, "an anchor is not 1-32 bytes");
    }
    for (size_t i = 0; i < an; i++) {
        if (!printable(anchor[i])) {
            return refuse(why, "an anchor holds an unprintable byte");
        }
    }
    memcpy(p.anchor, anchor, an);
    p.anchor_len = (uint8_t)an;

    if (!parse_chars(p.key_chars, chars, why)) {
        return false;
    }
    if (key_max == 0 || key_max >= GPHOTOS_KEY_MAX) {
        return refuse(why, "key max is not 1-255");
    }
    p.key_max = (uint16_t)key_max;

    if (n_after > GPHOTOS_AFTER_MAX) {
        return refuse(why, "more than 8 after tokens");
    }
    for (size_t i = 0; i < n_after; i++) {
        if (!parse_token(&p.after[i], after[i], why)) {
            return false;
        }
    }
    p.n_after = (uint8_t)n_after;

    if (!check_url(url, why)) {
        return false;
    }
    memcpy(p.url, url, strlen(url));

    r->patterns[r->n++] = p;
    return true;
}

bool gphotos_rules_set_ua(gphotos_rules_t *r, const char *ua, const char **why)
{
    const size_t n = ua ? strlen(ua) : 0;
    if (n == 0 || n >= GPHOTOS_UA_SIZE) {
        return refuse(why, "ua is not 1-191 characters");
    }
    for (size_t i = 0; i < n; i++) {
        if (!printable(ua[i])) {
            return refuse(why, "ua holds an unprintable byte");
        }
    }
    memset(r->ua, 0, sizeof(r->ua));
    memcpy(r->ua, ua, n);
    return true;
}

bool gphotos_rules_check(const gphotos_rules_t *r, const char **why)
{
    if (r->n == 0 || r->n > GPHOTOS_RULES_MAX_PATTERNS) {
        return refuse(why, "no pattern");
    }
    if (r->ua[0] == '\0') {
        return refuse(why, "no ua");
    }
    for (size_t i = 0; i < r->n; i++) {
        for (size_t j = i + 1; j < r->n; j++) {
            if (strcmp(r->patterns[i].id, r->patterns[j].id) == 0) {
                return refuse(why, "two patterns share an id");
            }
        }
    }
    return true;
}

void gphotos_rules_default(gphotos_rules_t *r)
{
    static const char *const AFTER[] = {"lit:\"", "lit:,", "num", "lit:,", "num"};
    memset(r, 0, sizeof(*r));
    r->seq = GPHOTOS_RULES_BUILTIN_SEQ;
    gphotos_rules_set_ua(r, UA_DEFAULT, NULL);
    gphotos_rules_add_pattern(r, GPHOTOS_RULES_DEFAULT_ID, "googleusercontent.com/pw/",
                              "A-Za-z0-9_-", GPHOTOS_KEY_MAX - 1, AFTER,
                              sizeof(AFTER) / sizeof(AFTER[0]),
                              "https://lh3.googleusercontent.com/pw/{key}=w{edge}-h{edge}", NULL);
}

int gphotos_rules_find(const gphotos_rules_t *r, const char *id)
{
    for (size_t i = 0; i < r->n; i++) {
        if (strcmp(r->patterns[i].id, id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool gphotos_rules_url(const gphotos_pattern_t *p, const char *key, size_t key_len, unsigned edge,
                       char *out, size_t out_size)
{
    if (out_size == 0) {
        return false;
    }
    for (size_t i = 0; i < key_len; i++) {
        if (!gphotos_key_char_allowed(key[i])) {
            return false;
        }
    }
    char num[12];
    const int nn = snprintf(num, sizeof(num), "%u", edge);
    size_t at = 0;
    for (const char *q = p->url; *q != '\0';) {
        const char *piece = q;
        size_t len = 1;
        if (strncmp(q, "{key}", 5) == 0) {
            piece = key;
            len = key_len;
            q += 5;
        } else if (strncmp(q, "{edge}", 6) == 0) {
            piece = num;
            len = (size_t)nn;
            q += 6;
        } else {
            q++;
        }
        if (at + len >= out_size) {
            return false;
        }
        memcpy(out + at, piece, len);
        at += len;
    }
    out[at] = '\0';
    return true;
}
