// heapwatch: the low-water record that has to keep its value and its attribution together.
//
// Worth its own suite for a reason that is not obvious: the arithmetic is three lines, and the
// failure this guards against is not in the comparison. It is a record whose `value` moves while
// `flags` and `at_ms` stay behind -- which produces a log line that names a moment and a set of
// concurrent activity for a low that happened somewhere else entirely. On hardware that reads as
// an attribution, and there is nothing in the capture to contradict it. Ticket 47 is open because
// an unattributed number cannot be argued with; a wrongly attributed one is worse.

#include <string.h>

#include "unity.h"

#include "heapwatch.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------- seeding

static void test_first_sample_seeds_every_field(void)
{
    heapwatch_low_t low;
    memset(&low, 0, sizeof(low));

    TEST_ASSERT_TRUE(heapwatch_note(&low, 40000, 31744, 1234, HEAPWATCH_F_SYNC, -1));
    TEST_ASSERT_TRUE(low.seen);
    TEST_ASSERT_EQUAL_UINT32(40000, low.value);
    TEST_ASSERT_EQUAL_UINT32(31744, low.companion);
    TEST_ASSERT_EQUAL_INT64(1234, low.at_ms);
    TEST_ASSERT_EQUAL_UINT32(HEAPWATCH_F_SYNC, low.flags);
    TEST_ASSERT_EQUAL_INT64(-1, low.http_age_ms);
}

// `seen` exists because 0 is a legitimate reading -- an exhausted pool is exactly the event this
// is for, and a zero-initialised record must not already claim to hold it.
static void test_zero_is_a_real_value_not_an_empty_record(void)
{
    heapwatch_low_t low;
    memset(&low, 0, sizeof(low));

    TEST_ASSERT_TRUE(heapwatch_note(&low, 0, 0, 500, HEAPWATCH_F_HTTP, 12));
    TEST_ASSERT_TRUE(low.seen);
    TEST_ASSERT_EQUAL_UINT32(0, low.value);
    TEST_ASSERT_EQUAL_INT64(500, low.at_ms);

    // And nothing displaces it afterwards, because nothing is lower.
    TEST_ASSERT_FALSE(heapwatch_note(&low, 1, 1, 600, HEAPWATCH_F_SYNC, 0));
    TEST_ASSERT_EQUAL_INT64(500, low.at_ms);
    TEST_ASSERT_EQUAL_UINT32(HEAPWATCH_F_HTTP, low.flags);
}

// ---------------------------------------------------------------- what must not displace

static void test_a_higher_value_changes_nothing(void)
{
    heapwatch_low_t low;
    memset(&low, 0, sizeof(low));
    heapwatch_note(&low, 13675, 23000, 8000, HEAPWATCH_F_SYNC, 4000);

    TEST_ASSERT_FALSE(heapwatch_note(&low, 70000, 31744, 9000, HEAPWATCH_F_DISPLAY, 10));

    TEST_ASSERT_EQUAL_UINT32(13675, low.value);
    TEST_ASSERT_EQUAL_UINT32(23000, low.companion);
    TEST_ASSERT_EQUAL_INT64(8000, low.at_ms);
    TEST_ASSERT_EQUAL_UINT32(HEAPWATCH_F_SYNC, low.flags);
    TEST_ASSERT_EQUAL_INT64(4000, low.http_age_ms);
}

// The rule with a reason rather than a convention: the FIRST arrival at a floor is the
// attributable one. A frame sitting at a plateau samples the same depth fifty times a second at
// the LED task's tick, and taking the last of them would walk `at_ms` and `flags` forward to a
// moment when nothing was happening -- which is the shape of a wrong attribution, not a missing
// one. Idle would overwrite the event that caused the depth.
static void test_an_equal_value_does_not_walk_the_attribution_forward(void)
{
    heapwatch_low_t low;
    memset(&low, 0, sizeof(low));
    heapwatch_note(&low, 2899, 23000, 60000, HEAPWATCH_F_SYNC | HEAPWATCH_F_DISPLAY, 200);

    // The same depth, later, with nothing running.
    TEST_ASSERT_FALSE(heapwatch_note(&low, 2899, 31744, 900000, 0, 500000));

    TEST_ASSERT_EQUAL_INT64(60000, low.at_ms);
    TEST_ASSERT_EQUAL_UINT32(HEAPWATCH_F_SYNC | HEAPWATCH_F_DISPLAY, low.flags);
    TEST_ASSERT_EQUAL_UINT32(23000, low.companion);
    TEST_ASSERT_EQUAL_INT64(200, low.http_age_ms);
}

// ---------------------------------------------------------------- what must displace, wholly

static void test_a_lower_value_replaces_the_whole_record(void)
{
    heapwatch_low_t low;
    memset(&low, 0, sizeof(low));
    heapwatch_note(&low, 20399, 31744, 30000, HEAPWATCH_F_SYNC, 25000);

    TEST_ASSERT_TRUE(heapwatch_note(&low, 2899, 2048, 61234, HEAPWATCH_F_DISPLAY | HEAPWATCH_F_HTTP,
                                    15));

    // Every field, checked individually. A record that kept any one of the old values would
    // still look plausible in a log line, which is why this is five assertions and not one.
    TEST_ASSERT_EQUAL_UINT32(2899, low.value);
    TEST_ASSERT_EQUAL_UINT32(2048, low.companion);
    TEST_ASSERT_EQUAL_INT64(61234, low.at_ms);
    TEST_ASSERT_EQUAL_UINT32(HEAPWATCH_F_DISPLAY | HEAPWATCH_F_HTTP, low.flags);
    TEST_ASSERT_EQUAL_INT64(15, low.http_age_ms);
}

// Flags of zero are a real observation -- "the low happened with none of the three running", which
// would be the most interesting result of the run, since it would mean the cause is something this
// instrument does not watch. So a lower value with no flags must still replace, and must clear the
// flags rather than leave the previous set standing.
static void test_no_flags_still_replaces_and_clears(void)
{
    heapwatch_low_t low;
    memset(&low, 0, sizeof(low));
    heapwatch_note(&low, 13675, 23000, 8000, HEAPWATCH_F_SYNC | HEAPWATCH_F_DISPLAY, 4000);

    TEST_ASSERT_TRUE(heapwatch_note(&low, 12095, 22000, 8500, 0, -1));

    TEST_ASSERT_EQUAL_UINT32(0, low.flags);
    TEST_ASSERT_EQUAL_INT64(-1, low.http_age_ms);
    TEST_ASSERT_EQUAL_UINT32(12095, low.value);
}

// The two pools are two records, and the companion figure is context that must NOT drive the
// decision. dma_largest falling while int_free rises is the case that matters: lwIP drops frames
// on the DMA pool's largest block, so that event has to land in the DMA record and leave the
// internal record alone.
static void test_the_companion_never_drives_the_decision(void)
{
    heapwatch_low_t int_low, dma_low;
    memset(&int_low, 0, sizeof(int_low));
    memset(&dma_low, 0, sizeof(dma_low));

    heapwatch_note(&int_low, 40000, 31744, 1000, 0, -1);
    heapwatch_note(&dma_low, 31744, 40000, 1000, 0, -1);

    // int_free up, dma_largest collapsing.
    TEST_ASSERT_FALSE(heapwatch_note(&int_low, 45000, 1800, 2000, HEAPWATCH_F_HTTP, 5));
    TEST_ASSERT_TRUE(heapwatch_note(&dma_low, 1800, 45000, 2000, HEAPWATCH_F_HTTP, 5));

    TEST_ASSERT_EQUAL_UINT32(40000, int_low.value);
    TEST_ASSERT_EQUAL_UINT32(31744, int_low.companion);  // NOT 1800
    TEST_ASSERT_EQUAL_UINT32(1800, dma_low.value);
    TEST_ASSERT_EQUAL_INT64(2000, dma_low.at_ms);
}

static void test_a_null_record_is_refused(void)
{
    TEST_ASSERT_FALSE(heapwatch_note(NULL, 1, 2, 3, 0, 4));
}

// ---------------------------------------------------------------- the flags string

// Fixed order, so two records on one log line can be compared by eye rather than by parsing.
static void test_flags_string_is_ordered_and_complete(void)
{
    char buf[HEAPWATCH_FLAGS_STR_MIN];

    TEST_ASSERT_EQUAL_STRING("-", heapwatch_flags_str(0, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("s", heapwatch_flags_str(HEAPWATCH_F_SYNC, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("d", heapwatch_flags_str(HEAPWATCH_F_DISPLAY, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("h", heapwatch_flags_str(HEAPWATCH_F_HTTP, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(
        "sd", heapwatch_flags_str(HEAPWATCH_F_DISPLAY | HEAPWATCH_F_SYNC, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(
        "sh", heapwatch_flags_str(HEAPWATCH_F_HTTP | HEAPWATCH_F_SYNC, buf, sizeof(buf)));
    // The three-way overlap: the whole reason ticket 47 is open, so it must be legible.
    TEST_ASSERT_EQUAL_STRING("sdh",
                             heapwatch_flags_str(
                                 HEAPWATCH_F_SYNC | HEAPWATCH_F_DISPLAY | HEAPWATCH_F_HTTP, buf,
                                 sizeof(buf)));
}

// The heartbeat formats both records in one printf and the buffers are stack arrays; a formatter
// that overran one of them would corrupt the other's flags and there would be nothing in the log
// to say so.
static void test_flags_string_never_overruns(void)
{
    char buf[8];
    memset(buf, 'X', sizeof(buf));
    heapwatch_flags_str(HEAPWATCH_F_SYNC | HEAPWATCH_F_DISPLAY | HEAPWATCH_F_HTTP, buf,
                        HEAPWATCH_FLAGS_STR_MIN);
    TEST_ASSERT_EQUAL_STRING("sdh", buf);
    TEST_ASSERT_EQUAL_CHAR('X', buf[4]);  // untouched past the size it was given

    // A buffer too small truncates rather than writing past the end.
    memset(buf, 'X', sizeof(buf));
    heapwatch_flags_str(HEAPWATCH_F_SYNC | HEAPWATCH_F_DISPLAY | HEAPWATCH_F_HTTP, buf, 2);
    TEST_ASSERT_EQUAL_STRING("s", buf);
    TEST_ASSERT_EQUAL_CHAR('X', buf[2]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_sample_seeds_every_field);
    RUN_TEST(test_zero_is_a_real_value_not_an_empty_record);
    RUN_TEST(test_a_higher_value_changes_nothing);
    RUN_TEST(test_an_equal_value_does_not_walk_the_attribution_forward);
    RUN_TEST(test_a_lower_value_replaces_the_whole_record);
    RUN_TEST(test_no_flags_still_replaces_and_clears);
    RUN_TEST(test_the_companion_never_drives_the_decision);
    RUN_TEST(test_a_null_record_is_refused);
    RUN_TEST(test_flags_string_is_ordered_and_complete);
    RUN_TEST(test_flags_string_never_overruns);
    return UNITY_END();
}
