#include "app_auth.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "auth";

// Still the old product name after the 2026-09-17 rename to PaperFrame, and deliberately: this
// namespace holds the API token and the AP password, so renaming it un-pairs every browser and
// every phone at once. app_settings.c carries the argument. Nothing outside the flash sees it.
#define NVS_NAMESPACE "papercolor"
// NVS keys are limited to 15 characters. This one is new -- the stock firmware never
// reads it, so unlike the FR-8 settings it carries no compatibility constraint.
#define NVS_KEY "api_token"
// Ticket 30 step 2. Same namespace, same 15-character limit, same "the stock firmware never reads
// it" freedom.
#define NVS_KEY_AP_PASS "ap_password"
// Ticket 66. 11 characters, inside NVS's 15.
#define NVS_KEY_PAIRED "paired_once"

#define TOKEN_BYTES 16

// WPA2's floor, which board_wifi_ap_secure() enforces. The AP password is exactly this long since
// ticket 66 -- the operator asked for the minimum, because it is typed by hand -- so a change to the
// pairing code's length must not silently take the passphrase under it.
#define WPA2_MIN_PASSPHRASE 8
_Static_assert(PAIR_CODE_LEN >= WPA2_MIN_PASSPHRASE,
               "the AP password is PAIR_CODE_LEN characters and WPA2 refuses fewer than 8");

static char s_token[APP_AUTH_TOKEN_HEX_SIZE];
static char s_ap_pass[APP_AUTH_AP_PASS_SIZE];

// The pairing window (ticket 66). RAM only: a reboot closes it, which is what we want -- a code
// that outlived a power cycle would be a live credential nobody remembers showing.
//
// **No mutex, and that is a decision rather than an omission.** The window is opened on the
// application task (the button hold) and claimed on the httpd task, seconds apart at best, since a
// person has to read the panel in between. The only reachable race is a claim that reads a code
// mid-mint, which cannot make a wrong code match -- it can only waste one of the five attempts.
// The rest of this module is lock-free on the same argument: s_token is written once at init.
static char s_pair_code[PAIR_CODE_SIZE];
static int64_t s_pair_deadline_us;
static int s_pair_attempts_left;

static void to_hex(const uint8_t *bytes, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

static void mint(char *out, size_t bytes)
{
    uint8_t raw[TOKEN_BYTES];
    if (bytes > sizeof(raw)) {
        bytes = sizeof(raw);
    }
    // Not esp_random() in a loop: esp_fill_random() is the documented whole-buffer call,
    // and the RF subsystem is up by the time this runs (see the header).
    esp_fill_random(raw, bytes);
    to_hex(raw, bytes, out);
}

// The access point's password: 8 Crockford base32 characters, which is the same encoder the pairing
// code uses (src/core/pair_code.h) and for the same reason -- a short secret a person reads off the
// glass and types. See app_auth.h for why the alphabet matters more than the length here, and why
// Crockford's *input* folding deliberately does not come with it.
//
// Returns false only on an RNG or encoder failure, which cannot happen with fixed buffers; the
// caller treats it as "do not persist this".
static bool mint_ap_pass(char *out, size_t size)
{
    uint8_t raw[PAIR_CODE_BYTES];
    esp_fill_random(raw, sizeof(raw));
    return pair_code_from_bytes(raw, sizeof(raw), out, size);
}

// True when a stored password is in the form this firmware now mints. Anything else is from before
// 2026-09-19 -- 16 lowercase hex characters -- and gets re-keyed once, loudly.
//
// **Checked by shape rather than by a stored version number**, because the shape is the thing that
// matters to the person typing it and because a version key would be a second thing to keep in step.
static bool ap_pass_is_current(const char *pass)
{
    if (strlen(pass) != PAIR_CODE_LEN) {
        return false;
    }
    for (const char *p = pass; *p; p++) {
        if (strchr(PAIR_CODE_ALPHABET, *p) == NULL) {
            return false;
        }
    }
    return true;
}

static esp_err_t persist(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

// True when `out` came back from NVS non-empty.
static bool load(const char *key, char *out, size_t size)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = size;
    const esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err == ESP_OK && out[0]) {
        return true;
    }
    out[0] = '\0';
    return false;
}

// The access point's password, loaded or minted. Separate from the token's own path below
// because the two are independent: a device that has been running since before ticket 30 step 2
// has a stored token and no stored AP password, and must mint the one without disturbing the
// other. Not doing this would re-key the API on the boot that first closes the AP.
static void init_ap_password(void)
{
    // **Big enough for the PRE-2026-09-19 form**, 16 hex characters, so a stored password can be
    // read and then judged. Loading straight into s_ap_pass would make an old one indistinguishable
    // from no password at all -- nvs_get_str() refuses a short buffer without writing it -- so the
    // re-key below would happen by accident, silently, which is not the same thing as happening on
    // purpose. This buffer can shrink again once no device in the world holds the old form, which
    // is not a thing this project can know.
    char stored[17];
    if (load(NVS_KEY_AP_PASS, stored, sizeof(stored))) {
        if (ap_pass_is_current(stored)) {
            // memcpy of an exact length, not snprintf: the check above has established that
            // `stored` is PAIR_CODE_LEN characters, so this is the string and its terminator --
            // and -Werror=format-truncation cannot see that and refuses the snprintf.
            memcpy(s_ap_pass, stored, PAIR_CODE_LEN + 1);
            ESP_LOGI(TAG, "ap password loaded from NVS");
            return;
        }
        // Ticket 66: the operator asked for the shortest typeable password, so a device provisioned
        // before that carries 16 hex characters and is re-keyed ONCE -- after which the shape check
        // passes and this never fires again. **Said out loud because it is the only thing in this
        // module that invalidates something outside the device: a phone that had joined this access
        // point has to join again.** On the two bench units none ever had; ticket 30's phone
        // acceptance is still open.
        ESP_LOGW(TAG, "ap password was %u chars, not the current %d -- re-keying ONCE (ticket 66); "
                      "a phone that had joined must join again",
                 (unsigned)strlen(stored), PAIR_CODE_LEN);
    }

    if (!mint_ap_pass(s_ap_pass, sizeof(s_ap_pass))) {
        // Cannot happen with fixed buffers, and it leaves the AP OPEN rather than half-secured:
        // board_wifi_ap_secure() refuses a password outside WPA2's 8-63, and the card says
        // NO PASSWORD when there is none. Loud, because both of those are visible states.
        ESP_LOGE(TAG, "ap password not minted -- the access point will stay open");
        s_ap_pass[0] = '\0';
        return;
    }

    const esp_err_t err = persist(NVS_KEY_AP_PASS, s_ap_pass);
    if (err != ESP_OK) {
        // Louder than the token's equivalent, because the consequence is worse: an AP password
        // that changes on the next boot is a phone that silently stops being able to join, and
        // FR-2.1's access point is the recovery path for a frame with no station link.
        ESP_LOGW(TAG, "ap password not persisted: %s -- the AP will re-key on the next boot",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "ap password minted");
    }
}

esp_err_t app_auth_init(void)
{
    // nvs_flash_init() has already run in app_settings_init(); this only opens the
    // namespace it created.
    init_ap_password();

    if (load(NVS_KEY, s_token, sizeof(s_token))) {
        ESP_LOGI(TAG, "token loaded from NVS");
        app_auth_log_prefix();
        return ESP_OK;
    }

    // A build seed, for the bench. This PC cannot join the frame's access point at all
    // (the location permission netsh needs is denied by group policy), so it cannot read
    // a pairing QR, so an unattended run here would have no way to learn a randomly
    // minted token. FRAME_API_TOKEN comes out of git-ignored platformio_local.ini,
    // exactly as FRAME_WIFI_SSID does in frame_main.c -- and like it, it is a fallback
    // for a device with nothing stored, never an override.
#ifdef FRAME_API_TOKEN
    strncpy(s_token, FRAME_API_TOKEN, sizeof(s_token) - 1);
    s_token[sizeof(s_token) - 1] = '\0';
    ESP_LOGI(TAG, "token seeded from build flags");
#else
    mint(s_token, TOKEN_BYTES);
    ESP_LOGI(TAG, "token minted");
#endif

    const esp_err_t perr = persist(NVS_KEY, s_token);
    if (perr != ESP_OK) {
        // Said out loud rather than returned: the token works for this boot either way,
        // and a web server that refused to start because NVS was full would be a much
        // worse outcome than one that has to be re-paired after a reboot.
        ESP_LOGW(TAG, "token not persisted: %s -- it will change on the next boot",
                 esp_err_to_name(perr));
    }
    app_auth_log_prefix();
    return ESP_OK;
}

bool app_auth_check(const char *candidate)
{
    if (!candidate || !s_token[0]) {
        return false;
    }
    // Fixed trip count over the stored token, so the loop's length says nothing about
    // how much of the candidate matched. A candidate shorter than the token compares
    // its terminator against the rest, which cannot match.
    const size_t n = strlen(s_token);
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)candidate[i];
        diff |= (unsigned char)(c ^ (unsigned char)s_token[i]);
        if (c == '\0') {
            // Stop reading past the end of the candidate, but keep the difference: a
            // short candidate has already accumulated a mismatch on this byte.
            diff |= 1;
            break;
        }
    }
    // A longer candidate whose prefix matches is still wrong.
    return diff == 0 && candidate[n] == '\0';
}

void app_auth_token_hex(char *out, size_t size)
{
    if (!out || size == 0) {
        return;
    }
    strncpy(out, s_token, size - 1);
    out[size - 1] = '\0';
}

void app_auth_ap_password(char *out, size_t size)
{
    if (!out || size == 0) {
        return;
    }
    strncpy(out, s_ap_pass, size - 1);
    out[size - 1] = '\0';
}

// A 16-bit FNV-1a of a secret, printed instead of a prefix. Not a security primitive -- it is a
// label, and 65,536 of them collide easily -- but it answers "is the live password the same one as
// last boot" without publishing any of it.
static uint16_t fingerprint(const char *s)
{
    uint32_t h = 2166136261u;
    for (const char *p = s; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    return (uint16_t)((h >> 16) ^ h);
}

void app_auth_log_prefix(void)
{
    // **The token keeps its four-character prefix and the AP password no longer has one.** The
    // prefix was four for both on the rule "enough to tell which secret is live, not enough to
    // use", and the AP password being 16 characters made four of them a quarter of it. Ticket 66
    // took it to 8, where four characters would be HALF the secret in a log file that gets
    // committed. So it gets a fingerprint instead, which identifies it and reveals nothing.
    //
    // The token stays a prefix because a real check depends on it: ticket 29's acceptance compares
    // this line against `-DFRAME_API_TOKEN` in platformio_local.ini to prove the build seed
    // reached the binary, and four of 32 characters is an eighth.
    printf("# api token %.4s... (%u chars), ap password fp=%04x (%u chars)\n", s_token,
           (unsigned)strlen(s_token), fingerprint(s_ap_pass), (unsigned)strlen(s_ap_pass));
}

esp_err_t app_auth_rotate(void)
{
    mint(s_token, TOKEN_BYTES);
    const esp_err_t err = persist(NVS_KEY, s_token);

    // Both, because FR-8.2's factory reset is "this device no longer belongs to that person" and
    // a rotated token beside the old AP password would leave the previous owner's phone still
    // able to associate. See the header for why the radio keeps the old one until a reboot.
    esp_err_t aerr = ESP_FAIL;
    if (mint_ap_pass(s_ap_pass, sizeof(s_ap_pass))) {
        aerr = persist(NVS_KEY_AP_PASS, s_ap_pass);
    } else {
        s_ap_pass[0] = '\0';
    }

    // And the device is unpaired again, so the next boot draws the connect card: the person in
    // front of it is the one this card exists for. A live window is closed for the same reason --
    // it was opened for the previous owner.
    app_auth_pairing_close();
    persist(NVS_KEY_PAIRED, "");

    app_auth_log_prefix();
    return err != ESP_OK ? err : aerr;
}

// ---------------------------------------------------------------- the pairing window

void app_auth_pairing_open(void)
{
    uint8_t raw[PAIR_CODE_BYTES];
    // Same RNG and the same "after the radio is up" argument as the token: this runs on the
    // button hold, long after app_auth_init().
    esp_fill_random(raw, sizeof(raw));

    if (!pair_code_from_bytes(raw, sizeof(raw), s_pair_code, sizeof(s_pair_code))) {
        // Cannot happen with a fixed buffer and a fixed byte count, and said out loud rather than
        // returned because the caller's next act is a 15 s refresh of a card with no code on it.
        ESP_LOGW(TAG, "pairing code not minted");
        app_auth_pairing_close();
        return;
    }
    s_pair_deadline_us = esp_timer_get_time() + (int64_t)FRAME_PAIRING_CODE_MS * 1000;
    s_pair_attempts_left = APP_AUTH_PAIR_ATTEMPTS;

    // The code itself is NOT logged, for the reason the AP password is not: this console output
    // ends up in capture files under .scratch/, and `.scratch/captures/*.log` is not git-ignored.
    // Unlike the token there is no prefix either -- four of eight symbols is half of a short secret.
    printf("# pair window open: %u s, %d attempts\n", (unsigned)(FRAME_PAIRING_CODE_MS / 1000u),
           s_pair_attempts_left);

#ifdef FRAME_PAIR_CODE_LOG
    // BENCH ONLY, and it exists for exactly the reason FRAME_API_TOKEN does (see the note in
    // app_auth_init()): this PC cannot read the panel. It has no camera on the glass, the flatbed
    // needs somebody to switch it on, and it cannot join the frame's access point at all -- so
    // without this, the one route that the whole of ticket 66 is about could never be exercised from
    // here. Out of git-ignored platformio_local.ini, never from platformio.ini, and the line says so
    // where somebody reading a log will see it.
    {
        char shown[PAIR_CODE_DISPLAY_SIZE];
        app_auth_pairing_code_display(shown, sizeof(shown));
        printf("# pair code (BENCH BUILD -- FRAME_PAIR_CODE_LOG, never ship): %s\n", shown);
    }
#endif
    fflush(stdout);
}

void app_auth_pairing_close(void)
{
    memset(s_pair_code, 0, sizeof(s_pair_code));
    s_pair_deadline_us = 0;
    s_pair_attempts_left = 0;
}

bool app_auth_pairing_is_open(void)
{
    if (!s_pair_code[0] || s_pair_attempts_left <= 0) {
        return false;
    }
    if (esp_timer_get_time() >= s_pair_deadline_us) {
        // Wiped on the way past rather than left to rot: an expired code sitting in RAM is a
        // string a future bug can still compare against.
        app_auth_pairing_close();
        return false;
    }
    return true;
}

void app_auth_pairing_code_display(char *out, size_t size)
{
    if (!out || size == 0) {
        return;
    }
    out[0] = '\0';
    if (!app_auth_pairing_is_open()) {
        return;
    }
    pair_code_format(s_pair_code, out, size);
}

app_auth_pair_result_t app_auth_pairing_claim(const char *typed, char *token_out,
                                              size_t token_size, int *attempts_left)
{
    if (attempts_left) {
        *attempts_left = 0;
    }
    if (!app_auth_pairing_is_open()) {
        return APP_AUTH_PAIR_NO_WINDOW;
    }
    if (attempts_left) {
        *attempts_left = s_pair_attempts_left;
    }

    char canonical[PAIR_CODE_SIZE];
    if (!pair_code_normalise(typed, canonical, sizeof(canonical))) {
        // Not counted as an attempt. A string that is not even the shape of a code carries no
        // information about the live one, and spending an attempt on it would let a slip of the
        // keyboard close a window the user then has to walk back to the frame to reopen.
        return APP_AUTH_PAIR_MALFORMED;
    }

    // app_auth_check()'s shape: a fixed trip count over the whole code, so the loop's length says
    // nothing about how much of the candidate matched. Both strings are known to be exactly
    // PAIR_CODE_LEN by here -- the normaliser refuses anything else -- so there is no terminator
    // case to carry.
    unsigned char diff = 0;
    for (size_t i = 0; i < PAIR_CODE_LEN; i++) {
        diff |= (unsigned char)((unsigned char)canonical[i] ^ (unsigned char)s_pair_code[i]);
    }

    if (diff != 0) {
        s_pair_attempts_left--;
        if (attempts_left) {
            *attempts_left = s_pair_attempts_left;
        }
        if (s_pair_attempts_left <= 0) {
            app_auth_pairing_close();
        }
        return APP_AUTH_PAIR_WRONG;
    }

    // One code, one browser: the window closes on success, so a code read off the glass by two
    // people is not two ways in.
    app_auth_pairing_close();
    app_auth_mark_paired();
    app_auth_token_hex(token_out, token_size);
    return APP_AUTH_PAIR_OK;
}

// -------------------------------------------------------------- has anything ever paired

bool app_auth_paired_once(void)
{
    char flag[4];
    return load(NVS_KEY_PAIRED, flag, sizeof(flag));
}

void app_auth_mark_paired(void)
{
    if (app_auth_paired_once()) {
        return;  // one write per device, not one per pairing
    }
    const esp_err_t err = persist(NVS_KEY_PAIRED, "1");
    if (err != ESP_OK) {
        // Harmless on its own: the cost of losing this is a connect card at the next boot that
        // nobody needed, which is fifteen seconds and no lost state.
        ESP_LOGW(TAG, "paired flag not persisted: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "marked as paired");
    }
}
