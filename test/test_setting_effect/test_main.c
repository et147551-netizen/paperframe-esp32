// app_setting_effect(): what a settings change owes beyond being persisted.
//
// Worth its own suite for the reason the table exists. The obligation it records -- push the value
// into app_display, or call app_charge_apply(), and then redraw once -- was prose in app_settings.h
// and hand-written code at every call site, and **forgetting it is silent in every direction**: the
// value persists, reads back correctly over HTTP, and nothing on the glass changes. ticket 69's
// account of exactly that is a frame that "accepts rotation: 2, answers 200, renders it, and is
// back to 1 after a reboot with nothing in the console".
//
// The table spans every app_setting_key_t, so the cases below can assert the six interesting keys
// **in both directions**, which is what makes each of the three ways it can be wrong turn red:
//
//   1. A key that owes a live half and has no row. test_exactly_these_keys_owe_a_live_half fails on
//      the count, and the named check below it says which.
//   2. A spurious row -- a key given a live half it does not have, which would call
//      app_display_set_*() for a setting the display knows nothing about.
//      test_keys_outside_the_list_owe_nothing is the one that sees it.
//   3. Every key made to imply a redraw, which is the tidy-looking mistake. A redraw is a 15 s
//      panel refresh on this board and 31 s on the other, and the charge cap changes nothing a
//      person can see, so test_charge_limit_implies_no_redraw fails a table that redraws for
//      everything.
//
// Verified falsifiable on 2026-09-21 by adding a seventh key to the enum with no row: case 1 fails
// with "add the row, or the key does not belong in the list below", and nothing else does.

#include <stdbool.h>
#include <stddef.h>

#include "unity.h"

#include "app_setting_effect.h"

void setUp(void) {}
void tearDown(void) {}

// The six keys that owe something beyond a persist, as a list, so the assertions below can run in
// both directions against it. **This list is the specification** -- the table in
// app_setting_effect.c is what is under test.
static const app_setting_key_t LIVE_KEYS[] = {
    APP_SETTING_ROTATION,
    APP_SETTING_PALETTE,
    APP_SETTING_AUTO_ADJUST,
    APP_SETTING_DITHER_DIFFUSE,
    APP_SETTING_AUTO_ROTATE,
    APP_SETTING_CHARGE_LIMIT,
};
static const size_t LIVE_KEY_COUNT = sizeof(LIVE_KEYS) / sizeof(LIVE_KEYS[0]);

// The five of those that reach the panel. APP_SETTING_CHARGE_LIMIT is the sixth and goes elsewhere.
static const app_setting_key_t RENDER_KEYS[] = {
    APP_SETTING_ROTATION,
    APP_SETTING_PALETTE,
    APP_SETTING_AUTO_ADJUST,
    APP_SETTING_DITHER_DIFFUSE,
    APP_SETTING_AUTO_ROTATE,
};
static const size_t RENDER_KEY_COUNT = sizeof(RENDER_KEYS) / sizeof(RENDER_KEYS[0]);

static bool in_live_list(app_setting_key_t key)
{
    for (size_t i = 0; i < LIVE_KEY_COUNT; i++) {
        if (LIVE_KEYS[i] == key) {
            return true;
        }
    }
    return false;
}

static void test_every_key_has_a_row(void)
{
    for (int k = 0; k < APP_SETTING_COUNT; k++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(app_setting_effect((app_setting_key_t)k),
                                     "every key below APP_SETTING_COUNT has a row");
    }
}

static void test_bad_key_is_null_not_a_zeroed_row(void)
{
    // APP_SETTING_COUNT itself is the off-by-one a loop bound gets wrong, so it is asserted by name
    // rather than left to the values beyond it. NULL and not a zeroed row, because "owes nothing"
    // and "is not a setting" are different mistakes and a caller may want to tell them apart.
    TEST_ASSERT_NULL_MESSAGE(app_setting_effect(APP_SETTING_COUNT), "COUNT is not a key");
    TEST_ASSERT_NULL(app_setting_effect((app_setting_key_t)(APP_SETTING_COUNT + 1)));
    TEST_ASSERT_NULL(app_setting_effect((app_setting_key_t)999));
}

static void test_exactly_these_keys_owe_a_live_half(void)
{
    size_t found = 0;
    for (int k = 0; k < APP_SETTING_COUNT; k++) {
        const app_setting_effect_t *e = app_setting_effect((app_setting_key_t)k);
        TEST_ASSERT_NOT_NULL(e);
        if (e->live != APP_SETTING_LIVE_NONE) {
            found++;
        }
    }
    TEST_ASSERT_EQUAL_size_t_MESSAGE(LIVE_KEY_COUNT, found,
                                     "add the row, or the key does not belong in the list below");
}

static void test_each_listed_key_owes_a_live_half(void)
{
    for (size_t i = 0; i < LIVE_KEY_COUNT; i++) {
        const app_setting_effect_t *e = app_setting_effect(LIVE_KEYS[i]);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(APP_SETTING_LIVE_NONE, e->live,
                                      "a listed key owes a live half");
    }
}

static void test_keys_outside_the_list_owe_nothing(void)
{
    // The spurious-row case. A live half for a setting the display or the charger knows nothing
    // about would call into it with a value it cannot use, and nothing else in the build would say
    // so -- app_apply.c logs the routing failure, which is a console line on a device.
    for (int k = 0; k < APP_SETTING_COUNT; k++) {
        if (in_live_list((app_setting_key_t)k)) {
            continue;
        }
        const app_setting_effect_t *e = app_setting_effect((app_setting_key_t)k);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL_MESSAGE(APP_SETTING_LIVE_NONE, e->live,
                                  "an unlisted key owes no live half");
        TEST_ASSERT_FALSE_MESSAGE(e->implies_redraw, "an unlisted key owes no redraw");
    }
}

static void test_render_keys_reach_the_display(void)
{
    for (size_t i = 0; i < RENDER_KEY_COUNT; i++) {
        const app_setting_effect_t *e = app_setting_effect(RENDER_KEYS[i]);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL_MESSAGE(APP_SETTING_LIVE_DISPLAY, e->live,
                                  "a render setting's live half is app_display_set_*()");
    }
}

static void test_render_keys_imply_a_redraw(void)
{
    // FR-5.2 for rotation, and h_mode_cfg_set()'s own argument for the other four: the point of
    // choosing one is to see it. A table that persisted these without redrawing would give a
    // setting that sticks, reads back, and shows nothing until the slideshow's next advance.
    for (size_t i = 0; i < RENDER_KEY_COUNT; i++) {
        TEST_ASSERT_TRUE_MESSAGE(app_setting_implies_redraw(RENDER_KEYS[i]),
                                 "a render setting owes a re-render");
    }
}

static void test_charge_limit_reaches_the_charger(void)
{
    const app_setting_effect_t *e = app_setting_effect(APP_SETTING_CHARGE_LIMIT);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_MESSAGE(APP_SETTING_LIVE_CHARGER, e->live,
                              "ticket 71: the charger, not the panel");
}

static void test_charge_limit_implies_no_redraw(void)
{
    // The tidy-looking mistake. A redraw here costs a 15 s refresh for a change nobody can see.
    TEST_ASSERT_FALSE_MESSAGE(app_setting_implies_redraw(APP_SETTING_CHARGE_LIMIT),
                              "the charge cap changes nothing on the glass");
}

static void test_a_redraw_never_stands_alone(void)
{
    // A key that owes a re-render but pushes nothing is incoherent: the render would read the
    // display module's unchanged copy and produce the same picture, at the cost of a refresh.
    for (int k = 0; k < APP_SETTING_COUNT; k++) {
        const app_setting_effect_t *e = app_setting_effect((app_setting_key_t)k);
        TEST_ASSERT_NOT_NULL(e);
        if (e->implies_redraw) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(APP_SETTING_LIVE_NONE, e->live,
                                          "a redraw with nothing pushed re-renders the same picture");
        }
    }
}

static void test_implies_redraw_agrees_with_the_row(void)
{
    // Two readings of one fact, so the convenience cannot drift from the table it reads.
    for (int k = 0; k < APP_SETTING_COUNT; k++) {
        const app_setting_effect_t *e = app_setting_effect((app_setting_key_t)k);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL(e->implies_redraw, app_setting_implies_redraw((app_setting_key_t)k));
    }
}

static void test_implies_redraw_is_false_for_a_bad_key(void)
{
    // Fails closed: an unknown key must not cost a refresh.
    TEST_ASSERT_FALSE(app_setting_implies_redraw(APP_SETTING_COUNT));
    TEST_ASSERT_FALSE(app_setting_implies_redraw((app_setting_key_t)999));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_key_has_a_row);
    RUN_TEST(test_bad_key_is_null_not_a_zeroed_row);
    RUN_TEST(test_exactly_these_keys_owe_a_live_half);
    RUN_TEST(test_each_listed_key_owes_a_live_half);
    RUN_TEST(test_keys_outside_the_list_owe_nothing);
    RUN_TEST(test_render_keys_reach_the_display);
    RUN_TEST(test_render_keys_imply_a_redraw);
    RUN_TEST(test_charge_limit_reaches_the_charger);
    RUN_TEST(test_charge_limit_implies_no_redraw);
    RUN_TEST(test_a_redraw_never_stands_alone);
    RUN_TEST(test_implies_redraw_agrees_with_the_row);
    RUN_TEST(test_implies_redraw_is_false_for_a_bad_key);
    return UNITY_END();
}
