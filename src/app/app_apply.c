#include "app_apply.h"

#include "esp_log.h"

#include "app_charge.h"
#include "app_display.h"
#include "app_settings.h"

static const char *TAG = "apply";

// ---------------------------------------------------------------------------------------------
// The three switches below carry THE SAME SIX CASES, and they are kept adjacent for that reason:
// a key present in one and missing from another is precisely the class of defect this module
// exists to remove. Each has a `default:` that refuses rather than falling through, so the worst
// a missed case can do is decline to act -- never persist a value it then fails to apply.
//
// app_setting_effect.h's table is the policy (which destination, and whether a redraw is owed);
// these are the plumbing (which function reaches that destination for this key). The split is
// where it is because the table is in src/core/, which is ESP-IDF-free and cannot name
// app_display or app_charge at all.
// ---------------------------------------------------------------------------------------------

// The stored value, so a body that re-sends what is already set costs neither an NVS write nor a
// 15 s refresh. Returns false for a key this module does not handle.
static bool read_current(app_setting_key_t key, int *out)
{
    switch (key) {
    case APP_SETTING_ROTATION:
        *out = (int)app_settings_rotation();
        return true;
    case APP_SETTING_PALETTE:
        *out = (int)app_settings_palette();
        return true;
    case APP_SETTING_AUTO_ADJUST:
        *out = app_settings_auto_adjust() ? 1 : 0;
        return true;
    case APP_SETTING_DITHER_DIFFUSE:
        *out = app_settings_dither_diffuse() ? 1 : 0;
        return true;
    case APP_SETTING_AUTO_ROTATE:
        *out = app_settings_auto_rotate() ? 1 : 0;
        return true;
    case APP_SETTING_CHARGE_LIMIT:
        *out = (int)app_settings_charge_limit_pct();
        return true;
    default:
        return false;
    }
}

static esp_err_t persist(app_setting_key_t key, int value)
{
    switch (key) {
    case APP_SETTING_ROTATION:
        return app_settings_set_rotation((uint8_t)value);
    case APP_SETTING_PALETTE:
        return app_settings_set_palette((uint8_t)value);
    case APP_SETTING_AUTO_ADJUST:
        return app_settings_set_auto_adjust(value != 0);
    case APP_SETTING_DITHER_DIFFUSE:
        return app_settings_set_dither_diffuse(value != 0);
    case APP_SETTING_AUTO_ROTATE:
        return app_settings_set_auto_rotate(value != 0);
    case APP_SETTING_CHARGE_LIMIT:
        return app_settings_set_charge_limit_pct(value);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

// The display's own copy of the setting. Not a cache to be invalidated -- the display task reads
// these under app_display's mutex, and app_display.c:126-129 is the account of why the palette is
// read once per render rather than once per stage.
static bool push_display(app_setting_key_t key, int value)
{
    switch (key) {
    case APP_SETTING_ROTATION:
        app_display_set_rotation((uint8_t)value);
        return true;
    case APP_SETTING_PALETTE:
        app_display_set_palette((uint8_t)value);
        return true;
    case APP_SETTING_AUTO_ADJUST:
        app_display_set_auto_adjust(value != 0);
        return true;
    case APP_SETTING_DITHER_DIFFUSE:
        app_display_set_dither_diffuse(value != 0);
        return true;
    case APP_SETTING_AUTO_ROTATE:
        app_display_set_auto_rotate(value != 0);
        return true;
    default:
        return false;
    }
}

void app_apply_begin(app_apply_txn_t *txn)
{
    if (txn) {
        txn->redraw = false;
    }
}

esp_err_t app_apply_set(app_apply_txn_t *txn, app_setting_key_t key, int value)
{
    if (!txn) {
        return ESP_ERR_INVALID_ARG;
    }
    const app_setting_effect_t *effect = app_setting_effect(key);
    if (!effect) {
        return ESP_ERR_INVALID_ARG;
    }
    // A key that owes nothing beyond a persist has no business in a transaction -- thirty-four of
    // the forty are like that, and their callers use app_settings_set_*() directly. Refused rather
    // than quietly persisted here, so a caller that reached for the wrong tool finds out.
    if (effect->live == APP_SETTING_LIVE_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    // Read and compare BEFORE persisting, which is also what refuses a key that has a row in the
    // table but no case in this file.
    int current = 0;
    if (!read_current(key, &current)) {
        ESP_LOGE(TAG, "key %d has a live half in the table but no case here", (int)key);
        return ESP_ERR_INVALID_ARG;
    }
    if (current == value) {
        return ESP_OK;
    }

    const esp_err_t err = persist(key, value);
    if (err != ESP_OK) {
        // Nothing is applied and no redraw is recorded. See app_apply.h on why this is checked
        // here when h_mode_cfg_set() used to ignore it: a panel showing what NVS does not hold
        // reverts at the next boot with nothing in the console.
        ESP_LOGW(TAG, "key %d not stored (%s); live half skipped", (int)key, esp_err_to_name(err));
        return err;
    }

    // Routed by the table, not by a switch on the key: a row that said CHARGER for a render setting
    // would send it to the charger. That is what keeps app_setting_effect.h load-bearing rather
    // than documentation with a test attached.
    switch (effect->live) {
    case APP_SETTING_LIVE_DISPLAY:
        if (!push_display(key, value)) {
            ESP_LOGE(TAG, "key %d routes to the display but has no case there", (int)key);
        }
        break;
    case APP_SETTING_LIVE_CHARGER:
        // Ticket 71. Writes the charger and prints what it did, so a cap that could not be applied
        // says so instead of looking saved.
        app_charge_apply();
        break;
    case APP_SETTING_LIVE_NONE:
        // Refused above; this case exists so the switch is exhaustive.
        break;
    }

    if (effect->implies_redraw) {
        txn->redraw = true;
    }
    return ESP_OK;
}

esp_err_t app_apply_commit(app_apply_txn_t *txn)
{
    if (!txn || !txn->redraw) {
        return ESP_OK;
    }
    txn->redraw = false;
    return app_display_redraw();
}
