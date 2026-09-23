// Host tests for the pure string half of request identity (ticket 79): percent-coding and the
// path-traversal guard.
//
// These functions shipped untested for the whole life of the API, not because anyone decided they
// did not need tests but because they were `static` inside a 181 KB ESP-IDF translation unit and
// `pio test -e native` links src/core/ only. That is the whole reason ticket 79 moved them.
//
// The live sweep in tools/api_path_sweep.py is the other half of the same question and neither
// replaces the other: the sweep proves the running server rejects a traversal END TO END through
// esp_http_server, and these prove the rule itself, including the cases a sweep cannot reach --
// a `%` at the very end of a buffer, an exact-fit encode, the round trip the album listing needs.
//
// Non-ASCII is written as \x escapes rather than literal UTF-8, following test_smb_manifest: this
// machine's default codepage is cp932 and tools here have silently re-encoded a source file.

#include <string.h>

#include <unity.h>

#include "req_name.h"

void setUp(void) {}
void tearDown(void) {}

// ------------------------------------------------------------------ the guard

static void test_a_plain_name_is_safe(void)
{
    TEST_ASSERT_TRUE(req_name_is_safe("imaged001.png"));
    TEST_ASSERT_TRUE(req_name_is_safe("IMG_0042.JPG"));
    TEST_ASSERT_TRUE(req_name_is_safe("a b c.jpg"));  // a space is not a path separator
    TEST_ASSERT_TRUE(req_name_is_safe("a.b.c.jpg"));  // single dots are not ..
}

static void test_nothing_with_a_separator_is_safe(void)
{
    TEST_ASSERT_FALSE(req_name_is_safe("dir/a.jpg"));
    TEST_ASSERT_FALSE(req_name_is_safe("/a.jpg"));
    TEST_ASSERT_FALSE(req_name_is_safe("a.jpg/"));
    TEST_ASSERT_FALSE(req_name_is_safe("dir\\a.jpg"));
    TEST_ASSERT_FALSE(req_name_is_safe("\\a.jpg"));
}

static void test_no_dotdot_anywhere_not_just_at_the_front(void)
{
    TEST_ASSERT_FALSE(req_name_is_safe(".."));
    TEST_ASSERT_FALSE(req_name_is_safe("../a.jpg"));
    TEST_ASSERT_FALSE(req_name_is_safe("a/../b.jpg"));
    // No separator at all, and still refused: the rule is about the SEQUENCE, so a name FAT
    // could legitimately hold is collateral. That is the right trade for a three-line guard.
    TEST_ASSERT_FALSE(req_name_is_safe("a..b.jpg"));
}

// The forms that defeat a guard which strips rather than refuses. This one refuses, so there is
// nothing for them to survive -- but the test says so out loud, because the tempting "improvement"
// to this function is to canonicalise first.
static void test_the_forms_that_beat_a_stripping_guard(void)
{
    TEST_ASSERT_FALSE(req_name_is_safe("....//a.jpg"));
    TEST_ASSERT_FALSE(req_name_is_safe(".../.../a.jpg"));
    TEST_ASSERT_FALSE(req_name_is_safe("..;/a.jpg"));
}

static void test_empty_and_null_are_not_safe(void)
{
    TEST_ASSERT_FALSE(req_name_is_safe(""));
    TEST_ASSERT_FALSE(req_name_is_safe(NULL));
}

// A single dot IS accepted, and the path becomes /data/. -- the directory. Recorded rather than
// fixed: every consumer then fails it as a file (404 on both boards, measured by the live sweep),
// and a guard for it would be a fourth condition for no gain.
static void test_a_bare_dot_is_accepted_and_that_is_known(void)
{
    TEST_ASSERT_TRUE(req_name_is_safe("."));
}

// A dotfile is accepted, which is FR-4.6's ported rule rather than an oversight: photo_list keeps
// dotfiles out of the album, and ticket 38's rejection is the MIRROR's rule. So GET /data/.smbidx
// serves the ownership record to an authenticated client, deliberately.
static void test_a_dotfile_is_accepted_deliberately(void)
{
    TEST_ASSERT_TRUE(req_name_is_safe(".smbidx"));
    TEST_ASSERT_TRUE(req_name_is_safe(".gpidx"));
}

// ----------------------------------------------------------------- decoding

static void test_decode_leaves_a_plain_name_alone(void)
{
    char out[32];
    TEST_ASSERT_TRUE(req_url_decode("imaged001.png", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("imaged001.png", out);
}

static void test_decode_handles_escapes_and_plus(void)
{
    char out[32];
    TEST_ASSERT_TRUE(req_url_decode("a%20b.jpg", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a b.jpg", out);
    TEST_ASSERT_TRUE(req_url_decode("a+b.jpg", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a b.jpg", out);
    TEST_ASSERT_TRUE(req_url_decode("%2Fa%5Cb", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/a\\b", out);
    // Case-insensitive hex, both halves.
    TEST_ASSERT_TRUE(req_url_decode("%2f%2F%aB%Ab", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("//\xAB\xAB", out);
}

static void test_a_malformed_escape_is_refused_not_passed_through(void)
{
    char out[32];
    TEST_ASSERT_FALSE(req_url_decode("%zz.jpg", out, sizeof(out)));
    TEST_ASSERT_FALSE(req_url_decode("%2.jpg", out, sizeof(out)));
    TEST_ASSERT_FALSE(req_url_decode("a%", out, sizeof(out)));
    TEST_ASSERT_FALSE(req_url_decode("a%2", out, sizeof(out)));
}

// The one place in this code that could read past a terminator: on '%' it reads p[1] and p[2].
// It cannot, and the reason is the evaluation order rather than a length check -- p[2] is read
// only when p[1] was a hex digit, so a string ending in '%' stops at p[1] == '\0'. A rewrite
// that computes both halves up front would break this with no visible symptom, so it is pinned.
static void test_a_trailing_percent_does_not_read_past_the_terminator(void)
{
    char src[4];
    char out[8];
    // The terminator is the last byte of its own buffer, so an over-read is a read out of bounds
    // that a sanitiser would catch. Behaviour: refused.
    src[0] = 'a';
    src[1] = 'b';
    src[2] = '%';
    src[3] = '\0';
    TEST_ASSERT_FALSE(req_url_decode(src, out, sizeof(out)));
}

static void test_decode_refuses_rather_than_truncating(void)
{
    char out[4];
    // Would need 5 bytes with the terminator. A truncation here is how a rejected name becomes
    // an accepted one, so the contract is false-and-nothing.
    TEST_ASSERT_FALSE(req_url_decode("abcd", out, sizeof(out)));
    // Exactly fits: three bytes and a terminator.
    TEST_ASSERT_TRUE(req_url_decode("abc", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("abc", out);
    // An escape that decodes to one byte still counts as one byte, not three.
    char out2[4];
    TEST_ASSERT_TRUE(req_url_decode("%41%42%43", out2, sizeof(out2)));
    TEST_ASSERT_EQUAL_STRING("ABC", out2);
}

static void test_decode_of_nothing(void)
{
    char out[4];
    TEST_ASSERT_TRUE(req_url_decode("", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(req_url_decode(NULL, out, sizeof(out)));
    TEST_ASSERT_FALSE(req_url_decode("a", out, 0));
}

// ------------------------------------------- the pair, which is the actual guard
//
// THE ORDER IS THE PROPERTY. request_file_name() is `req_url_decode(...) && req_name_is_safe(...)`,
// and a review that swapped them would leave every test above passing while the API accepted
// %2e%2e%2f. So this asserts both directions explicitly.

static bool decode_then_check(const char *raw, char *out, size_t size)
{
    return req_url_decode(raw, out, size) && req_name_is_safe(out);
}

static void test_an_encoded_traversal_is_caught_only_by_the_pair(void)
{
    char out[96];

    // The raw string passes the guard on its own -- there is no '/' and no ".." in it yet.
    TEST_ASSERT_TRUE(req_name_is_safe("%2e%2e%2fzzz.jpg"));
    // Decoding first is what makes it visible, and then the pair refuses it.
    TEST_ASSERT_FALSE(decode_then_check("%2e%2e%2fzzz.jpg", out, sizeof(out)));

    TEST_ASSERT_FALSE(decode_then_check("..%2fzzz.jpg", out, sizeof(out)));
    TEST_ASSERT_FALSE(decode_then_check("%2e%2e/zzz.jpg", out, sizeof(out)));
    TEST_ASSERT_FALSE(decode_then_check("..%5czzz.jpg", out, sizeof(out)));
    TEST_ASSERT_FALSE(decode_then_check("%2fdata%2fzzz.jpg", out, sizeof(out)));
    TEST_ASSERT_FALSE(decode_then_check("%2e%2e%2f%2e%2e%2fnvs", out, sizeof(out)));

    // And a real name still gets through, which is what makes the rest evidence.
    TEST_ASSERT_TRUE(decode_then_check("imaged001.png", out, sizeof(out)));
    TEST_ASSERT_TRUE(decode_then_check("a%20photograph.jpg", out, sizeof(out)));
}

// Double encoding: %252e decodes ONCE to %2e and stops. The name then contains a literal '%',
// which is not a separator, so it is accepted -- and it is accepted as the *literal* name
// "%2e%2e%2fzzz.jpg", which is a file that does not exist rather than a path that escapes.
// A second decode pass is what would make this dangerous, so the single pass is the guarantee.
static void test_double_encoding_decodes_once_and_stays_inside(void)
{
    char out[96];
    TEST_ASSERT_TRUE(decode_then_check("%252e%252e%252fzzz.jpg", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("%2e%2e%2fzzz.jpg", out);
}

// An encoded NUL truncates the C string at the decode, so everything after it is invisible to
// the guard AND to fopen alike -- the name becomes "zzz", which is inside /data. Recorded
// because it looks alarming and is not: the two see the same string.
static void test_an_encoded_nul_truncates_for_the_guard_and_the_filesystem_alike(void)
{
    char out[96];
    TEST_ASSERT_TRUE(decode_then_check("zzz%00%2e%2e%2fzzz.jpg", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("zzz", out);
}

// ----------------------------------------------------------------- encoding

static void test_encode_keeps_the_unreserved_set(void)
{
    char out[64];
    TEST_ASSERT_TRUE(req_url_encode("aZ09-_.~", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("aZ09-_.~", out);
}

static void test_encode_escapes_everything_else_upper_case(void)
{
    char out[64];
    TEST_ASSERT_TRUE(req_url_encode("a b/c\\d?e", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a%20b%2Fc%5Cd%3Fe", out);
    // High bytes -- a UTF-8 name from a share -- go out as two escapes per byte.
    TEST_ASSERT_TRUE(req_url_encode("\xE6\x97\xA5", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("%E6%97%A5", out);
}

static void test_encode_refuses_rather_than_truncating(void)
{
    char out[4];
    TEST_ASSERT_TRUE(req_url_encode("abc", out, sizeof(out)));
    TEST_ASSERT_FALSE(req_url_encode("abcd", out, sizeof(out)));

    // One escape is three bytes plus the terminator, so FOUR is exactly enough -- and this is
    // the boundary the test itself got wrong first, asserting a refusal at four and failing
    // against code that is right. `w + 3 >= size` permits a write when size >= w + 4, which is
    // the terminator counted. Left as two assertions rather than one so the off-by-one cannot
    // be reintroduced in either direction.
    char out2[4];
    TEST_ASSERT_TRUE(req_url_encode(" ", out2, sizeof(out2)));
    TEST_ASSERT_EQUAL_STRING("%20", out2);
    char out3[3];
    TEST_ASSERT_FALSE(req_url_encode(" ", out3, sizeof(out3)));
}

// THE ROUND TRIP IS WHAT THE ALBUM LISTING DEPENDS ON: h_photos_list() builds `url` with
// req_url_encode() and the page hands that straight back to GET /data/, where it is decoded. A
// name that survives one and not the other is a photograph the grid lists and cannot fetch.
static void test_encode_decode_round_trips_including_the_awkward_names(void)
{
    static const char *const names[] = {
        "imaged001.png",
        "IMG_0042.JPG",
        "a photograph.jpg",
        "a+b.jpg",                          // '+' must come back as '+', not as a space
        "100%.jpg",                         // a literal percent
        "\xE6\x97\xA5\xE6\x9C\xAC.jpg",     // UTF-8
        "smb_deadbeef.jpg",
        ".smbidx",
        "a..b.jpg",                         // the guard refuses it; the coding must not mangle it
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char enc[256];
        char dec[256];
        TEST_ASSERT_TRUE(req_url_encode(names[i], enc, sizeof(enc)));
        TEST_ASSERT_TRUE(req_url_decode(enc, dec, sizeof(dec)));
        TEST_ASSERT_EQUAL_STRING(names[i], dec);
    }
}

// The listing's own buffers, which is where the round trip can fail for a reason that is not a
// bug in either function: `char encoded[192]` against a name that is legal on the volume. Three
// bytes of escape per byte means the ceiling is 63 characters of UTF-8, and the mirror's own
// names fit by construction (SMB_MANIFEST_NAME_SIZE is 64) -- but a file put on the card by hand
// need not. What happens then is `continue`, i.e. the photograph is silently absent from the
// grid while photo_list still counted it. Recorded here as the reachable edge rather than fixed.
static void test_the_listings_encode_buffer_is_the_real_ceiling(void)
{
    char enc[192];
    char name[128];

    // 63 three-byte characters = 189 bytes of escapes + terminator: fits.
    memset(name, 0, sizeof(name));
    for (int i = 0; i < 63; i++) {
        name[i] = (char)0xE6;
    }
    TEST_ASSERT_TRUE(req_url_encode(name, enc, sizeof(enc)));
    TEST_ASSERT_EQUAL_UINT(189, (unsigned)strlen(enc));

    // 64 of them does not, and the caller's answer is to omit the photograph.
    name[63] = (char)0xE6;
    TEST_ASSERT_FALSE(req_url_encode(name, enc, sizeof(enc)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_plain_name_is_safe);
    RUN_TEST(test_nothing_with_a_separator_is_safe);
    RUN_TEST(test_no_dotdot_anywhere_not_just_at_the_front);
    RUN_TEST(test_the_forms_that_beat_a_stripping_guard);
    RUN_TEST(test_empty_and_null_are_not_safe);
    RUN_TEST(test_a_bare_dot_is_accepted_and_that_is_known);
    RUN_TEST(test_a_dotfile_is_accepted_deliberately);

    RUN_TEST(test_decode_leaves_a_plain_name_alone);
    RUN_TEST(test_decode_handles_escapes_and_plus);
    RUN_TEST(test_a_malformed_escape_is_refused_not_passed_through);
    RUN_TEST(test_a_trailing_percent_does_not_read_past_the_terminator);
    RUN_TEST(test_decode_refuses_rather_than_truncating);
    RUN_TEST(test_decode_of_nothing);

    RUN_TEST(test_an_encoded_traversal_is_caught_only_by_the_pair);
    RUN_TEST(test_double_encoding_decodes_once_and_stays_inside);
    RUN_TEST(test_an_encoded_nul_truncates_for_the_guard_and_the_filesystem_alike);

    RUN_TEST(test_encode_keeps_the_unreserved_set);
    RUN_TEST(test_encode_escapes_everything_else_upper_case);
    RUN_TEST(test_encode_refuses_rather_than_truncating);
    RUN_TEST(test_encode_decode_round_trips_including_the_awkward_names);
    RUN_TEST(test_the_listings_encode_buffer_is_the_real_ceiling);
    return UNITY_END();
}
