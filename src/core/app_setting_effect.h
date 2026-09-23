// What a settings change owes BEYOND being persisted.
//
// app_settings_set_*() only writes NVS. Five of this project's settings have a second half that
// must run for the change to mean anything -- app_display_set_*() pushes the value into the
// display module's own copy, which the display task reads under its own mutex -- and a sixth,
// charge_limit_pct, has app_charge_apply() instead. app_settings.h already says so, in prose, at
// the two setters that happened to get documented:
//
//     app_settings.h  **Saving it is not enough to turn the panel** -- app_display_set_rotation()
//                     is the other half, and app_display_redraw() the third.
//     app_settings.h  **Saving it is not enough to change the charger** -- app_charge_apply() is
//                     the other half, exactly as app_settings_set_rotation() needs
//                     app_display_set_rotation().
//
// Prose was all there was, and the halves were then written out by hand at every call site:
// app_server.c's h_mode_cfg_set() for six keys and frame_main.c's TOP-button cycle for one.
// **Omitting the second half is silent in every direction** -- the value persists, reads back
// correctly over HTTP, and the panel does not change. app_settings.h records that having happened
// to low_power_mode, and ticket 69 records it happening to rotation, where the frame "accepts
// rotation: 2, answers 200, renders it, and is back to 1 after a reboot with nothing in the
// console", making the acceptance test a power cycle rather than a GET.
//
// This table is that fact in one place. It lives in src/core/ rather than beside the setters
// because it is pure, which is the same reason board_smb_classify.c and app_schedule.c are here:
// test/test_setting_effect then asserts which keys owe a live half and which owe a redraw, so
// getting it wrong is a red test rather than a frame that looks saved. src/app/app_apply.c is the
// one module that acts on it.
//
// **The table spans EVERY key, not only the six.** A row per app_setting_key_t, almost all of them
// saying "nothing owed", is what lets the host test assert the six BY NAME and assert that no
// seventh has quietly appeared -- a spurious live half is as much a defect as a missing one, and a
// table holding only the interesting keys could not see it.
//
// **This is NOT where a value is validated, and it must not become that.** Ticket 12 put
// validation at the route AND the setter deliberately; tickets 68 and 71 say why -- a body with
// one bad field must change nothing, and a charge cap of 90 must be refused by name rather than
// clamped. Nothing here reads a value: the table is keyed by setting, not by value, and knows only
// what a change owes once it has been accepted.

#ifndef APP_SETTING_EFFECT_H
#define APP_SETTING_EFFECT_H

#include <stdbool.h>

#include "app_setting_key.h"

// Which live half a change has to run. Two distinct destinations rather than one boolean, because
// they are not interchangeable: the display half pushes a value into another module's copy and is
// cheap, the charger half writes an I2C register and prints what it did (and on the M5 Paper Color
// prints that it could not, permanently -- docs/board-pinmap.md's "Charging" section).
typedef enum {
    APP_SETTING_LIVE_NONE = 0,
    APP_SETTING_LIVE_DISPLAY,
    APP_SETTING_LIVE_CHARGER,
} app_setting_live_t;

typedef struct {
    app_setting_live_t live;
    // Whether the change alters the picture ALREADY ON THE GLASS, and so owes a re-render.
    //
    // False is the interesting value and it is a judgement, not an oversight. h_mode_cfg_set()
    // argues it three times: slideshow_random "changes which photograph comes NEXT, not how the
    // displayed one looks, so redrawing would be a visible no-op that costs a 15 s refresh"; the
    // timezone and the active hours change WHEN the frame refreshes; and turning standby_white on
    // "must not park the panel immediately ... a photograph replaced by white the moment a switch
    // is flipped in the afternoon would read as the frame having broken". APP_SETTING_CHARGE_LIMIT
    // is the one key with a live half and the same answer.
    bool implies_redraw;
} app_setting_effect_t;

// The table row for a key, or NULL when the key is out of range. A valid key always has a row, and
// for most keys that row says nothing is owed -- so a caller distinguishes "owes nothing" (a row
// with APP_SETTING_LIVE_NONE) from "not a setting" (NULL), which are different mistakes.
const app_setting_effect_t *app_setting_effect(app_setting_key_t key);

// True when a change to this key owes a re-render. Convenience over the row above, for the caller
// that is accumulating one redraw decision across a body carrying several settings. False for a
// key out of range: an unknown key must not cost a 15 s refresh.
bool app_setting_implies_redraw(app_setting_key_t key);

#endif  // APP_SETTING_EFFECT_H
