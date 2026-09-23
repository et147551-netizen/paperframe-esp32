#include "app_settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

// board_wifi_unit_id(), so that the default device name and the softAP name carry the same six
// characters. Header-only, and no cycle: board_wifi.h does not include this file.
#include "board_wifi.h"

// For EPD_PALETTE_ID_COUNT and the wire names. ESP-IDF-free and self-contained, so this
// costs nothing at this layer.
#include "epd_canvas.h"  // EPD_CANVAS_ROTATION_MAX -- the rotation's range lives with the mapping
#include "epd_dither.h"

static const char *TAG = "settings";

// The namespace and the key names are the stock firmware's. They are short because NVS
// keys are capped at 15 characters, which is why "wifi_password" is stored as
// "wifi_pass" and "interval_minutes" as "interval". Do not tidy them up: the `nvs`
// partition is shared with the shipping firmware (hal.cpp:290-322).
#define NVS_NAMESPACE "papercolor"

#define KEY_WIFI_SSID "wifi_ssid"
#define KEY_WIFI_PASS "wifi_pass"
#define KEY_ROTATION "rotation"
#define KEY_AUTO_SLIDE "auto_slide"
#define KEY_INTERVAL "interval"
#define KEY_CUR_MODE "cur_mode"
#define KEY_BOOT_SOUND "boot_sound"
#define KEY_DEVICE_NAME "device_name"
#define KEY_LOW_POWER "low_power"

// Ticket 25's keys. Not the shipping firmware's -- it has no SMB mirror -- but they obey
// the same 15-character NVS limit, which is why "smb_enabled" is stored as "smb_on".
#define KEY_SMB_HOST "smb_host"
#define KEY_SMB_SHARE "smb_share"
#define KEY_SMB_PATH "smb_path"
#define KEY_SMB_USER "smb_user"
#define KEY_SMB_PASS "smb_pass"
#define KEY_SMB_DOMAIN "smb_domain"
#define KEY_SMB_ON "smb_on"

// Not one of the shipping firmware's keys -- see the note on app_settings_t.palette.
#define KEY_PALETTE "palette"
// Likewise. 11 characters, inside the 15 NVS allows.
#define KEY_AUTO_ADJUST "auto_adjust"
// Likewise. 14 characters, inside the 15 NVS allows -- one to spare, so do not lengthen it.
#define KEY_DITHER_DIFFUSE "dither_diffuse"
// Likewise. "slideshow_random" is 16 characters, one over what NVS allows, which is why these
// two are abbreviated rather than spelled out.
#define KEY_SLIDE_RANDOM "ss_random"
#define KEY_SLIDE_SEED "ss_seed"
// Ticket 37 Phase 3. 12 characters, inside the 15 NVS allows.
#define KEY_SMB_ON_DEMAND "smb_ondemand"
// Ticket 49. 10 characters, inside the 15 NVS allows.
#define KEY_SMB_RESIZE "smb_resize"
// Ticket 51. 11 characters, inside the 15 NVS allows.
#define KEY_AUTO_ROTATE "auto_rotate"
// Ticket 60. All inside the 15 NVS allows. Slot 0 keeps the single-album build's names, so a
// device that already holds an album keeps it across the reflash that adds slots 1-3.
#define KEY_GPHOTOS_ON "gp_on"
static const char *const KEY_GPHOTOS_ALBUM[APP_SETTINGS_GPHOTOS_SLOTS] = {
    "gp_album", "gp_album1", "gp_album2", "gp_album3"};
static const char *const KEY_GPHOTOS_GOOD[APP_SETTINGS_GPHOTOS_SLOTS] = {
    "gp_album_ok", "gp_album_ok1", "gp_album_ok2", "gp_album_ok3"};

// Tickets 41 and 42. Within the same 15-character NVS limit as everything above, and not
// names the stock firmware shares -- it has neither a clock nor a schedule.
#define KEY_TZ_OFFSET "tz_offset"
#define KEY_ACTIVE_START "active_start"
#define KEY_ACTIVE_END "active_end"

// Ticket 68, same limit and the same "not a name the stock firmware shares" argument.
#define KEY_STANDBY_WHITE "stby_white"
#define KEY_STANDBY_DEEP "stby_deep"
#define KEY_MAINT_DAY "maint_day"

// Ticket 71. Same argument again: a key the stock firmware never wrote and will ignore.
#define KEY_CHARGE_LIMIT "chg_limit"

// The mDNS hostname and the fallback for an empty `device_name`, so it has to satisfy the same
// [a-z0-9-] rule the setter enforces.
//
// **"papercolor" until 2026-09-17, and the NVS NAMESPACE is still that** (`NVS_NAMESPACE` above).
// The project drives two boards now, so the product name is generic; the namespace is deliberately
// not renamed, because renaming it makes every device that has ever been configured lose its
// Wi-Fi credentials, its API token and its share password at once, and the recovery from that is an
// NVS erase and a reflash (ticket 10). Nothing outside the flash chip can see the namespace, so
// there is nothing to gain from moving it.
//
// A device that has been configured already keeps its stored name: this is a default, and defaults
// are only read where NVS has no value.
//
// **AND IT CARRIES THE UNIT'S OWN SIX HEX DIGITS, like the softAP's name** (owner, 2026-09-17):
// `paperframe-a1b2c3`, from `board_wifi_unit_id()`, the same three MAC bytes the SSID is written
// from. A bare product name made every frame answer to one `.local`, so two on a bench collided --
// which is what both of these did as `papercolor` before it, one shadowing the other with no
// symptom but a wrong photograph. Lowercase because the setter normalises to [a-z0-9-].
#define DEFAULT_DEVICE_NAME_PREFIX "paperframe"
#define INTERVAL_MIN 1
#define INTERVAL_MAX 255

// UTC-12 to UTC+14, the real range of civil offsets. Kaliningrad and Kiritimati are the ends.
#define TZ_OFFSET_MIN (-720)
#define TZ_OFFSET_MAX 840
#define TZ_OFFSET_DEFAULT 540  // JST
#define ACTIVE_START_DEFAULT 7
#define ACTIVE_END_DEFAULT 23
#define ACTIVE_HOUR_MAX 23

// Ticket 68. 0 = Sunday .. 6 = Saturday, 7 = never, so MAX is the "never" code and not a day.
#define MAINT_DAY_NEVER 7
#define MAINT_DAY_MAX MAINT_DAY_NEVER
// Monday, arbitrarily. The owner said to default to one day and make it a setting rather than
// spend a question on which; anything in 0..6 would do.
#define MAINT_DAY_DEFAULT 1

// Ticket 71. Three accepted values and nothing between them: 0 = leave the charger alone,
// 80 = cap, 100 = the part's own default. A set of three rather than a range because the
// millivolt mapping is a cell-chemistry fact that app_charge.c owns, not a dial for a user to
// sweep -- and because the charger's step is 32 mV, so most percentages in between would
// round to the same register value and look like a setting that does not stick.
#define CHARGE_LIMIT_OFF 0
#define CHARGE_LIMIT_CAP 80
#define CHARGE_LIMIT_FULL 100
// The owner's choice, 2026-09-20, against 「時々は無給電で使う」.
#define CHARGE_LIMIT_DEFAULT CHARGE_LIMIT_CAP

static bool charge_limit_valid(int pct)
{
    return pct == CHARGE_LIMIT_OFF || pct == CHARGE_LIMIT_CAP || pct == CHARGE_LIMIT_FULL;
}

static app_settings_t s_settings;
// Outside s_settings on purpose -- see the note where they would sit in app_settings_t. Under the
// same lock. The link is a bearer capability to the whole album and is never printed.
//
// IN PSRAM, allocated by app_settings_init(): eight 256-byte links are 2 KB, and .bss is internal
// RAM in this build (CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is not set).
static char (*s_gphotos_album)[APP_SETTINGS_GPHOTOS_ALBUM_SIZE];
static bool s_gphotos_enabled;
static char (*s_gphotos_good)[APP_SETTINGS_GPHOTOS_ALBUM_SIZE];
static SemaphoreHandle_t s_mutex;

static void lock(void)
{
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

static void copy_str(char *dst, const char *src, size_t size)
{
    if (!dst || size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= size) {
        n = size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// The default device name, built at the point of use because the MAC is not a compile-time
// constant. `size` must be at least sizeof("paperframe-") + 6, which APP_SETTINGS_NAME_SIZE is.
static void default_device_name(char *out, size_t size)
{
    char id[7];
    board_wifi_unit_id(id, sizeof(id), false);
    if (id[0] == '\0') {
        // esp_read_mac() does not fail on a booted chip, but a name that silently lost its suffix
        // would collide with the other frame again and read as somebody else's bug.
        copy_str(out, DEFAULT_DEVICE_NAME_PREFIX, size);
        return;
    }
    snprintf(out, size, "%s-%s", DEFAULT_DEVICE_NAME_PREFIX, id);
}

bool app_settings_normalize_mode_id(const char *input, char *output, size_t size)
{
    const char *normalized = "";
    bool changed = false;

    if (input && strcmp(input, APP_SETTINGS_MODE_LOCAL) == 0) {
        normalized = APP_SETTINGS_MODE_LOCAL;
    } else if (input && input[0]) {
        // Unknown mode: "no mode selected". Since ticket 22 that includes the stock
        // firmware's "mode_2", which a device carried over from it clears on first load.
        changed = true;
    }

    copy_str(output, normalized, size);
    return changed;
}

// Lowercase, [a-z0-9-] only, no leading or trailing dash, never empty. Ported from
// hal.cpp:89-144 -- the result is used as an mDNS hostname, where those are the rules.
bool app_settings_normalize_device_name(const char *input, char *output, size_t size)
{
    if (!output || size == 0) {
        return true;
    }

    size_t w = 0;
    bool changed = false;

    if (input) {
        while (*input != '\0' && w + 1 < size) {
            char ch = *input++;
            if (ch >= 'A' && ch <= 'Z') {
                ch = (char)(ch - 'A' + 'a');
                changed = true;
            }

            bool is_alpha = ch >= 'a' && ch <= 'z';
            bool is_digit = ch >= '0' && ch <= '9';
            bool is_dash = ch == '-';
            if (!(is_alpha || is_digit || is_dash)) {
                changed = true;
                continue;
            }
            if (is_dash && w == 0) {
                changed = true;
                continue;
            }
            output[w++] = ch;
        }
        while (w > 0 && output[w - 1] == '-') {
            --w;
            changed = true;
        }
    } else {
        changed = true;
    }

    output[w] = '\0';

    if (w == 0) {
        default_device_name(output, size);
        return true;
    }
    if (!input) {
        return true;
    }
    return changed || strcmp(output, input) != 0;
}

static void apply_defaults(app_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    for (size_t i = 0; i < APP_SETTINGS_GPHOTOS_SLOTS; i++) {
        s_gphotos_album[i][0] = '\0';
        s_gphotos_good[i][0] = '\0';
    }
    s_gphotos_enabled = false;
    s->rotation = 1;
    s->auto_slideshow = false;
    s->interval_minutes = 60;
    s->boot_sound = true;
    s->low_power_mode = false;
    s->smb_enabled = false;
    s->palette = EPD_PALETTE_ID_BOEBER;
    s->auto_adjust = true;
    s->dither_diffuse = true;
    s->slideshow_random = true;
    s->slideshow_seed = 0;  // "not yet minted"; app_slideshow mints one on first use
    s->smb_on_demand = false;
    // ON by owner decision, 2026-09-09, in the same breath as asking for the hardware
    // verification -- so this default is shipping AHEAD of its own measurements, which is
    // the opposite way round from auto_adjust and dither_diffuse (both judged on the glass
    // first). What that means for whoever reads this next: the arm to argue is now OFF, and
    // ticket 49's hardware section is the evidence that has to exist rather than evidence
    // that already did.
    s->smb_resize = true;
    // ON by owner decision, 2026-09-09, chosen with the 1.3 threshold in the same
    // breath. Like smb_resize above this ships ahead of its own hardware evidence, so the
    // arm to argue is OFF; ticket 51 is where that evidence has to appear.
    s->auto_rotate = true;
    // JST. A compile-time default has to be *some* zone and this is the bench's; it is a
    // setting precisely so that being wrong about it is a form field rather than a rebuild.
    s->tz_offset_minutes = TZ_OFFSET_DEFAULT;
    // 07:00-23:00, the default of an earlier Android photo-frame design's schedule.
    // Inert until low_power_mode is on, which is why shipping a real window rather than 0..0
    // costs nothing: with the master switch off app_schedule_active() returns true at every
    // hour regardless of these.
    s->active_start_hour = ACTIVE_START_DEFAULT;
    s->active_end_hour = ACTIVE_END_DEFAULT;
    // ON, because the owner asked for the white standby directly (2026-09-20) rather than asking
    // to be able to turn it on -- so the shipping behaviour is the one they asked for and the switch
    // exists to undo it. Inert until low_power_mode is on, for the same reason the window above is:
    // with the master switch off there is no window edge to park at.
    s->standby_white = true;
    // OFF: a clear every night is +47 % on the refresh count, against the +15 % the two
    // shipping halves cost. app_settings.h has the arithmetic.
    s->standby_deep = false;
    s->maint_day = MAINT_DAY_DEFAULT;
    // Ticket 71. ON by default, unlike ticket 68's standby_deep -- what it costs is capacity on a
    // frame that lives on mains, not refreshes on a panel with a finite count, and the owner
    // asked for the cap rather than for a switch that starts off.
    s->charge_limit_pct = CHARGE_LIMIT_DEFAULT;
    s->current_mode[0] = '\0';
    default_device_name(s->device_name, sizeof(s->device_name));
}

// Overlays NVS onto the defaults already in `s`. A key that is absent, or present with
// a value outside its range, leaves the default in place.
static void load_from_nvs(app_settings_t *s)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no %s namespace in NVS; using defaults", NVS_NAMESPACE);
        return;
    }

    size_t size;
    uint8_t u8;
    uint16_t u16;

    size = sizeof(s->wifi_ssid);
    nvs_get_str(h, KEY_WIFI_SSID, s->wifi_ssid, &size);

    size = sizeof(s->wifi_password);
    nvs_get_str(h, KEY_WIFI_PASS, s->wifi_password, &size);

    // **THE GUARD THAT MAKES A PARTIALLY-WIDENED BUILD LOOK LIKE "IT DOES NOT STICK".** A value
    // this rejects leaves the DEFAULT in place, silently -- so before ticket 69 widened it, a frame
    // could accept `rotation: 2`, answer 200, render it, and come back as 1 after a reboot with
    // nothing in the console. The acceptance test for this key is therefore a POWER CYCLE and not
    // a GET. It was one of SEVEN such guards in five files; ticket 69 §6 lists them.
    //
    // **The stored meaning did not change and no migration is needed.** It went from "native or
    // turned" to "quarter turns from native", under which 0 and 1 keep exactly the meaning they
    // had, so a device configured by an older build reads back unchanged.
    if (nvs_get_u8(h, KEY_ROTATION, &u8) == ESP_OK && u8 <= EPD_CANVAS_ROTATION_MAX) {
        s->rotation = u8;
    }
    if (nvs_get_u8(h, KEY_AUTO_SLIDE, &u8) == ESP_OK) {
        s->auto_slideshow = (u8 != 0);
    }
    if (nvs_get_u8(h, KEY_BOOT_SOUND, &u8) == ESP_OK) {
        s->boot_sound = (u8 != 0);
    }
    if (nvs_get_u8(h, KEY_LOW_POWER, &u8) == ESP_OK) {
        s->low_power_mode = (u8 != 0);
    }
    if (nvs_get_u16(h, KEY_INTERVAL, &u16) == ESP_OK && u16 >= INTERVAL_MIN && u16 <= INTERVAL_MAX) {
        s->interval_minutes = (int)u16;
    }

    size = sizeof(s->current_mode);
    if (nvs_get_str(h, KEY_CUR_MODE, s->current_mode, &size) == ESP_OK) {
        char normalized[APP_SETTINGS_MODE_SIZE];
        app_settings_normalize_mode_id(s->current_mode, normalized, sizeof(normalized));
        copy_str(s->current_mode, normalized, sizeof(s->current_mode));
    }

    size = sizeof(s->device_name);
    if (nvs_get_str(h, KEY_DEVICE_NAME, s->device_name, &size) == ESP_OK) {
        char normalized[APP_SETTINGS_NAME_SIZE];
        app_settings_normalize_device_name(s->device_name, normalized, sizeof(normalized));
        copy_str(s->device_name, normalized, sizeof(s->device_name));
    }

    size = sizeof(s->smb_host);
    nvs_get_str(h, KEY_SMB_HOST, s->smb_host, &size);
    size = sizeof(s->smb_share);
    nvs_get_str(h, KEY_SMB_SHARE, s->smb_share, &size);
    size = sizeof(s->smb_path);
    nvs_get_str(h, KEY_SMB_PATH, s->smb_path, &size);
    size = sizeof(s->smb_user);
    nvs_get_str(h, KEY_SMB_USER, s->smb_user, &size);
    size = sizeof(s->smb_password);
    nvs_get_str(h, KEY_SMB_PASS, s->smb_password, &size);
    size = sizeof(s->smb_domain);
    nvs_get_str(h, KEY_SMB_DOMAIN, s->smb_domain, &size);
    if (nvs_get_u8(h, KEY_SMB_ON, &u8) == ESP_OK) {
        s->smb_enabled = (u8 != 0);
    }
    // Range-checked on the way out of NVS as well as on the way in: a palette written by a
    // future build with more of them would otherwise index off the end of the table.
    if (nvs_get_u8(h, KEY_PALETTE, &u8) == ESP_OK && u8 < EPD_PALETTE_ID_COUNT) {
        s->palette = u8;
    }
    if (nvs_get_u8(h, KEY_AUTO_ADJUST, &u8) == ESP_OK) {
        s->auto_adjust = (u8 != 0);
    }
    if (nvs_get_u8(h, KEY_DITHER_DIFFUSE, &u8) == ESP_OK) {
        s->dither_diffuse = (u8 != 0);
    }
    if (nvs_get_u8(h, KEY_SLIDE_RANDOM, &u8) == ESP_OK) {
        s->slideshow_random = (u8 != 0);
    }
    // No range check, and none is possible: every 32-bit value is a legal seed. A zero read
    // back means the same as a zero default -- mint a new one.
    uint32_t u32;
    if (nvs_get_u32(h, KEY_SLIDE_SEED, &u32) == ESP_OK) {
        s->slideshow_seed = u32;
    }
    if (nvs_get_u8(h, KEY_SMB_ON_DEMAND, &u8) == ESP_OK) {
        s->smb_on_demand = (u8 != 0);
    }
    if (nvs_get_u8(h, KEY_SMB_RESIZE, &u8) == ESP_OK) {
        s->smb_resize = (u8 != 0);
    }
    if (nvs_get_u8(h, KEY_AUTO_ROTATE, &u8) == ESP_OK) {
        s->auto_rotate = (u8 != 0);
    }
    for (size_t i = 0; i < APP_SETTINGS_GPHOTOS_SLOTS; i++) {
        size = APP_SETTINGS_GPHOTOS_ALBUM_SIZE;
        if (nvs_get_str(h, KEY_GPHOTOS_ALBUM[i], s_gphotos_album[i], &size) != ESP_OK) {
            s_gphotos_album[i][0] = '\0';
        }
        size = APP_SETTINGS_GPHOTOS_ALBUM_SIZE;
        if (nvs_get_str(h, KEY_GPHOTOS_GOOD[i], s_gphotos_good[i], &size) != ESP_OK) {
            s_gphotos_good[i][0] = '\0';
        }
    }
    if (nvs_get_u8(h, KEY_GPHOTOS_ON, &u8) == ESP_OK) {
        s_gphotos_enabled = (u8 != 0);
    }
    int16_t i16 = 0;
    if (nvs_get_i16(h, KEY_TZ_OFFSET, &i16) == ESP_OK &&
        i16 >= TZ_OFFSET_MIN && i16 <= TZ_OFFSET_MAX) {
        s->tz_offset_minutes = i16;
    }
    // Range-checked on the way out of NVS as well as in the setter, because an entry written
    // by a future build -- or a corrupt one -- must not be able to hand app_schedule_active()
    // a window it would have to fail open on. Checked INDEPENDENTLY rather than as a pair: an
    // out-of-range start leaves the default start with whatever end was stored, which is a
    // defined window, where dropping both would silently discard a good value.
    if (nvs_get_u8(h, KEY_ACTIVE_START, &u8) == ESP_OK && u8 <= ACTIVE_HOUR_MAX) {
        s->active_start_hour = u8;
    }
    if (nvs_get_u8(h, KEY_ACTIVE_END, &u8) == ESP_OK && u8 <= ACTIVE_HOUR_MAX) {
        s->active_end_hour = u8;
    }
    if (nvs_get_u8(h, KEY_STANDBY_WHITE, &u8) == ESP_OK) {
        s->standby_white = (u8 != 0);
    }
    if (nvs_get_u8(h, KEY_STANDBY_DEEP, &u8) == ESP_OK) {
        s->standby_deep = (u8 != 0);
    }
    // Range-checked here as well as in the setter, and the check is not decoration: a stored value
    // this rejects leaves the DEFAULT in place, silently, which is exactly how a setting comes to
    // look as though it "does not stick" -- it accepts the write, answers 200, behaves, and is back
    // to Monday after a power cycle. So the acceptance test for this key is a reboot, not a GET.
    if (nvs_get_u8(h, KEY_MAINT_DAY, &u8) == ESP_OK && u8 <= MAINT_DAY_MAX) {
        s->maint_day = u8;
    }
    // Same reboot-is-the-acceptance-test warning as maint_day above: a stored value this rejects
    // leaves the default in place silently.
    if (nvs_get_u8(h, KEY_CHARGE_LIMIT, &u8) == ESP_OK && charge_limit_valid((int)u8)) {
        s->charge_limit_pct = u8;
    }

    nvs_close(h);
}

esp_err_t app_settings_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs_flash_init: %s, erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    if (!s_gphotos_album) {
        s_gphotos_album = heap_caps_calloc(APP_SETTINGS_GPHOTOS_SLOTS,
                                           APP_SETTINGS_GPHOTOS_ALBUM_SIZE, MALLOC_CAP_SPIRAM);
        s_gphotos_good = heap_caps_calloc(APP_SETTINGS_GPHOTOS_SLOTS,
                                          APP_SETTINGS_GPHOTOS_ALBUM_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_gphotos_album || !s_gphotos_good) {
            return ESP_ERR_NO_MEM;
        }
    }

    lock();
    apply_defaults(&s_settings);
    load_from_nvs(&s_settings);
    unlock();
    return ESP_OK;
}

// THE PERSISTENCE HALVES OF A SETTING HAVE NO OTHER GUARD, so this is it (ticket 80, 2026-09-22).
// A new setting owes FOUR things and three of them already fail loudly if forgotten: the NVS write
// (`save_locked()` below), the NVS read (`load_from_nvs()` above), its row in
// `src/core/app_setting_effect.c` and its case in `src/app/app_apply.c` -- where the last two are
// ticket 77's, and `test_setting_effect` asserts them exhaustively in both directions.
//
// The first two are not covered by anything. `save_locked()`'s `default:` means a missing case is a
// RUNTIME `ESP_ERR_INVALID_ARG` and one `ESP_LOGE` on a console nobody is attached to, not a build
// failure -- and `-Wswitch` cannot help while that `default:` is there, which it must be, because a
// key from outside the enum has to be refused too. `load_from_nvs()` is a straight-line sequence of
// `nvs_get_*` calls, so nothing could ever have checked it. **A setting saved but never read, or
// never saved, is ticket 69's defect exactly: it persists, reads back correctly over HTTP, and is
// gone after a reboot with nothing in the console.**
//
// An audit on 2026-09-22 found all 40 keys present on both sides -- every one of the 32 `KEY_*`
// macros appears in exactly one `nvs_set_*` and one `nvs_get_*`, and the eight Google album/good
// slots are written by the two indexed cases below and read by the loop in `load_from_nvs()`. This
// assertion is what keeps that true: adding a key stops the build here rather than shipping a
// setting that does not survive a power cut.
_Static_assert(APP_SETTING_COUNT == 40,
               "a new setting needs a case in save_locked() AND a line in load_from_nvs(), plus a "
               "row in src/core/app_setting_effect.c and a case in src/app/app_apply.c; then make "
               "this number match");

// Called with the lock held. One key, one entry, one commit.
static esp_err_t save_locked(app_setting_key_t key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    switch (key) {
    case APP_SETTING_WIFI_SSID:
        err = nvs_set_str(h, KEY_WIFI_SSID, s_settings.wifi_ssid);
        break;
    case APP_SETTING_WIFI_PASSWORD:
        err = nvs_set_str(h, KEY_WIFI_PASS, s_settings.wifi_password);
        break;
    case APP_SETTING_ROTATION:
        err = nvs_set_u8(h, KEY_ROTATION, s_settings.rotation);
        break;
    case APP_SETTING_AUTO_SLIDESHOW:
        err = nvs_set_u8(h, KEY_AUTO_SLIDE, s_settings.auto_slideshow ? 1 : 0);
        break;
    case APP_SETTING_INTERVAL:
        err = nvs_set_u16(h, KEY_INTERVAL, (uint16_t)s_settings.interval_minutes);
        break;
    case APP_SETTING_CURRENT_MODE:
        err = nvs_set_str(h, KEY_CUR_MODE, s_settings.current_mode);
        break;
    case APP_SETTING_BOOT_SOUND:
        err = nvs_set_u8(h, KEY_BOOT_SOUND, s_settings.boot_sound ? 1 : 0);
        break;
    case APP_SETTING_DEVICE_NAME:
        err = nvs_set_str(h, KEY_DEVICE_NAME, s_settings.device_name);
        break;
    case APP_SETTING_LOW_POWER_MODE:
        err = nvs_set_u8(h, KEY_LOW_POWER, s_settings.low_power_mode ? 1 : 0);
        break;
    case APP_SETTING_SMB_HOST:
        err = nvs_set_str(h, KEY_SMB_HOST, s_settings.smb_host);
        break;
    case APP_SETTING_SMB_SHARE:
        err = nvs_set_str(h, KEY_SMB_SHARE, s_settings.smb_share);
        break;
    case APP_SETTING_SMB_PATH:
        err = nvs_set_str(h, KEY_SMB_PATH, s_settings.smb_path);
        break;
    case APP_SETTING_SMB_USER:
        err = nvs_set_str(h, KEY_SMB_USER, s_settings.smb_user);
        break;
    case APP_SETTING_SMB_PASSWORD:
        err = nvs_set_str(h, KEY_SMB_PASS, s_settings.smb_password);
        break;
    case APP_SETTING_SMB_DOMAIN:
        err = nvs_set_str(h, KEY_SMB_DOMAIN, s_settings.smb_domain);
        break;
    case APP_SETTING_SMB_ENABLED:
        err = nvs_set_u8(h, KEY_SMB_ON, s_settings.smb_enabled ? 1 : 0);
        break;
    case APP_SETTING_PALETTE:
        err = nvs_set_u8(h, KEY_PALETTE, s_settings.palette);
        break;
    case APP_SETTING_AUTO_ADJUST:
        err = nvs_set_u8(h, KEY_AUTO_ADJUST, s_settings.auto_adjust ? 1 : 0);
        break;
    case APP_SETTING_DITHER_DIFFUSE:
        err = nvs_set_u8(h, KEY_DITHER_DIFFUSE, s_settings.dither_diffuse ? 1 : 0);
        break;
    case APP_SETTING_SLIDESHOW_RANDOM:
        err = nvs_set_u8(h, KEY_SLIDE_RANDOM, s_settings.slideshow_random ? 1 : 0);
        break;
    case APP_SETTING_SLIDESHOW_SEED:
        err = nvs_set_u32(h, KEY_SLIDE_SEED, s_settings.slideshow_seed);
        break;
    case APP_SETTING_SMB_ON_DEMAND:
        err = nvs_set_u8(h, KEY_SMB_ON_DEMAND, s_settings.smb_on_demand ? 1 : 0);
        break;
    case APP_SETTING_SMB_RESIZE:
        err = nvs_set_u8(h, KEY_SMB_RESIZE, s_settings.smb_resize ? 1 : 0);
        break;
    case APP_SETTING_AUTO_ROTATE:
        err = nvs_set_u8(h, KEY_AUTO_ROTATE, s_settings.auto_rotate ? 1 : 0);
        break;
    case APP_SETTING_GPHOTOS_ALBUM:
        err = nvs_set_str(h, KEY_GPHOTOS_ALBUM[0], s_gphotos_album[0]);
        break;
    case APP_SETTING_GPHOTOS_ALBUM1:
    case APP_SETTING_GPHOTOS_ALBUM2:
    case APP_SETTING_GPHOTOS_ALBUM3: {
        const size_t slot = 1 + (size_t)(key - APP_SETTING_GPHOTOS_ALBUM1);
        err = nvs_set_str(h, KEY_GPHOTOS_ALBUM[slot], s_gphotos_album[slot]);
        break;
    }
    case APP_SETTING_GPHOTOS_ENABLED:
        err = nvs_set_u8(h, KEY_GPHOTOS_ON, s_gphotos_enabled ? 1 : 0);
        break;
    case APP_SETTING_GPHOTOS_GOOD:
        err = nvs_set_str(h, KEY_GPHOTOS_GOOD[0], s_gphotos_good[0]);
        break;
    case APP_SETTING_GPHOTOS_GOOD1:
    case APP_SETTING_GPHOTOS_GOOD2:
    case APP_SETTING_GPHOTOS_GOOD3: {
        const size_t slot = 1 + (size_t)(key - APP_SETTING_GPHOTOS_GOOD1);
        err = nvs_set_str(h, KEY_GPHOTOS_GOOD[slot], s_gphotos_good[slot]);
        break;
    }
    case APP_SETTING_TZ_OFFSET:
        err = nvs_set_i16(h, KEY_TZ_OFFSET, s_settings.tz_offset_minutes);
        break;
    case APP_SETTING_ACTIVE_START:
        err = nvs_set_u8(h, KEY_ACTIVE_START, s_settings.active_start_hour);
        break;
    case APP_SETTING_ACTIVE_END:
        err = nvs_set_u8(h, KEY_ACTIVE_END, s_settings.active_end_hour);
        break;
    case APP_SETTING_STANDBY_WHITE:
        err = nvs_set_u8(h, KEY_STANDBY_WHITE, s_settings.standby_white ? 1 : 0);
        break;
    case APP_SETTING_STANDBY_DEEP:
        err = nvs_set_u8(h, KEY_STANDBY_DEEP, s_settings.standby_deep ? 1 : 0);
        break;
    case APP_SETTING_MAINT_DAY:
        err = nvs_set_u8(h, KEY_MAINT_DAY, s_settings.maint_day);
        break;
    case APP_SETTING_CHARGE_LIMIT:
        err = nvs_set_u8(h, KEY_CHARGE_LIMIT, s_settings.charge_limit_pct);
        break;
    default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }

    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save key %d: %s", (int)key, esp_err_to_name(err));
    }
    return err;
}

esp_err_t app_settings_save(app_setting_key_t key)
{
    lock();
    esp_err_t err = save_locked(key);
    unlock();
    return err;
}

void app_settings_get(app_settings_t *out)
{
    if (!out) {
        return;
    }
    lock();
    *out = s_settings;
    unlock();
}

uint8_t app_settings_rotation(void)
{
    lock();
    uint8_t v = s_settings.rotation;
    unlock();
    return v;
}

bool app_settings_auto_slideshow(void)
{
    lock();
    bool v = s_settings.auto_slideshow;
    unlock();
    return v;
}

int app_settings_interval_minutes(void)
{
    lock();
    int v = s_settings.interval_minutes;
    unlock();
    return v;
}

bool app_settings_low_power_mode(void)
{
    lock();
    bool v = s_settings.low_power_mode;
    unlock();
    return v;
}

bool app_settings_boot_sound(void)
{
    lock();
    bool v = s_settings.boot_sound;
    unlock();
    return v;
}

uint8_t app_settings_palette(void)
{
    lock();
    uint8_t v = s_settings.palette;
    unlock();
    return v;
}

bool app_settings_auto_adjust(void)
{
    lock();
    bool v = s_settings.auto_adjust;
    unlock();
    return v;
}

bool app_settings_dither_diffuse(void)
{
    lock();
    bool v = s_settings.dither_diffuse;
    unlock();
    return v;
}

bool app_settings_slideshow_random(void)
{
    lock();
    bool v = s_settings.slideshow_random;
    unlock();
    return v;
}

uint32_t app_settings_slideshow_seed(void)
{
    lock();
    uint32_t v = s_settings.slideshow_seed;
    unlock();
    return v;
}

bool app_settings_smb_on_demand(void)
{
    lock();
    bool v = s_settings.smb_on_demand;
    unlock();
    return v;
}

bool app_settings_smb_resize(void)
{
    lock();
    bool v = s_settings.smb_resize;
    unlock();
    return v;
}

// A getter rather than app_settings_get(): the photo selector asks this from the main task on
// every advance, and app_settings_t is a stack object there (see the note in app_settings.h).
bool app_settings_smb_enabled(void)
{
    lock();
    bool v = s_settings.smb_enabled;
    unlock();
    return v;
}

bool app_settings_auto_rotate(void)
{
    lock();
    bool v = s_settings.auto_rotate;
    unlock();
    return v;
}

int16_t app_settings_tz_offset_minutes(void)
{
    lock();
    int16_t v = s_settings.tz_offset_minutes;
    unlock();
    return v;
}

void app_settings_active_hours(uint8_t *start_hour, uint8_t *end_hour)
{
    lock();
    if (start_hour) {
        *start_hour = s_settings.active_start_hour;
    }
    if (end_hour) {
        *end_hour = s_settings.active_end_hour;
    }
    unlock();
}

bool app_settings_standby_white(void)
{
    lock();
    const bool on = s_settings.standby_white;
    unlock();
    return on;
}

bool app_settings_standby_deep(void)
{
    lock();
    const bool on = s_settings.standby_deep;
    unlock();
    return on;
}

uint8_t app_settings_maint_day(void)
{
    lock();
    const uint8_t day = s_settings.maint_day;
    unlock();
    return day;
}

uint8_t app_settings_charge_limit_pct(void)
{
    lock();
    const uint8_t pct = s_settings.charge_limit_pct;
    unlock();
    return pct;
}

static void get_str(char *out, size_t size, const char *src)
{
    lock();
    copy_str(out, src, size);
    unlock();
}

void app_settings_device_name(char *out, size_t size)
{
    get_str(out, size, s_settings.device_name);
}

void app_settings_current_mode(char *out, size_t size)
{
    get_str(out, size, s_settings.current_mode);
}

void app_settings_wifi_ssid(char *out, size_t size)
{
    get_str(out, size, s_settings.wifi_ssid);
}

void app_settings_gphotos_album(size_t slot, char *out, size_t size)
{
    get_str(out, size, slot < APP_SETTINGS_GPHOTOS_SLOTS ? s_gphotos_album[slot] : "");
}

void app_settings_gphotos_good(size_t slot, char *out, size_t size)
{
    get_str(out, size, slot < APP_SETTINGS_GPHOTOS_SLOTS ? s_gphotos_good[slot] : "");
}

bool app_settings_gphotos_switch(void)
{
    lock();
    bool v = s_gphotos_enabled;
    unlock();
    return v;
}

bool app_settings_gphotos_enabled(void)
{
    lock();
    bool v = false;
    for (size_t i = 0; s_gphotos_enabled && i < APP_SETTINGS_GPHOTOS_SLOTS && !v; i++) {
        v = s_gphotos_album[i][0] != '\0';
    }
    unlock();
    return v;
}

void app_settings_wifi_password(char *out, size_t size)
{
    get_str(out, size, s_settings.wifi_password);
}

esp_err_t app_settings_set_wifi(const char *ssid, const char *password)
{
    // An SSID longer than the field is a client bug, not something to silently truncate
    // into a network that does not exist.
    if (!ssid || strlen(ssid) >= APP_SETTINGS_SSID_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password && strlen(password) >= APP_SETTINGS_PASS_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    copy_str(s_settings.wifi_ssid, ssid, sizeof(s_settings.wifi_ssid));
    copy_str(s_settings.wifi_password, password ? password : "", sizeof(s_settings.wifi_password));
    esp_err_t err = save_locked(APP_SETTING_WIFI_SSID);
    if (err == ESP_OK) {
        err = save_locked(APP_SETTING_WIFI_PASSWORD);
    }
    unlock();
    return err;
}

esp_err_t app_settings_set_rotation(uint8_t rotation)
{
    if (rotation > EPD_CANVAS_ROTATION_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    s_settings.rotation = rotation;
    esp_err_t err = save_locked(APP_SETTING_ROTATION);
    unlock();
    return err;
}

esp_err_t app_settings_set_auto_slideshow(bool on)
{
    lock();
    s_settings.auto_slideshow = on;
    esp_err_t err = save_locked(APP_SETTING_AUTO_SLIDESHOW);
    unlock();
    return err;
}

esp_err_t app_settings_set_interval_minutes(int minutes)
{
    if (minutes < INTERVAL_MIN || minutes > INTERVAL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    s_settings.interval_minutes = minutes;
    esp_err_t err = save_locked(APP_SETTING_INTERVAL);
    unlock();
    return err;
}

esp_err_t app_settings_set_current_mode(const char *mode_id)
{
    char normalized[APP_SETTINGS_MODE_SIZE];
    if (app_settings_normalize_mode_id(mode_id, normalized, sizeof(normalized))) {
        // Something unrecognised came in. The reference stores "" and carries on; we
        // refuse instead, so the caller can answer 400 rather than quietly losing the
        // mode the user asked for.
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    copy_str(s_settings.current_mode, normalized, sizeof(s_settings.current_mode));
    esp_err_t err = save_locked(APP_SETTING_CURRENT_MODE);
    unlock();
    return err;
}

esp_err_t app_settings_set_boot_sound(bool on)
{
    lock();
    s_settings.boot_sound = on;
    esp_err_t err = save_locked(APP_SETTING_BOOT_SOUND);
    unlock();
    return err;
}

esp_err_t app_settings_set_device_name(const char *name)
{
    if (!name || strlen(name) >= APP_SETTINGS_NAME_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    char normalized[APP_SETTINGS_NAME_SIZE];
    app_settings_normalize_device_name(name, normalized, sizeof(normalized));

    lock();
    copy_str(s_settings.device_name, normalized, sizeof(s_settings.device_name));
    esp_err_t err = save_locked(APP_SETTING_DEVICE_NAME);
    unlock();
    return err;
}

esp_err_t app_settings_set_low_power_mode(bool on)
{
    lock();
    s_settings.low_power_mode = on;
    esp_err_t err = save_locked(APP_SETTING_LOW_POWER_MODE);
    unlock();
    return err;
}

esp_err_t app_settings_set_palette(uint8_t palette)
{
    if (palette >= EPD_PALETTE_ID_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    s_settings.palette = palette;
    esp_err_t err = save_locked(APP_SETTING_PALETTE);
    unlock();
    return err;
}

esp_err_t app_settings_set_auto_adjust(bool on)
{
    lock();
    s_settings.auto_adjust = on;
    esp_err_t err = save_locked(APP_SETTING_AUTO_ADJUST);
    unlock();
    return err;
}

esp_err_t app_settings_set_dither_diffuse(bool on)
{
    lock();
    s_settings.dither_diffuse = on;
    esp_err_t err = save_locked(APP_SETTING_DITHER_DIFFUSE);
    unlock();
    return err;
}

esp_err_t app_settings_set_slideshow_random(bool on)
{
    lock();
    s_settings.slideshow_random = on;
    esp_err_t err = save_locked(APP_SETTING_SLIDESHOW_RANDOM);
    unlock();
    return err;
}

esp_err_t app_settings_set_slideshow_seed(uint32_t seed)
{
    lock();
    s_settings.slideshow_seed = seed;
    esp_err_t err = save_locked(APP_SETTING_SLIDESHOW_SEED);
    unlock();
    return err;
}

esp_err_t app_settings_set_smb_on_demand(bool on)
{
    lock();
    s_settings.smb_on_demand = on;
    esp_err_t err = save_locked(APP_SETTING_SMB_ON_DEMAND);
    unlock();
    return err;
}

esp_err_t app_settings_set_smb_resize(bool on)
{
    lock();
    s_settings.smb_resize = on;
    esp_err_t err = save_locked(APP_SETTING_SMB_RESIZE);
    unlock();
    return err;
}

esp_err_t app_settings_set_auto_rotate(bool on)
{
    lock();
    s_settings.auto_rotate = on;
    esp_err_t err = save_locked(APP_SETTING_AUTO_ROTATE);
    unlock();
    return err;
}

static app_setting_key_t gphotos_key(app_setting_key_t first, app_setting_key_t second, size_t slot)
{
    return slot == 0 ? first : (app_setting_key_t)(second + (slot - 1));
}

esp_err_t app_settings_set_gphotos_good(size_t slot, const char *album)
{
    if (slot >= APP_SETTINGS_GPHOTOS_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    copy_str(s_gphotos_good[slot], album ? album : "", APP_SETTINGS_GPHOTOS_ALBUM_SIZE);
    esp_err_t err =
        save_locked(gphotos_key(APP_SETTING_GPHOTOS_GOOD, APP_SETTING_GPHOTOS_GOOD1, slot));
    unlock();
    return err;
}

esp_err_t app_settings_set_gphotos_album(size_t slot, const char *album)
{
    if (slot >= APP_SETTINGS_GPHOTOS_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    copy_str(s_gphotos_album[slot], album ? album : "", APP_SETTINGS_GPHOTOS_ALBUM_SIZE);
    esp_err_t err =
        save_locked(gphotos_key(APP_SETTING_GPHOTOS_ALBUM, APP_SETTING_GPHOTOS_ALBUM1, slot));
    unlock();
    return err;
}

esp_err_t app_settings_set_gphotos_enabled(bool enabled)
{
    lock();
    s_gphotos_enabled = enabled;
    esp_err_t err = save_locked(APP_SETTING_GPHOTOS_ENABLED);
    unlock();
    return err;
}

esp_err_t app_settings_set_tz_offset_minutes(int minutes)
{
    if (minutes < TZ_OFFSET_MIN || minutes > TZ_OFFSET_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    s_settings.tz_offset_minutes = (int16_t)minutes;
    esp_err_t err = save_locked(APP_SETTING_TZ_OFFSET);
    unlock();
    return err;
}

// Two NVS writes under one lock. The pair is validated before either lands, so a rejected
// request changes nothing -- a start that saved and an end that did not would leave a window
// the client never asked for, which for a schedule is worse than a 400.
esp_err_t app_settings_set_standby_white(bool on)
{
    lock();
    s_settings.standby_white = on;
    const esp_err_t err = save_locked(APP_SETTING_STANDBY_WHITE);
    unlock();
    return err;
}

esp_err_t app_settings_set_standby_deep(bool on)
{
    lock();
    s_settings.standby_deep = on;
    const esp_err_t err = save_locked(APP_SETTING_STANDBY_DEEP);
    unlock();
    return err;
}

esp_err_t app_settings_set_maint_day(int day)
{
    if (day < 0 || day > MAINT_DAY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    s_settings.maint_day = (uint8_t)day;
    const esp_err_t err = save_locked(APP_SETTING_MAINT_DAY);
    unlock();
    return err;
}

esp_err_t app_settings_set_charge_limit_pct(int pct)
{
    if (!charge_limit_valid(pct)) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    s_settings.charge_limit_pct = (uint8_t)pct;
    const esp_err_t err = save_locked(APP_SETTING_CHARGE_LIMIT);
    unlock();
    return err;
}

esp_err_t app_settings_set_active_hours(int start_hour, int end_hour)
{
    if (start_hour < 0 || start_hour > ACTIVE_HOUR_MAX ||
        end_hour < 0 || end_hour > ACTIVE_HOUR_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    s_settings.active_start_hour = (uint8_t)start_hour;
    s_settings.active_end_hour = (uint8_t)end_hour;
    esp_err_t err = save_locked(APP_SETTING_ACTIVE_START);
    const esp_err_t err2 = save_locked(APP_SETTING_ACTIVE_END);
    if (err == ESP_OK) {
        err = err2;
    }
    unlock();
    return err;
}

esp_err_t app_settings_set_smb(const char *host, const char *share, const char *path,
                               const char *user, const char *password,
                               const char *domain, bool enabled)
{
    // Reject rather than truncate, as app_settings_set_wifi does: a truncated host is a
    // name that does not resolve, and the failure surfaces minutes later in a sync log
    // instead of immediately in the 400 the client deserved.
    if (host && strlen(host) >= APP_SETTINGS_SMB_HOST_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (share && strlen(share) >= APP_SETTINGS_SMB_SHARE_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (path && strlen(path) >= APP_SETTINGS_SMB_PATH_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (user && strlen(user) >= APP_SETTINGS_SMB_USER_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password && strlen(password) >= APP_SETTINGS_SMB_PASS_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (domain && strlen(domain) >= APP_SETTINGS_SMB_USER_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    if (host) {
        copy_str(s_settings.smb_host, host, sizeof(s_settings.smb_host));
    }
    if (share) {
        copy_str(s_settings.smb_share, share, sizeof(s_settings.smb_share));
    }
    if (path) {
        copy_str(s_settings.smb_path, path, sizeof(s_settings.smb_path));
    }
    if (user) {
        copy_str(s_settings.smb_user, user, sizeof(s_settings.smb_user));
    }
    // NULL means "leave it alone" -- see the header. An empty string is a deliberate
    // clear, and the two are different requests.
    if (password) {
        copy_str(s_settings.smb_password, password, sizeof(s_settings.smb_password));
    }
    if (domain) {
        copy_str(s_settings.smb_domain, domain, sizeof(s_settings.smb_domain));
    }
    s_settings.smb_enabled = enabled;

    esp_err_t err = ESP_OK;
    for (app_setting_key_t k = APP_SETTING_SMB_HOST; k <= APP_SETTING_SMB_ENABLED; k++) {
        const esp_err_t e = save_locked(k);
        if (e != ESP_OK && err == ESP_OK) {
            err = e;
        }
    }
    unlock();
    return err;
}

esp_err_t app_settings_reset_defaults(void)
{
    lock();
    apply_defaults(&s_settings);
    esp_err_t err = ESP_OK;
    for (app_setting_key_t k = 0; k < APP_SETTING_COUNT; k++) {
        esp_err_t e = save_locked(k);
        if (e != ESP_OK && err == ESP_OK) {
            err = e;
        }
    }
    unlock();
    return err;
}

void app_settings_dump(void)
{
    app_settings_t s;
    app_settings_get(&s);
    printf("# settings wifi_ssid=\"%s\" wifi_password_len=%u rotation=%u auto_slideshow=%d\n",
           s.wifi_ssid, (unsigned)strlen(s.wifi_password), s.rotation, s.auto_slideshow ? 1 : 0);
    printf("# settings interval_minutes=%d current_mode=\"%s\" boot_sound=%d device_name=\"%s\" "
           "low_power_mode=%d\n",
           s.interval_minutes, s.current_mode, s.boot_sound ? 1 : 0, s.device_name,
           s.low_power_mode ? 1 : 0);
    printf("# settings smb_on=%d smb_host=\"%s\" smb_share=\"%s\" smb_path=\"%s\" "
           "smb_user=\"%s\" smb_domain=\"%s\" smb_password_len=%u\n",
           s.smb_enabled ? 1 : 0, s.smb_host, s.smb_share, s.smb_path, s.smb_user,
           s.smb_domain, (unsigned)strlen(s.smb_password));
    printf("# settings palette=%u (%s) auto_adjust=%d dither_diffuse=%d auto_rotate=%d\n",
           s.palette, epd_palette_id_name((epd_palette_id_t)s.palette), s.auto_adjust ? 1 : 0,
           s.dither_diffuse ? 1 : 0, s.auto_rotate ? 1 : 0);
    printf("# settings slideshow_random=%d slideshow_seed=%lu smb_on_demand=%d smb_resize=%d\n",
           s.slideshow_random ? 1 : 0, (unsigned long)s.slideshow_seed,
           s.smb_on_demand ? 1 : 0, s.smb_resize ? 1 : 0);
    // The album links are capabilities, so only their lengths are printed.
    lock();
    printf("# settings gphotos_enabled=%d gphotos_album_len=%u,%u,%u,%u\n",
           s_gphotos_enabled ? 1 : 0, (unsigned)strlen(s_gphotos_album[0]),
           (unsigned)strlen(s_gphotos_album[1]), (unsigned)strlen(s_gphotos_album[2]),
           (unsigned)strlen(s_gphotos_album[3]));
    unlock();
    printf("# settings tz_offset_minutes=%d active_hours=%u..%u schedule_on=%d\n",
           (int)s.tz_offset_minutes, (unsigned)s.active_start_hour,
           (unsigned)s.active_end_hour, s.low_power_mode ? 1 : 0);
    // Ticket 68. maint_day is printed as the raw code, 7 included: "never" is a value of this
    // setting and a line that hid it would leave a frame that runs no course looking identical to
    // one whose stored day the loader rejected.
    printf("# settings standby_white=%d standby_deep=%d maint_day=%u\n",
           s.standby_white ? 1 : 0, s.standby_deep ? 1 : 0, (unsigned)s.maint_day);
    // Ticket 71. Printed on BOTH boards even though only one can act on it, because a setting that
    // is stored and inert is exactly the state this project has shipped twice by accident -- the
    // line pairs with app_charge.c's own "not supported on this board" line, and the two together
    // say whether the value is merely persisted or actually applied.
    printf("# settings charge_limit_pct=%u\n", (unsigned)s.charge_limit_pct);
}
