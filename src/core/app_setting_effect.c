#include "app_setting_effect.h"

#include <stddef.h>

// One row per app_setting_key_t. The designated initialisers carry only the keys that owe
// something; every other key gets a zeroed row, which reads as APP_SETTING_LIVE_NONE with no
// redraw -- correct, and the reason the table can span all forty keys without forty lines.
//
// **Adding a row here is a claim that app_settings_set_*() alone leaves the device in a state the
// user did not ask for.** test_setting_effect asserts this list by name in both directions, so a
// seventh row is a red test until somebody has said why it is one.
static const app_setting_effect_t TABLE[APP_SETTING_COUNT] = {
    // FR-5.2: an orientation change re-renders what is displayed.
    [APP_SETTING_ROTATION] = { APP_SETTING_LIVE_DISPLAY, true },
    // "The point of choosing one is to see it, and waiting for the slideshow's next tick to find
    // out would make the setting feel broken" -- h_mode_cfg_set()'s own argument, which applies
    // unchanged to the three below it.
    [APP_SETTING_PALETTE] = { APP_SETTING_LIVE_DISPLAY, true },
    [APP_SETTING_AUTO_ADJUST] = { APP_SETTING_LIVE_DISPLAY, true },
    [APP_SETTING_DITHER_DIFFUSE] = { APP_SETTING_LIVE_DISPLAY, true },
    [APP_SETTING_AUTO_ROTATE] = { APP_SETTING_LIVE_DISPLAY, true },
    // Ticket 71. The charger, not the panel -- and no redraw, because the cap changes nothing a
    // person can see. It is in this table for the live half alone.
    [APP_SETTING_CHARGE_LIMIT] = { APP_SETTING_LIVE_CHARGER, false },
};

const app_setting_effect_t *app_setting_effect(app_setting_key_t key)
{
    if ((size_t)key >= (size_t)APP_SETTING_COUNT) {
        return NULL;
    }
    return &TABLE[key];
}

bool app_setting_implies_redraw(app_setting_key_t key)
{
    const app_setting_effect_t *e = app_setting_effect(key);
    return e && e->implies_redraw;
}
