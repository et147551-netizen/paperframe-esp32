// smb_evict_may_drop(): the eviction walk's protected set.
//
// Worth its own suite because none of what it protects is observable except on hardware, with the
// cache at its ceiling first, at 15 to 31 s a look. smb_evict.h lists what each mistake costs; in
// short, dropping the current photograph blanks the frame, dropping the pending one makes an
// unbounded fetch-and-evict loop, and dropping a wanted one strands the want so the slideshow shows
// the matte.
//
// The cases are written against the ways a plausible implementation goes wrong:
//
//   * an EMPTY current_name or pending_name protecting everything, which is what a bare
//     strcmp(local, "") == 0 would do on an entry that is also empty, and what dropping the
//     original's `ss.current_name[0] &&` guard would risk;
//   * a PREFIX or case-insensitive comparison, which would over-protect and slowly fill the cache
//     until the mirror reports `capped` for ever;
//   * the protected array being consulted past `protected_count`, which reads uninitialised stack in
//     the caller -- app_smb_sync.c declares it as SMB_SYNC_WANT_MAX rows and fills only some.

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "unity.h"

#include "smb_evict.h"

#define PROTECTED_ROWS 4

static char s_protected[PROTECTED_ROWS][SMB_MANIFEST_NAME_SIZE];

void setUp(void)
{
    // 0xAA rather than zero, so a case that reads past protected_count sees garbage instead of the
    // empty strings that would quietly behave themselves.
    memset(s_protected, 0xAA, sizeof(s_protected));
}

void tearDown(void) {}

static void put(size_t row, const char *name)
{
    memset(s_protected[row], 0, SMB_MANIFEST_NAME_SIZE);
    strncpy(s_protected[row], name, SMB_MANIFEST_NAME_SIZE - 1);
}

static void test_an_ordinary_entry_may_be_dropped(void)
{
    TEST_ASSERT_TRUE(smb_evict_may_drop("a.jpg", "shown.jpg", "queued.jpg", NULL, 0));
}

static void test_the_photograph_on_the_glass_is_kept(void)
{
    TEST_ASSERT_FALSE_MESSAGE(smb_evict_may_drop("shown.jpg", "shown.jpg", "queued.jpg", NULL, 0),
                              "deleting what is displayed blanks the frame on the next render");
}

static void test_the_queued_photograph_is_kept(void)
{
    TEST_ASSERT_FALSE_MESSAGE(smb_evict_may_drop("queued.jpg", "shown.jpg", "queued.jpg", NULL, 0),
                              "dropping the pending fetch loops: it is asked for again at once");
}

static void test_a_wanted_photograph_is_kept(void)
{
    put(0, "wanted0.jpg");
    put(1, "wanted1.jpg");
    TEST_ASSERT_FALSE(smb_evict_may_drop("wanted0.jpg", "shown.jpg", "queued.jpg", s_protected, 2));
    TEST_ASSERT_FALSE(smb_evict_may_drop("wanted1.jpg", "shown.jpg", "queued.jpg", s_protected, 2));
    TEST_ASSERT_TRUE_MESSAGE(smb_evict_may_drop("other.jpg", "shown.jpg", "queued.jpg",
                                                s_protected, 2),
                             "only the named wants are protected");
}

static void test_the_array_is_not_read_past_the_count(void)
{
    // setUp filled every row with 0xAA. A loop bound that used SMB_SYNC_WANT_MAX instead of
    // protected_count would compare against that garbage -- and in the caller it would be whatever
    // the previous window left on the stack, which is worse than garbage because it is plausible.
    put(0, "wanted0.jpg");
    TEST_ASSERT_TRUE(smb_evict_may_drop("a.jpg", NULL, NULL, s_protected, 1));
    // Row 1 is still 0xAA... and must not be consulted at all when the count is 1.
    TEST_ASSERT_TRUE(smb_evict_may_drop("\xAA\xAA\xAA", NULL, NULL, s_protected, 1));
}

static void test_an_empty_current_or_pending_protects_nothing(void)
{
    // app_slideshow_state_t's buffers are empty when there is nothing showing or nothing queued. An
    // empty name must not match, or eviction stops entirely on a frame that has not rendered yet --
    // which is exactly the state a first boot is in when the cache first fills.
    TEST_ASSERT_TRUE(smb_evict_may_drop("a.jpg", "", "", NULL, 0));
    TEST_ASSERT_TRUE(smb_evict_may_drop("a.jpg", NULL, NULL, NULL, 0));
    put(0, "");
    TEST_ASSERT_TRUE_MESSAGE(smb_evict_may_drop("a.jpg", "", "", s_protected, 1),
                             "an empty protected row protects nothing either");
}

static void test_the_match_is_exact(void)
{
    // A prefix or case-insensitive comparison would over-protect, and the symptom is the opposite of
    // a crash: the cache never comes down, the window reports `capped`, and nothing says why.
    TEST_ASSERT_TRUE_MESSAGE(smb_evict_may_drop("a.jpg", "a.jpeg", NULL, NULL, 0),
                             "a.jpeg is not a.jpg");
    TEST_ASSERT_TRUE_MESSAGE(smb_evict_may_drop("a.jpg", "a.jp", NULL, NULL, 0),
                             "a prefix is not a match");
    TEST_ASSERT_TRUE_MESSAGE(smb_evict_may_drop("a.jpg", "a.jpgx", NULL, NULL, 0),
                             "a longer name is not a match");
    TEST_ASSERT_TRUE_MESSAGE(smb_evict_may_drop("a.jpg", "A.JPG", NULL, NULL, 0),
                             "the comparison is case-sensitive, as strcmp is");
}

static void test_a_nameless_entry_is_never_dropped(void)
{
    // The walk should never hand one over -- a manifest entry has a name -- but "delete the file
    // called nothing" is the wrong way to discover that it did.
    TEST_ASSERT_FALSE(smb_evict_may_drop(NULL, "shown.jpg", "queued.jpg", NULL, 0));
    TEST_ASSERT_FALSE(smb_evict_may_drop("", "shown.jpg", "queued.jpg", NULL, 0));
}

static void test_a_full_protected_set_still_lets_others_go(void)
{
    // The cache is bounded and the want list is not: with every row used, an unrelated entry must
    // still be droppable or the eviction can never make progress.
    for (size_t i = 0; i < PROTECTED_ROWS; i++) {
        char name[32];
        name[0] = 'w';
        name[1] = (char)('0' + (int)i);
        name[2] = '\0';
        put(i, name);
    }
    TEST_ASSERT_TRUE(smb_evict_may_drop("free.jpg", "shown.jpg", "queued.jpg", s_protected,
                                        PROTECTED_ROWS));
    TEST_ASSERT_FALSE(smb_evict_may_drop("w3", "shown.jpg", "queued.jpg", s_protected,
                                         PROTECTED_ROWS));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_an_ordinary_entry_may_be_dropped);
    RUN_TEST(test_the_photograph_on_the_glass_is_kept);
    RUN_TEST(test_the_queued_photograph_is_kept);
    RUN_TEST(test_a_wanted_photograph_is_kept);
    RUN_TEST(test_the_array_is_not_read_past_the_count);
    RUN_TEST(test_an_empty_current_or_pending_protects_nothing);
    RUN_TEST(test_the_match_is_exact);
    RUN_TEST(test_a_nameless_entry_is_never_dropped);
    RUN_TEST(test_a_full_protected_set_still_lets_others_go);
    return UNITY_END();
}
