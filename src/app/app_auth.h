// The API token: who is allowed to talk to the HTTP server.
//
// Ticket .scratch/digital-frame/issues/29. Until it existed, all 21 API routes were
// unauthenticated on two surfaces at once -- the open access point of FR-2.1 and the
// house LAN of FR-2.4 -- so anyone in radio range could list, download and delete every
// photograph and read the NAS host, share and username.
//
// The model is deliberately small, and worth stating plainly because it is easy to read
// more into it than is there: **physical access to the frame is the trust boundary.**
// The token is handed out by the pairing screen (ticket 30) to whoever can hold the TOP
// button for five seconds and read the panel. There are no accounts, no expiry and no
// revocation short of app_auth_rotate(). That is the right size for a photo frame on a
// home network, and it is not more than that.
//
// Two things this module is careful about:
//
//   * The token is generated AFTER the radio is up. esp_fill_random() is only a true
//     random number generator once the RF subsystem is enabled; before that it is a
//     bootloader-seeded PRNG, and a predictable token is not a token. app_auth_init()
//     must therefore be called after board_wifi_init(), which is why it does not simply
//     live alongside app_settings_init().
//   * It is never printed whole and never added to app_settings_dump(). A capture log
//     gets four characters and a length, which is enough to tell which token is live
//     and not enough to use.

#ifndef APP_AUTH_H
#define APP_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "pair_code.h"

// 16 random bytes, hex-encoded: 32 characters and a terminator.
#define APP_AUTH_TOKEN_HEX_SIZE 33

// The access point's WPA2 password: **8 characters and a terminator, since 2026-09-19.** Ticket 30
// step 2 keeps it here rather than in board_wifi.c because it is the same kind of secret as the
// token, minted by the same RNG at the same moment and shown on the same screen.
//
// **It was 16 hex characters and the owner asked for the minimum, because it is typed by hand**
// (ticket 66). 8 is WPA2's own floor -- board_wifi_ap_secure() refuses anything outside 8-63 -- so
// there is nothing shorter to go to.
//
// **The alphabet changed with the length, and that is the part worth keeping.** 8 hex characters
// would be 32 bits; 8 Crockford base32 characters are **40**, for exactly the same typing effort,
// because the alphabet is 32 symbols wide rather than 16. It is the pairing code's alphabet
// (src/core/pair_code.h) and it is the right one here for the same reason: it omits I, L, O and U,
// so the glyph pairs a reader confuses are not in it. **Crockford's input folding does NOT apply
// here** -- a pairing code is compared by this module and can forgive O for 0, while a WPA2
// passphrase is compared byte-for-byte by the supplicant, so ambiguity has to be removed by
// exclusion rather than corrected on the way in. It is also inside QR alphanumeric mode, as hex was.
//
// **40 bits is the honest number and it is not large.** A captured 4-way handshake can be ground
// offline at PBKDF2-SHA1 speeds -- **an ESTIMATE of days on one GPU, arithmetic from published
// hash rates and not a measurement anybody here took**, and it is written that way because this
// project's rule is that a prediction carries its status (`docs/method.md`). So it rests on
// the same trust boundary everything else here does: physical access to the frame, a home network,
// and a password only ever shown on the glass. **Ticket 67 is the other half of the trade** -- the
// access point is down unless somebody asked for it, so there is no beacon to capture a handshake
// from for all but thirty minutes at a time. Each character added multiplies the work by 32 if the
// length is ever re-taken.
#define APP_AUTH_AP_PASS_SIZE PAIR_CODE_SIZE

// Loads the token from NVS, and mints one if there is none. The order of preference for
// a fresh device is the build seed first, then the RNG -- see the note on
// FRAME_API_TOKEN in the implementation.
//
// Call after board_wifi_init(). Returns ESP_OK even when persistence failed: a token
// that lives only in RAM still protects this boot, and refusing to start the web server
// because NVS is full would be a worse failure than a token that forgets itself.
esp_err_t app_auth_init(void);

// True when `candidate` is the token. Constant-time in the length of the token, and
// false for NULL. The comparison cost is not the interesting attack surface here --
// 128 bits over Wi-Fi is not going to be guessed -- but a variable-time compare is the
// kind of thing that gets copied into somewhere it does matter.
bool app_auth_check(const char *candidate);

// The token itself, for the pairing QR (ticket 30). Nothing else should call this.
void app_auth_token_hex(char *out, size_t size);

// The access point's password, for the join QR and for board_wifi_ap_secure(). Empty only if
// app_auth_init() has not run.
//
// **Deliberately NOT derived from the MAC or the SSID**, which is ticket 30's own rule and the
// reason this is a stored secret rather than a computed one: the SSID contains the MAC's low three
// bytes and is broadcast in every beacon, so any derivation published in an open-source firmware
// is a password anybody in radio range can compute.
//
// **And deliberately not seeded from a build flag either**, unlike the token. FRAME_API_TOKEN
// exists because this bench cannot read a pairing QR; a seeded AP password would not help it
// either way, since the bench PC cannot join a Wi-Fi network from a command line. So the only way to learn it is the panel, which is where ticket 30
// wants it.
void app_auth_ap_password(char *out, size_t size);

// Four characters and a length, for the boot log.
void app_auth_log_prefix(void);

// A new token AND a new access-point password, both persisted. Every paired browser is logged out
// and every joined phone stops being able to associate. FR-8.2's factory reset (ticket 16) should
// call this: a device handed to somebody else must not still trust the previous owner's phone.
//
// The new AP password does not reach the radio until the next boot -- board_wifi_ap_secure() is
// called once during init -- so a rotate leaves the running AP on the old one. That is deliberate:
// re-keying a live access point would drop whoever called this route, and the reply would go down
// with it (the same defect ticket 10 found in POST /api/wifi/config).
esp_err_t app_auth_rotate(void);

// --------------------------------------------------------------- the pairing window
//
// Ticket 66. A PC has no camera, so until this existed a desktop browser could not be paired at
// all: it got the page, a 401 on its first call, and a banner telling it to scan a QR code.
//
// **This is not a second authentication scheme, and the distinction is the whole argument.**
// Ticket 29 rejected a PIN because "a PIN needs rate limiting and a session table, an unlock
// window breaks every unattended client". What is below hands out the SAME permanent token to
// whoever can read the panel -- so there is no session, nothing expires for an already-paired
// client, and the rate limiting is one counter. The trust boundary is unchanged: physical access
// to the frame, since only a 5 s button hold opens a window.
//
// State is RAM only, on purpose. A reboot closes the window, and a code that survived one would
// be a credential nobody remembers is live.

// How long a window stays open. Longer than the three minutes the glass holds the card
// (FRAME_PAIRING_CLEAR_MS in frame_main.c), and that difference is deliberate: the card leaks to
// anybody in the room and should clear early, while the code only reaches whoever already read
// it -- and they may have to walk to another room to type it.
#ifndef FRAME_PAIRING_CODE_MS
#define FRAME_PAIRING_CODE_MS (10u * 60u * 1000u)
#endif

// Wrong codes allowed before the window closes and another button hold is needed. Five guesses
// against 2^40 is what makes the code short enough to read off a dithered panel.
#define APP_AUTH_PAIR_ATTEMPTS 5

// Mints a code and opens the window. Called by the TOP hold, beside the render of the card, so a
// code only ever exists while it is on the glass.
void app_auth_pairing_open(void);

// Closes it early. Nothing calls this on the happy path -- a successful claim closes its own
// window -- but a factory reset should.
void app_auth_pairing_close(void);

// True when a window is open and has not expired. Cheap; both the card and the route ask.
bool app_auth_pairing_is_open(void);

// The live code in its display form, "XXXX-XXXX", or "" when no window is open. For the panel
// and for nothing else: it must not reach a log or an API response.
void app_auth_pairing_code_display(char *out, size_t size);

typedef enum {
    APP_AUTH_PAIR_OK = 0,        // the token is yours; the window is now closed
    APP_AUTH_PAIR_WRONG,         // wrong code, attempts remain
    APP_AUTH_PAIR_NO_WINDOW,     // nothing open, or it expired, or the attempts ran out
    APP_AUTH_PAIR_MALFORMED,     // not even the shape of a code
} app_auth_pair_result_t;

// Checks what the user typed. On APP_AUTH_PAIR_OK the window is closed, the device is marked as
// having paired at least once, and `token_out` holds the API token.
//
// `attempts_left` is written on every outcome so a caller can say how many are left without
// asking a second question and racing.
app_auth_pair_result_t app_auth_pairing_claim(const char *typed, char *token_out,
                                              size_t token_size, int *attempts_left);

// ------------------------------------------------------------- has anything ever paired
//
// One NVS flag, so a factory-fresh frame can draw the connect card by itself at boot: a user
// with only a PC has no way to guess that a five-second button hold is what they need, and the
// panel is the only surface the device has before anyone is paired.
//
// Set by any successful pairing -- a ?t= that mints a cookie, or a claimed code -- and CLEARED by
// app_auth_rotate(), because a rotate is FR-8.2's "this device no longer belongs to that person"
// and the next owner is exactly the user this card exists for.
bool app_auth_paired_once(void);
void app_auth_mark_paired(void);

#endif // APP_AUTH_H
