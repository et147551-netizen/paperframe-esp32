// The Google Photos scrape rules on the device: which set is in force, a newer one on trial, where
// rule files come from, and whether their signature holds.
//
// Designed in the scrape-rules plan. The operator's rules of 2026-09-13:
//
//   * A RULE FILE IS SIGNED (ECDSA P-256 over SHA-256, the public key compiled in from
//     gphotos_rules_pubkey.h) and one that does not verify is never used, however it arrived.
//     Line one is the base64 DER signature over exactly the bytes after that line, which are JSON.
//   * IT ARRIVES TWO WAYS: fetched from a URL (a build default, overridable in NVS) before each
//     album run, or uploaded through POST /api/gphotos/config. Both are only decoded on the gphotos
//     task: a verify and a cJSON parse do not belong on httpd's or main's stack.
//   * seq ONLY GOES UP. A file whose seq is not above the one in force, the one on trial and the
//     highest one refused is ignored, so an old signed file cannot be replayed.
//   * A NEWER SET IS ON TRIAL until an album run decides it: adopted (and kept in NVS) when any
//     album read whole with a photograph in it; refused for good when an album read whole and none
//     found one; still on trial when no album read whole, since the network is not the rules'
//     fault. Until adopted, photographs are fetched with the set in force.
//   * Nothing stored means the built-in set (gphotos_rules_default()), which is today's rule.

#ifndef APP_GPHOTOS_RULES_H
#define APP_GPHOTOS_RULES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gphotos_rules.h"

// Room for the rule URL, NUL included.
#define APP_GPHOTOS_RULES_URL_SIZE 256

typedef enum {
    APP_GPRULES_SRC_BUILTIN = 0,
    APP_GPRULES_SRC_URL,
    APP_GPRULES_SRC_UPLOAD,
} app_gprules_source_t;

typedef struct {
    bool key_compiled_in;
    uint32_t adopted_seq;                 // GPHOTOS_RULES_BUILTIN_SEQ: the built-in set
    app_gprules_source_t adopted_source;
    uint32_t candidate_seq;               // 0: none on trial
    app_gprules_source_t candidate_source;
    uint32_t rejected_seq;                // the highest seq refused after a trial, 0 none
    const char *last_check;               // what the last rule file came to; a literal
    const char *last_reason;              // why, when refused; a literal, "" otherwise
    int last_http;                        // the rule URL's last HTTP status, 0 none
    int64_t last_check_ms;                // uptime, -1 never
} app_gprules_status_t;

// Main task, once, before anything else here: the lock and the built-in set. Cheap.
void app_gphotos_rules_init(void);

// The gphotos task, once: the set kept in NVS, verified again.
void app_gphotos_rules_load(void);

// The rule URL: NVS's, else the build's -DGPHOTOS_RULES_URL. False when there is none.
bool app_gphotos_rules_get_url(char *out, size_t size, bool *is_default);
// "" goes back to the build's.
esp_err_t app_gphotos_rules_set_url(const char *url);

// httpd: queue an uploaded rule file for the gphotos task. Nothing is decoded here.
esp_err_t app_gphotos_rules_submit(const char *text, size_t len);

// The gphotos task, before an album run.
void app_gphotos_rules_consider_upload(void);
void app_gphotos_rules_consider(const char *text, size_t len, app_gprules_source_t src, int http);
void app_gphotos_rules_fetch_failed(int http);

// The set an album run reads with: the one on trial if there is one (returns true), else the one in
// force.
bool app_gphotos_rules_for_read(gphotos_rules_t *out);
// After that run: did any album read whole, and did any read whole with a photograph in it.
void app_gphotos_rules_verdict(bool any_whole, bool any_ok);

// A photograph's URL under the set in force. False when no pattern has that id.
bool app_gphotos_rules_photo_url(const char *id, const char *key, unsigned edge, char *out,
                                 size_t size);
void app_gphotos_rules_ua(char *out, size_t size);

void app_gphotos_rules_get_status(app_gprules_status_t *out);
const char *app_gphotos_rules_source_str(app_gprules_source_t s);

#endif // APP_GPHOTOS_RULES_H
