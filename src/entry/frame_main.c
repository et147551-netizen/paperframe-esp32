// The digital frame application.
//
// Tickets .scratch/digital-frame/issues/10-13, built in that order:
//   A  settings in NVS            (12)  verified on hardware 2026-09-02
//   B  Wi-Fi AP+STA, DNS, mDNS    (10)  verified on hardware 2026-09-02
//   C  HTTP server and web UI     (11)  verified on hardware 2026-09-02
//   D  slideshow                  (13)  <- here
//
// The boot order is fixed by the hardware: the EPD rail comes up through the PM1
// before the panel is touched (FR-1.3), and the SPI bus is brought up by the caller so
// epd_init() only adds its device (ticket 03).

#ifdef BUILD_FRAME

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_apply.h"
#include "app_auth.h"
#include "app_clock.h"
#include "app_display.h"
#include "app_gphotos.h"
#include "app_gphotos_sync.h"
#include "app_heapwatch.h"
#include "app_charge.h"
#include "app_maint.h"
#include "app_ota.h"
#include "app_server.h"
#include "app_settings.h"
#include "app_slideshow.h"
#include "app_smb_sync.h"
#include "board_audio.h"
#include "board_buttons.h"
#include "board_led.h"
#include "board.h"
#include "board_spi.h"
#include "board_wifi.h"
#include "board_storage.h"
#include "board_usb_msc.h"
#include "epd_canvas.h"  // EPD_CANVAS_ROTATION_MAX, for the TOP button's cycle
#include "epd_panel.h"
#include "photo_list.h"
#include "trace.h"

static char s_name_arena[16384];
static photo_list_t s_list;

// ---------------------------------------------------------------------- buttons
//
// FR-6.1 and FR-6.2, against the physical layout the owner read off the unit:
// TOP (GPIO1) toggles the orientation and holds for the access point, UP (GPIO10) goes
// back one picture and DOWN (GPIO9) goes forward one.
//
// The callback runs on the button task, which has a small stack and
// must not block: a navigation re-scans /data and takes the storage lock, so it is a flag
// here and work in the application tick below. The 1 s settle window in app_slideshow is
// what makes ten rapid presses cost one refresh, so the flags are counters rather than
// booleans -- losing a press to a race would be invisible and wrong.

static volatile uint32_t s_want_next;
static volatile uint32_t s_want_prev;
static volatile uint32_t s_want_rotate;
static volatile uint32_t s_want_pairing;

// When the pairing screen went up, in ms since boot; 0 when there is nothing to clear.
// See clear_pairing_if_due().
static uint32_t s_pairing_at_ms;

static void on_button(board_button_t button, board_button_event_t event, uint32_t held_ms)
{
    if (event == BOARD_BUTTON_HOLD_TICK) {
        // FR-6.3's feedback while a 5 s hold is counted: white, every 200 ms. The
        // access-point/QR action itself is ticket 16; this at least tells the user the
        // hold is registering, which is the whole reason the reference does it.
        if (button == BOARD_BUTTON_TOP) {
            board_led_set(BOARD_LED_HOLD);
        }
        return;
    }
    if (event == BOARD_BUTTON_LONG_PRESS) {
        // FR-6.3. A flag, not the render: this runs on the button task, whose stack is
        // small, and building the payloads touches settings and the auth module.
        if (button == BOARD_BUTTON_TOP) {
            s_want_pairing++;
        }
        board_led_set(BOARD_LED_IDLE);
        return;
    }

    // FR-6.4, a tone per press, on this task: it blocks it for the length of the tone (160 ms
    // on the M5Paper Color, 60 ms on the E1002), which delays only the next button poll. Not
    // gated by `boot_sound`, as in the shipping firmware.
    board_audio_beep(button);

    switch (button) {
    case BOARD_BUTTON_DOWN:
        s_want_next++;
        break;
    case BOARD_BUTTON_UP:
        s_want_prev++;
        break;
    case BOARD_BUTTON_TOP:
        s_want_rotate++;
        break;
    default:
        break;
    }
}

// FR-6.3's payloads, assembled where blocking is allowed.
//
// **The join URI describes the radio, not an intention.** It says T:WPA;P:<password> when
// board_wifi_ap_secured() is true and T:nopass when it is not, read at draw time -- because a QR
// claiming the network is open when it is not is worse than no QR at all, and because the AP does
// stay open in two real cases: FRAME_AP_OPEN, and a board_wifi_ap_secure() that refused. Ticket 30
// step 2 said these two must move together; asking the radio is how they cannot come apart.
// **And since ticket 66 it carries the same things in words, plus a pairing code.** The QR half
// serves a phone; a PC has no camera, and until this existed a desktop browser could not be paired
// at all -- it got the page, a 401, and a banner telling it to scan a code it could not see.
static void show_connect_card(void)
{
    // **Ticket 67: this is the ONLY thing that raises the access point.** The radio is down at boot
    // and closes itself again after FRAME_AP_WINDOW_MS with nobody on it, so the AP exists for as
    // long as the card that describes it is useful and no longer. Opened FIRST, before the card is
    // built, so that board_wifi_ap_secured() below describes a live radio rather than a stored
    // intention -- the join QR says T:WPA or T:nopass by asking, and asking a radio that is down
    // would put the wrong answer on the glass for fifteen seconds.
    //
    // Both ways in come through here: the 5 s button hold, and card_at_boot_if_due() on a device
    // that has never paired, which is first setup and cannot ask for a gesture it has no way to
    // describe. So "the AP is only up after a long press" holds for every frame that has been set
    // up, and a factory-fresh one still works out of the box.
    const esp_err_t aperr = board_wifi_ap_window_open();
    printf("# ap window: %s (%u s, %u s left)\n", esp_err_to_name(aperr),
           (unsigned)(FRAME_AP_WINDOW_MS / 1000u), (unsigned)board_wifi_ap_window_left_s());

    board_wifi_status_t st;
    board_wifi_status(&st);

    char token[APP_AUTH_TOKEN_HEX_SIZE];
    app_auth_token_hex(token, sizeof(token));

    // Minted here rather than at boot, so a code only ever exists while it is on the glass. The
    // window outlives the card by design (FRAME_PAIRING_CODE_MS against FRAME_PAIRING_CLEAR_MS):
    // see app_auth.h.
    app_auth_pairing_open();

    app_display_card_t card = {0};
    char join_log[160];
    char ap_pass[APP_AUTH_AP_PASS_SIZE];
    if (board_wifi_ap_secured()) {
        app_auth_ap_password(ap_pass, sizeof(ap_pass));
        snprintf(card.join, sizeof(card.join), "WIFI:S:%s;T:WPA;P:%s;;", st.ap_ssid, ap_pass);
        // **The length and nothing else.** This used to be the first four characters and a length,
        // which was a quarter of a 16-character password; ticket 66 made it 8, where four
        // characters is HALF of it and this line goes into a capture file that is not git-ignored.
        // Identity, when a run needs it, is `ap password fp=` in app_auth_log_prefix().
        snprintf(join_log, sizeof(join_log), "WIFI:S:%s;T:WPA;P:<%u chars>;;", st.ap_ssid,
                 (unsigned)strlen(ap_pass));
        snprintf(card.ap_pass, sizeof(card.ap_pass), "%s", ap_pass);
    } else {
        snprintf(card.join, sizeof(card.join), "WIFI:S:%s;T:nopass;;", st.ap_ssid);
        snprintf(join_log, sizeof(join_log), "%s", card.join);
        // Left empty, which is what makes the card say NO PASSWORD rather than leave a gap.
    }

    const char *ap_ip = st.ap_ip[0] ? st.ap_ip : "192.168.4.1";
    snprintf(card.url, sizeof(card.url), "http://%s/?t=%s", ap_ip, token);
    snprintf(card.ssid, sizeof(card.ssid), "%s", st.ap_ssid);
    snprintf(card.ap_ip, sizeof(card.ap_ip), "%s", ap_ip);
    snprintf(card.sta_ip, sizeof(card.sta_ip), "%s", st.ip);

    // FR-2.3's name, which is the address worth printing first: it survives a DHCP lease changing,
    // and it is the only one of the two that is the same over the AP and over the LAN.
    char device[APP_SETTINGS_NAME_SIZE];
    app_settings_device_name(device, sizeof(device));
    snprintf(card.host, sizeof(card.host), "%s.local", device);

    app_auth_pairing_code_display(card.code, sizeof(card.code));

    // **Neither the join URI nor the code is logged whole.** The URI changed with ticket 30 step 2:
    // it used to be printed in full, correctly, because on an open access point it held no secret.
    // It now carries the AP password, and this console output ends up in capture files under
    // .scratch/ -- so it is logged as everything but the secret plus four characters and a length,
    // which tells one scan from another without putting the password in a file. The code gets not
    // even that: four of eight symbols is half of a short secret.
    printf("# connect card: join=\"%s\" url=http://%s/?t=<token> host=%s sta=%s code=%s\n",
           join_log, ap_ip, card.host, card.sta_ip[0] ? card.sta_ip : "-",
           card.code[0] ? "<set>" : "-");
    const esp_err_t err = app_display_request_card(&card);
    printf("# connect card request: %s\n", esp_err_to_name(err));
    fflush(stdout);
    if (err == ESP_OK) {
        s_pairing_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
}

// FR-6.3's other half, and the acceptance item ticket 30 found unimplemented: E Ink holds
// its image with no power, so a card left up is an API token -- and, after ticket 30 step 2, an
// access-point password, and after ticket 66 all three of those IN PLAIN WORDS plus a pairing code
// -- legible on a frame that has been switched off and carried out of the house. Ticket 66 raises
// what is at stake here rather than changing the mechanism: the QR codes needed a camera, and the
// text needs eyes. `app_display.h` says whoever draws it owns clearing it, and nothing did.
//
// The rule is "whatever renders next, or this, whichever comes first". A slideshow advance
// already overwrites it, which is why this never looked broken at the shipping
// interval_minutes of 5 -- but auto_slideshow is a setting the Web UI exposes, and with it
// off nothing was ever going to draw again.
//
// Three minutes: long enough to scan two codes and join a network without hurrying, and it
// is a bound rather than a guess at how fast somebody reads. **The pairing code deliberately
// outlives it** (FRAME_PAIRING_CODE_MS, ten minutes): the glass leaks to anybody in the room and
// should clear early, while the code only reaches whoever already read it -- and they may have to
// walk to another room to type it.
//
// Overridable for the bench, and it has to be: `bringup_capture.py` stops at the `@@DONE`
// this file prints at t=120 s, and closing the port RESETS THE BOARD -- so at three minutes
// the clear is not merely unrecorded, the clock starts again from zero on every attach and
// the arm can never happen. Two runs were spent finding that out. With
// -DFRAME_PAIRING_CLEAR_MS=30000 the whole sequence lands inside one capture.
#ifndef FRAME_PAIRING_CLEAR_MS
#define FRAME_PAIRING_CLEAR_MS (3u * 60u * 1000u)
#endif
#define PAIRING_CLEAR_MS ((uint32_t)(FRAME_PAIRING_CLEAR_MS))

static void clear_pairing_if_due(void)
{
    if (!s_pairing_at_ms) {
        return;
    }
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - s_pairing_at_ms < PAIRING_CLEAR_MS) {
        return;
    }
    // Not while a refresh is in flight: the request would queue behind it and the log would
    // read as a clear that took 30 s.
    if (app_display_busy()) {
        return;
    }

    // A pairing (or blank) render leaves s_current EMPTY on purpose, so a non-empty one
    // means a photograph has been drawn since and the credentials are already off the
    // glass. Checked only AFTER the deadline, because at the moment the pairing screen is
    // queued s_current still names the photograph it is about to replace.
    app_display_state_t ds;
    app_display_state(&ds);
    if (ds.current[0]) {
        s_pairing_at_ms = 0;
        return;
    }

    // WHITE FIRST, AND THAT IS NOT TIDINESS. Measured on the flatbed 2026-09-10: drawing the
    // photograph straight over the pairing screen leaves the two QR codes plainly legible as
    // a ghost on top of it -- light modules over a dark picture, decodable by eye and
    // certainly by a camera. One refresh does not erase a 1-bit black-on-white image on this
    // panel; only the white flash does. So a clear that skips this looks like it worked from
    // the console (renders+1, no failure) and leaves the token on the glass.
    //
    // The photograph back afterwards rather than a white panel, because this is a picture
    // frame and app_display_request_blank() alone is FR-5.7's "there is nothing to show". The
    // slideshow keeps its own index across a pairing render, so the name is still there.
    //
    // Two refreshes -- ~31 s on the EL040EF1 and ~62 s on the ED2208-GCA -- blocking this tick:
    // the application task is where blocking is allowed, and a clear that returns before the
    // panel has changed cannot be checked.
    //
    // **THE WAIT MUST EXCEED ONE REFRESH ON THE SLOWER PANEL, AND 30 s DID NOT.** It was 30,000,
    // which is comfortable against the M5Paper Color's 15,0xx ms and just short of the reTerminal
    // E1002's: measured 2026-09-19, that board's blank took `panel_ms=30896.5` and this wait timed
    // out a hundred milliseconds before it finished, so `app_slideshow_show_name()` was never
    // called and the panel was left WHITE. The console said `cleared after 40 s (blank)` and the
    // flatbed showed a blank frame -- which is safe, because no credential is left on the glass,
    // and wrong, because this is a picture frame. **Read off the `how` field**: `blank+photo` is
    // the working path and a bare `blank` on a board whose slideshow has a current name is this
    // defect. 60 s is one refresh on the slower panel plus most of another, not a guess at
    // scheduling. Ticket 66.
    app_slideshow_state_t ss;
    app_slideshow_get_state(&ss);
    esp_err_t err = app_display_request_blank();
    const char *how = "blank";
    if (err == ESP_OK && ss.current_name[0] && app_display_wait_idle(60000)) {
        const esp_err_t rerr = app_slideshow_show_name(ss.current_name);
        if (rerr == ESP_OK) {
            how = "blank+photo";
        }
    }
    printf("# pairing screen cleared after %u s (%s): %s\n",
           (unsigned)((now - s_pairing_at_ms) / 1000), how, esp_err_to_name(err));
    fflush(stdout);
    s_pairing_at_ms = 0;
}

// Ticket 66. A frame that nothing has ever paired with draws the connect card by itself, once.
//
// **Why it is not enough to leave this to the button.** A user whose only computer is a PC has no
// way to guess that a five-second hold on an unlabelled side button is what produces the password
// and the code, and the panel is the only surface this device has before anybody is paired. The
// flag is in NVS (app_auth_paired_once()), set by a claimed code or by a ?t= that mints a cookie.
//
// **Waited for rather than drawn at app_server_start().** The card carries the station address, and
// at that point the DHCP lease usually has not arrived -- a card printing the AP address only, on a
// frame that is on the house LAN, sends a PC user to the one address they cannot reach. Whichever
// comes first: an IP, or this.
#define FIRST_CARD_WAIT_MS 10000

static bool s_want_first_card;

static void card_at_boot_if_due(void)
{
    if (!s_want_first_card) {
        return;
    }
    // Not while a refresh is in flight: the slideshow's first photograph is usually still on the
    // panel at ten seconds, and queueing behind it would make this card's own timing unreadable.
    if (app_display_busy()) {
        return;
    }
    board_wifi_status_t st;
    board_wifi_status(&st);
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!st.ip[0] && now < FIRST_CARD_WAIT_MS) {
        return;
    }

    s_want_first_card = false;
    printf("# first boot: nothing has paired yet, drawing the connect card (sta=%s at %u ms)\n",
           st.ip[0] ? st.ip : "-", (unsigned)now);
    show_connect_card();
}

// Runs on the application task, where blocking is allowed.
static void service_buttons(void)
{
    while (s_want_next > 0) {
        s_want_next--;
        app_slideshow_next();
    }
    while (s_want_prev > 0) {
        s_want_prev--;
        app_slideshow_prev();
    }
    while (s_want_pairing > 0) {
        s_want_pairing--;
        show_connect_card();
    }
    while (s_want_rotate > 0) {
        s_want_rotate--;
        // **A CYCLE, 0->1->2->3->0, since ticket 69** (the owner's answer of 2026-09-20). FR-6.2
        // says this click "toggles the orientation between 0 and 1" and that is a deliberate
        // departure, recorded in docs/requirements/digital-frame.md beside ticket 42's.
        //
        // The cost was put to the owner and accepted: returning to a known orientation is four
        // clicks rather than one, on a frame whose user is an elderly parent. The 5 s hold that
        // raises the access point is a different gesture and is untouched.
        //
        // The same transaction the web UI's settings save uses (app_apply.h). This site and
        // h_mode_cfg_set() are its two adapters, and they used to disagree about a failed persist:
        // this one checked the setter's return before turning the panel, that one did not.
        const uint8_t next = (uint8_t)((app_settings_rotation() + 1u) % (EPD_CANVAS_ROTATION_MAX + 1u));
        app_apply_txn_t txn;
        app_apply_begin(&txn);
        if (app_apply_set(&txn, APP_SETTING_ROTATION, (int)next) == ESP_OK) {
            printf("# rotation -> %u\n", (unsigned)next);
        }
        app_apply_commit(&txn);
    }
}

// ------------------------------------------------------------------------ boot

// esp_reset_reason() as read at boot, kept so the heartbeat can repeat it. Printing it once
// at app_main was not enough for the one case it exists for: on a POWER-ON the line is gone
// before the host has re-enumerated the USB serial device, and while the board is off COM11
// does not exist at all, so a capture cannot be attached in advance either. It was therefore
// readable only after a reset a capture had itself caused -- exactly the case the distinction
// does not matter for. Ticket 13, 2026-09-09.
static int s_reset_reason;

void app_main(void)
{
    printf("\n# frame: digital frame application\n");

    trace_init();
    // Logged because the firmware cannot otherwise tell a power cut from a reset, and
    // ticket 13's index-persistence claim turns on exactly that distinction. Also kept in
    // s_reset_reason for the heartbeat, for the reason given there.
    s_reset_reason = (int)esp_reset_reason();
    printf("# reset reason: %d\n", s_reset_reason);
    // Ticket 09. Read this early because it also hands the USB PHY back to the console after a
    // USB-mode boot; acted on below, once /data is mounted.
    const bool usb_mode = board_usb_msc_pending();

    ESP_ERROR_CHECK(board_i2c_init());
    // Also resolves the RX8130 address, which the slideshow index depends on.
    board_i2c_scan();

    const esp_err_t lerr = board_led_init();
    printf("# led init: %s\n", esp_err_to_name(lerr));
    // Ticket 47's internal-RAM sampler, on the LED task's 20 ms tick rather than a task of its
    // own. Installed here and this early on purpose: the lowest figure this project has recorded
    // was already present at the FIRST heartbeat of its run, so whatever caused it happens during
    // boot -- an instrument installed after the mirror and the first render would miss it by
    // construction.
    board_led_set_tick_hook(app_heapwatch_sample);
    board_led_set(BOARD_LED_REFRESH);  // something is happening; boot takes ~10 s

    ESP_ERROR_CHECK(board_epd_power(true));
    bool powered = false;
    ESP_ERROR_CHECK(board_epd_power_state(&powered));
    printf("# epd rail readback: %s\n", powered ? "on" : "OFF");

    board_power_t pwr = {0};
    if (board_power_read(&pwr) == ESP_OK) {
        printf("# power: vin=%d (%u mV) vinout=%d bat=%d (%u mV)\n",
               (int)pwr.vin_present, (unsigned)pwr.vin_mv, (int)pwr.vinout_present,
               (int)pwr.bat_present, (unsigned)pwr.vbat_mv);
        if (!pwr.vin_present) {
            printf("# WARNING: running on battery. The PM1 forcibly powers off below\n"
                   "#   BATT_LVP (2.50 V default) and nothing but the power button\n"
                   "#   brings this board back. Do not start a long unattended run.\n");
        }
    } else {
        printf("# power: PM1 did not answer\n");
    }

    // The charger's whole register block, beside the supply line and for the same reason: it is
    // printed once at boot because that is where a reading of the cell's state belongs, and it is
    // printed RAW because REG08's field positions are not settled (ticket 71 §9.2). One line on a
    // board that has no reachable charger, which is the M5Paper Color permanently.
    board_charger_dump();

    ESP_ERROR_CHECK(board_spi_init());
    ESP_ERROR_CHECK(epd_init());

    board_storage_watch_t watch = {0};
    // The debounce state for the FR-4.2 poll in the loop below. Seeded from the boot
    // reading so a frame that starts with no card does not report a change on its first
    // pass; `acted_on_detect` is what the media currently reflects, not what was last read.
    bool last_detect = board_card_present();
    bool acted_on_detect = last_detect;
    int detect_agrees = 2;
    const esp_err_t serr = board_storage_select(&watch);
    if (usb_mode) {
        // Whichever medium select() chose, card or internal flash, is the one the PC gets. Nothing
        // below this line runs in USB drive mode -- no Wi-Fi, no slideshow, no mirror -- which is
        // what makes the host the volume's only writer (board_usb_msc.h). Never returns.
        board_usb_msc_run();
    }
    // POST /api/storage/rescan needs THIS watch, not a copy: it clears the fallback lock that
    // the 10 s poll below then acts on. app_main() never returns, so the pointer outlives httpd.
    app_server_set_storage_watch(&watch);
    if (serr != ESP_OK) {
        printf("# mount FAILED: %s\n", esp_err_to_name(serr));
    } else {
        printf("# media=%s fallback_locked=%d\n",
               board_storage_get_media() == BOARD_STORAGE_MEDIA_SD ? "sd" : "flash",
               (int)watch.sd_fallback_locked);
        photo_list_init(&s_list, s_name_arena, sizeof(s_name_arena));
        const esp_err_t scan_err = board_storage_scan(&s_list);
        printf("# scan err=%s count=%u\n", esp_err_to_name(scan_err),
               (unsigned)photo_list_count(&s_list));
        for (size_t i = 0; i < photo_list_count(&s_list); i++) {
            printf("#   [%u] %s\n", (unsigned)i, photo_list_at(&s_list, i));
        }
    }

    const esp_err_t sett_err = app_settings_init();
    printf("# settings init: %s\n", esp_err_to_name(sett_err));
    app_settings_dump();

    // FR-1.2, as soon as the setting that gates it can be read. Blocks for the sound's length
    // (1.8 s for the WAV) and allocates nothing that outlives it. There is no RTC-wake boot to
    // exempt, since FR-7 is not built, so every boot plays it.
    const esp_err_t auderr = board_audio_init();
    const bool boot_sound = app_settings_boot_sound();
    printf("# audio init: %s (%s) boot_sound=%d\n", esp_err_to_name(auderr),
           board_audio_describe(), boot_sound ? 1 : 0);
    if (auderr == ESP_OK && boot_sound) {
        board_audio_play_boot();
    }

#ifdef FRAME_GPHOTOS_ALBUM
    // Ticket 60's bench seed, on the same terms as the Wi-Fi one below: used only when NVS has no
    // album, never an override. `static` because main's stack has ~650 bytes spare and the buffer
    // is 256. The link is a capability, so it is not printed.
    {
        static char album[APP_SETTINGS_GPHOTOS_ALBUM_SIZE];
        app_settings_gphotos_album(0, album, sizeof(album));
        if (album[0] == '\0') {
            esp_err_t gerr = app_settings_set_gphotos_album(0, FRAME_GPHOTOS_ALBUM);
            if (gerr == ESP_OK) {
                gerr = app_settings_set_gphotos_enabled(true);
            }
            printf("# seeded NVS gphotos album from build flags: %s\n", esp_err_to_name(gerr));
        } else {
            printf("# gphotos seed: compiled in, not used (NVS already has an album)\n");
        }
    }
#endif

#ifdef FRAME_WIFI_SSID
    // Station credentials normally arrive from the Web UI (`/api/wifi/config`) and live in
    // NVS, which survives an app upload. But erasing flash takes them with it, and the
    // only way to type them back in is a phone on the frame's own AP -- the bench PC cannot
    // join one from a command line. So a build
    // may carry a seed, used only when NVS has nothing, out of git-ignored
    // platformio_local.ini. It is a fallback, never an override: whatever the Web UI
    // wrote wins.
    {
        char ssid[64] = {0};
        app_settings_wifi_ssid(ssid, sizeof(ssid));
        if (ssid[0] == '\0') {
            const esp_err_t serr = app_settings_set_wifi(FRAME_WIFI_SSID, FRAME_WIFI_PASS);
            printf("# seeded NVS wifi_ssid from build flags: %s\n", esp_err_to_name(serr));
        } else {
            printf("# wifi seed: compiled in, not used (NVS already has an ssid)\n");
        }
    }
#else
    // Said out loud because the failure is otherwise silent: a build_flags typo or a
    // platformio_local.ini that did not get merged looks exactly like a build with no
    // seed, and nobody finds out until NVS is empty and there is no way to fill it.
    printf("# wifi seed: not compiled in\n");
#endif

    app_settings_t cfg;
    app_settings_get(&cfg);

    const esp_err_t derr = app_display_init(cfg.rotation);
    printf("# display init: %s (rotation=%u)\n", esp_err_to_name(derr), cfg.rotation);

    const esp_err_t werr = board_wifi_init(cfg.device_name, cfg.wifi_ssid, cfg.wifi_password);
    printf("# wifi init: %s\n", esp_err_to_name(werr));

    board_wifi_status_t st;
    board_wifi_status(&st);
    printf("# ap ssid=\"%s\" ip=%s mdns=%s.local\n", st.ap_ssid, st.ap_ip, cfg.device_name);

    // One scan at boot, so the log carries the surrounding networks with RSSI whether
    // or not anyone opens the UI (FR-2.4).
    static board_wifi_network_t nets[20];
    size_t n = 0;
    const esp_err_t scan_err2 = board_wifi_scan(nets, sizeof(nets) / sizeof(nets[0]), &n);
    printf("# scan err=%s count=%u\n", esp_err_to_name(scan_err2), (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        printf("#   %-32s rssi=%d secure=%d\n", nets[i].ssid, nets[i].rssi, nets[i].secure ? 1 : 0);
    }
    fflush(stdout);

    // After board_wifi_init(), not before: esp_fill_random() is only a true RNG once the
    // RF subsystem is running, and a token minted from a bootloader-seeded PRNG on a
    // factory-fresh device would be predictable. Ticket 29.
    const esp_err_t aerr = app_auth_init();
    printf("# auth init: %s\n", esp_err_to_name(aerr));

    // Ticket 30 step 2. Here rather than inside board_wifi_init() because the password is minted
    // by the line above, from an RNG that needs the radio the line above that started.
    //
    // FRAME_AP_OPEN is the way back, and it is one flag for two things on purpose: it leaves the
    // access point OPEN and, because app_server.c asks board_wifi_ap_secured() rather than asking
    // the preprocessor, it also stops the captive portal handing out the API token. The pair
    // cannot come apart, which is the objection docs/web-api.md raises against fixing the
    // portal at all.
#ifdef FRAME_AP_OPEN
    printf("# ap: FRAME_AP_OPEN is set -- the access point stays OPEN and the captive portal will\n"
           "#     not carry the token. This is ticket 30's recovery path, not the shipping state.\n");
#else
    {
        char ap_pass[APP_AUTH_AP_PASS_SIZE];
        app_auth_ap_password(ap_pass, sizeof(ap_pass));
        const esp_err_t serr = board_wifi_ap_secure(ap_pass);
        // Not fatal, and the log line is the whole report: board_wifi_ap_secure() refuses rather
        // than half-applying, so a failure here leaves a working open AP -- reachable, and with
        // the token still withheld from the portal because that gate reads the radio.
        // `up=` since ticket 67: the AP is DOWN at this point, so "secured" means the password has
        // been accepted and stored and the next window will carry it -- not that anything is on the
        // air. Two fields because they are two facts and reading one as the other is how a check
        // passes against a radio that is not there.
        printf("# ap secure: %s (secured=%d up=%d)\n", esp_err_to_name(serr),
               board_wifi_ap_secured() ? 1 : 0, board_wifi_ap_is_up() ? 1 : 0);
    }
#endif

    // After board_wifi_init(), and it arms a timer rather than syncing here: a sync needs the
    // station to have associated, which is asynchronous, and nothing in the boot path may wait
    // on a network. Ticket 41.
    const esp_err_t cerr = app_clock_init();
    printf("# clock init: %s (ntp once a day, tz_offset=%+d min)\n", esp_err_to_name(cerr),
           (int)app_settings_tz_offset_minutes());

    const esp_err_t berr = board_buttons_init(on_button);
    printf("# buttons init: %s\n", esp_err_to_name(berr));

    // Before the slideshow, whose first draw reads a `.mta` from where this puts it -- and before
    // httpd and the mirrors, which is what makes the orphan sweep safe here and nowhere else.
    app_smb_sync_sidecars_prepare(true);

    const esp_err_t slerr = app_slideshow_start();
    printf("# slideshow start: %s\n", esp_err_to_name(slerr));

    const esp_err_t smberr = app_smb_sync_init();
    printf("# smb mirror init: %s\n", esp_err_to_name(smberr));

    // Ticket 68. Latches only -- the first tick establishes whether the window is open, and no edge
    // is inferred from a boot. The two settings are printed because a frame that runs no course and
    // one whose stored day the loader rejected look identical otherwise.
    const esp_err_t merr = app_maint_init();
    printf("# maint init: %s (standby_white=%d maint_day=%u)\n", esp_err_to_name(merr),
           app_settings_standby_white() ? 1 : 0, (unsigned)app_settings_maint_day());

    // Ticket 71. AFTER settings are loaded and before the server is up, so the cap is in force
    // before anything can change it. Prints only when it had to write, and prints once on a board
    // whose charger this firmware cannot reach -- which is the M5Paper Color, permanently.
    app_charge_apply();

    const esp_err_t herr = app_server_start();
    board_led_set(herr == ESP_OK ? BOARD_LED_IDLE : BOARD_LED_ERROR);
    printf("# http server: %s (http://%s/ and http://%s.local/)\n", esp_err_to_name(herr),
           st.ap_ip, cfg.device_name);
    // Ticket 61: which slot this is and whether it is on trial. The verdict is app_ota_tick()'s.
    app_ota_boot_check();
    fflush(stdout);

    // Ticket 66's first-boot card: read once here rather than every 200 ms, since it is an NVS
    // lookup. See card_at_boot_if_due() for why the draw itself waits.
    s_want_first_card = !app_auth_paired_once();
    printf("# paired before: %s\n", s_want_first_card ? "no (the card will be drawn)" : "yes");

#ifdef FRAME_PAIRING_AT_BOOT
    // Bench only, and since ticket 66 it means "draw it even though this device has paired". The
    // card is a 5 s button hold, and a hold needs a finger -- which makes the scan-and-decode loop
    // of ticket 30 an attended job for no good reason. With this flag the same screen is drawn once
    // at boot, so render -> scan -> decode runs with nobody at the bench. It does not replace the
    // button test; it makes the QR and the text checkable without one.
    printf("# FRAME_PAIRING_AT_BOOT: drawing the connect card\n");
    s_want_first_card = false;
    show_connect_card();
#endif

    // Status every 10 s: link, panel and what the server last did. Raw counters, so a
    // failed render is visible in the log rather than inferred from a blank panel.
    for (int i = 0;; i++) {
        // 200 ms is the application tick: it drives the 1 s settle window and the
        // automatic advance. The status line below is printed once every 50 ticks.
        for (int t = 0; t < 50; t++) {
            service_buttons();
            card_at_boot_if_due();
            clear_pairing_if_due();
            app_slideshow_update();
            // Ticket 68's window edge and colour course. Here rather than in the 10 s block below
            // because a back-to-back course must not wait ten seconds per flat; the tick does its
            // own 10 s gating on the part that reads the clock.
            app_maint_tick();
            // Ticket 71. Rate-limits itself to one charger read every 20 s and returns immediately
            // on a board with no reachable charger, so it costs nothing here. It exists because the
            // part's I2C watchdog may restore REG04's default and no datasheet on this disk says
            // whether it does -- the tick makes either answer survivable and counts which it was.
            app_charge_tick();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        // FR-4.2, wired 2026-09-08 (ticket 08). board_storage_ensure() has done the whole
        // job since ticket 08 -- read the detect line, tear the volume down, switch, fall
        // back, hold the fallback lock -- and NOTHING HAS EVER CALLED IT. board_storage_select()
        // above runs once at boot and that was the entirety of the media handling, so a card
        // pulled from a running frame was never noticed: measured that day as a mount that
        // still reported the card's 1.98 GB while every read 404'd, with no error anywhere.
        //
        // DEBOUNCED, because board_card_present() returns false on any PM1 I2C
        // failure and that is indistinguishable from an empty slot -- one hiccup would
        // otherwise unmount a working card. Two agreeing readings at 10 s is 20 s to act on a
        // removal, which is cheap now that an undetected one no longer storms the bus
        // (app_slideshow.c's count==0 branch).
        //
        // The poll is three PM1 I2C transactions and takes no bus. Only a CHANGE calls
        // board_storage_switch(), which takes the storage lock and SPI2 and can therefore
        // wait out a 15 s refresh -- correct, because it must not tear a volume down under a
        // reader, and it stalls this loop only when the card actually moves.
        {
            const bool present = board_card_present();
            if (present == last_detect) {
                if (detect_agrees < 2) {
                    detect_agrees++;
                }
            } else {
                detect_agrees = 1;
                last_detect = present;
            }
            if (detect_agrees >= 2 && present != acted_on_detect) {
                acted_on_detect = present;
                if (board_storage_ensure(&watch)) {
                    const bool sd = board_storage_get_media() == BOARD_STORAGE_MEDIA_SD;
                    printf("# media change: card_present=%d media=%s fallback_locked=%d\n",
                           (int)present, sd ? "sd" : "flash", (int)watch.sd_fallback_locked);
                    // The manifest and the catalogue are FILES ON THE VOLUME THAT JUST
                    // CHANGED, so the in-RAM copies now describe the wrong one. And the new
                    // volume may not have the sidecar folder yet.
                    app_smb_sync_sidecars_prepare(false); // the mirror may be mid-store
                    app_smb_sync_media_changed();
                    // FR-4.2's "reset the displayed-image index" is the caller's, which is
                    // this one: board_storage.h says why the module does not touch it.
                    app_slideshow_invalidate();
                }
            }
        }

        // Ticket 55, and the owner picked these two rows of its §4 on 2026-09-10: a frame
        // with no web UI, and a frame whose /data did not mount. Polled from this loop rather
        // than given a task of its own, because internal RAM is the scarce resource and two
        // small tasks were once enough to make httpd_start() fail -- which is, with some irony,
        // the first of the two faults being reported here.
        //
        // `herr` is this boot's httpd result and cannot change, so it is honest to hold the LED
        // until a reboot. The mount side reads board_storage_is_mounted(), which is LIVE and is
        // what makes "it clears when the condition clears" true across a POST
        // /api/storage/rescan or a card being reseated; the boot-time `serr` a few hundred lines
        // up would have been wrong for exactly that case.
        //
        // set_resting() and not set(): a render's SUCCESS must not fall back past the fault to
        // the green pulse that means "on and working". Ticket 55 §2 is that defect.
        board_led_set_resting((herr != ESP_OK || !board_storage_is_mounted()) ? BOARD_LED_FAULT
                                                                             : BOARD_LED_IDLE);

        // Once every 10 s, not once per 200 ms tick: the decision is about one-hour
        // periods and twenty-second gaps, and the check itself copies the settings and
        // asks the radio.
        app_smb_sync_tick();
        // Google Photos (ticket 60). Same cadence and the same reason as the line above.
        app_gphotos_sync_tick();

        board_wifi_status(&st);
        // Ticket 61. A no-op unless this image came by OTA and is still on trial.
        app_ota_tick(herr == ESP_OK, st.sta_connected);
        app_display_state_t ds;
        app_display_state(&ds);
        app_slideshow_state_t ss;
        app_slideshow_get_state(&ss);
        char ap_state[16];
        if (board_wifi_ap_is_up()) {
            snprintf(ap_state, sizeof(ap_state), "%us", (unsigned)board_wifi_ap_window_left_s());
        } else {
            snprintf(ap_state, sizeof(ap_state), "down");
        }
        // `led=` is board_led_state_t as an integer, and it is here because the LED is the one
        // instrument this bench cannot read: the flatbed photographs the panel, not the LEDs
        // (ticket 55 §3). Without this line ticket 55's arm has no unattended check at all.
        // BOARD_LED_FAULT is 6; a transient (REFRESH 2, SUCCESS 3) may sit on top of it for the
        // length of a refresh, which is accepted and visibly cyan.
        // `ap=` is ticket 67's window: `down`, or the seconds left before it closes itself. It sits
        // beside ap_clients because the pair is the whole state -- a number that stops falling with
        // a client on it is the hold working, and `down` with clients would be a contradiction.
        printf("# t=%4ds boot=%d sta=%d ip=%s ap=%s ap_clients=%d | display busy=%d pending=%d "
               "current=\"%s\" renders=%u failures=%u last_ms=%.0f err=\"%s\" | idle_ms=%u "
               "led=%d\n",
               (i + 1) * 10, s_reset_reason, st.sta_connected ? 1 : 0, st.ip, ap_state,
               st.ap_clients, ds.busy ? 1 : 0,
               ds.pending ? 1 : 0, ds.current, (unsigned)ds.renders, (unsigned)ds.failures,
               (double)ds.last_total_ms, ds.last_error,
               (unsigned)app_server_ms_since_activity(), (int)board_led_get());
        // Tickets 41 and 42. `hold=1` is the whole of what the schedule does, and without it
        // on this line a scheduled hold looks exactly like a slideshow that stopped advancing
        // for one of the reasons docs/defect-log.md already records. `step` is what the
        // last sync moved the clock by, repeated here rather than printed once at sync time,
        // because a fact printed once is a fact this bench cannot read on a run it joined late
        // (docs/hardware-runs.md).
        {
            app_clock_status_t ck;
            app_clock_get_status(&ck);
            uint8_t sh = 0, eh = 0;
            app_settings_active_hours(&sh, &eh);
            const bool sched_on = app_settings_low_power_mode();
            // Ticket 68's two fields ride this line rather than getting one of their own, because
            // both are only readable against the schedule beside them: `parked=1` with `hold=0` is
            // a contradiction, and a course step without the hour it started in cannot be told from
            // one a clock-sync edge produced at 03:00. `maint_day` is on the `# settings` and
            // `# maint init` lines, so it is not repeated here.
            app_maint_status_t mt;
            app_maint_get_status(&mt);
            printf("#        clock synced=%d trying=%d attempts=%u step=%d since=%ld hour=%d "
                   "| sched on=%d %02u..%02u hold=%d | maint run=%d %d/%d parked=%d courses=%u\n",
                   ck.synced ? 1 : 0, ck.trying ? 1 : 0, (unsigned)ck.attempts,
                   (int)ck.last_step_sec,
                   ck.since_sync_s == UINT32_MAX ? -1L : (long)ck.since_sync_s, ck.local_hour,
                   sched_on ? 1 : 0, (unsigned)sh, (unsigned)eh,
                   app_clock_schedule_active() ? 0 : 1, mt.running ? 1 : 0, mt.step, mt.steps,
                   mt.parked ? 1 : 0, (unsigned)mt.courses);
        }
        printf("#        slideshow current=%u pending=%u count=%u settling=%d name=\"%s\"\n",
               (unsigned)ss.current_index, (unsigned)ss.pending_index, (unsigned)ss.count,
               ss.refresh_pending ? 1 : 0, ss.current_name);
        // The mirror's lifeline. A sync stops httpd for the length of a window, so
        // GET /api/smb/status cannot be the only way to see one -- and with the owner
        // remote this line is how a run that is merely slow is told from one that is
        // stuck. stack_free is BYTES: ESP-IDF's uxTaskGetStackHighWaterMark() returns
        // bytes, unlike TaskStatus_t.usStackHighWaterMark above, which is words.
        {
            app_smb_sync_status_t sy;
            app_smb_sync_get_status(&sy);
            printf("#        smb on=%d syncing=%d %u/%u mirrored=%u bytes=%llu capped=%d "
                   "err=\"%s\" last=\"%s\" stack_free=%u\n",
                   sy.enabled ? 1 : 0, sy.syncing ? 1 : 0, (unsigned)sy.files_done,
                   (unsigned)sy.files_total, (unsigned)sy.files_mirrored,
                   (unsigned long long)sy.bytes_mirrored, sy.capped ? 1 : 0,
                   board_smb_err_str(sy.last_error), sy.last_file,
                   (unsigned)sy.stack_free);
        }
        // Both figures, because esp_get_free_heap_size() counts PSRAM here --
        // CONFIG_SPIRAM_USE_MALLOC puts the 8 MB pool in the same allocator, so the
        // total is dominated by it and says nothing about internal RAM. The internal
        // number is the one the upload path has to leave alone: a body that was
        // buffered rather than streamed would show up in min_int even after it finished.
        // Ticket 21: where the internal RAM actually goes. Stack high-water marks are in
        // WORDS from FreeRTOS; printed as bytes here so they can be compared against the
        // xTaskCreate() sizes directly.
        {
            // static, not a local: 24 TaskStatus_t is about a kilobyte, and the main
            // task's stack is the IDF default 3.5 KB. Putting it on the stack overflowed
            // it during Wi-Fi init and rebooted the device on the first run of this
            // instrumentation -- "A stack overflow in task main has been detected".
            static TaskStatus_t tasks[24];
            const UBaseType_t n_tasks = uxTaskGetSystemState(tasks, 24, NULL);
            printf("#        stacks");
            for (UBaseType_t k = 0; k < n_tasks; k++) {
                printf(" %s=%u", tasks[k].pcTaskName,
                       (unsigned)(tasks[k].usStackHighWaterMark * sizeof(StackType_t)));
            }
            printf("\n");
        }
        // The largest free block, not just the total: on 2026-09-04 the SD driver failed
        // reads with ESP_ERR_NO_MEM while int_free still read 8.5 KB, which a total cannot
        // explain and a largest-block can. DMA-capable internal memory is its own pool and
        // is what sdmmc and the Wi-Fi path actually draw from, so it is printed separately.
        printf("#        heap total_free=%u total_min=%u int_free=%u int_min=%u "
               "int_largest=%u dma_free=%u dma_largest=%u\n",
               (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

        // Ticket 47. The line above says how bad it got and nothing about when: int_min is a
        // since-boot watermark, which is why the 2,899 B floor of 2026-09-09 and ticket 52's
        // 15,487 B are both recorded as unattributed. This one carries the moment and what was
        // running at it, for each pool separately.
        //
        // **Read int_low against int_min on the line above before reading anything else here.**
        // int_min is the truth and int_low is what a 20 ms sampler at priority 3 managed to see,
        // so `int_min < int_low` means the trough was faster than the sampler and the difference
        // is this instrument's error in bytes -- not a contradiction, and not something to explain
        // away. samples= is how many ticks the sampler actually got, so a starved one is visible
        // as a rate rather than inferred.
        //
        // at= is a DURATION since boot in milliseconds, on esp_timer's clock, so it can be lined
        // up against the `render` and `window` lines. http= is the age in milliseconds of the last
        // request to reach routing, -1 for never; the `h` flag is that age within
        // HEAPWATCH_HTTP_RECENT_MS and is a convenience, not the datum. flags are s/d/h/g
        // for sync/display/http/https-fetch and `-` for none -- and none is a real, interesting answer: it would
        // mean the cause is something this instrument does not watch.
        {
            // On the stack, unlike `tasks` above, and that is measured rather than assumed: eleven
            // records is ~360 bytes and the main task's high-water spare read **984 bytes** on every
            // run of 2026-09-11 -- and **648 bytes** on every run of 2026-09-12/13, after
            // HEAPWATCH_FLAGS_STR_MIN went from 4 to 5 for the `g` flag and four buffers here grew
            // with it. Still spare, 336 bytes lower, and the margin this paragraph argues from is
            // now that much thinner: the next flag is the one to re-measure before, not after.
            // run of 2026-09-11 with this struct already on it. Making it static would spend 360
            // bytes of permanently-allocated internal RAM -- the scarce resource this very block
            // exists to watch -- to buy a margin the measurement says is already there. If a flag or
            // a record is added, re-read `stacks main=` before assuming that still holds.
            app_heapwatch_t hw;
            app_heapwatch_get(&hw);
            // Two buffers, because both are formatted in one printf. See heapwatch.h.
            char if_str[HEAPWATCH_FLAGS_STR_MIN], df_str[HEAPWATCH_FLAGS_STR_MIN];
            char wf_str[HEAPWATCH_FLAGS_STR_MIN];
            heapwatch_flags_str(hw.int_low.flags, if_str, sizeof(if_str));
            heapwatch_flags_str(hw.dma_low.flags, df_str, sizeof(df_str));
            heapwatch_flags_str(hw.wm_low.flags, wf_str, sizeof(wf_str));
            printf("#        heapwatch samples=%u int_low=%u dma_at_low=%u at=%lld flags=%s "
                   "http=%lld | dma_low=%u int_at_low=%u at=%lld flags=%s http=%lld\n",
                   (unsigned)hw.samples, (unsigned)hw.int_low.value,
                   (unsigned)hw.int_low.companion, (long long)hw.int_low.at_ms,
                   if_str, (long long)hw.int_low.http_age_ms, (unsigned)hw.dma_low.value,
                   (unsigned)hw.dma_low.companion, (long long)hw.dma_low.at_ms, df_str,
                   (long long)hw.dma_low.http_age_ms);
            // The exact-depth record, on its own line because it is the one to read: wm= must
            // equal int_min above, and at=/flags= are that fall located to within one 20 ms tick.
            // int_at= is the instantaneous free size on the tick that noticed, which is ABOVE the
            // watermark by however much the pool recovered in those 20 ms -- so the gap between
            // wm= and int_at= is a measure of how fast the trough was.
            printf("#        heapwatch wm=%u int_at=%u at=%lld flags=%s http=%lld\n",
                   (unsigned)hw.wm_low.value, (unsigned)hw.wm_low.companion,
                   (long long)hw.wm_low.at_ms, wf_str, (long long)hw.wm_low.http_age_ms);

            // The descent itself, oldest first, as `<bytes>@<ms>/<flags>` -- so a fall can be read
            // against the log lines around its timestamp, which is the only way an unflagged one
            // gets attributed. falls= is the TOTAL, which may exceed the eight kept.
            printf("#        heapwatch falls=%u:", (unsigned)hw.fall_count);
            const unsigned kept =
                hw.fall_count < HEAPWATCH_FALLS ? (unsigned)hw.fall_count : HEAPWATCH_FALLS;
            // Oldest kept first, so the sequence reads left to right in time and the rightmost
            // entry is the deepest. `first` is where the ring's oldest surviving entry sits.
            const unsigned first = hw.fall_count < HEAPWATCH_FALLS ? 0
                                                                   : (unsigned)(hw.fall_count %
                                                                                HEAPWATCH_FALLS);
            for (unsigned k = 0; k < kept; k++) {
                const heapwatch_low_t *f = &hw.falls[(first + k) % HEAPWATCH_FALLS];
                char ff[HEAPWATCH_FLAGS_STR_MIN];
                printf(" %u@%lld/%s", (unsigned)f->value, (long long)f->at_ms,
                       heapwatch_flags_str(f->flags, ff, sizeof(ff)));
            }
            printf("\n");
        }
        // Ticket 60's combined arm. Compiles to nothing without -DFRAME_GPHOTOS_PROBE, and even
        // built in it does nothing until FRAME_GPHOTOS_URL is set. Called from here rather than
        // from a task of its own, for the reason app_heapwatch has no task either: an instrument
        // for internal-RAM scarcity must not spend internal RAM on a stack to watch it. `g` in the
        // flags above is what says a trough coincided with a fetch.
        app_gphotos_tick();

        fflush(stdout);

        if (i == 11) {
            // Two minutes in, tell the capture tool it has a complete boot. The
            // application keeps running afterwards.
            printf("@@DONE\n");
            fflush(stdout);
        }
    }
}

#endif // BUILD_FRAME
