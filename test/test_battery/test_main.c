// Host-side tests for the battery curve.
//
// This is a PORT of `batteryPercentFromMV()` in `assets/index.html`, so the acceptance test is
// "identical", not "reasonable" -- the same rule the epdoptimize port follows. These cases are the
// ones a human can check by reading the JavaScript; `tools/battery_parity.py` is what compares the
// two at every millivolt, and it is the real check.

#include <stdio.h>

#include <unity.h>

#include "battery.h"

void setUp(void) {}
void tearDown(void) {}

static void test_the_ends_saturate(void)
{
    TEST_ASSERT_EQUAL_INT(100, battery_percent_from_mv(4200));
    TEST_ASSERT_EQUAL_INT(100, battery_percent_from_mv(4500));
    TEST_ASSERT_EQUAL_INT(100, battery_percent_from_mv(65535));
    TEST_ASSERT_EQUAL_INT(0, battery_percent_from_mv(3200));
    TEST_ASSERT_EQUAL_INT(0, battery_percent_from_mv(3000));
    // A board that cannot read its cell reports 0 mV. It reads as 0 % here, which is why the caller
    // has to check the millivolts to tell "empty" from "unknown" -- app_display.c does.
    TEST_ASSERT_EQUAL_INT(0, battery_percent_from_mv(0));
}

// Each segment's lower boundary is its base exactly, which is what makes the curve continuous.
static void test_the_segment_boundaries_are_the_bases(void)
{
    TEST_ASSERT_EQUAL_INT(85, battery_percent_from_mv(4000));
    TEST_ASSERT_EQUAL_INT(50, battery_percent_from_mv(3800));
    TEST_ASSERT_EQUAL_INT(15, battery_percent_from_mv(3600));
    TEST_ASSERT_EQUAL_INT(5, battery_percent_from_mv(3400));
}

// **The segment midpoints, and two of them are the page's floating-point error rather than its
// arithmetic.** Every one lands on a half, so each is decided by rounding -- and the page computes in
// doubles, where `3.3 - 3.2` is 0.09999999999999964 and `4.1 - 4.0` is 0.10000000000000053. So 3300
// and 4100 come out 2 and 92 where exact arithmetic gives 3 and 93, while 3900, 3700 and 3500 happen
// to land on the other side of their halves.
//
// These numbers were read out of `assets/index.html` with node, not derived. An integer
// implementation was written first and got two of them wrong, which `tools/battery_parity.py` caught
// on its first run. **Do not "correct" them**: the acceptance test for a port is identical, the same
// rule `src/core/epd_epdopt.c` states about four of its own oddities.
static void test_the_midpoints_are_the_pages_own_values(void)
{
    TEST_ASSERT_EQUAL_INT(92, battery_percent_from_mv(4100));
    TEST_ASSERT_EQUAL_INT(68, battery_percent_from_mv(3900));
    TEST_ASSERT_EQUAL_INT(33, battery_percent_from_mv(3700));
    TEST_ASSERT_EQUAL_INT(10, battery_percent_from_mv(3500));
    TEST_ASSERT_EQUAL_INT(2, battery_percent_from_mv(3300));
    TEST_ASSERT_EQUAL_INT(24, battery_percent_from_mv(3650)); // a non-boundary, for good measure
}

// The curve exists because a lithium cell is not linear: half its charge sits between 3.8 and 4.2 V.
// A linear 3.2-4.2 V ramp would call 3.7 V "50 %" where this calls it 33 %.
static void test_the_curve_is_not_linear(void)
{
    const int mid = battery_percent_from_mv(3700);
    TEST_ASSERT_LESS_THAN_INT(50, mid);
    TEST_ASSERT_GREATER_THAN_INT(15, mid);
}

// Monotonic and in range at every millivolt across the whole span, which is the property the icon
// depends on: a fill that went backwards as the cell drained would be worse than no icon.
static void test_it_is_monotonic_and_bounded(void)
{
    int prev = -1;
    for (uint32_t mv = 0; mv <= 5000; mv++) {
        const int pct = battery_percent_from_mv((uint16_t)mv);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pct);
        TEST_ASSERT_LESS_OR_EQUAL_INT(100, pct);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(prev, pct);
        prev = pct;
    }
    TEST_ASSERT_EQUAL_INT(100, prev);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_ends_saturate);
    RUN_TEST(test_the_segment_boundaries_are_the_bases);
    RUN_TEST(test_the_midpoints_are_the_pages_own_values);
    RUN_TEST(test_the_curve_is_not_linear);
    RUN_TEST(test_it_is_monotonic_and_bounded);
    return UNITY_END();
}
