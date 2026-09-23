// The maintenance course's colour order (ticket 68).
//
// Every assertion below is written as an INVARIANT rather than as the table typed a second time. A
// test that re-lists "black, white, red, ..." passes whenever the two lists agree and says nothing
// about whether either is right -- and the properties are what the ticket actually argues for:
//
//   * a white render between every pair, because a flat inspected straight after another flat
//     carries the measured ghost of docs/measurements.md:123-133 and a person looking at it
//     would read the residue as a panel fault;
//   * every step a colour this panel can actually show, which means index 4 (orange) must never
//     appear -- it is invalid on both boards and has been guessed wrong twice;
//   * the last step white, which is the vendor's storage precaution and is also what lets the
//     course end without an eleventh refresh.
//
// Board-independent on purpose, so it runs under both -e native (400x600) and -e native_e1002
// (800x480): the order is a property of the Spectra 6 pigments, not of a panel's geometry.

#include <stdbool.h>
#include <string.h>

#include "unity.h"

#include "epd_cmds.h"
#include "epd_format.h"
#include "epd_maint_course.h"

void setUp(void) {}
void tearDown(void) {}

static void test_ten_steps(void)
{
    // Ten is not arbitrary: five colours to inspect, each preceded by a white clear.
    TEST_ASSERT_EQUAL_INT(10, EPD_MAINT_COURSE_STEPS);
}

static void test_every_step_is_renderable(void)
{
    for (int i = 0; i < EPD_MAINT_COURSE_STEPS; i++) {
        TEST_ASSERT_TRUE_MESSAGE(epd_color_valid(epd_maint_course_colour(i)),
                                 "a step the panel cannot render");
    }
}

static void test_orange_never_appears(void)
{
    // Called out separately from the validity check above, because this is the specific mistake
    // the repository has made twice: index 4 is orange and is on neither of this project's panels.
    for (int i = 0; i < EPD_MAINT_COURSE_STEPS; i++) {
        TEST_ASSERT_NOT_EQUAL_HEX8(EPD_COLOR_ORANGE_UNUSED, epd_maint_course_colour(i));
    }
}

static void test_white_between_every_pair(void)
{
    // The odd steps are the clears. Asserting the positions rather than counting whites is what
    // makes "K W R W ..." distinguishable from "K R W W ...", which has the same tally.
    for (int i = 1; i < EPD_MAINT_COURSE_STEPS; i += 2) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(EPD_COLOR_WHITE, epd_maint_course_colour(i),
                                        "odd steps are the white clears");
    }
}

static void test_ends_on_white(void)
{
    TEST_ASSERT_EQUAL_UINT8(EPD_COLOR_WHITE, epd_maint_course_colour(EPD_MAINT_COURSE_STEPS - 1));
}

static void test_each_colour_inspected_once(void)
{
    // The five non-white colours appear exactly once each, at the even steps. A course that showed
    // one of them twice and another not at all would still pass every test above.
    int seen[16] = {0};
    for (int i = 0; i < EPD_MAINT_COURSE_STEPS; i += 2) {
        const uint8_t c = epd_maint_course_colour(i);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(EPD_COLOR_WHITE, c, "even steps are the colours");
        seen[c]++;
    }
    TEST_ASSERT_EQUAL_INT(1, seen[EPD_COLOR_BLACK]);
    TEST_ASSERT_EQUAL_INT(1, seen[EPD_COLOR_RED]);
    TEST_ASSERT_EQUAL_INT(1, seen[EPD_COLOR_YELLOW]);
    TEST_ASSERT_EQUAL_INT(1, seen[EPD_COLOR_GREEN]);
    TEST_ASSERT_EQUAL_INT(1, seen[EPD_COLOR_BLUE]);
}

static void test_green_and_blue_follow_a_clear(void)
{
    // The two colours that fail first here, so neither may be read through another colour's
    // residue (measurements.md's FRS sweep). Implied by the white-between rule above, and asserted
    // anyway: this is the reason that rule exists, and a future reordering should have to break it
    // on purpose.
    for (int i = 0; i < EPD_MAINT_COURSE_STEPS; i++) {
        const uint8_t c = epd_maint_course_colour(i);
        if (c == EPD_COLOR_GREEN || c == EPD_COLOR_BLUE) {
            TEST_ASSERT_GREATER_THAN_INT(0, i);
            TEST_ASSERT_EQUAL_UINT8(EPD_COLOR_WHITE, epd_maint_course_colour(i - 1));
        }
    }
}

static void test_out_of_range_is_refused(void)
{
    // 0xFF, not a white fallback: a caller that walks off the end must not quietly refresh once
    // more, and epd_color_valid() rejects this so the request layer turns it into an error.
    TEST_ASSERT_EQUAL_UINT8(0xFF, epd_maint_course_colour(-1));
    TEST_ASSERT_EQUAL_UINT8(0xFF, epd_maint_course_colour(EPD_MAINT_COURSE_STEPS));
    TEST_ASSERT_EQUAL_UINT8(0xFF, epd_maint_course_colour(1000));
    TEST_ASSERT_FALSE(epd_color_valid(epd_maint_course_colour(EPD_MAINT_COURSE_STEPS)));
    TEST_ASSERT_EQUAL_STRING("?", epd_maint_course_name(EPD_MAINT_COURSE_STEPS));
}

static void test_names_match_the_indices(void)
{
    // The name is what a console capture is counted by, so a name that disagreed with the index
    // would make a run unreadable in exactly the way the figures are checked.
    TEST_ASSERT_EQUAL_STRING("black", epd_maint_course_name(0));
    TEST_ASSERT_EQUAL_STRING("white", epd_maint_course_name(1));
    for (int i = 0; i < EPD_MAINT_COURSE_STEPS; i++) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, strcmp("?", epd_maint_course_name(i)),
                                      "a step with no name");
    }
}

// ------------------------------------------------------------------- the clear cycle

// **Every property below is asserted at EVERY allowed repeat count**, not at one. A repeat count is
// the whole of what the 2026-09-20 continuous-mode request turned into, so "it works for one cycle"
// is the thing this suite must not be satisfied by.

// 6N + 1, because the whites at the seams are shared: 7 for one cycle and 13 for two, not 7 and 14.
// Two consecutive identical whites clear nothing the first did not.
static void test_clear_steps_share_the_seam_whites(void)
{
    TEST_ASSERT_EQUAL_INT(7, epd_maint_clear_steps(1));
    TEST_ASSERT_EQUAL_INT(13, epd_maint_clear_steps(2));
    for (int n = 1; n <= EPD_MAINT_CLEAR_CYCLES_MAX; n++) {
        TEST_ASSERT_EQUAL_INT(6 * n + 1, epd_maint_clear_steps(n));
    }
    // A count outside the range is 0 and not a clamp, so a caller cannot smuggle one through.
    TEST_ASSERT_EQUAL_INT(0, epd_maint_clear_steps(0));
    TEST_ASSERT_EQUAL_INT(0, epd_maint_clear_steps(-1));
    TEST_ASSERT_EQUAL_INT(0, epd_maint_clear_steps(EPD_MAINT_CLEAR_CYCLES_MAX + 1));
}

// Every length has to be ODD, because that is what makes it start and end on white — and ending
// white is the vendor's ship-and-store state. An even count would end on black and leave the panel
// in the worst state for sticking, which is the opposite of the point.
static void test_clear_is_odd_and_bracketed_by_white(void)
{
    for (int n = 1; n <= EPD_MAINT_CLEAR_CYCLES_MAX; n++) {
        const int steps = epd_maint_clear_steps(n);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, steps % 2, "an even clear ends on black");
        TEST_ASSERT_EQUAL_UINT8(EPD_COLOR_WHITE, epd_maint_clear_colour(0, steps));
        TEST_ASSERT_EQUAL_UINT8(EPD_COLOR_WHITE, epd_maint_clear_colour(steps - 1, steps));
    }
}

static void test_clear_alternates_and_uses_two_colours(void)
{
    for (int n = 1; n <= EPD_MAINT_CLEAR_CYCLES_MAX; n++) {
        const int steps = epd_maint_clear_steps(n);
        for (int i = 0; i < steps; i++) {
            const uint8_t c = epd_maint_clear_colour(i, steps);
            TEST_ASSERT_TRUE_MESSAGE(epd_color_valid(c), "a step the panel cannot render");
            TEST_ASSERT_TRUE_MESSAGE(c == EPD_COLOR_WHITE || c == EPD_COLOR_BLACK,
                                     "the clear is white and black only");
            if (i > 0) {
                TEST_ASSERT_NOT_EQUAL_MESSAGE(epd_maint_clear_colour(i - 1, steps), c,
                                              "two of the same in a row clears nothing new");
            }
        }
    }
}

// Three black passes PER CYCLE: the owner asked for "white, black, white, three times", and the
// repeat count multiplies that. Counting blacks is what pins the intent — a length change that
// altered it would be visible here rather than only in the elapsed time.
static void test_clear_has_three_black_passes_per_cycle(void)
{
    for (int n = 1; n <= EPD_MAINT_CLEAR_CYCLES_MAX; n++) {
        const int steps = epd_maint_clear_steps(n);
        int blacks = 0;
        for (int i = 0; i < steps; i++) {
            if (epd_maint_clear_colour(i, steps) == EPD_COLOR_BLACK) {
                blacks++;
            }
        }
        TEST_ASSERT_EQUAL_INT(EPD_MAINT_CLEAR_BLACKS_PER_CYCLE * n, blacks);
    }
}

static void test_clear_out_of_range_is_refused(void)
{
    const int steps = epd_maint_clear_steps(1);
    TEST_ASSERT_EQUAL_UINT8(0xFF, epd_maint_clear_colour(-1, steps));
    TEST_ASSERT_EQUAL_UINT8(0xFF, epd_maint_clear_colour(steps, steps));
    TEST_ASSERT_EQUAL_UINT8(0xFF, epd_maint_clear_colour(0, 0));
    TEST_ASSERT_FALSE(epd_color_valid(epd_maint_clear_colour(steps, steps)));
    TEST_ASSERT_EQUAL_STRING("?", epd_maint_clear_name(steps, steps));
    TEST_ASSERT_EQUAL_STRING("white", epd_maint_clear_name(0, steps));
    TEST_ASSERT_EQUAL_STRING("black", epd_maint_clear_name(1, steps));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ten_steps);
    RUN_TEST(test_every_step_is_renderable);
    RUN_TEST(test_orange_never_appears);
    RUN_TEST(test_white_between_every_pair);
    RUN_TEST(test_ends_on_white);
    RUN_TEST(test_each_colour_inspected_once);
    RUN_TEST(test_green_and_blue_follow_a_clear);
    RUN_TEST(test_out_of_range_is_refused);
    RUN_TEST(test_names_match_the_indices);
    RUN_TEST(test_clear_steps_share_the_seam_whites);
    RUN_TEST(test_clear_is_odd_and_bracketed_by_white);
    RUN_TEST(test_clear_alternates_and_uses_two_colours);
    RUN_TEST(test_clear_has_three_black_passes_per_cycle);
    RUN_TEST(test_clear_out_of_range_is_refused);
    return UNITY_END();
}
