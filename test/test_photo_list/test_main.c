// Host-side tests for FR-4.6's directory rules.
//
// Run: ~/.platformio/penv/Scripts/pio.exe test -e native
//
// Two of the assertions here look wrong at a glance and are deliberate: the sort is
// byte ascending, not natural, and the 500 cap takes files in insertion order rather
// than the alphabetically first 500. Both match the shipping firmware. If one is ever
// changed, it should be because someone decided to, not because a test was loose.

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "photo_list.h"

static char s_arena[32768];
static photo_list_t s_list;

void setUp(void)
{
    photo_list_init(&s_list, s_arena, sizeof(s_arena));
}

void tearDown(void) {}

// ------------------------------------------------------------------ extensions

static void test_accepts_the_four_extensions(void)
{
    TEST_ASSERT_TRUE(photo_list_is_image("a.jpg"));
    TEST_ASSERT_TRUE(photo_list_is_image("a.jpeg"));
    TEST_ASSERT_TRUE(photo_list_is_image("a.png"));
    TEST_ASSERT_TRUE(photo_list_is_image("a.bmp"));
}

static void test_extensions_are_case_insensitive(void)
{
    TEST_ASSERT_TRUE(photo_list_is_image("A.JPG"));
    TEST_ASSERT_TRUE(photo_list_is_image("b.JpEg"));
    TEST_ASSERT_TRUE(photo_list_is_image("C.PNG"));
    TEST_ASSERT_TRUE(photo_list_is_image("d.BmP"));
}

static void test_rejects_non_images(void)
{
    TEST_ASSERT_FALSE(photo_list_is_image("x.gif"));
    TEST_ASSERT_FALSE(photo_list_is_image("x.jpgx"));   // extension must be the tail
    TEST_ASSERT_FALSE(photo_list_is_image("x.jpg.txt"));
    TEST_ASSERT_FALSE(photo_list_is_image("jpg"));      // no dot at all
    TEST_ASSERT_FALSE(photo_list_is_image(""));
    TEST_ASSERT_FALSE(photo_list_is_image("."));
    TEST_ASSERT_FALSE(photo_list_is_image(".."));
    TEST_ASSERT_FALSE(photo_list_is_image(NULL));
}

static void test_bare_extension_is_a_dotfile_not_a_photo(void)
{
    // ".jpg" has no stem. FAT treats it as a hidden file; the reference finds the dot
    // with rfind and would take an empty name. Rejecting it is the decision.
    TEST_ASSERT_FALSE(photo_list_is_image(".jpg"));
    TEST_ASSERT_FALSE(photo_list_is_image(".png"));
}

// ------------------------------------------------------------------------ add

static void test_add_filters_and_counts(void)
{
    TEST_ASSERT_TRUE(photo_list_add(&s_list, "one.jpg"));
    TEST_ASSERT_FALSE(photo_list_add(&s_list, "notes.txt"));
    TEST_ASSERT_TRUE(photo_list_add(&s_list, "two.PNG"));
    TEST_ASSERT_EQUAL_UINT32(2, photo_list_count(&s_list));
    TEST_ASSERT_EQUAL_STRING("one.jpg", photo_list_at(&s_list, 0));
    TEST_ASSERT_EQUAL_STRING("two.PNG", photo_list_at(&s_list, 1));
    TEST_ASSERT_NULL(photo_list_at(&s_list, 2));
}

static void test_empty_directory_is_a_normal_state(void)
{
    // FR-5.7: no images is not an error.
    TEST_ASSERT_EQUAL_UINT32(0, photo_list_count(&s_list));
    TEST_ASSERT_NULL(photo_list_at(&s_list, 0));
    photo_list_sort(&s_list); // must not crash on an empty list
    TEST_ASSERT_EQUAL_UINT32(0, photo_list_count(&s_list));
}

// ----------------------------------------------------------------------- sort

static void test_sort_is_byte_ascending_not_natural(void)
{
    photo_list_add(&s_list, "9.jpg");
    photo_list_add(&s_list, "10.jpg");
    photo_list_add(&s_list, "2.jpg");
    photo_list_sort(&s_list);

    // Natural sort would give 2, 9, 10. Byte sort gives 10, 2, 9, and byte sort is
    // what the reference does.
    TEST_ASSERT_EQUAL_STRING("10.jpg", photo_list_at(&s_list, 0));
    TEST_ASSERT_EQUAL_STRING("2.jpg", photo_list_at(&s_list, 1));
    TEST_ASSERT_EQUAL_STRING("9.jpg", photo_list_at(&s_list, 2));
}

static void test_sort_puts_uppercase_before_lowercase(void)
{
    photo_list_add(&s_list, "a.jpg");
    photo_list_add(&s_list, "Z.jpg");
    photo_list_sort(&s_list);

    // 'Z' is 0x5A, 'a' is 0x61. The match is case-insensitive; the sort is not.
    TEST_ASSERT_EQUAL_STRING("Z.jpg", photo_list_at(&s_list, 0));
    TEST_ASSERT_EQUAL_STRING("a.jpg", photo_list_at(&s_list, 1));
}

// ------------------------------------------------------------------ the cap

static void test_cap_is_500_and_applies_before_sorting(void)
{
    char name[32];
    // Insert descending, so that if the cap were applied after sorting the survivors
    // would be the alphabetically-first ones instead of the first-seen ones.
    for (int i = 600; i > 0; i--) {
        snprintf(name, sizeof(name), "%03d.jpg", i);
        photo_list_add(&s_list, name);
    }
    TEST_ASSERT_EQUAL_UINT32(PHOTO_LIST_MAX, photo_list_count(&s_list));
    TEST_ASSERT_TRUE(s_list.truncated);

    photo_list_sort(&s_list);
    // Kept 600 down to 101; 001-100 were never seen. So the first entry is 101, not
    // 001 -- the cap took an arbitrary 500 in directory order.
    TEST_ASSERT_EQUAL_STRING("101.jpg", photo_list_at(&s_list, 0));
    TEST_ASSERT_EQUAL_STRING("600.jpg", photo_list_at(&s_list, PHOTO_LIST_MAX - 1));
}

static void test_arena_exhaustion_is_reported_not_fatal(void)
{
    char small[16];
    photo_list_t l;
    TEST_ASSERT_TRUE(photo_list_init(&l, small, sizeof(small)));

    TEST_ASSERT_TRUE(photo_list_add(&l, "aaaa.jpg"));  // 9 bytes
    TEST_ASSERT_FALSE(photo_list_add(&l, "bbbb.jpg")); // would need 9 more of 7 left
    TEST_ASSERT_TRUE(l.arena_full);
    TEST_ASSERT_EQUAL_UINT32(1, photo_list_count(&l));
    TEST_ASSERT_EQUAL_STRING("aaaa.jpg", photo_list_at(&l, 0));
}

static void test_reset_reuses_the_arena(void)
{
    photo_list_add(&s_list, "one.jpg");
    photo_list_reset(&s_list);
    TEST_ASSERT_EQUAL_UINT32(0, photo_list_count(&s_list));
    TEST_ASSERT_FALSE(s_list.truncated);
    TEST_ASSERT_FALSE(s_list.arena_full);
    TEST_ASSERT_TRUE(photo_list_add(&s_list, "two.jpg"));
    TEST_ASSERT_EQUAL_STRING("two.jpg", photo_list_at(&s_list, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_accepts_the_four_extensions);
    RUN_TEST(test_extensions_are_case_insensitive);
    RUN_TEST(test_rejects_non_images);
    RUN_TEST(test_bare_extension_is_a_dotfile_not_a_photo);
    RUN_TEST(test_add_filters_and_counts);
    RUN_TEST(test_empty_directory_is_a_normal_state);
    RUN_TEST(test_sort_is_byte_ascending_not_natural);
    RUN_TEST(test_sort_puts_uppercase_before_lowercase);
    RUN_TEST(test_cap_is_500_and_applies_before_sorting);
    RUN_TEST(test_arena_exhaustion_is_reported_not_fatal);
    RUN_TEST(test_reset_reuses_the_arena);
    return UNITY_END();
}
