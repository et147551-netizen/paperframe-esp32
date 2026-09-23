// The scrape-rules plan: the Google Photos scrape as a rule set. What the validator
// refuses, what a second pattern finds, and how a list file line is read.
//
// Synthetic fixtures only, for test_gphotos_parse's reason. The fife pattern here is a SHAPE the
// validator and matcher must handle, not a measured rule for that host.

#include <stdio.h>
#include <string.h>

#include "gphotos_parse.h"
#include "gphotos_rules.h"
#include "unity.h"

#define GOT_MAX 8

typedef struct {
    size_t n;
    char key[GOT_MAX][GPHOTOS_KEY_MAX];
    size_t pattern[GOT_MAX];
    uint32_t w[GOT_MAX];
    uint32_t h[GOT_MAX];
} got_t;

static got_t s_got;
static gphotos_rules_t s_rules;
static gphotos_parse_t s_p;
static const char *s_why;

static const char *const AFTER[] = {"lit:\"", "lit:,", "num", "lit:,", "num"};
#define N_AFTER (sizeof(AFTER) / sizeof(AFTER[0]))

static void collect(const char *key, size_t key_len, uint32_t w, uint32_t h, size_t pattern,
                    void *ctx)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_size_t(strlen(key), key_len);
    if (s_got.n < GOT_MAX) {
        snprintf(s_got.key[s_got.n], GPHOTOS_KEY_MAX, "%s", key);
        s_got.pattern[s_got.n] = pattern;
        s_got.w[s_got.n] = w;
        s_got.h[s_got.n] = h;
    }
    s_got.n++;
}

void setUp(void)
{
    memset(&s_got, 0, sizeof(s_got));
    memset(&s_rules, 0, sizeof(s_rules));
    s_why = NULL;
}

void tearDown(void)
{
}

static size_t parse_split(const char *text, size_t len, size_t split)
{
    memset(&s_got, 0, sizeof(s_got));
    gphotos_parse_init(&s_p, &s_rules, collect, NULL);
    gphotos_parse_feed(&s_p, text, split);
    gphotos_parse_feed(&s_p, text + split, len - split);
    gphotos_parse_finish(&s_p);
    return s_got.n;
}

static bool add(const char *id, const char *anchor, const char *chars, unsigned max,
                const char *const *after, size_t n_after, const char *url)
{
    s_why = NULL;
    const bool ok =
        gphotos_rules_add_pattern(&s_rules, id, anchor, chars, max, after, n_after, url, &s_why);
    TEST_ASSERT_TRUE_MESSAGE(ok || s_why != NULL, "a refusal must say why");
    return ok;
}

#define URL "https://h.example/pw/{key}=w{edge}"

static void test_default_is_valid(void)
{
    gphotos_rules_default(&s_rules);
    TEST_ASSERT_TRUE(gphotos_rules_check(&s_rules, &s_why));
    TEST_ASSERT_EQUAL_UINT32(0, s_rules.seq);
    TEST_ASSERT_EQUAL_INT(0, gphotos_rules_find(&s_rules, GPHOTOS_RULES_DEFAULT_ID));
    TEST_ASSERT_EQUAL_INT(-1, gphotos_rules_find(&s_rules, "fife"));
}

static void test_a_valid_pattern_is_accepted(void)
{
    TEST_ASSERT_TRUE(add("a1", "x/pw/", "A-Za-z0-9._~%/-", 255, AFTER, N_AFTER, URL));
    TEST_ASSERT_EQUAL_UINT8(1, s_rules.n);
}

static void test_ids_are_refused(void)
{
    TEST_ASSERT_FALSE(add("", "x", "A", 10, NULL, 0, URL));
    TEST_ASSERT_FALSE(add("abcdefghi", "x", "A", 10, NULL, 0, URL));
    TEST_ASSERT_FALSE(add("Lh3", "x", "A", 10, NULL, 0, URL));
    TEST_ASSERT_FALSE(add("a b", "x", "A", 10, NULL, 0, URL));
    TEST_ASSERT_TRUE(add("abcdefgh", "x", "A", 10, NULL, 0, URL));
}

static void test_anchors_are_refused(void)
{
    TEST_ASSERT_FALSE(add("a", "", "A", 10, NULL, 0, URL));
    TEST_ASSERT_FALSE(add("a", "123456789012345678901234567890123", "A", 10, NULL, 0, URL));
    TEST_ASSERT_FALSE(add("a", "x\ny", "A", 10, NULL, 0, URL));
    TEST_ASSERT_TRUE(add("a", "12345678901234567890123456789012", "A", 10, NULL, 0, URL));
}

static void test_key_chars_beyond_the_safe_set_are_refused(void)
{
    TEST_ASSERT_FALSE(add("a", "x", "", 10, NULL, 0, URL));
    TEST_ASSERT_FALSE(add("a", "x", "A-z", 10, NULL, 0, URL)); // takes in [ \ ] ^ `
    TEST_ASSERT_FALSE(add("a", "x", "A-Z ", 10, NULL, 0, URL));
    TEST_ASSERT_FALSE(add("a", "x", "A-Z\"", 10, NULL, 0, URL));
    TEST_ASSERT_FALSE(add("a", "x", "A-Z?", 10, NULL, 0, URL));
    TEST_ASSERT_FALSE(add("a", "x", "z-a", 10, NULL, 0, URL));
    TEST_ASSERT_FALSE(add("a", "x", "A", 0, NULL, 0, URL));
    TEST_ASSERT_FALSE(add("a", "x", "A", GPHOTOS_KEY_MAX, NULL, 0, URL));
}

static void test_tokens_are_refused(void)
{
    static const char *const NINE[] = {"num", "num", "num", "num", "num",
                                       "num", "num", "num", "num"};
    static const char *const EMPTY_LIT[] = {"lit:"};
    static const char *const LONG_LIT[] = {"lit:123456789"};
    static const char *const BAD[] = {"number"};
    static const char *const EIGHT_LIT[] = {"lit:12345678"};
    TEST_ASSERT_FALSE(add("a", "x", "A", 10, NINE, 9, URL));
    TEST_ASSERT_FALSE(add("a", "x", "A", 10, EMPTY_LIT, 1, URL));
    TEST_ASSERT_FALSE(add("a", "x", "A", 10, LONG_LIT, 1, URL));
    TEST_ASSERT_FALSE(add("a", "x", "A", 10, BAD, 1, URL));
    TEST_ASSERT_TRUE(add("a", "x", "A", 10, EIGHT_LIT, 1, URL));
}

static void test_urls_are_refused(void)
{
    TEST_ASSERT_FALSE(add("a", "x", "A", 10, NULL, 0, "http://h.example/{key}"));
    TEST_ASSERT_FALSE(add("a", "x", "A", 10, NULL, 0, "https://h.example/pw/"));
    TEST_ASSERT_FALSE(add("a", "x", "A", 10, NULL, 0, "https://h.example/{key}/{key}"));
    TEST_ASSERT_FALSE(add("a", "x", "A", 10, NULL, 0, "https://{key}.example/x"));
    TEST_ASSERT_FALSE(add("a", "x", "A", 10, NULL, 0, "https://h.example@evil/{key}"));
    TEST_ASSERT_FALSE(add("a", "x", "A", 10, NULL, 0, "https://h.example"));
    TEST_ASSERT_FALSE(add("a", "x", "A", 10, NULL, 0, "https://h.example/{foo}{key}"));
    TEST_ASSERT_FALSE(add("a", "x", "A", 10, NULL, 0, "https://h.example/{key}}"));
    TEST_ASSERT_FALSE(add("a", "x", "A", 10, NULL, 0, "https://h.example/ {key}"));

    char long_url[GPHOTOS_URL_TEMPLATE_SIZE + 8];
    memset(long_url, 'a', sizeof(long_url));
    memcpy(long_url, "https://h.example/{key}", 23);
    long_url[GPHOTOS_URL_TEMPLATE_SIZE] = '\0';
    TEST_ASSERT_FALSE(add("a", "x", "A", 10, NULL, 0, long_url));
    long_url[GPHOTOS_URL_TEMPLATE_SIZE - 1] = '\0';
    TEST_ASSERT_TRUE(add("a", "x", "A", 10, NULL, 0, long_url));
}

static void test_the_whole_set_is_checked(void)
{
    TEST_ASSERT_FALSE(gphotos_rules_check(&s_rules, &s_why));
    TEST_ASSERT_TRUE(gphotos_rules_set_ua(&s_rules, "UA/1", &s_why));
    TEST_ASSERT_FALSE(gphotos_rules_check(&s_rules, &s_why)); // no pattern
    TEST_ASSERT_TRUE(add("a", "x", "A", 10, NULL, 0, URL));
    TEST_ASSERT_TRUE(gphotos_rules_check(&s_rules, &s_why));
    TEST_ASSERT_TRUE(add("a", "y", "A", 10, NULL, 0, URL));
    TEST_ASSERT_FALSE(gphotos_rules_check(&s_rules, &s_why)); // two called "a"

    memset(&s_rules, 0, sizeof(s_rules));
    TEST_ASSERT_TRUE(add("a", "x", "A", 10, NULL, 0, URL));
    TEST_ASSERT_TRUE(add("b", "x", "A", 10, NULL, 0, URL));
    TEST_ASSERT_TRUE(add("c", "x", "A", 10, NULL, 0, URL));
    TEST_ASSERT_TRUE(add("d", "x", "A", 10, NULL, 0, URL));
    TEST_ASSERT_FALSE(add("e", "x", "A", 10, NULL, 0, URL));

    TEST_ASSERT_FALSE(gphotos_rules_set_ua(&s_rules, "", &s_why));
    TEST_ASSERT_FALSE(gphotos_rules_set_ua(&s_rules, "UA\r\nX-Evil: 1", &s_why));
    char ua[GPHOTOS_UA_SIZE + 1];
    memset(ua, 'u', sizeof(ua));
    ua[GPHOTOS_UA_SIZE] = '\0';
    TEST_ASSERT_FALSE(gphotos_rules_set_ua(&s_rules, ua, &s_why));
    ua[GPHOTOS_UA_SIZE - 1] = '\0';
    TEST_ASSERT_TRUE(gphotos_rules_set_ua(&s_rules, ua, &s_why));
}

// The hole FR-10.1 records: an album whose tiles come from fife, which the built-in anchor never
// matches. Two patterns in one stream, every split, each item attributed to its own pattern.
static void test_a_second_pattern_every_split(void)
{
    gphotos_rules_default(&s_rules);
    TEST_ASSERT_TRUE(add("fife", "fife.usercontent.google.com/pw/", "A-Za-z0-9_-", 255, AFTER,
                         N_AFTER, "https://photos.fife.usercontent.google.com/pw/{key}=w{edge}"));
    char t[512];
    const int len = snprintf(t, sizeof(t),
                             "[\"https://photos.fife.usercontent.google.com/pw/FIFE_1\",640,480],"
                             "[\"https://lh3.googleusercontent.com/pw/LH3-2\",768,1344]"
                             "\"https://photos.fife.usercontent.google.com/pw/NOPE=w96-h72\"");
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof(t));
    for (size_t split = 0; split <= (size_t)len; split++) {
        TEST_ASSERT_EQUAL_size_t_MESSAGE(2, parse_split(t, (size_t)len, split), "split");
        TEST_ASSERT_EQUAL_STRING("FIFE_1", s_got.key[0]);
        TEST_ASSERT_EQUAL_size_t(1, s_got.pattern[0]);
        TEST_ASSERT_EQUAL_UINT32(640, s_got.w[0]);
        TEST_ASSERT_EQUAL_UINT32(480, s_got.h[0]);
        TEST_ASSERT_EQUAL_STRING("LH3-2", s_got.key[1]);
        TEST_ASSERT_EQUAL_size_t(0, s_got.pattern[1]);
    }
}

// (b) changes: a different sequence after the key, including none at all and a literal last.
static void test_other_sequences(void)
{
    TEST_ASSERT_TRUE(gphotos_rules_set_ua(&s_rules, "UA/1", &s_why));
    static const char *const LIT_LAST[] = {"lit:\"]"};
    static const char *const NUM_FIRST[] = {"lit:|", "num"};
    TEST_ASSERT_TRUE(add("bare", "b/", "A-Z", 10, NULL, 0, URL));
    TEST_ASSERT_TRUE(add("lit", "l/", "A-Z", 10, LIT_LAST, 1, URL));
    TEST_ASSERT_TRUE(add("num", "n/", "A-Z", 10, NUM_FIRST, 2, URL));

    const char *t = "b/ABC l/DEF\"] l/XYZ\"x n/GHI|12 n/Q|1 b/END";
    TEST_ASSERT_EQUAL_size_t(4, parse_split(t, strlen(t), 7));
    TEST_ASSERT_EQUAL_STRING("ABC", s_got.key[0]);
    TEST_ASSERT_EQUAL_STRING("DEF", s_got.key[1]);
    TEST_ASSERT_EQUAL_STRING("GHI", s_got.key[2]);
    TEST_ASSERT_EQUAL_UINT32(12, s_got.w[2]);
    TEST_ASSERT_EQUAL_STRING("END", s_got.key[3]); // only through finish
    TEST_ASSERT_EQUAL_size_t(0, s_got.pattern[3]);
}

static void test_url_from_a_template(void)
{
    TEST_ASSERT_TRUE(add("a", "x", "A-Za-z0-9_-", 255, NULL, 0,
                         "https://h.example/pw/{key}=w{edge}-h{edge}-no"));
    char out[64];
    TEST_ASSERT_TRUE(gphotos_rules_url(&s_rules.patterns[0], "K_1", 3, 600, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://h.example/pw/K_1=w600-h600-no", out);
    TEST_ASSERT_FALSE(gphotos_rules_url(&s_rules.patterns[0], "K?x", 3, 600, out, sizeof(out)));
    TEST_ASSERT_FALSE(gphotos_rules_url(&s_rules.patterns[0], "K\n", 2, 600, out, sizeof(out)));
}

static void test_dir_lines(void)
{
    char id[GPHOTOS_ID_SIZE];
    size_t off = 99, n = 99;
    TEST_ASSERT_TRUE(gphotos_dir_line("AbC_-9", 6, 1, id, &off, &n));
    TEST_ASSERT_EQUAL_STRING(GPHOTOS_RULES_DEFAULT_ID, id);
    TEST_ASSERT_EQUAL_size_t(0, off);
    TEST_ASSERT_EQUAL_size_t(6, n);

    TEST_ASSERT_TRUE(gphotos_dir_line("fife AbC", 8, 2, id, &off, &n));
    TEST_ASSERT_EQUAL_STRING("fife", id);
    TEST_ASSERT_EQUAL_size_t(5, off);
    TEST_ASSERT_EQUAL_size_t(3, n);

    TEST_ASSERT_FALSE(gphotos_dir_line("AbC", 3, 2, id, &off, &n));        // no id
    TEST_ASSERT_FALSE(gphotos_dir_line("Fife AbC", 8, 2, id, &off, &n));   // id case
    TEST_ASSERT_FALSE(gphotos_dir_line("fife ", 5, 2, id, &off, &n));      // no key
    TEST_ASSERT_FALSE(gphotos_dir_line(" AbC", 4, 2, id, &off, &n));       // empty id
    TEST_ASSERT_FALSE(gphotos_dir_line("abcdefghi K", 11, 2, id, &off, &n));
    TEST_ASSERT_FALSE(gphotos_dir_line("a K K", 5, 2, id, &off, &n));      // space in key
    TEST_ASSERT_FALSE(gphotos_dir_line("K\"", 2, 1, id, &off, &n));
    TEST_ASSERT_FALSE(gphotos_dir_line("K", 1, 3, id, &off, &n));

    char big[GPHOTOS_KEY_MAX + 8];
    memset(big, 'K', sizeof(big));
    TEST_ASSERT_FALSE(gphotos_dir_line(big, GPHOTOS_KEY_MAX, 1, id, &off, &n));
    TEST_ASSERT_TRUE(gphotos_dir_line(big, GPHOTOS_KEY_MAX - 1, 1, id, &off, &n));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_is_valid);
    RUN_TEST(test_a_valid_pattern_is_accepted);
    RUN_TEST(test_ids_are_refused);
    RUN_TEST(test_anchors_are_refused);
    RUN_TEST(test_key_chars_beyond_the_safe_set_are_refused);
    RUN_TEST(test_tokens_are_refused);
    RUN_TEST(test_urls_are_refused);
    RUN_TEST(test_the_whole_set_is_checked);
    RUN_TEST(test_a_second_pattern_every_split);
    RUN_TEST(test_other_sequences);
    RUN_TEST(test_url_from_a_template);
    RUN_TEST(test_dir_lines);
    return UNITY_END();
}
