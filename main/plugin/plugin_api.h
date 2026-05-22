/*
 * plugin_api.h — the public C ABI a third-party esp32-agent-dashboard
 * plugin author #includes. Strictly C (NOT C++); see docs/PLUGIN_SDK.md
 * §6 for ABI versioning rules.
 *
 * A plugin is a *signed component pack* (manifest.toml + at least one
 * scene_t registered via PLUGIN_SCENE_REGISTER + ed25519 author and
 * user-trust signatures). Plugins are linked into the firmware at
 * build time; runtime hot-load is a v2 stretch (see SDK doc §2c).
 *
 * Everything visible here is part of the stable plugin ABI. Adding a
 * field to a struct, changing a function signature, or renaming any
 * symbol below bumps PLUGIN_ABI_VERSION.
 */

#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "harness/scene_framework.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ABI version --------------------------------------------------- */

/* Bump when ANYTHING in this header changes shape. Plugins refuse to
 * load if their compiled-against ABI > firmware's ABI. */
#define PLUGIN_ABI_VERSION   1

/* Bound on plugin identifier length. snake_case, [a-z0-9_]. */
#define PLUGIN_NAME_MAX      32

/* Bound on a plugin's exported console verb (dash plugin <name> <verb>). */
#define PLUGIN_VERB_MAX      32

/* Bound on the number of console verbs one plugin may register at init. */
#define PLUGIN_VERB_REG_MAX  8


/* ---- Read-only agent_state snapshot -------------------------------- */

/* Plugins MUST NOT touch agent_state.h directly — that would let a
 * misbehaving plugin corrupt the bridge state. Instead they call
 * plugin_agent_state_borrow_const(); the snapshot is a *copy* taken
 * under the lock and safe to read without any further locking.
 *
 * The shape mirrors agent_state.h's public fields but is intentionally
 * narrower (no internal cursors, no last_seen_monotonic_ms, no NVS-
 * mirrored config strings — plugins shouldn't depend on those). */

#define PLUGIN_AGENT_KIND_MAX        16
#define PLUGIN_AGENT_SESSION_ID_MAX  32
#define PLUGIN_AGENT_MSG_MAX        128
#define PLUGIN_AGENT_SLOT_MAX         4

typedef enum {
    PLUGIN_AGENT_STATUS_IDLE    = 0,
    PLUGIN_AGENT_STATUS_RUNNING = 1,
    PLUGIN_AGENT_STATUS_WAITING = 2,
} plugin_agent_status_t;

typedef struct {
    bool                   in_use;
    char                   kind[PLUGIN_AGENT_KIND_MAX];
    char                   session_id[PLUGIN_AGENT_SESSION_ID_MAX];
    char                   msg[PLUGIN_AGENT_MSG_MAX];
    plugin_agent_status_t  status;
    uint64_t               tokens_cumulative;
    uint64_t               tokens_today;
    uint32_t               last_active_unix;
} plugin_agent_slot_t;

typedef struct {
    plugin_agent_slot_t slots[PLUGIN_AGENT_SLOT_MAX];
    int                 slot_count;
    int                 total;
    int                 running;
    int                 waiting;
    uint64_t            tokens_cumulative;
    uint64_t            tokens_today;
    /* True iff the device has ever received a dash snapshot. Lets a
     * plugin distinguish "no agents now" from "never connected". */
    bool                ever_received;
    /* Monotonic ms when the snapshot was taken — for staleness checks. */
    uint32_t            snapshot_ms;
} plugin_agent_state_t;

/* Take a snapshot of agent_state under the lock, copy into *out, and
 * return. Cheap (single memcpy + bounded conversion). Returns false if
 * the framework isn't initialised yet (call from on_init only after
 * lvgl is up). */
bool plugin_agent_state_borrow_const(plugin_agent_state_t *out);


/* ---- Console command registration ---------------------------------- */

/* A plugin console command dispatched via:
 *    dash plugin <plugin-name> <verb> <args...>
 *
 * The host bridge prefixes every plugin command with `dash plugin
 * <name>`; the framework strips that and invokes the plugin's
 * matching handler with the remaining argv (so a plugin handler sees
 * `verb argv[0] argv[1] ...`). */

typedef struct {
    int          argc;
    const char  *argv[8];
} plugin_console_args_t;

typedef int (*plugin_console_handler_fn)(const plugin_console_args_t *args);

/* Register a verb. Pointers (name, help, fn) must remain valid for
 * the lifetime of the program — pass string literals. Returns 0 on
 * success, -1 if the verb is malformed, -2 if the registry is full,
 * -3 if a duplicate verb is already registered for THIS plugin.
 *
 * Call from on_init only. Registering from on_show/on_tick is
 * undefined behaviour (and will likely race the boot sequence). */
int plugin_console_register(const char *plugin_name,
                            const char *verb,
                            plugin_console_handler_fn fn,
                            const char *help);

/* Reply helpers — wrap console_reply_ok / console_reply_err so a
 * plugin author doesn't need to pull in harness/console_protocol.h.
 * Auto-prepend the [plugin:<name>] tag for traceability. */
void plugin_reply_ok(const char *plugin_name, const char *fmt, ...);
void plugin_reply_err(const char *plugin_name, const char *fmt, ...);


/* ---- Logging ------------------------------------------------------- */

typedef enum {
    PLUGIN_LOG_ERROR = 1,
    PLUGIN_LOG_WARN  = 2,
    PLUGIN_LOG_INFO  = 3,
    PLUGIN_LOG_DEBUG = 4,
    PLUGIN_LOG_VERBOSE = 5,
} plugin_log_level_t;

/* Routes to ESP_LOG with tag "plugin:<plugin_name>". The first arg is
 * NOT the LVGL scene_t — it's the plugin's own name string so a
 * developer reading serial output can see which plugin emitted what. */
void plugin_log(const char *plugin_name,
                plugin_log_level_t level,
                const char *fmt, ...);


/* ---- Lifecycle hooks (plugin-scene shape) -------------------------- */

/* A plugin's scene_t is identical to the harness's scene_t (see
 * harness/scene_framework.h). The fields below are the SUBSET a plugin
 * author should care about — we don't expose on_tilt/on_long_press
 * here because plugin scenes can't take exclusive control of buttons
 * (that's the host firmware's territory). They're still available if
 * the plugin author needs them; just NULL them in the struct literal
 * to opt out (which is the default for designated initialisers). */

/* Plugin-author-friendly aliases. Same shape as the harness's hooks;
 * we keep the harness's parameter shapes so PLUGIN_SCENE_REGISTER is a
 * thin compile-time wrapper. */
typedef void (*plugin_on_init_fn)(scene_t *s, lv_obj_t *parent);
typedef void (*plugin_on_show_fn)(scene_t *s);
typedef void (*plugin_on_hide_fn)(scene_t *s);
typedef void (*plugin_on_tick_fn)(scene_t *s, uint32_t t_ms);

/* on_dash_command — fires when the bridge issues
 *   `dash plugin <plugin-name> <verb> [args]`.
 * Return 0 to indicate handled; non-zero falls through to a generic
 * `EVT: plugin_dash_unknown` emission. */
typedef int (*plugin_on_dash_command_fn)(scene_t *s,
                                         const char *verb,
                                         const plugin_console_args_t *args);


/* ---- PLUGIN_SCENE_REGISTER ------------------------------------------ */

/* The plugin author writes:
 *
 *   static void my_init(scene_t *s, lv_obj_t *parent) { ... }
 *   static void my_tick(scene_t *s, uint32_t t_ms)    { ... }
 *   static int  my_dash(scene_t *s, const char *verb,
 *                       const plugin_console_args_t *a) { ... }
 *
 *   PLUGIN_SCENE_REGISTER(my_scene,
 *       .id           = "weather",
 *       .display_name = "Weather",
 *       .accent       = LV_COLOR_MAKE(0x4D, 0xA6, 0xFF),
 *       .description  = "Four-day forecast.",
 *       .tags         = "plugin,weather",
 *       .init         = my_init,
 *       .frame        = my_tick,
 *       .on_dash      = my_dash);
 *
 * What this macro emits at link time:
 *
 *   (a) a `scene_t` named `my_scene` filled with the designated
 *       initialisers above;
 *
 *   (b) a `plugin_registration_t` placed in the linker section
 *       `.esp32_agent_plugins` so that plugin_loader_init can walk
 *       __start_esp32_agent_plugins .. __stop_esp32_agent_plugins and
 *       discover every plugin scene without any central registry edit;
 *
 *   (c) a static `plugin_on_dash_command_fn` slot (the harness's
 *       scene_t has no on_dash field — we route dash commands through
 *       the plugin loader's registration record, not the scene).
 *
 * The trailing comma after the last field is fine — designated
 * initialisers tolerate it. */

typedef struct {
    const char                *plugin_name;   /* matches manifest [plugin].name */
    scene_t                   *scene;         /* the scene_t emitted by the macro */
    plugin_on_dash_command_fn  on_dash;       /* may be NULL */
    uint16_t                   abi_version;   /* PLUGIN_ABI_VERSION at compile time */
    uint16_t                   _pad;          /* reserved 0 */
} plugin_registration_t;

/* Linker section where every PLUGIN_SCENE_REGISTER stashes one
 * plugin_registration_t. plugin_loader walks the range below. */
#ifndef PLUGIN_SECTION_NAME
#define PLUGIN_SECTION_NAME ".esp32_agent_plugins"
#endif

/* Helper: compose the section attribute. GCC/Clang both accept this
 * form; ESP-IDF's xtensa-gcc inherits it. */
#define PLUGIN__SECTION_ATTR \
    __attribute__((used, section(PLUGIN_SECTION_NAME), aligned(4)))

/* The actual registration macro. `name` is the C identifier for the
 * emitted scene_t (e.g. my_scene). All other fields are scene_t
 * designated initialisers PLUS an optional `.on_dash` (a
 * plugin_on_dash_command_fn). */
#define PLUGIN_SCENE_REGISTER(name, ...)                                       \
    scene_t name = { __VA_ARGS__ };                                            \
    /* Optional on_dash extraction: the user passes .on_dash via __VA_ARGS__   \
     * BUT the harness's scene_t has no on_dash field, so we re-declare a      \
     * sibling fn pointer and pull it out via a second initialiser block.     \
     * Trick: the user writes `.on_dash = my_dash` in PLUGIN_SCENE_REGISTER;   \
     * the helper PLUGIN_REG_INIT() below extracts it with a designated-       \
     * initialiser trick — any unspecified field falls back to NULL. */       \
    static const struct { plugin_on_dash_command_fn on_dash; }                 \
        name##__plugin_dash_extract = { __VA_ARGS__ };                         \
    static const plugin_registration_t name##__plugin_reg                      \
        PLUGIN__SECTION_ATTR = {                                               \
            .plugin_name = #name,                                              \
            .scene       = &name,                                              \
            .on_dash     = name##__plugin_dash_extract.on_dash,                \
            .abi_version = PLUGIN_ABI_VERSION,                                 \
            ._pad        = 0,                                                  \
        }

/* NOTE on the macro trick: passing `.on_dash = X` to both the scene_t
 * initialiser AND the extractor's anonymous struct relies on the
 * scene_t having an `on_dash` field. The harness's scene_t does NOT,
 * so the plugin author must pass `.on_dash = X` ONLY in the second
 * struct OR via a wrapper. v1 keeps it simple: plugin authors set
 * `.on_dash` as a top-level field in the macro and the framework
 * ignores the unknown initialiser on scene_t.
 *
 * Practical consequence: GCC's `-Werror=missing-field-initializers`
 * is OK (we use designated initialisers), but
 * `-Werror=designated-init` would reject the unknown field. The
 * dashboard's CMakeLists.txt does NOT pass either flag today. If a
 * future SDK consumer enables them, the workaround is to declare
 * on_dash in a separate `PLUGIN_DASH(name, fn)` macro instead. */


#ifdef __cplusplus
}
#endif
