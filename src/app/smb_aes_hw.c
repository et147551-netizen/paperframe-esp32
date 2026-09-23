// libsmb2's per-block AES, on the ESP32-S3's AES peripheral instead of in portable C.
//
// Ticket .scratch/digital-frame/issues/33. This server requires SMB signing, so libsmb2
// verifies the signature of every received PDU (lib/socket.c:567-582), and at an SMB 3.x
// dialect that is AES-128-CMAC over the whole PDU -- 261 AES blocks for a 4 KB read reply
// (lib/smb2-signing.c:119-160). Measured on hardware: 79.9 us per block, 19,176 CPU cycles
// at 240 MHz, which is 82 % of what a 4 KB read costs. The reason is in one place:
//
//     void AES128_ECB_encrypt(uint8_t* input, const uint8_t* key, uint8_t* output)
//     {
//       BlockCopy(output, input);
//       uint8_t roundKey[176];
//       KeyExpansion(key, roundKey);        // <- per 16-byte block
//       Cipher(roundKey, (state_t*)output);
//     }
//                                             -- lib/aes.c:444-456
//
// The key is the session signing key and does not change within a session, so the expansion
// is recomputed 261 times per read reply for no reason at all. And CONFIG_MBEDTLS_HARDWARE_AES
// is already y in every env here.
//
// WHY A LINKER WRAP AND NOT A PATCH. libsmb2 is a managed component fetched at build time
// (sahlberg/libsmb2 3.0.1, the only version on the registry, four years old), so editing it
// means either vendoring the library or carrying a patch step. The symbol is defined in
// lib/aes.c and called from lib/smb2-signing.c, a different translation unit, so
// `-Wl,--wrap=AES128_ECB_encrypt` intercepts every call the signing path makes without
// touching a vendored file. That flag is a SINGLE token, which matters: PlatformIO's espidf
// builder sorts the flag list, so a two-token flag never survives (src/CMakeLists.txt).
//
// A GREEN BUILD IS NOT EVIDENCE THE WRAP TOOK. PlatformIO re-does the link in SCons from
// CMake's file API and is known not to carry every kind of CMake target across -- the
// esp_mmap_assets case in docs/build-system.md is a UTILITY target that silently never
// ran. So this file
// checks the artifact rather than the exit code: it verifies itself against the FIPS-197
// AES-128 vector on first use and prints one line saying so. If that line is absent from a
// capture, the wrap did not happen and any timing taken from that run is the unwrapped path.
//
// Thread safety: the cached key schedule is a single static context. Every AES call libsmb2
// makes happens inside board_smb.c's own mutex, on whichever task is using the share, and
// only one task uses it at a time. Nothing else in this project uses mbedTLS AES.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "mbedtls/aes.h"

#include "smb_aes_hw.h"

static const char *TAG = "smbaes";

// The wrapped-away original, still in the archive and reachable by this name. It is what
// makes an A/B possible inside one run: see smb_aes_hw.h.
void __real_AES128_ECB_encrypt(uint8_t *input, const uint8_t *key, uint8_t *output);

static bool s_enabled = true;

void smb_aes_hw_set_enabled(bool on) { s_enabled = on; }
bool smb_aes_hw_enabled(void) { return s_enabled; }

#define AES_KEY_BYTES 16
#define AES_BLOCK_BYTES 16

static mbedtls_aes_context s_ctx;
static uint8_t s_key[AES_KEY_BYTES];
static bool s_have_key;
static bool s_checked;

// FIPS-197 appendix C.1: the AES-128 example. If the wrap is in place this is the first
// thing it computes, and a wrong answer here is a broken signature on every PDU afterwards,
// which is worth failing loudly and early rather than as "the share stopped working".
static void self_check(void)
{
    static const uint8_t key[AES_KEY_BYTES] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                               0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
                                               0x0c, 0x0d, 0x0e, 0x0f};
    static const uint8_t plain[AES_BLOCK_BYTES] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
                                                   0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
                                                   0xcc, 0xdd, 0xee, 0xff};
    static const uint8_t expect[AES_BLOCK_BYTES] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b,
                                                    0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80,
                                                    0x70, 0xb4, 0xc5, 0x5a};
    uint8_t got[AES_BLOCK_BYTES] = {0};

    // A context of its own, so the check cannot disturb the cached session key.
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    const int rc = mbedtls_aes_setkey_enc(&ctx, key, 128);
    const int rc2 = rc ? rc : mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, plain, got);
    mbedtls_aes_free(&ctx);

    if (rc2 != 0 || memcmp(got, expect, sizeof(expect)) != 0) {
        ESP_LOGE(TAG, "FIPS-197 self-check FAILED (rc=%d) -- SMB signatures will be wrong",
                 rc2);
        return;
    }
    ESP_LOGI(TAG, "hardware AES wrap active, FIPS-197 self-check ok");
}

void __wrap_AES128_ECB_encrypt(uint8_t *input, const uint8_t *key, uint8_t *output)
{
    if (!s_enabled) {
        __real_AES128_ECB_encrypt(input, key, output);
        return;
    }

    if (!s_checked) {
        s_checked = true; // before the check, which must not recurse into here
        self_check();
    }

    if (!s_have_key || memcmp(s_key, key, AES_KEY_BYTES) != 0) {
        if (s_have_key) {
            mbedtls_aes_free(&s_ctx);
        }
        mbedtls_aes_init(&s_ctx);
        if (mbedtls_aes_setkey_enc(&s_ctx, key, 128) != 0) {
            ESP_LOGE(TAG, "setkey failed");
            return;
        }
        memcpy(s_key, key, AES_KEY_BYTES);
        s_have_key = true;
    }

    // A local block because the original works in place and a caller is free to pass the
    // same pointer twice. libsmb2's own callers do not, today.
    uint8_t block[AES_BLOCK_BYTES];
    memcpy(block, input, sizeof(block));
    if (mbedtls_aes_crypt_ecb(&s_ctx, MBEDTLS_AES_ENCRYPT, block, output) != 0) {
        ESP_LOGE(TAG, "ecb failed");
    }
}
