/*
 * plugin_loader — see plugin_loader.h for the contract.
 *
 * Section-symbol trick:
 *   PLUGIN_SCENE_REGISTER emits a plugin_registration_t into the
 *   linker section ".esp32_agent_plugins". The GNU linker
 *   auto-synthesises __start_<section> and __stop_<section> symbols
 *   when the section name is a valid C identifier (no dots). To make
 *   the dot-containing name work we declare the bounds as weak
 *   externs and rely on the linker script bundled with ESP-IDF, which
 *   exports `_section_start_<name>` / `_section_stop_<name>` when
 *   asked via `SECTIONS { ... __start_X = .; ... __stop_X = .; }`.
 *
 *   In practice ESP-IDF's default `esp32s3.project.ld.in` does NOT
 *   emit those symbols for arbitrary sections. The plugin author
 *   adds a fragment under `main/plugin/plugin_section.ld.in` that
 *   slots into the ld template — that's a job for F2 when v1.1.0
 *   lands. For now we declare the section bounds + ifdef-guard the
 *   walk so the file COMPILES even without the linker fragment;
 *   plugin_loader_init() returns 0 (no plugins discovered) in that
 *   case and logs a clear "linker fragment missing" warning so we
 *   don't silently report success when the discovery path is dead.
 */

#include "plugin_loader.h"
#include "plugin_api.h"

#include <string.h>

#include "esp_log.h"
#include "harness/scene_framework.h"
#include "harness/console_protocol.h"

static const char *TAG = "plugin_loader";

/* Linker-synthesised symbols. We mark them weak so the binary links
 * even without the as-yet-uncommitted linker fragment. When the
 * fragment IS in place, both symbols point at the start/stop of the
 * `.esp32_agent_plugins` section's contents. */
extern const plugin_registration_t __start_esp32_agent_plugins[]
    __attribute__((weak));
extern const plugin_registration_t __stop_esp32_agent_plugins[]
    __attribute__((weak));

static plugin_loader_status_t s_status;
static bool                   s_initialised;

/* Up to PLUGIN_LOADER_MAX accepted plugins are remembered for the
 * dispatch table. 16 is plenty for the foreseeable v1.x roadmap;
 * bump as needed. */
#define PLUGIN_LOADER_MAX  16

typedef struct {
    char                       name[PLUGIN_NAME_MAX];
    plugin_on_dash_command_fn  on_dash;
    scene_t                   *scene;
} plugin_loader_slot_t;

static plugin_loader_slot_t s_slots[PLUGIN_LOADER_MAX];
static int                  s_slot_count;


/* ------------- signature + manifest verification (stub) -------------
 *
 * The actual ed25519 verify lives in tools/sign/ once SEC1 ships
 * the firmware-side helpers. For the scaffold we expose two
 * weakly-linked hooks so SEC1 can drop in real implementations
 * without touching this file:
 *
 *   verify_author_sig(...) → 0 ok, non-zero reject
 *   verify_user_sig(...)   → 0 ok, non-zero reject
 *
 * Default weak implementations: REJECT everything (fail-closed).
 * That means until SEC1 wires real keys, NO plugin can load, which
 * is the safe default — better than the alternative of "scaffold
 * accidentally trusts every plugin".
 *
 * Developers iterating locally override this by linking in
 * `tools/sign/dev_self_trust.c` (TBD by SEC1), which provides a
 * strong symbol that trusts the self-signed user keypair. */

__attribute__((weak))
int plugin_verify_author_sig(const plugin_registration_t *reg)
{
    (void)reg;
    return -1;   /* fail-closed; SEC1 overrides */
}

__attribute__((weak))
int plugin_verify_user_sig(const plugin_registration_t *reg)
{
    (void)reg;
    return -1;   /* fail-closed; SEC1 overrides */
}


/* ------------- helpers ---------------------------------------------- */

static bool reg_is_valid(const plugin_registration_t *reg)
{
    if (reg == NULL)              return false;
    if (reg->plugin_name == NULL) return false;
    if (reg->scene == NULL)       return false;
    if (reg->plugin_name[0] == '\0') return false;
    /* identifier sanity: snake_case, [a-z0-9_], <= PLUGIN_NAME_MAX-1 */
    size_t n = strnlen(reg->plugin_name, PLUGIN_NAME_MAX);
    if (n == 0 || n >= PLUGIN_NAME_MAX) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = reg->plugin_name[i];
        bool ok = (c >= 'a' && c <= 'z')
               || (c >= '0' && c <= '9')
               || c == '_';
        if (!ok) return false;
    }
    return true;
}

static bool name_already_loaded(const char *name)
{
    for (int i = 0; i < s_slot_count; ++i) {
        if (strncmp(s_slots[i].name, name, PLUGIN_NAME_MAX) == 0) {
            return true;
        }
    }
    return false;
}

static void emit_reject_evt(const char *name, const char *reason)
{
    console_send_evt("plugin_reject name=%s reason=%s",
                     name ? name : "?",
                     reason);
}

static void emit_loaded_evt(const plugin_registration_t *reg)
{
    console_send_evt("plugin_loaded name=%s abi=%u",
                     reg->plugin_name,
                     (unsigned)reg->abi_version);
}


/* ------------- main flow -------------------------------------------- */

int plugin_loader_init(void)
{
    if (s_initialised) {
        ESP_LOGW(TAG, "already initialised — ignoring re-entry");
        return s_status.accepted;
    }
    s_initialised = true;
    memset(&s_status, 0, sizeof(s_status));
    s_slot_count = 0;

    /* Linker fragment present? */
    if (__start_esp32_agent_plugins == NULL
            || __stop_esp32_agent_plugins == NULL
            || __stop_esp32_agent_plugins < __start_esp32_agent_plugins) {
        ESP_LOGW(TAG, "no plugin section bounds — linker fragment "
                       "main/plugin/plugin_section.ld.in not installed. "
                       "Plugin discovery skipped (this is OK in scaffold "
                       "builds; F2 wires it up for v1.1.0).");
        return 0;
    }

    const plugin_registration_t *p = __start_esp32_agent_plugins;
    const plugin_registration_t *end = __stop_esp32_agent_plugins;

    for (; p < end; ++p) {
        s_status.total_discovered++;

        if (!reg_is_valid(p)) {
            ESP_LOGE(TAG, "reject [unknown]: malformed registration record");
            s_status.rejected_manifest++;
            emit_reject_evt(p ? p->plugin_name : "unknown", "manifest");
            continue;
        }

        if (p->abi_version != PLUGIN_ABI_VERSION) {
            ESP_LOGE(TAG, "reject [%s]: abi=%u, firmware abi=%u",
                     p->plugin_name,
                     (unsigned)p->abi_version,
                     (unsigned)PLUGIN_ABI_VERSION);
            s_status.rejected_abi++;
            emit_reject_evt(p->plugin_name, "abi");
            continue;
        }

        if (name_already_loaded(p->plugin_name)) {
            ESP_LOGE(TAG, "reject [%s]: duplicate plugin name", p->plugin_name);
            s_status.rejected_duplicate++;
            emit_reject_evt(p->plugin_name, "duplicate");
            continue;
        }

        if (plugin_verify_author_sig(p) != 0) {
            ESP_LOGE(TAG, "reject [%s]: author signature failed",
                     p->plugin_name);
            s_status.rejected_sig_author++;
            emit_reject_evt(p->plugin_name, "sig_author");
            continue;
        }

        if (plugin_verify_user_sig(p) != 0) {
            ESP_LOGE(TAG, "reject [%s]: user countersignature failed",
                     p->plugin_name);
            s_status.rejected_sig_user++;
            emit_reject_evt(p->plugin_name, "sig_user");
            continue;
        }

        /* Slot it into our dispatch table BEFORE registering the
         * scene, so that if scene_fw_register's init hook triggers
         * an on_dash_command (unlikely but possible via EVT echo),
         * the dispatcher already knows about us. */
        if (s_slot_count >= PLUGIN_LOADER_MAX) {
            ESP_LOGE(TAG, "reject [%s]: loader table full (max %d)",
                     p->plugin_name, PLUGIN_LOADER_MAX);
            s_status.rejected_init++;
            emit_reject_evt(p->plugin_name, "init");
            continue;
        }
        plugin_loader_slot_t *slot = &s_slots[s_slot_count++];
        strncpy(slot->name, p->plugin_name, sizeof(slot->name) - 1);
        slot->name[sizeof(slot->name) - 1] = '\0';
        slot->on_dash = p->on_dash;
        slot->scene   = p->scene;

        /* Register the plugin's scene with the harness. The harness
         * keeps the pointer; it must outlive the program (true here —
         * scene_t comes from PLUGIN_SCENE_REGISTER's file-scope decl). */
        scene_fw_register(p->scene);

        s_status.accepted++;
        ESP_LOGI(TAG, "accepted [%s] abi=%u",
                 p->plugin_name, (unsigned)p->abi_version);
        emit_loaded_evt(p);
    }

    ESP_LOGI(TAG,
             "discovery: %d found, %d accepted, "
             "%d manifest, %d abi, %d sig_author, "
             "%d sig_user, %d duplicate, %d init",
             s_status.total_discovered,
             s_status.accepted,
             s_status.rejected_manifest,
             s_status.rejected_abi,
             s_status.rejected_sig_author,
             s_status.rejected_sig_user,
             s_status.rejected_duplicate,
             s_status.rejected_init);

    return s_status.accepted;
}

int plugin_loader_init_from_ota(void)
{
    ESP_LOGI(TAG, "OTA plugin partition path not yet wired (v1.1.0 stretch)");
    return 0;
}

void plugin_loader_status(plugin_loader_status_t *out)
{
    if (out == NULL) return;
    *out = s_status;
}

int plugin_loader_dispatch(const char *plugin_name,
                           const char *verb,
                           const plugin_loader_args_t *args)
{
    if (plugin_name == NULL || verb == NULL || args == NULL) return -1;

    for (int i = 0; i < s_slot_count; ++i) {
        if (strncmp(s_slots[i].name, plugin_name, PLUGIN_NAME_MAX) != 0) {
            continue;
        }
        if (s_slots[i].on_dash == NULL) {
            console_send_evt("plugin_dash_unknown name=%s verb=%s",
                             plugin_name, verb);
            return -2;
        }
        /* The plugin_loader_args_t and plugin_console_args_t are
         * shape-identical by design — we copy explicitly rather than
         * cast so a future ABI bump can change one without the other. */
        plugin_console_args_t pa;
        pa.argc = args->argc;
        for (int j = 0; j < args->argc && j < (int)(sizeof(pa.argv)/sizeof(pa.argv[0])); ++j) {
            pa.argv[j] = args->argv[j];
        }
        int rc = s_slots[i].on_dash(s_slots[i].scene, verb, &pa);
        if (rc != 0) {
            ESP_LOGW(TAG, "plugin [%s] on_dash(%s) returned %d",
                     plugin_name, verb, rc);
            return -3;
        }
        return 0;
    }
    /* Plugin name not found at all. */
    return -1;
}
