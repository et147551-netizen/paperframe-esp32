// Ticket 60: finding a shared album's photographs in its HTML, on the host.
//
// No real album body is used or kept -- a body carries members' names, account ids, EXIF, the
// share key and every photo URL (gphotos_prereg.md, "derive before you retain"). The fixtures are
// synthetic and built to the shape the arms measured: a 120-character key after the lh3 /pw/
// prefix, which is a 157-character URL, followed by `",w,h`.

#include <stdio.h>
#include <string.h>

#include "gphotos_parse.h"
#include "unity.h"

#define GOT_MAX 8

typedef struct {
    size_t n;
    char key[GOT_MAX][GPHOTOS_KEY_MAX];
    uint32_t w[GOT_MAX];
    uint32_t h[GOT_MAX];
} got_t;

static got_t s_got;

// Every case runs through the built-in rule set, which must reproduce the hard-coded parser these
// cases were written against (the scrape-rules plan).
static gphotos_rules_t s_rules;

static void collect(const char *key, size_t key_len, uint32_t w, uint32_t h, size_t pattern,
                    void *ctx)
{
    (void)pattern;
    (void)ctx;
    TEST_ASSERT_EQUAL_size_t(strlen(key), key_len);
    if (s_got.n < GOT_MAX) {
        snprintf(s_got.key[s_got.n], GPHOTOS_KEY_MAX, "%s", key);
        s_got.w[s_got.n] = w;
        s_got.h[s_got.n] = h;
    }
    s_got.n++;
}

void setUp(void)
{
    memset(&s_got, 0, sizeof(s_got));
    gphotos_rules_default(&s_rules);
}

void tearDown(void)
{
}

static gphotos_parse_t s_p;

static size_t parse_split(const char *text, size_t len, size_t split)
{
    memset(&s_got, 0, sizeof(s_got));
    gphotos_parse_init(&s_p, &s_rules, collect, NULL);
    gphotos_parse_feed(&s_p, text, split);
    gphotos_parse_feed(&s_p, text + split, len - split);
    gphotos_parse_finish(&s_p);
    return s_got.n;
}

static void make_key(char *out, size_t n, size_t salt)
{
    static const char cs[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";
    for (size_t i = 0; i < n; i++) {
        out[i] = cs[(i * 7 + salt) % (sizeof(cs) - 1)];
    }
    out[n] = '\0';
}

static void test_one_item(void)
{
    const char *t = "[\"https://lh3.googleusercontent.com/pw/AbC_-9\",4000,3000,null]";
    TEST_ASSERT_EQUAL_size_t(1, parse_split(t, strlen(t), 0));
    TEST_ASSERT_EQUAL_STRING("AbC_-9", s_got.key[0]);
    TEST_ASSERT_EQUAL_UINT32(4000, s_got.w[0]);
    TEST_ASSERT_EQUAL_UINT32(3000, s_got.h[0]);
}

static void test_preview_and_avatar_are_not_photographs(void)
{
    const char *t = "<img src=\"https://lh3.googleusercontent.com/pw/AbC=w96-h72-no\">"
                    "\"https://lh3.googleusercontent.com/a/ACg8ocXYZ\",96,96"
                    "\"https://lh3.googleusercontent.com/ogw/default-user\",40,40";
    TEST_ASSERT_EQUAL_size_t(0, parse_split(t, strlen(t), 0));
}

// The memo's defect class: a 157-character URL across a chunk boundary. Every split position,
// and one byte at a time, must give exactly the same two items.
static void test_every_split_and_byte_at_a_time(void)
{
    char k1[121], k2[121];
    make_key(k1, 120, 1);
    make_key(k2, 120, 5);
    char t[1024];
    const int len = snprintf(t, sizeof(t),
                             "<img src=\"https://lh3.googleusercontent.com/pw/%s=w96-h72-no\">"
                             "[\"https://lh3.googleusercontent.com/pw/%s\",4000,3000,null],"
                             "[\"https://lh3.googleusercontent.com/pw/%s\",768,1344]",
                             k1, k1, k2);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof(t));

    for (size_t split = 0; split <= (size_t)len; split++) {
        TEST_ASSERT_EQUAL_size_t_MESSAGE(2, parse_split(t, (size_t)len, split), "split");
        TEST_ASSERT_EQUAL_STRING(k1, s_got.key[0]);
        TEST_ASSERT_EQUAL_UINT32(4000, s_got.w[0]);
        TEST_ASSERT_EQUAL_UINT32(3000, s_got.h[0]);
        TEST_ASSERT_EQUAL_STRING(k2, s_got.key[1]);
        TEST_ASSERT_EQUAL_UINT32(768, s_got.w[1]);
        TEST_ASSERT_EQUAL_UINT32(1344, s_got.h[1]);
    }

    memset(&s_got, 0, sizeof(s_got));
    gphotos_parse_init(&s_p, &s_rules, collect, NULL);
    for (int i = 0; i < len; i++) {
        gphotos_parse_feed(&s_p, t + i, 1);
    }
    gphotos_parse_finish(&s_p);
    TEST_ASSERT_EQUAL_size_t(2, s_got.n);
    TEST_ASSERT_EQUAL_STRING(k2, s_got.key[1]);
}

// "goo" then "google...": a matcher that restarts at zero on a mismatch loses this one.
static void test_literal_overlap(void)
{
    const char *t = "googoogleusercontent.com/pw/ABCDEFGH\",400,600]";
    TEST_ASSERT_EQUAL_size_t(1, parse_split(t, strlen(t), 0));
    TEST_ASSERT_EQUAL_STRING("ABCDEFGH", s_got.key[0]);
}

static void test_overlong_key_is_skipped_and_the_next_item_survives(void)
{
    char big[GPHOTOS_KEY_MAX + 1];
    make_key(big, GPHOTOS_KEY_MAX, 3);
    char t[1024];
    const int len = snprintf(t, sizeof(t),
                             "googleusercontent.com/pw/%s\",400,600]"
                             "googleusercontent.com/pw/OK\",400,600]",
                             big);
    TEST_ASSERT_EQUAL_size_t(1, parse_split(t, (size_t)len, 0));
    TEST_ASSERT_EQUAL_STRING("OK", s_got.key[0]);
    TEST_ASSERT_EQUAL_size_t(1, s_p.overlong);
}

static void test_dimensions_need_two_digits(void)
{
    const char *t = "googleusercontent.com/pw/K\",4,600]googleusercontent.com/pw/K\",400,6]";
    TEST_ASSERT_EQUAL_size_t(0, parse_split(t, strlen(t), 0));
}

static void test_item_at_the_very_end_needs_finish(void)
{
    const char *t = "googleusercontent.com/pw/END\",400,600";
    gphotos_parse_init(&s_p, &s_rules, collect, NULL);
    gphotos_parse_feed(&s_p, t, strlen(t));
    TEST_ASSERT_EQUAL_size_t(0, s_got.n);
    gphotos_parse_finish(&s_p);
    TEST_ASSERT_EQUAL_size_t(1, s_got.n);
    TEST_ASSERT_EQUAL_UINT32(600, s_got.h[0]);
}

// The identity is pinned: FNV-1a 32 of "a" is 0xe40c292c.
static void test_local_name_is_pinned(void)
{
    char out[32];
    TEST_ASSERT_TRUE(gphotos_local_name("a", 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("gp_e40c292c.jpg", out);

    char k1[121], k2[121], n1[32], n2[32];
    make_key(k1, 120, 1);
    make_key(k2, 120, 2);
    TEST_ASSERT_TRUE(gphotos_local_name(k1, 120, n1, sizeof(n1)));
    TEST_ASSERT_TRUE(gphotos_local_name(k2, 120, n2, sizeof(n2)));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(n1, n2));

    char small[15]; // "gp_xxxxxxxx.jpg" is 15 characters and needs 16 bytes
    TEST_ASSERT_FALSE(gphotos_local_name("a", 1, small, sizeof(small)));
}

static void test_photo_url(void)
{
    char out[64];
    TEST_ASSERT_TRUE(gphotos_photo_url(&s_rules.patterns[0], "KEY", 3, 600, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://lh3.googleusercontent.com/pw/KEY=w600-h600", out);

    const size_t need = strlen("https://lh3.googleusercontent.com/pw/KEY=w600-h600");
    char exact[64];
    TEST_ASSERT_TRUE(gphotos_photo_url(&s_rules.patterns[0], "KEY", 3, 600, exact, need + 1));
    TEST_ASSERT_FALSE(gphotos_photo_url(&s_rules.patterns[0], "KEY", 3, 600, exact, need));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_one_item);
    RUN_TEST(test_preview_and_avatar_are_not_photographs);
    RUN_TEST(test_every_split_and_byte_at_a_time);
    RUN_TEST(test_literal_overlap);
    RUN_TEST(test_overlong_key_is_skipped_and_the_next_item_survives);
    RUN_TEST(test_dimensions_need_two_digits);
    RUN_TEST(test_item_at_the_very_end_needs_finish);
    RUN_TEST(test_local_name_is_pinned);
    RUN_TEST(test_photo_url);
    return UNITY_END();
}
