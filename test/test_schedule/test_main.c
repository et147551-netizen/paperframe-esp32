// app_schedule_active(): ticket 42's active-hours predicate.
//
// Worth its own suite for one reason: the WRAPPING window (22:00-06:00) is where the obvious
// implementation is wrong, and it is wrong SILENTLY -- an `&&` where the wrap needs an `||`
// yields the empty set, so the frame holds its picture for ever and looks broken rather than
// scheduled. Every case below is written so that swapping that operator, or making the
// interval closed at both ends, turns a test red rather than leaving it green.
//
// The second thing under test is the fail-open ladder. A frame with no clock must refresh, and
// that case is the easy one to omit because it needs no arithmetic at all.

#include <stdbool.h>

#include "unity.h"

#include "app_schedule.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------- the switch is off

static void test_off_always_refreshes(void)
{
    // Every hour, including ones the window would exclude, and a nonsense window besides.
    for (int h = 0; h < 24; h++) {
        TEST_ASSERT_TRUE(app_schedule_active(false, h, 7, 23));
        TEST_ASSERT_TRUE(app_schedule_active(false, h, 23, 7));
    }
    TEST_ASSERT_TRUE(app_schedule_active(false, APP_SCHEDULE_NO_CLOCK, 7, 23));
}

// ------------------------------------------------------- the non-wrapping window, 07..23

static void test_default_window_inside(void)
{
    // The shipping default, taken from the Android frame's priority-2 layer.
    for (int h = 7; h <= 22; h++) {
        TEST_ASSERT_TRUE_MESSAGE(app_schedule_active(true, h, 7, 23), "inside 07..23");
    }
}

static void test_default_window_outside(void)
{
    for (int h = 0; h <= 6; h++) {
        TEST_ASSERT_FALSE_MESSAGE(app_schedule_active(true, h, 7, 23), "before 07:00");
    }
    TEST_ASSERT_FALSE_MESSAGE(app_schedule_active(true, 23, 7, 23), "23:00 is outside");
}

static void test_default_window_boundaries(void)
{
    // Closed at the start, open at the end. Both halves stated as their own assertion,
    // because an off-by-one at either end passes the loops above.
    TEST_ASSERT_FALSE(app_schedule_active(true, 6, 7, 23));   // one hour before
    TEST_ASSERT_TRUE(app_schedule_active(true, 7, 7, 23));    // the start hour itself
    TEST_ASSERT_TRUE(app_schedule_active(true, 22, 7, 23));   // the last active hour
    TEST_ASSERT_FALSE(app_schedule_active(true, 23, 7, 23));  // the end hour itself
}

// ----------------------------------------------------------- the wrapping window, 22..06

static void test_wrapping_window_inside(void)
{
    // The two disjoint halves: the evening side and the small hours. An `&&` implementation
    // returns false for every one of these.
    TEST_ASSERT_TRUE_MESSAGE(app_schedule_active(true, 22, 22, 6), "22:00, evening side");
    TEST_ASSERT_TRUE_MESSAGE(app_schedule_active(true, 23, 22, 6), "23:00, evening side");
    TEST_ASSERT_TRUE_MESSAGE(app_schedule_active(true, 0, 22, 6), "midnight, morning side");
    TEST_ASSERT_TRUE_MESSAGE(app_schedule_active(true, 5, 22, 6), "05:00, morning side");
}

static void test_wrapping_window_outside(void)
{
    // The daytime hours a 22..06 window excludes. This is the half that stays correct under
    // the `&&` bug, which is why the test above is the discriminating one and this is the
    // guard against a predicate that simply returns true.
    for (int h = 6; h <= 21; h++) {
        TEST_ASSERT_FALSE_MESSAGE(app_schedule_active(true, h, 22, 6), "daytime, 22..06");
    }
}

static void test_wrapping_window_boundaries(void)
{
    TEST_ASSERT_FALSE(app_schedule_active(true, 21, 22, 6));  // one hour before the start
    TEST_ASSERT_TRUE(app_schedule_active(true, 22, 22, 6));   // the start hour itself
    TEST_ASSERT_TRUE(app_schedule_active(true, 5, 22, 6));    // the last active hour
    TEST_ASSERT_FALSE(app_schedule_active(true, 6, 22, 6));   // the end hour itself
}

// A window that wraps by one hour at each edge, so that a rule keyed on "start > 12" or any
// other guess about which side is which comes out wrong.
static void test_narrow_and_wide_wraps(void)
{
    // 23..00 -- one active hour only.
    TEST_ASSERT_TRUE(app_schedule_active(true, 23, 23, 0));
    for (int h = 0; h <= 22; h++) {
        TEST_ASSERT_FALSE_MESSAGE(app_schedule_active(true, h, 23, 0), "only 23:00 is active");
    }
    // 1..0 -- twenty-three active hours, inactive at midnight alone.
    TEST_ASSERT_FALSE(app_schedule_active(true, 0, 1, 0));
    for (int h = 1; h <= 23; h++) {
        TEST_ASSERT_TRUE_MESSAGE(app_schedule_active(true, h, 1, 0), "all but midnight");
    }
}

// ------------------------------------------------------------------- the fail-open ladder

static void test_no_clock_refreshes(void)
{
    // The case that matters: an unsynced boot must behave exactly as a build with no schedule
    // in it. Checked against both window shapes so it cannot pass by falling into a window.
    TEST_ASSERT_TRUE(app_schedule_active(true, APP_SCHEDULE_NO_CLOCK, 7, 23));
    TEST_ASSERT_TRUE(app_schedule_active(true, APP_SCHEDULE_NO_CLOCK, 22, 6));
}

static void test_impossible_hour_refreshes(void)
{
    // Not reachable from app_clock_local_hour(), but a clock that decoded into nonsense must
    // not be able to freeze the panel either.
    TEST_ASSERT_TRUE(app_schedule_active(true, 24, 7, 23));
    TEST_ASSERT_TRUE(app_schedule_active(true, -2, 7, 23));
    TEST_ASSERT_TRUE(app_schedule_active(true, 99, 22, 6));
}

static void test_impossible_window_refreshes(void)
{
    // A half-filled settings form, or an NVS entry from a future build.
    TEST_ASSERT_TRUE(app_schedule_active(true, 12, -1, 23));
    TEST_ASSERT_TRUE(app_schedule_active(true, 12, 7, 24));
    TEST_ASSERT_TRUE(app_schedule_active(true, 3, 25, 30));
}

static void test_zero_width_window_refreshes(void)
{
    // start == end could mean "always" or "never"; it is read as always. Asserted at an hour
    // on each side of the pair so an implementation that special-cased one of them fails.
    for (int h = 0; h < 24; h++) {
        TEST_ASSERT_TRUE_MESSAGE(app_schedule_active(true, h, 9, 9), "9..9 means always");
        TEST_ASSERT_TRUE_MESSAGE(app_schedule_active(true, h, 0, 0), "0..0 means always");
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_off_always_refreshes);
    RUN_TEST(test_default_window_inside);
    RUN_TEST(test_default_window_outside);
    RUN_TEST(test_default_window_boundaries);
    RUN_TEST(test_wrapping_window_inside);
    RUN_TEST(test_wrapping_window_outside);
    RUN_TEST(test_wrapping_window_boundaries);
    RUN_TEST(test_narrow_and_wide_wraps);
    RUN_TEST(test_no_clock_refreshes);
    RUN_TEST(test_impossible_hour_refreshes);
    RUN_TEST(test_impossible_window_refreshes);
    RUN_TEST(test_zero_width_window_refreshes);
    return UNITY_END();
}
