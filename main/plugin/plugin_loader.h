/*
 * plugin_loader — boot-time enumeration + signature verification of
 * the plugin_registration_t records emitted by PLUGIN_SCENE_REGISTER.
 *
 * Lifecycle:
 *
 *   1. esp32_agent_dashboard_main calls plugin_loader_init() AFTER
 *      agent_state_init / theme_init / console_protocol_init / scene_fw_init
 *      and BEFORE agent_commands_register.
 *
 *   2. plugin_loader walks the linker section between
 *      __start_esp32_agent_plugins and __stop_esp32_agent_plugins,
 *      discovers every plugin_registration_t baked in at link time.
 *
 *   3. For each registration: load the plugin's manifest blob, verify
 *      ed25519 author + user-trust signatures (delegated to
 *      tools/sign/-generated key infra; see docs/PLUGIN_SDK.md §5),
 *      check ABI compatibility. On failure: log + emit
 *      `EVT: plugin_reject` + skip.
 *
 *   4. For each surviving plugin: register its scene via
 *      scene_fw_register() and install its on_dash_command into the
 *      `dash plugin <name> <verb>` dispatcher.
 *
 *   5. plugin_loader_status() returns a JSON-shaped count of accepted
 *      / rejected so `dash plugin list` can answer the bridge.
 *
 * v1 ONLY does load-time discovery. The OTA-partition path (§2b in
 * docs/PLUGIN_SDK.md) is wired here too but the partition-table
 * change is deferred to F2 + SEC1; until that lands,
 * plugin_loader_init_from_ota() is a no-op stub.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLUGIN_REJECT_NONE       = 0,
    PLUGIN_REJECT_MANIFEST   = 1,   /* TOML parse / missing field */
    PLUGIN_REJECT_SIG_AUTHOR = 2,   /* author signature invalid / missing */
    PLUGIN_REJECT_SIG_USER   = 3,   /* user-trust countersignature invalid */
    PLUGIN_REJECT_ABI        = 4,   /* plugin_abi_version mismatch */
    PLUGIN_REJECT_DUPLICATE  = 5,   /* two plugins claim same [plugin].name */
    PLUGIN_REJECT_INIT       = 6,   /* on_init returned error / asserted */
} plugin_reject_reason_t;

typedef struct {
    int total_discovered;     /* registrations in the .esp32_agent_plugins section */
    int accepted;             /* loaded successfully */
    int rejected_manifest;
    int rejected_sig_author;
    int rejected_sig_user;
    int rejected_abi;
    int rejected_duplicate;
    int rejected_init;
} plugin_loader_status_t;

/* Run discovery, verification, and registration. Idempotent — calling
 * twice is a no-op. Returns the number of accepted plugins. */
int plugin_loader_init(void);

/* OTA-partition entry point. v1.1.0: stub. Returns 0 and logs INFO. */
int plugin_loader_init_from_ota(void);

/* Snapshot the loader's accept/reject totals. Used by
 * `dash plugin list` reply construction. Safe to call any time after
 * plugin_loader_init. */
void plugin_loader_status(plugin_loader_status_t *out);

/* Internal: route a `dash plugin <name> <verb> <argv...>` console
 * command to the matching plugin's on_dash_command handler. Returns
 * 0 if handled, -1 if name not found, -2 if plugin has no on_dash,
 * -3 if the plugin's handler returned non-zero. agent_commands.c
 * delegates here when it sees a `dash plugin` verb. The plumbing
 * inside agent_commands.c itself is owned by F2 — for v1.1.0
 * scaffolding we just expose the entry point. */
typedef struct {
    int          argc;
    const char  *argv[8];
} plugin_loader_args_t;

int plugin_loader_dispatch(const char *plugin_name,
                           const char *verb,
                           const plugin_loader_args_t *args);

#ifdef __cplusplus
}
#endif
