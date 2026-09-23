#include "gphotos_parse.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { PH_LIT, PH_KEY, PH_TOK };

// A KMP table rather than "restart at zero on a mismatch", and the literal is why: the built-in
// anchor contains "g" again at offset 3, so a naive restart misses a real match in
// "googoogleusercontent...". The host test has that case.
static void build_fail(const gphotos_pattern_t *pat, uint8_t *fail)
{
    const char *lit = pat->anchor;
    fail[0] = 0;
    size_t k = 0;
    for (size_t i = 1; i < pat->anchor_len; i++) {
        while (k > 0 && lit[i] != lit[k]) {
            k = fail[k - 1];
        }
        if (lit[i] == lit[k]) {
            k++;
        }
        fail[i] = (uint8_t)k;
    }
}

void gphotos_parse_init(gphotos_parse_t *p, const gphotos_rules_t *rules, gphotos_item_cb cb,
                        void *ctx)
{
    *p = (gphotos_parse_t){0};
    p->rules = rules;
    p->cb = cb;
    p->ctx = ctx;
    for (size_t i = 0; i < rules->n; i++) {
        build_fail(&rules->patterns[i], p->m[i].fail);
    }
}

static bool is_key_char(const gphotos_pattern_t *pat, char c)
{
    const uint8_t u = (uint8_t)c;
    return u < 128 && (pat->key_chars[u >> 3] & (1u << (u & 7))) != 0;
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static void emit(gphotos_parse_t *p, size_t i)
{
    gphotos_match_t *m = &p->m[i];
    m->key[m->key_len] = '\0';
    if (p->cb) {
        p->cb(m->key, m->key_len, m->nums[0], m->nums[1], i, p->ctx);
    }
    p->items++;
}

static void restart(gphotos_match_t *m)
{
    m->phase = PH_LIT;
    m->lit = 0;
}

// The next after token, or the item itself when there is none. False when the item was emitted,
// so the caller goes back to looking for the anchor.
static bool next_token(gphotos_parse_t *p, size_t i, uint8_t tok)
{
    gphotos_match_t *m = &p->m[i];
    if (tok >= p->rules->patterns[i].n_after) {
        emit(p, i);
        restart(m);
        return false;
    }
    m->phase = PH_TOK;
    m->tok = tok;
    m->tok_pos = 0;
    m->num = 0;
    m->digits = 0;
    return true;
}

static void feed_one(gphotos_parse_t *p, size_t i, char c)
{
    const gphotos_pattern_t *pat = &p->rules->patterns[i];
    gphotos_match_t *m = &p->m[i];
    for (;;) {
        switch (m->phase) {
        case PH_LIT:
            while (m->lit > 0 && c != pat->anchor[m->lit]) {
                m->lit = m->fail[m->lit - 1];
            }
            if (c == pat->anchor[m->lit]) {
                m->lit++;
            }
            if (m->lit == pat->anchor_len) {
                m->phase = PH_KEY;
                m->key_len = 0;
                m->n_nums = 0;
                m->nums[0] = m->nums[1] = 0;
                m->lit = 0;
            }
            return;

        case PH_KEY:
            if (is_key_char(pat, c)) {
                if (m->key_len < pat->key_max) {
                    m->key[m->key_len++] = c;
                    return;
                }
                p->overlong++;
                break;
            }
            if (m->key_len == 0) {
                break;
            }
            // The key ends at the first character that cannot be in it, which is then looked at
            // as the start of what follows -- or, with nothing to follow, as a fresh start.
            next_token(p, i, 0);
            continue;

        case PH_TOK: {
            const gphotos_token_t *t = &pat->after[m->tok];
            if (t->kind == GPHOTOS_TOK_LIT) {
                if (c != t->lit[m->tok_pos]) {
                    break;
                }
                if (++m->tok_pos == t->len) {
                    next_token(p, i, (uint8_t)(m->tok + 1));
                }
                return;
            }
            if (is_digit(c) && m->digits < 5) {
                m->num = m->num * 10 + (uint32_t)(c - '0');
                m->digits++;
                return;
            }
            if (m->digits < 2) {
                break;
            }
            // The number ends at the first non-digit (or a sixth digit), which is then looked at
            // afresh: it may be the next token, or where the next item's anchor begins.
            if (m->n_nums < 2) {
                m->nums[m->n_nums] = m->num;
            }
            m->n_nums++;
            next_token(p, i, (uint8_t)(m->tok + 1));
            continue;
        }
        }
        // Not an item after all. The character that ended it is looked at again from the start of
        // the anchor, so a 'g' that broke a partial item can still begin the next one.
        restart(m);
    }
}

void gphotos_parse_feed(gphotos_parse_t *p, const char *buf, size_t len)
{
    const size_t n = p->rules->n;
    for (size_t k = 0; k < len; k++) {
        for (size_t i = 0; i < n; i++) {
            feed_one(p, i, buf[k]);
        }
    }
}

void gphotos_parse_finish(gphotos_parse_t *p)
{
    for (size_t i = 0; i < p->rules->n; i++) {
        const gphotos_pattern_t *pat = &p->rules->patterns[i];
        gphotos_match_t *m = &p->m[i];
        const bool last_num = m->phase == PH_TOK && m->tok + 1u == pat->n_after &&
                              pat->after[m->tok].kind == GPHOTOS_TOK_NUM && m->digits >= 2;
        const bool bare_key = m->phase == PH_KEY && pat->n_after == 0 && m->key_len > 0;
        if (last_num) {
            if (m->n_nums < 2) {
                m->nums[m->n_nums] = m->num;
            }
            m->n_nums++;
        }
        if (last_num || bare_key) {
            emit(p, i);
        }
        restart(m);
    }
}

bool gphotos_local_name(const char *key, size_t key_len, char *out, size_t out_size)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < key_len; i++) {
        h ^= (uint8_t)key[i];
        h *= 16777619u;
    }
    const int n = snprintf(out, out_size, "gp_%08" PRIx32 ".jpg", h);
    return n > 0 && (size_t)n < out_size;
}

bool gphotos_photo_url(const gphotos_pattern_t *pattern, const char *key, size_t key_len,
                       unsigned edge, char *out, size_t out_size)
{
    return gphotos_rules_url(pattern, key, key_len, edge, out, out_size);
}

bool gphotos_dir_line(const char *line, size_t n, unsigned version, char *id, size_t *key_off,
                      size_t *key_len)
{
    size_t at = 0;
    memset(id, 0, GPHOTOS_ID_SIZE);
    if (version == 1) {
        memcpy(id, GPHOTOS_RULES_DEFAULT_ID, sizeof(GPHOTOS_RULES_DEFAULT_ID) - 1);
    } else if (version == 2) {
        while (at < n && line[at] != ' ') {
            const char c = line[at];
            if (at + 1 >= GPHOTOS_ID_SIZE || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
                return false;
            }
            id[at] = c;
            at++;
        }
        if (at == 0 || at >= n) {
            return false;
        }
        at++; // the space
    } else {
        return false;
    }
    const size_t k = n - at;
    if (k == 0 || k >= GPHOTOS_KEY_MAX) {
        return false;
    }
    for (size_t i = at; i < n; i++) {
        if (!gphotos_key_char_allowed(line[i])) {
            return false;
        }
    }
    *key_off = at;
    *key_len = k;
    return true;
}
