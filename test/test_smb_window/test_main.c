// smb_window_is_due(): may the SMB mirror open a window right now?
//
// Worth its own suite for the reason test_schedule gives for app_schedule_active(): this is where the
// obvious implementation is wrong, and **it is wrong SILENTLY**. A branch that never fires gives a
// mirror that never opens a window; one that always fires gives windows back to back with httpd
// down. Both read from the console as a network or a storage problem, which is what ticket 59 spent
// three arms discovering before it found the rule.
//
// This predicate was the most revised in app_smb_sync.c -- ticket 28 gave it the deadline, ticket 42
// the active-hours hold, ticket 37 Phase 3 the two on-demand reasons, ticket 59 corrected it twice --
// and until 2026-09-21 it had never executed on the host. Every case below is written so that a
// plausible wrong version turns red:
//
//   * the four branches are asserted in PRECEDENCE order, because the bugs available here are about
//     which branch wins, not about arithmetic: a manual request must beat ticket 42's hold, and an
//     open window must beat everything;
//   * ticket 42's hold is asserted to apply to the periodic branch AND NOT to the other three, which
//     is the distinction its own comment argues for at length and the one a tidier implementation
//     would flatten;
//   * every period test has a pair one microsecond either side of the threshold, so an `>` where the
//     code needs `>=` fails rather than passing by a rounding accident;
//   * the SMB_WINDOW_NEVER sentinels are asserted to short-circuit, because `now_us - INT64_MIN`
//     overflows and the only thing standing between this and that is the order of an `||`.
//
// The periods are small round numbers rather than the shipping hour, which is why they are a
// parameter: smb_window.h keeps them out of src/core/ so app_smb_sync.h can go on owning the values
// and their ticket references.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "unity.h"

#include "smb_window.h"

#define GAP 20
#define FIRST 30
#define PERIOD 3600

static const smb_window_timing_t T = {
    .window_gap_us = GAP,
    .first_delay_us = FIRST,
    .period_us = PERIOD,
};

// A state that is due on the periodic branch, so each case below can turn exactly one thing off and
// attribute the result to it.
static smb_window_state_t base(void)
{
    smb_window_state_t s = {
        .run_active = false,
        .last_window_end_us = 1000,
        .manual_request = false,
        .last_run_end_us = 1000,
        .on_demand = false,
        .want_count = 0,
        .last_catalog_us = 1000,
        .schedule_active = true,
    };
    return s;
}

void setUp(void) {}
void tearDown(void) {}

// --------------------------------------------------------------- the open window beats everything

static void test_an_open_window_obeys_only_the_gap(void)
{
    smb_window_state_t s = base();
    s.run_active = true;
    s.schedule_active = false;   // ticket 42's hold must not stop a run in progress
    s.manual_request = false;
    s.last_window_end_us = 1000;

    TEST_ASSERT_FALSE_MESSAGE(smb_window_is_due(&s, &T, 1000 + GAP - 1),
                              "inside the gap, a run in progress waits");
    TEST_ASSERT_TRUE_MESSAGE(smb_window_is_due(&s, &T, 1000 + GAP),
                             "at the gap it continues -- abandoning it mid-window leaves httpd down");
}

static void test_an_open_window_before_any_window_has_ended(void)
{
    // app_smb_sync.c leaves s_last_window_end_us at ZERO rather than SMB_WINDOW_NEVER, and the first
    // run sets run_active while it is still zero. The gap between windows has no meaning before
    // there has been one, so it must pass -- and zero is also what keeps this from overflowing.
    smb_window_state_t s = base();
    s.run_active = true;
    s.last_window_end_us = 0;

    TEST_ASSERT_FALSE(smb_window_is_due(&s, &T, GAP - 1));
    TEST_ASSERT_TRUE_MESSAGE(smb_window_is_due(&s, &T, GAP),
                             "the first run must not be blocked by a gap that has no predecessor");
}

// --------------------------------------------------------------- a user at the UI beats the hold

static void test_a_manual_request_beats_the_schedule_hold(void)
{
    // Ticket 42's own argument: "A manual request is a user at the UI and must always work."
    smb_window_state_t s = base();
    s.manual_request = true;
    s.schedule_active = false;

    TEST_ASSERT_TRUE(smb_window_is_due(&s, &T, 1000));
}

static void test_a_manual_request_beats_the_on_demand_gap(void)
{
    smb_window_state_t s = base();
    s.manual_request = true;
    s.on_demand = true;
    s.last_window_end_us = 1000;   // well inside the gap

    TEST_ASSERT_TRUE_MESSAGE(smb_window_is_due(&s, &T, 1001),
                             "the gap throttles the mirror's own asks, not the user's");
}

static void test_a_manual_request_does_not_beat_an_open_window(void)
{
    // Precedence, the other way round: run_active is tested first, so a manual request inside the
    // gap still waits. Pressing the button twice must not open two windows.
    smb_window_state_t s = base();
    s.run_active = true;
    s.manual_request = true;
    s.last_window_end_us = 1000;

    TEST_ASSERT_FALSE(smb_window_is_due(&s, &T, 1000 + GAP - 1));
}

// --------------------------------------------------------------- the first run

static void test_nothing_has_run_yet_waits_for_the_first_delay(void)
{
    // `now_us` is uptime here, so this branch is an ABSOLUTE comparison and not a delta. A version
    // that measured it from last_window_end_us would pass the second case and fail the first.
    smb_window_state_t s = base();
    s.last_run_end_us = SMB_WINDOW_NEVER;
    s.schedule_active = false;   // does not apply on this branch

    TEST_ASSERT_FALSE(smb_window_is_due(&s, &T, FIRST - 1));
    TEST_ASSERT_TRUE(smb_window_is_due(&s, &T, FIRST));
}

static void test_the_first_delay_ignores_the_schedule(void)
{
    // A frame booted inside its quiet hours must still get its first catalogue, or a first boot in
    // the evening shows nothing until morning.
    smb_window_state_t s = base();
    s.last_run_end_us = SMB_WINDOW_NEVER;
    s.schedule_active = false;

    TEST_ASSERT_TRUE(smb_window_is_due(&s, &T, FIRST + 1));
}

// --------------------------------------------------------------- on demand: two reasons, not one

static void test_on_demand_respects_the_gap_even_with_wants(void)
{
    smb_window_state_t s = base();
    s.on_demand = true;
    s.want_count = 3;
    s.last_window_end_us = 1000;

    TEST_ASSERT_FALSE_MESSAGE(smb_window_is_due(&s, &T, 1000 + GAP - 1),
                              "a burst of asks must not open windows back to back");
    TEST_ASSERT_TRUE(smb_window_is_due(&s, &T, 1000 + GAP));
}

static void test_on_demand_opens_for_a_want(void)
{
    smb_window_state_t s = base();
    s.on_demand = true;
    s.want_count = 1;
    s.last_window_end_us = 0;
    s.last_catalog_us = 1000;   // fresh, so the want is the only reason

    TEST_ASSERT_TRUE(smb_window_is_due(&s, &T, 1000));
}

static void test_on_demand_opens_for_a_stale_catalogue_with_no_wants(void)
{
    smb_window_state_t s = base();
    s.on_demand = true;
    s.want_count = 0;
    s.last_window_end_us = 0;
    s.last_catalog_us = 1000;

    TEST_ASSERT_FALSE(smb_window_is_due(&s, &T, 1000 + PERIOD - 1));
    TEST_ASSERT_TRUE(smb_window_is_due(&s, &T, 1000 + PERIOD));
}

static void test_on_demand_opens_when_the_catalogue_has_never_been_listed(void)
{
    // The sentinel short-circuit. Were the `||` the other way round this would compute
    // now_us - INT64_MIN, which overflows -- so this case is about the order of an operator, not
    // about a schedule.
    smb_window_state_t s = base();
    s.on_demand = true;
    s.want_count = 0;
    s.last_window_end_us = 0;
    s.last_catalog_us = SMB_WINDOW_NEVER;

    TEST_ASSERT_TRUE(smb_window_is_due(&s, &T, GAP));
}

static void test_on_demand_is_not_held_by_the_schedule(void)
{
    // Ticket 42 left this branch alone ON PURPOSE, and the comment says why: "the want list is fed
    // by slideshow advances, which the schedule has already stopped, so it self-suppresses without
    // stranding a want that was declared before the window closed -- while the catalogue keeps its
    // hourly clock ON PURPOSE, so the morning's catalogue is current."
    //
    // A tidier version that moved the hold above the on_demand branch would fail both of these, and
    // the symptom would be a want declared just before the quiet hours never being fetched.
    smb_window_state_t s = base();
    s.on_demand = true;
    s.schedule_active = false;
    s.last_window_end_us = 0;

    s.want_count = 1;
    TEST_ASSERT_TRUE_MESSAGE(smb_window_is_due(&s, &T, GAP),
                             "a want declared before the hold must not be stranded");

    s.want_count = 0;
    s.last_catalog_us = 0;
    TEST_ASSERT_TRUE_MESSAGE(smb_window_is_due(&s, &T, PERIOD),
                             "the catalogue keeps its hourly clock through the quiet hours");
}

// --------------------------------------------------------------- the periodic run, and its hold

static void test_the_periodic_run_waits_for_its_period(void)
{
    smb_window_state_t s = base();
    s.last_run_end_us = 1000;

    TEST_ASSERT_FALSE(smb_window_is_due(&s, &T, 1000 + PERIOD - 1));
    TEST_ASSERT_TRUE(smb_window_is_due(&s, &T, 1000 + PERIOD));
}

static void test_the_schedule_hold_stops_the_periodic_run(void)
{
    // TICKET 42'S HOLD, on the one branch it belongs to: the periodic mirror run, "which fetches a
    // whole folder on its own clock and is the write-rate term ticket 39 names."
    smb_window_state_t s = base();
    s.last_run_end_us = 1000;
    s.schedule_active = false;

    TEST_ASSERT_FALSE_MESSAGE(smb_window_is_due(&s, &T, 1000 + PERIOD * 10),
                              "the hold must outlast any number of elapsed periods");
}

static void test_the_hold_applies_to_the_periodic_branch_alone(void)
{
    // The whole point, in one case: with the schedule inactive, FOUR branches still open a window and
    // only the periodic one does not. All five are asserted here rather than left to the cases above,
    // because this is the claim the module's comment makes and a reader should be able to check it in
    // one place.
    //
    // **The on_demand arm was missing from this case until the falsification pass.** Moving the hold
    // above the on_demand branch in smb_window.c turned test_on_demand_is_not_held_by_the_schedule
    // red and left THIS case green, which is how the hole was found -- a case that claims "the
    // periodic branch alone" and checks three of the four others.
    smb_window_state_t s = base();
    s.schedule_active = false;

    smb_window_state_t open = s;
    open.run_active = true;
    open.last_window_end_us = 0;
    TEST_ASSERT_TRUE_MESSAGE(smb_window_is_due(&open, &T, GAP), "an open window is not held");

    smb_window_state_t manual = s;
    manual.manual_request = true;
    TEST_ASSERT_TRUE_MESSAGE(smb_window_is_due(&manual, &T, 1000), "a user is not held");

    smb_window_state_t first = s;
    first.last_run_end_us = SMB_WINDOW_NEVER;
    TEST_ASSERT_TRUE_MESSAGE(smb_window_is_due(&first, &T, FIRST), "the first run is not held");

    smb_window_state_t demand = s;
    demand.on_demand = true;
    demand.want_count = 1;
    demand.last_window_end_us = 0;
    TEST_ASSERT_TRUE_MESSAGE(smb_window_is_due(&demand, &T, GAP), "on demand is not held");

    smb_window_state_t periodic = s;
    periodic.last_run_end_us = 1000;
    TEST_ASSERT_FALSE_MESSAGE(smb_window_is_due(&periodic, &T, 1000 + PERIOD),
                              "the periodic run IS held -- this is the one ticket 42 is about");
}

// --------------------------------------------------------------- refusals

static void test_no_state_means_no_window(void)
{
    const smb_window_state_t s = base();
    TEST_ASSERT_FALSE(smb_window_is_due(NULL, &T, 1000000));
    TEST_ASSERT_FALSE(smb_window_is_due(&s, NULL, 1000000));
    TEST_ASSERT_FALSE(smb_window_is_due(NULL, NULL, 1000000));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_an_open_window_obeys_only_the_gap);
    RUN_TEST(test_an_open_window_before_any_window_has_ended);
    RUN_TEST(test_a_manual_request_beats_the_schedule_hold);
    RUN_TEST(test_a_manual_request_beats_the_on_demand_gap);
    RUN_TEST(test_a_manual_request_does_not_beat_an_open_window);
    RUN_TEST(test_nothing_has_run_yet_waits_for_the_first_delay);
    RUN_TEST(test_the_first_delay_ignores_the_schedule);
    RUN_TEST(test_on_demand_respects_the_gap_even_with_wants);
    RUN_TEST(test_on_demand_opens_for_a_want);
    RUN_TEST(test_on_demand_opens_for_a_stale_catalogue_with_no_wants);
    RUN_TEST(test_on_demand_opens_when_the_catalogue_has_never_been_listed);
    RUN_TEST(test_on_demand_is_not_held_by_the_schedule);
    RUN_TEST(test_the_periodic_run_waits_for_its_period);
    RUN_TEST(test_the_schedule_hold_stops_the_periodic_run);
    RUN_TEST(test_the_hold_applies_to_the_periodic_branch_alone);
    RUN_TEST(test_no_state_means_no_window);
    return UNITY_END();
}
