#include "app_gphotos_rules.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "gphotos_rules_pubkey.h"

static const char *TAG = "gprules";

// Its own namespace, not app_settings' "papercolor": nothing here belongs in app_settings_t, which
// is copied whole onto main's stack (docs/board-and-storage.md).
#define NVS_NS "gprules"
#define KEY_TEXT "text"  // the adopted file, signature line included
#define KEY_SRC "src"
#define KEY_BAD "bad"
#define KEY_URL "url"

// The default rule URL. A secret Gist's raw URL is a capability like an album link, so it lives in
// git-ignored platformio_local.ini and is never logged.
#ifndef GPHOTOS_RULES_URL
#define GPHOTOS_RULES_URL ""
#endif

static SemaphoreHandle_t s_mutex;
// All in PSRAM: a rule set is ~1.5 KB and internal RAM is the scarce resource.
static gphotos_rules_t *s_adopted;
static gphotos_rules_t *s_candidate;
static gphotos_rules_t *s_scratch;  // the task's alone
static char *s_candidate_text;
static size_t s_candidate_len;
static char *s_upload;
static size_t s_upload_len;
static bool s_upload_pending;
static bool s_loaded;

static app_gprules_status_t s_status = {
    .last_check = "none",
    .last_reason = "",
    .last_check_ms = -1,
};

static void lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

static bool refuse(const char **why, const char *msg)
{
    *why = msg;
    return false;
}

const char *app_gphotos_rules_source_str(app_gprules_source_t s)
{
    switch (s) {
    case APP_GPRULES_SRC_BUILTIN:
        return "builtin";
    case APP_GPRULES_SRC_URL:
        return "url";
    case APP_GPRULES_SRC_UPLOAD:
        return "upload";
    }
    return "unknown";
}

void app_gphotos_rules_init(void)
{
    if (s_mutex) {
        return;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return;
    }
    s_adopted = heap_caps_calloc(1, sizeof(gphotos_rules_t), MALLOC_CAP_SPIRAM);
    s_candidate = heap_caps_calloc(1, sizeof(gphotos_rules_t), MALLOC_CAP_SPIRAM);
    s_scratch = heap_caps_calloc(1, sizeof(gphotos_rules_t), MALLOC_CAP_SPIRAM);
    s_candidate_text = heap_caps_malloc(GPHOTOS_RULES_FILE_MAX, MALLOC_CAP_SPIRAM);
    s_upload = heap_caps_malloc(GPHOTOS_RULES_FILE_MAX, MALLOC_CAP_SPIRAM);
    if (s_adopted) {
        gphotos_rules_default(s_adopted);
    }
    // The floor a rule file has to clear, before NVS is read.
    s_status.adopted_seq = GPHOTOS_RULES_BUILTIN_SEQ;
    s_status.key_compiled_in = GPHOTOS_RULES_PUBKEY_PEM[0] != '\0';
    if (!s_adopted || !s_candidate || !s_scratch || !s_candidate_text || !s_upload) {
        ESP_LOGE(TAG, "no PSRAM for the rule sets; only the built-in rules can be used");
    }
}

// ---------------------------------------------------------------------------- decoding

static const char *str(const cJSON *o, const char *name)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem(o, name));
}

static bool from_json(const char *json, size_t len, gphotos_rules_t *r, const char **why)
{
    memset(r, 0, sizeof(*r));
    cJSON *j = cJSON_ParseWithLength(json, len);
    if (!j) {
        return refuse(why, "the rules are not JSON");
    }
    bool ok = false;
    const cJSON *v = cJSON_GetObjectItem(j, "v");
    const cJSON *seq = cJSON_GetObjectItem(j, "seq");
    const cJSON *pats = cJSON_GetObjectItem(j, "patterns");
    const cJSON *pt = NULL;
    if (!cJSON_IsNumber(v) || v->valuedouble != 1) {
        refuse(why, "v is not 1");
        goto out;
    }
    if (!cJSON_IsNumber(seq) || seq->valuedouble < 1 || seq->valuedouble > 4294967295.0 ||
        seq->valuedouble != (double)(uint32_t)seq->valuedouble) {
        refuse(why, "seq is not a whole number from 1");
        goto out;
    }
    r->seq = (uint32_t)seq->valuedouble;
    if (!gphotos_rules_set_ua(r, str(j, "ua"), why)) {
        goto out;
    }
    if (!cJSON_IsArray(pats) || cJSON_GetArraySize(pats) > GPHOTOS_RULES_MAX_PATTERNS) {
        refuse(why, "patterns is not an array of at most 4");
        goto out;
    }
    cJSON_ArrayForEach(pt, pats)
    {
        const cJSON *key = cJSON_GetObjectItem(pt, "key");
        const cJSON *max = cJSON_GetObjectItem(key, "max");
        const cJSON *after = cJSON_GetObjectItem(pt, "after");
        if ((after && !cJSON_IsArray(after)) || cJSON_GetArraySize(after) > GPHOTOS_AFTER_MAX) {
            refuse(why, "after is not an array of at most 8 tokens");
            goto out;
        }
        const char *toks[GPHOTOS_AFTER_MAX];
        size_t n = 0;
        const cJSON *tk = NULL;
        cJSON_ArrayForEach(tk, after)
        {
            toks[n++] = cJSON_GetStringValue(tk);
        }
        const unsigned key_max = (cJSON_IsNumber(max) && max->valuedouble >= 0 &&
                                  max->valuedouble < GPHOTOS_KEY_MAX)
                                     ? (unsigned)max->valuedouble
                                     : 0;
        if (!gphotos_rules_add_pattern(r, str(pt, "id"), str(pt, "anchor"), str(key, "chars"),
                                       key_max, toks, n, str(pt, "url"), why)) {
            goto out;
        }
    }
    ok = gphotos_rules_check(r, why);
out:
    cJSON_Delete(j);
    return ok;
}

static bool decode(const char *text, size_t len, gphotos_rules_t *out, const char **why)
{
    if (len == 0 || len > GPHOTOS_RULES_FILE_MAX) {
        return refuse(why, "the file is empty or larger than 4 KB");
    }
    if (GPHOTOS_RULES_PUBKEY_PEM[0] == '\0') {
        return refuse(why, "no public key is compiled into this firmware");
    }
    const char *nl = memchr(text, '\n', len);
    if (!nl) {
        return refuse(why, "no signature line");
    }
    size_t b64 = (size_t)(nl - text);
    if (b64 > 0 && text[b64 - 1] == '\r') {
        b64--;
    }
    unsigned char sig[160];
    size_t sig_len = 0;
    if (b64 == 0 ||
        mbedtls_base64_decode(sig, sizeof(sig), &sig_len, (const unsigned char *)text, b64) != 0) {
        return refuse(why, "the signature line is not base64");
    }
    const unsigned char *payload = (const unsigned char *)nl + 1;
    const size_t plen = len - (size_t)(payload - (const unsigned char *)text);
    unsigned char hash[32];
    if (mbedtls_sha256(payload, plen, hash, 0) != 0) {
        return refuse(why, "could not hash the rules");
    }
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)GPHOTOS_RULES_PUBKEY_PEM,
                                         sizeof(GPHOTOS_RULES_PUBKEY_PEM));
    if (rc != 0) {
        mbedtls_pk_free(&pk);
        ESP_LOGE(TAG, "the compiled-in public key does not parse: -0x%04x", (unsigned)-rc);
        return refuse(why, "the compiled-in public key does not parse");
    }
    rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sig_len);
    mbedtls_pk_free(&pk);
    if (rc != 0) {
        return refuse(why, "the signature does not verify");
    }
    return from_json((const char *)payload, plen, out, why);
}

// --------------------------------------------------------------------------------- NVS

static void nvs_store_adopted(const char *text, size_t len, app_gprules_source_t src)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, KEY_TEXT, text, len);
        if (err == ESP_OK) {
            err = nvs_set_u8(h, KEY_SRC, (uint8_t)src);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adopted rules not kept in NVS: %s", esp_err_to_name(err));
        s_status.last_reason = "adopted, but NVS could not keep them; a reboot returns to the "
                               "previous rules";
    }
}

static void nvs_store_bad(uint32_t seq)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u32(h, KEY_BAD, seq);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "refused seq not kept in NVS: %s", esp_err_to_name(err));
    }
}

void app_gphotos_rules_load(void)
{
    if (s_loaded || !s_mutex || !s_scratch) {
        return;
    }
    s_loaded = true;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "nothing stored; the built-in rules are in force");
        return;
    }
    uint32_t bad = 0;
    uint8_t src = APP_GPRULES_SRC_BUILTIN;
    nvs_get_u32(h, KEY_BAD, &bad);
    nvs_get_u8(h, KEY_SRC, &src);
    char *text = heap_caps_malloc(GPHOTOS_RULES_FILE_MAX, MALLOC_CAP_SPIRAM);
    size_t n = GPHOTOS_RULES_FILE_MAX;
    const bool have = text && nvs_get_blob(h, KEY_TEXT, text, &n) == ESP_OK;
    nvs_close(h);

    const char *why = "";
    bool ok = have && decode(text, n, s_scratch, &why);
    heap_caps_free(text);
    // Ticket 61: a firmware update can carry a newer built-in set than the file adopted under the
    // old firmware, and the older one must not win just for being stored.
    if (ok && s_scratch->seq <= GPHOTOS_RULES_BUILTIN_SEQ) {
        ok = false;
        why = "not above the built-in set's seq";
    }
    lock();
    s_status.rejected_seq = bad;
    if (ok && s_adopted) {
        *s_adopted = *s_scratch;
        s_status.adopted_seq = s_adopted->seq;
        s_status.adopted_source = (app_gprules_source_t)src;
    }
    unlock();
    if (ok) {
        ESP_LOGI(TAG, "rules seq %u (%s) from NVS", (unsigned)s_scratch->seq,
                 app_gphotos_rules_source_str((app_gprules_source_t)src));
    } else if (have) {
        ESP_LOGW(TAG, "stored rules refused (%s); the built-in rules are in force", why);
    }
}

bool app_gphotos_rules_get_url(char *out, size_t size, bool *is_default)
{
    out[0] = '\0';
    bool dflt = true;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = size;
        if (nvs_get_str(h, KEY_URL, out, &n) == ESP_OK && out[0] != '\0') {
            dflt = false;
        } else {
            out[0] = '\0';
        }
        nvs_close(h);
    }
    if (dflt) {
        snprintf(out, size, "%s", GPHOTOS_RULES_URL);
    }
    if (is_default) {
        *is_default = dflt;
    }
    return out[0] != '\0';
}

esp_err_t app_gphotos_rules_set_url(const char *url)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (url[0] == '\0') {
        err = nvs_erase_key(h, KEY_URL);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_str(h, KEY_URL, url);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

// ------------------------------------------------------------------------ the trial

esp_err_t app_gphotos_rules_submit(const char *text, size_t len)
{
    if (len == 0 || len > GPHOTOS_RULES_FILE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_mutex || !s_upload) {
        return ESP_ERR_NO_MEM;
    }
    lock();
    memcpy(s_upload, text, len);
    s_upload_len = len;
    s_upload_pending = true;
    unlock();
    return ESP_OK;
}

void app_gphotos_rules_consider_upload(void)
{
    if (!s_mutex || !s_upload) {
        return;
    }
    char *text = heap_caps_malloc(GPHOTOS_RULES_FILE_MAX, MALLOC_CAP_SPIRAM);
    if (!text) {
        return;
    }
    lock();
    const bool have = s_upload_pending;
    const size_t len = s_upload_len;
    if (have) {
        memcpy(text, s_upload, len);
        s_upload_pending = false;
    }
    unlock();
    if (have) {
        app_gphotos_rules_consider(text, len, APP_GPRULES_SRC_UPLOAD, 0);
    }
    heap_caps_free(text);
}

void app_gphotos_rules_consider(const char *text, size_t len, app_gprules_source_t src, int http)
{
    if (!s_mutex || !s_scratch || !s_candidate || !s_candidate_text) {
        return;
    }
    const char *why = "";
    const bool ok = decode(text, len, s_scratch, &why);
    lock();
    s_status.last_check_ms = esp_timer_get_time() / 1000;
    s_status.last_http = http;
    if (!ok) {
        s_status.last_check = "refused";
        s_status.last_reason = why;
    } else {
        uint32_t floor = s_status.adopted_seq;
        if (s_status.rejected_seq > floor) {
            floor = s_status.rejected_seq;
        }
        if (s_status.candidate_seq > floor) {
            floor = s_status.candidate_seq;
        }
        if (s_scratch->seq <= floor) {
            // The daily case: the file at the URL is the one already in force.
            s_status.last_check = "not newer";
            s_status.last_reason = "";
        } else {
            *s_candidate = *s_scratch;
            memcpy(s_candidate_text, text, len);
            s_candidate_len = len;
            s_status.candidate_seq = s_candidate->seq;
            s_status.candidate_source = src;
            s_status.last_check = "on trial";
            s_status.last_reason = "";
        }
    }
    const char *check = s_status.last_check;
    unlock();
    ESP_LOGI(TAG, "rule file from %s: %s%s%s, seq %u", app_gphotos_rules_source_str(src), check,
             ok ? "" : ": ", ok ? "" : why, ok ? (unsigned)s_scratch->seq : 0u);
}

void app_gphotos_rules_fetch_failed(int http)
{
    if (!s_mutex) {
        return;
    }
    lock();
    s_status.last_check_ms = esp_timer_get_time() / 1000;
    s_status.last_http = http;
    s_status.last_check = "fetch failed";
    s_status.last_reason = "";
    unlock();
    ESP_LOGW(TAG, "rule file fetch failed: http %d", http);
}

bool app_gphotos_rules_for_read(gphotos_rules_t *out)
{
    if (!s_mutex || !s_adopted) {
        gphotos_rules_default(out);
        return false;
    }
    lock();
    const bool trial = s_status.candidate_seq != 0;
    *out = trial ? *s_candidate : *s_adopted;
    unlock();
    return trial;
}

void app_gphotos_rules_verdict(bool any_whole, bool any_ok)
{
    if (!s_mutex || !s_adopted) {
        return;
    }
    lock();
    const uint32_t seq = s_status.candidate_seq;
    if (seq == 0 || (!any_ok && !any_whole)) {
        unlock();
        if (seq != 0) {
            ESP_LOGI(TAG, "rules seq %u stay on trial: no album read whole", (unsigned)seq);
        }
        return;
    }
    s_status.candidate_seq = 0;
    if (any_ok) {
        *s_adopted = *s_candidate;
        s_status.adopted_seq = seq;
        s_status.adopted_source = s_status.candidate_source;
        s_status.last_check = "adopted";
        s_status.last_reason = "";
        nvs_store_adopted(s_candidate_text, s_candidate_len, s_status.candidate_source);
    } else {
        s_status.rejected_seq = seq;
        s_status.last_check = "rejected";
        s_status.last_reason = "an album read whole and these rules found no photograph in it";
        nvs_store_bad(seq);
    }
    unlock();
    ESP_LOGI(TAG, "rules seq %u %s", (unsigned)seq, any_ok ? "adopted" : "rejected");
}

bool app_gphotos_rules_photo_url(const char *id, const char *key, unsigned edge, char *out,
                                 size_t size)
{
    if (!s_mutex || !s_adopted) {
        return false;
    }
    lock();
    const int i = gphotos_rules_find(s_adopted, id);
    const bool ok = i >= 0 &&
                    gphotos_rules_url(&s_adopted->patterns[i], key, strlen(key), edge, out, size);
    unlock();
    return ok;
}

void app_gphotos_rules_ua(char *out, size_t size)
{
    if (!s_mutex || !s_adopted) {
        gphotos_rules_t *d = heap_caps_malloc(sizeof(*d), MALLOC_CAP_SPIRAM);
        if (d) {
            gphotos_rules_default(d);
            snprintf(out, size, "%s", d->ua);
        } else if (size > 0) {
            out[0] = '\0';
        }
        heap_caps_free(d);
        return;
    }
    lock();
    snprintf(out, size, "%s", s_adopted->ua);
    unlock();
}

void app_gphotos_rules_get_status(app_gprules_status_t *out)
{
    if (!s_mutex) {
        *out = s_status;
        return;
    }
    lock();
    *out = s_status;
    unlock();
}
