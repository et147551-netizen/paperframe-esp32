// The name of every persisted setting.
//
// One key per NVS entry, because ticket 12 requires one key, one entry, one commit: "a full-struct
// write on every change wears the flash and turns one bad field into nine". save_locked() in
// app_settings.c is what writes one, and app_settings_save() is the public way to ask for it.
//
// **It lives in src/core/ rather than in app_settings.h, which is where it was until 2026-09-21,
// for one reason: it is a name and nothing else.** app_setting_effect.h needs to be keyed by these
// names and is host-tested, so it cannot include app_settings.h -- that header names esp_err_t and
// src/core/ is ESP-IDF-free by construction (docs/board-and-storage.md has the layer map).
// The alternative was a second enum of the same settings under different names, which is the exact
// duplication app_setting_effect.h exists to remove, and the compiler found the collision on the
// first build. app_settings.h includes this file, so every existing user sees the enum unchanged.
//
// Two orderings in here are load-bearing and must not be tidied:
//   * the six SMB keys are CONTIGUOUS -- app_settings.c:1239 iterates the range;
//   * the GPHOTOS album and good-link slots are each contiguous -- save_locked() indexes them.

#ifndef APP_SETTING_KEY_H
#define APP_SETTING_KEY_H

typedef enum {
    APP_SETTING_WIFI_SSID = 0,
    APP_SETTING_WIFI_PASSWORD,
    APP_SETTING_ROTATION,
    APP_SETTING_AUTO_SLIDESHOW,
    APP_SETTING_INTERVAL,
    APP_SETTING_CURRENT_MODE,
    APP_SETTING_BOOT_SOUND,
    APP_SETTING_DEVICE_NAME,
    APP_SETTING_LOW_POWER_MODE,
    APP_SETTING_SMB_HOST,
    APP_SETTING_SMB_SHARE,
    APP_SETTING_SMB_PATH,
    APP_SETTING_SMB_USER,
    APP_SETTING_SMB_PASSWORD,
    APP_SETTING_SMB_DOMAIN,
    APP_SETTING_SMB_ENABLED,
    APP_SETTING_PALETTE,
    APP_SETTING_AUTO_ADJUST,
    APP_SETTING_DITHER_DIFFUSE,
    APP_SETTING_SLIDESHOW_RANDOM,
    APP_SETTING_SLIDESHOW_SEED,
    APP_SETTING_SMB_ON_DEMAND,
    APP_SETTING_SMB_RESIZE,
    APP_SETTING_AUTO_ROTATE,
    APP_SETTING_TZ_OFFSET,
    APP_SETTING_ACTIVE_START,
    APP_SETTING_ACTIVE_END,
    APP_SETTING_GPHOTOS_ALBUM,
    APP_SETTING_GPHOTOS_ENABLED,
    APP_SETTING_GPHOTOS_GOOD,
    // Slots 1-3 of the album and its last good link. Each run contiguous: save_locked() indexes it.
    APP_SETTING_GPHOTOS_ALBUM1,
    APP_SETTING_GPHOTOS_ALBUM2,
    APP_SETTING_GPHOTOS_ALBUM3,
    APP_SETTING_GPHOTOS_GOOD1,
    APP_SETTING_GPHOTOS_GOOD2,
    APP_SETTING_GPHOTOS_GOOD3,
    // Ticket 68.
    APP_SETTING_STANDBY_WHITE,
    APP_SETTING_STANDBY_DEEP,
    APP_SETTING_MAINT_DAY,
    // Ticket 71.
    APP_SETTING_CHARGE_LIMIT,
    // Ticket 61.
    APP_SETTING_OTA_AUTO,
    APP_SETTING_COUNT,
} app_setting_key_t;

#endif  // APP_SETTING_KEY_H
