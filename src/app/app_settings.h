// The nine persisted settings of FR-8, in NVS.
//
// Ticket .scratch/digital-frame/issues/12. The key names, types and defaults are the
// shipping firmware's, verbatim (refs/M5PaperColor-UserDemo/main/hal/hal.cpp:270-460),
// because the `nvs` partition is shared with it: a device that was configured by the
// stock firmware and then reflashed with this one must read its own settings back, not
// fall back to defaults.
//
// Three things this module is careful about, each of which the reference earns:
//
//   * Saving is PER KEY. app_settings_save(key) writes one entry. A whole-struct write
//     on every change wears the flash and turns one bad field into nine.
//   * Every accessor takes a mutex. The HTTP server task and the slideshow task both
//     touch these; the slideshow reads rotation and interval every loop.
//   * Validation happens on the way IN, at the setter, not at the point of use --
//     device_name and current_mode arrive from an HTTP body (FR-8.3).
//
// `current_mode` and its `cur_mode` NVS key stay for storage compatibility: a device the
// stock firmware left in Ezdata mode must boot rather than fail to parse its own NVS.
// Since ticket 22, "mode_2" is no longer in the accepted vocabulary, so such a device
// normalises it to "" on load and rewrites the key -- the residue clears itself.

#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define APP_SETTINGS_SSID_SIZE 64
#define APP_SETTINGS_PASS_SIZE 64
#define APP_SETTINGS_MODE_SIZE 16
#define APP_SETTINGS_NAME_SIZE 64

// The SMB mirror's fields (ticket 25). Sized to match board_smb.h's BOARD_SMB_*_SIZE
// exactly, so a value that survives being stored here cannot be truncated on the way
// into board_smb_config_t. Not #included from there: app_settings.h is included by
// app_server.c, and board_smb.h drags in the classifier and the module's own contract
// for no reason at that layer.
#define APP_SETTINGS_SMB_HOST_SIZE 64
#define APP_SETTINGS_SMB_SHARE_SIZE 64
#define APP_SETTINGS_SMB_PATH_SIZE 128
#define APP_SETTINGS_SMB_USER_SIZE 64
#define APP_SETTINGS_SMB_PASS_SIZE 64

// Ticket 60, FR-10.1: a Google Photos shared-album link. A resolved share URL with its key is
// ~150 characters and a photos.app.goo.gl short link ~40.
#define APP_SETTINGS_GPHOTOS_ALBUM_SIZE 256
// Albums shown at once (the owner's change of 2026-09-13). Four is the UI's provisional limit.
#define APP_SETTINGS_GPHOTOS_SLOTS 4

#define APP_SETTINGS_MODE_LOCAL "mode_1"

// FR-8's table. Copied out under the lock by app_settings_get(); never a live pointer.
typedef struct {
    char wifi_ssid[APP_SETTINGS_SSID_SIZE];
    char wifi_password[APP_SETTINGS_PASS_SIZE];
    // Quarter turns from the panel's native orientation, 0..3 since ticket 69 (the owner's
    // request of 2026-09-20; it was 0 or 1 before, and FR-5.2 still says so -- the departure is
    // recorded in docs/requirements/digital-frame.md). The stored meaning of 0 and 1 is unchanged,
    // so no NVS migration exists or is needed. `epd_canvas.h` owns the range.
    uint8_t rotation;
    bool auto_slideshow;
    int interval_minutes;    // 1..255, NFR-6 puts the floor at 1
    char current_mode[APP_SETTINGS_MODE_SIZE];  // "" or "mode_1"
    bool boot_sound;
    char device_name[APP_SETTINGS_NAME_SIZE];
    bool low_power_mode;

    // The SMB mirror (ticket 25). There is deliberately no port: libsmb2 3.0.1 exposes
    // no setter for one, so a port field would be a setting that silently does nothing.
    char smb_host[APP_SETTINGS_SMB_HOST_SIZE];
    char smb_share[APP_SETTINGS_SMB_SHARE_SIZE];
    char smb_path[APP_SETTINGS_SMB_PATH_SIZE]; // directory inside the share, "" for root
    char smb_user[APP_SETTINGS_SMB_USER_SIZE];
    char smb_password[APP_SETTINGS_SMB_PASS_SIZE];
    char smb_domain[APP_SETTINGS_SMB_USER_SIZE];
    bool smb_enabled;

    // NOT HERE: the Google Photos album link and its switch (ticket 60). This struct is copied
    // whole onto stacks -- app_main keeps one for its whole life and app_smb_sync's config_load()
    // takes another on the same task every 10 s -- and putting the 256-byte link in it took main's
    // spare stack from 648 to 224 bytes (measured 2026-09-13). They live beside s_settings in
    // app_settings.c, behind app_settings_gphotos_album() and app_settings_gphotos_enabled().

    // Which palette photographs are quantised against (epd_dither.h). **Not one of FR-8's
    // nine** -- the shipping firmware has no such setting, so this is an addition and its
    // `palette` NVS key is one the stock firmware will ignore rather than one it shares.
    // A setting rather than a constant because no palette suits every photograph: the
    // uncalibrated one has the cleanest neutrals and the deepest shadow crush, and which
    // matters depends on the picture.
    uint8_t palette;

    // Whether to classify each photograph and choose its tone, range and dithering settings
    // from the class -- epdoptimize's own auto flow (src/core/epd_classify.h, src/core/epd_auto.h).
    // **Also not one of FR-8's nine**, same standing as `palette`.
    //
    // Default off, deliberately: auto overrides FR-3.3's filename rule, under which an
    // `imageN...` upload renders nearest and everything else renders dithered
    // (app_display.c). Overriding a requirement that is cited to the shipping firmware has to
    // be the user's choice rather than this build's.
    bool auto_adjust;

    // Whether the quantiser is epdoptimize's Floyd-Steinberg error diffusion
    // (src/core/epd_diffuse.h) instead of M5GFX's row-wise pair search. **Also not one of FR-8's
    // nine**, same standing as `palette` and `auto_adjust`.
    //
    // **Independent of `auto_adjust` on purpose, and it is not because they are unrelated.**
    // Both move the render towards what <https://paperlesspaper.github.io/epdoptimize> does by
    // default, and whether either *looks* better is still unanswered on the glass
    // (docs/handover-auto-flow.md). Folded into one setting there are two states to
    // compare and a difference cannot be attributed; separate there are four, and it can.
    //
    // Default off for the same reason `auto_adjust` is: it replaces the colour reduction every
    // photograph goes through, and the shipping default should not change on a measurement
    // alone. It costs less than the path it replaces -- 602.7 + 217.6 against 1076.3 ms,
    // docs/measurements.md -- so it is not off for speed.
    bool dither_diffuse;

    // Whether the slideshow walks a shuffled permutation instead of filename order
    // (app_slideshow.c). **Also not one of FR-8's nine**, same standing as `palette`,
    // `auto_adjust` and `dither_diffuse`: FR-5.4's advance is "the next image" and the
    // shipping firmware has no such setting.
    //
    // Default off for the FR reason rather than a measurement one -- filename order is what
    // the requirement says, so departing from it is the user's choice.
    bool slideshow_random;

    // NOT A SETTING. The shuffle epoch's PRNG seed, persisted so that a power cut resumes the
    // pass the frame was in the middle of rather than restarting it in a new order. It is here
    // because NVS is the only durable store this application has that is bigger than the
    // RX8130's four bytes, and because the write rate makes that affordable: the seed changes
    // once per PASS, which at 230 photographs and a five-minute interval is about one flash
    // write a day. A per-advance write would be ~288 and would not go here.
    //
    // Deliberately absent from the HTTP surface -- there is no setter for it in the API and no
    // reason for a client to see it. 0 means "not yet minted"; app_slideshow mints one.
    uint32_t slideshow_seed;

    // Whether /data is a CACHE OF WHAT IS BEING SHOWN rather than a mirror of one folder
    // (ticket 37 Phase 3). With it on, the hourly run stops fetching a whole folder and only
    // refreshes the catalogue, the slideshow selects over the catalogue instead of over /data,
    // and photographs are fetched a few ahead of where the selection is and evicted behind it.
    //
    // **Also not one of FR-8's nine**, and it is its own key rather than being implied by
    // `slideshow_random` because the two are orthogonal: walking the catalogue in order while
    // fetching on demand is meaningful, and so is shuffling a fully mirrored folder.
    //
    // Default off, and here the reason is not "until it has been judged" but reversibility: it
    // changes what /data is for, and it makes the mirror EVICT, which tickets 25 and 27 both
    // record as something the mirror never does. Off is exactly the behaviour of every build
    // before it existed.
    bool smb_on_demand;

    // Re-encode an imported photograph at the size the panel will actually draw it,
    // and write its thumbnail beside it, instead of storing the share's own bytes
    // (ticket 49). What it buys is storage and thumbnails, not display speed: the
    // decode is already 264 ms of a 3 165 ms render against a 15 015 ms refresh,
    // because epd_image already reduces at decode. What it costs is a longer
    // httpd_down inside the sync window and a lossy re-encode.
    //
    // **Also not one of FR-8's nine**, and a deliberate departure from mirroring the
    // share byte for byte -- the same class of departure as the leading-dot rule in
    // smb_manifest.h. Default **on** by owner decision, 2026-09-09 -- ahead of its
    // own hardware evidence, and apply_defaults() in app_settings.c carries that
    // argument. Note it is not reversible for anything already imported: turning it
    // back off leaves the shrunk copies where they are until the share's own entries
    // change.
    bool smb_resize;

    // Turn a picture 90 degrees when its orientation disagrees with `rotation` and it is
    // far enough from square that turning it is not overruling the composition
    // (epd_fit_wants_rotate(), and EPD_ROTATE_RATIO_X100 for the threshold argument).
    //
    // Half a real library is the way up the frame is not, and that half is drawn at
    // 44-50 % of the panel where a turn gives 84-100 % -- so this is the largest single
    // improvement ticket 50 found, and it only became so once the owner corrected the
    // bench share to test data. It is a DRAW-time decision and touches nothing on the
    // card: the stored fit target is a 600x600 square precisely so one stored file serves
    // either orientation, so changing this invalidates no cache.
    //
    // Not one of FR-8's nine, and FR-5's order says nothing about it. Default on by
    // owner decision, 2026-09-09.
    bool auto_rotate;

    // Minutes east of UTC, applied to the SNTP-synced system clock to get a local hour
    // (ticket 41). **An offset, deliberately, and not a TZ string**: a `TZ` with DST rules
    // costs tzset() and the IANA database, and the only consumer is ticket 42's "is it
    // night", which an offset answers exactly. JST has no DST, so the bench loses nothing;
    // a user in a DST zone gets an hour of error twice a year on a picture frame, which is
    // the trade being made and is written here so it is not rediscovered as a bug.
    //
    // Not one of FR-8's nine. Default +540 (JST).
    int16_t tz_offset_minutes;

    // The active-hours window of ticket 42, in local hours, wrapping midnight when
    // `active_start_hour` > `active_end_hour`. Closed at the start and open at the end:
    // 7..23 refreshes from 07:00 and holds from 23:00.
    //
    // **Gated by `low_power_mode`, which is the master switch rather than a fourth key.**
    // That is a deliberate departure from FR-8: the key is FR-8's and means FR-7's power-off
    // in the shipping firmware, which ticket 15 owns and nobody has built. Before this the
    // toggle was plumbed end to end -- NVS, accessor, API, a real control in index.html --
    // and consumed NOWHERE, so a user could flip it, watch it persist across a reboot, and
    // have it change nothing. Making it mean the reachable half of the same intent is argued
    // in ticket 42; leaving it inert was the alternative.
    //
    // Not one of FR-8's nine. Defaults 7 and 23, from the Android frame's priority-2 layer.
    uint8_t active_start_hour;
    uint8_t active_end_hour;

    // Ticket 68, both halves of the owner's request of 2026-09-20. **Two scalars and not the
    // three that ticket anticipated**: weekly is the only cadence anybody asked for, so the day
    // encodes the schedule and there is no separate on/off. They are in this struct rather than
    // beside `s_settings` (where the Google Photos link had to go) because two bytes is not 256 --
    // see the note above about this struct being copied whole onto main's stack.
    //
    // `standby_white`: park the panel on WHITE when the active window closes, instead of leaving
    // the last photograph on the glass for the whole window. E Ink holds its image with no power, so
    // today's `hold=1` is exactly the image-sticking condition GooDisplay's own precautions warn
    // about; white applies their storage advice nightly and clears the previous render completely,
    // which measurements.md:123-133 shows a white render is the only thing that does. **Pure white
    // was the owner's choice** -- a mostly-white standby carrying ticket 64's caption band was
    // offered and rejected. Costs +1 refresh/day, not +2: the morning resume is already free
    // because app_slideshow.c does not touch its interval clock while the window is closed.
    //
    // `maint_day`: the day of the week the maintenance course runs, 0 = Sunday .. 6 = Saturday,
    // **7 = never**. The course is ten flats spread across one hour inside the closed window;
    // weekly rather than nightly because e-paper has a finite refresh count and nightly would be
    // +63 % of it for a ghost clear that `standby_white` already provides every night.
    //
    // Not FR-8's, same standing as `palette`: the shipping firmware has no such settings.
    bool standby_white;
    uint8_t maint_day;

    // Ticket 68 §15, 2026-09-20. Make the window's white park a whole CLEAR CYCLE — seven refreshes,
    // `W B W B W B W` — instead of one white render.
    //
    // **DEFAULT OFF, and the arithmetic is the reason rather than caution.** §5's whole argument is a
    // refresh count: the baseline is ~16 automatic advances a day, and the two shipping halves take it
    // to ~18.4 (+15 %). A clear every night is 6 nights x 7 plus the course night's 11 = 53/week
    // ≈ 7.6/day, so **~23.6/day, +47 %** — within sight of the +63 % that got a nightly course
    // rejected. Shipping that silently would overturn §5 without arguing it, so the switch exists and
    // is off, and turning it on is a deliberate choice with the number attached.
    //
    // **It does nothing on the course's own night**: only one sequence can hold the panel, so the
    // course wins and the clear is skipped — the course is ten flats ending on white and already does
    // everything the clear does. `app_maint.c`'s `on_window_closed()` is where that lives.
    //
    // Inert without `standby_white`, which is itself inert without `low_power_mode`: no window, no edge.
    bool standby_deep;

    // Ticket 71, the owner's battery-longevity request of 2026-09-20. **How full the cell is
    // allowed to get, as a percentage: 0 = do not manage the charger at all, 80 = cap, 100 =
    // restore the part's own default.** A lithium cell held at full ages faster than one held
    // near 80 %, and this frame lives on mains, so the cap is the single largest lever there is.
    //
    // **It does nothing whatsoever on the M5Paper Color, permanently, and that is copper.** That
    // board's charge-enable pin is a no-connect, its charger's I2C is unwired, and its system rail
    // IS the battery node through a 0 ohm link -- so there is nothing to write and stopping the
    // charge would start a discharge. `board_charger_set_vreg_mv()` answers
    // ESP_ERR_NOT_SUPPORTED there and `app_charge.c` reports it rather than retrying.
    // docs/board-pinmap.md §"Charging" is the table; ticket 71 §1 and §8.1 the account.
    //
    // **A percentage and not millivolts**, although millivolts is what the hardware takes, because
    // the number a user reasons about is "80 %" and the mapping is a cell-chemistry fact rather
    // than a preference. app_charge.c owns the mapping (80 % -> 4016 mV) and says why.
    //
    // Only 0, 80 and 100 are accepted -- refused rather than clamped, for the reason
    // app_settings_set_palette() gives. Default **80**: the owner chose it, against 「時々は
    // 無給電で使う」, so the cell must still carry the frame for hours.
    //
    // Not one of FR-8's nine; the shipping firmware has no such setting and could not implement it.
    uint8_t charge_limit_pct;
} app_settings_t;

// app_setting_key_t -- one key per NVS entry -- moved to src/core/app_setting_key.h on 2026-09-21
// so that the host-tested app_setting_effect.h can be keyed by the same names instead of declaring
// a second enum of the same settings. Included here, so every user of this header is unaffected.
#include "app_setting_key.h"

// Initialises NVS, applies the FR-8 defaults, then overlays whatever the `papercolor` NVS
// namespace holds -- still that name after the 2026-09-17 product rename, deliberately, because
// renaming a namespace loses every configured device's credentials (app_settings.c). A value that fails validation on the way out of NVS is dropped and
// the default kept -- so a factory-fresh device and a device with a corrupt entry both
// end up in a defined state.
esp_err_t app_settings_init(void);

// One consistent snapshot, one lock acquisition. Use this rather than nine getters when
// several fields are read together.
void app_settings_get(app_settings_t *out);

// Individual reads, for the loops that want one field.
uint8_t app_settings_rotation(void);
bool app_settings_auto_slideshow(void);
int app_settings_interval_minutes(void);
bool app_settings_low_power_mode(void);
bool app_settings_boot_sound(void);
// An epd_palette_id_t; always in range, so a caller can index with it without checking.
uint8_t app_settings_palette(void);
bool app_settings_auto_adjust(void);
bool app_settings_dither_diffuse(void);
bool app_settings_slideshow_random(void);
uint32_t app_settings_slideshow_seed(void);
bool app_settings_smb_enabled(void);
bool app_settings_smb_on_demand(void);
bool app_settings_smb_resize(void);
bool app_settings_auto_rotate(void);
int16_t app_settings_tz_offset_minutes(void);
// The two window hours together: they are only ever read as a pair, and reading them under
// one lock is what stops a save landing between them and producing a window that was never
// configured.
void app_settings_active_hours(uint8_t *start_hour, uint8_t *end_hour);
// Ticket 68. `maint_day` is 0..6 with 7 meaning never, so a caller comparing it against
// app_clock_local_wday() needs no separate enabled check -- 7 matches no weekday.
bool app_settings_standby_white(void);
bool app_settings_standby_deep(void);
uint8_t app_settings_maint_day(void);
// 0, 80 or 100 -- always one of the three, so a caller can switch on it without a default case.
uint8_t app_settings_charge_limit_pct(void);
// Copies into `out`; always NUL-terminated.
void app_settings_device_name(char *out, size_t size);
void app_settings_current_mode(char *out, size_t size);
void app_settings_wifi_ssid(char *out, size_t size);
void app_settings_wifi_password(char *out, size_t size);
// Slot 0..APP_SETTINGS_GPHOTOS_SLOTS-1; "" for an empty or out-of-range slot.
void app_settings_gphotos_album(size_t slot, char *out, size_t size);
// The last link in `slot` that was read whole with photographs in it (ticket 60). A new link that
// fails is replaced by this one -- the owner's rule of 2026-09-13.
void app_settings_gphotos_good(size_t slot, char *out, size_t size);
// The switch alone, whatever the slots hold.
bool app_settings_gphotos_switch(void);
// The switch on AND at least one slot holding a link.
bool app_settings_gphotos_enabled(void);

// ------------------------------------------------------------------------- setters
//
// Each validates, updates the in-RAM copy and persists that one key. They return
// ESP_ERR_INVALID_ARG for a value that cannot be normalised into range -- the HTTP
// layer turns that into a 400 rather than storing something it will have to defend
// against later.
//
// Strings are normalised, not merely rejected: device_name is lowercased and stripped
// to [a-z0-9-] (hal.cpp:89-144), and an empty result becomes DEFAULT_DEVICE_NAME
// ("paperframe" since 2026-09-17; the NVS namespace is still "papercolor" and app_settings.c
// says why).

esp_err_t app_settings_set_wifi(const char *ssid, const char *password);
// Quarter turns, 0..EPD_CANVAS_ROTATION_MAX; anything else is ESP_ERR_INVALID_ARG rather than a
// clamp, for the reason app_settings_set_palette() gives. **Saving it is not enough to turn the
// panel** -- app_display_set_rotation() is the other half, and app_display_redraw() the third.
esp_err_t app_settings_set_rotation(uint8_t rotation);
esp_err_t app_settings_set_auto_slideshow(bool on);
esp_err_t app_settings_set_interval_minutes(int minutes);
esp_err_t app_settings_set_current_mode(const char *mode_id);
esp_err_t app_settings_set_boot_sound(bool on);
esp_err_t app_settings_set_device_name(const char *name);
esp_err_t app_settings_set_low_power_mode(bool on);
// Refuses an id outside epd_palette_id_t rather than clamping: the value arrives from an
// HTTP body and a silent clamp would answer 200 for a request the client got wrong.
esp_err_t app_settings_set_palette(uint8_t palette);
esp_err_t app_settings_set_auto_adjust(bool on);
esp_err_t app_settings_set_dither_diffuse(bool on);
esp_err_t app_settings_set_slideshow_random(bool on);
// State, not a setting -- see app_settings_t.slideshow_seed. Called by app_slideshow when a
// pass ends, and by nothing else.
esp_err_t app_settings_set_slideshow_seed(uint32_t seed);
esp_err_t app_settings_set_smb_on_demand(bool on);
esp_err_t app_settings_set_smb_resize(bool on);
// Per slot. The switch is one for all albums; with no slot holding a link it runs nothing.
esp_err_t app_settings_set_gphotos_album(size_t slot, const char *album);
esp_err_t app_settings_set_gphotos_good(size_t slot, const char *album);
esp_err_t app_settings_set_gphotos_enabled(bool enabled);
esp_err_t app_settings_set_auto_rotate(bool on);
// -720..+840 minutes, the real range of civil offsets (UTC-12 to UTC+14). Refuses anything
// else rather than clamping, for the reason app_settings_set_palette() does.
esp_err_t app_settings_set_tz_offset_minutes(int minutes);
// Both hours at once, 0..23 each. They move together because a schedule is the pair: saving
// one at a time would let a client leave the frame holding through a window it never asked
// for. `start == end` is accepted and means "always active" (app_schedule.h).
esp_err_t app_settings_set_active_hours(int start_hour, int end_hour);
// Ticket 68. Separate setters, unlike the two hours above: these are two independent decisions --
// whether the panel rests on white, and whether a colour check runs weekly -- and nothing breaks if
// one is saved without the other. `day` is 0..6 or 7 for never; anything else is refused rather
// than clamped, for the reason app_settings_set_palette() gives.
esp_err_t app_settings_set_standby_white(bool on);
esp_err_t app_settings_set_standby_deep(bool on);
esp_err_t app_settings_set_maint_day(int day);
// 0, 80 or 100 only. Anything else is ESP_ERR_INVALID_ARG rather than a clamp: a clamp would
// answer 200 to a client that asked for 90 and silently give it something else. **Saving it is not
// enough to change the charger** -- app_charge_apply() is the other half, exactly as
// app_settings_set_rotation() needs app_display_set_rotation().
esp_err_t app_settings_set_charge_limit_pct(int pct);

// The seven mirror fields move together -- a host without a share, or a user without a
// password, is not a state worth persisting -- so they get one setter rather than seven.
// A NULL `password` leaves the stored one alone, which is what makes the field
// write-only over HTTP: the UI can save a changed host without being handed the
// password first in order to send it back.
esp_err_t app_settings_set_smb(const char *host, const char *share, const char *path,
                               const char *user, const char *password,
                               const char *domain, bool enabled);

// Writes the current in-RAM value of one key to NVS. The setters call this for you; it
// is exposed for app_settings_reset_defaults() and for tests.
esp_err_t app_settings_save(app_setting_key_t key);

// FR-8.2's first half: every value back to its default, persisted. The guide image and
// the power-off that complete FR-8.2 belong to ticket 16 and are NOT done here.
esp_err_t app_settings_reset_defaults(void);

// ---------------------------------------------------------------------- normalisers
//
// Exposed because the HTTP layer wants to report what a value was turned into, and
// because ticket 10 normalises a device name before handing it to mDNS.

// True if `input` had to be changed to be acceptable. `output` always ends valid.
bool app_settings_normalize_device_name(const char *input, char *output, size_t size);
// "" and "mode_1" pass through; anything else becomes "" and returns true.
bool app_settings_normalize_mode_id(const char *input, char *output, size_t size);

// Prints every key and its value. Raw values, one per line, for the capture log.
void app_settings_dump(void);

#endif // APP_SETTINGS_H
