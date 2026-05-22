/*
 * i18n — string registry + language switcher (v1.8.0 scaffold).
 *
 * Keys are an enum; per-language tables in strings_<lang>.h map
 * the enum to a const char*. Falls back to English on missing key.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    I18N_IDLE_NO_SESSIONS = 0,
    I18N_IDLE_PULSE,
    I18N_SESSIONS_HEADER_TOTAL,
    I18N_SESSIONS_HEADER_RUNNING,
    I18N_SESSIONS_HEADER_WAITING,
    I18N_PROMPT_APPROVE,
    I18N_PROMPT_DENY,
    I18N_PROMPT_TIMEOUT,
    I18N_STATUS_HEAP,
    I18N_STATUS_UPTIME,
    I18N_STATUS_BATTERY,
    I18N_TOKENS_CUMULATIVE,
    I18N_TOKENS_TODAY,
    I18N_COUNT,
} i18n_key_t;

/* Switch the active language by name ("en" | "zh-CN" | "ja").
 * Returns false on unknown language (active stays unchanged). */
bool i18n_set_locale(const char *locale);

/* Get string for the active language, fallback to English on miss. */
const char *i18n_get(i18n_key_t key);

/* Current locale tag. */
const char *i18n_current_locale(void);

#ifdef __cplusplus
}
#endif
