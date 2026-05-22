/*
 * scene_weather — fully-buildable reference plugin under the v1.1.0
 * Plugin SDK (see docs/PLUGIN_SDK.md).
 *
 * Behaviour:
 *   • Shows the configured city + a 4-day forecast strip.
 *   • Console verbs (dispatched by the bridge via
 *     `dash plugin weather <verb> <json>`):
 *       - set_city <name>              — change the displayed city
 *       - refresh                       — force a redraw from the
 *                                          plugin's stub data
 *
 * STUB DATA CONTRACT
 *   In v1.1.0 the dashboard bridge does NOT fetch real weather. The
 *   contract is:
 *     • This plugin keeps a static 4-entry array of {label, icon-char,
 *       high-C, low-C} entries that is THE single source of truth.
 *     • The bridge MAY issue `dash plugin weather refresh "{...}"` with
 *       a payload of the same shape and the plugin will copy it in.
 *     • If the bridge never sends data, the plugin shows its built-in
 *       fake forecast so the demo is still meaningful offline.
 *
 *   When v1.4.0+ wires a real bridge fetcher, the bridge will start
 *   pushing real data via the same refresh verb; no firmware change
 *   needed (that's the point of the plugin SDK).
 *
 * Why this plugin is in examples/ and not built into firmware: the
 * orchestrator's hard rule for the PLUG1 scaffold says we must NOT
 * edit main/CMakeLists.txt. F2 wires this in when v1.1.0 lands.
 */

#include "scene_weather.h"
#include "plugin_api.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#define PLUG  "weather"
#define DAYS  4
#define CITY_MAX  32

typedef struct {
    char label[6];     /* "Mon", "Tue", ... */
    char icon[8];      /* "sun", "cld", "rain", "snow" — rendered as text */
    int  high_c;
    int  low_c;
} day_forecast_t;

typedef struct {
    lv_obj_t       *title;
    lv_obj_t       *city;
    lv_obj_t       *strip;
    lv_obj_t       *agent_hint;
    char            city_name[CITY_MAX];
    day_forecast_t  forecast[DAYS];
} weather_state_t;

/* Fake-data contract: the built-in forecast the plugin ships with. */
static const day_forecast_t FAKE_FORECAST[DAYS] = {
    { "Mon", "sun",  24, 13 },
    { "Tue", "cld",  22, 12 },
    { "Wed", "rain", 18, 11 },
    { "Thu", "sun",  26, 14 },
};


/* ----------------------- helpers --------------------------------------- */

static void render_strip(weather_state_t *st)
{
    lv_obj_clean(st->strip);

    /* horizontal flex container */
    lv_obj_set_layout(st->strip, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(st->strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st->strip, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < DAYS; ++i) {
        const day_forecast_t *d = &st->forecast[i];
        lv_obj_t *card = lv_obj_create(st->strip);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, 80, 110);
        lv_obj_set_style_pad_all(card, 4, 0);

        lv_obj_t *day_label = lv_label_create(card);
        lv_label_set_text(day_label, d->label);
        lv_obj_align(day_label, LV_ALIGN_TOP_MID, 0, 0);

        lv_obj_t *icon = lv_label_create(card);
        lv_label_set_text(icon, d->icon);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -4);

        lv_obj_t *temps = lv_label_create(card);
        lv_label_set_text_fmt(temps, "%d / %d", d->high_c, d->low_c);
        lv_obj_align(temps, LV_ALIGN_BOTTOM_MID, 0, -4);
    }
}

static void render_agent_hint(weather_state_t *st)
{
    /* Demonstrate plugin_agent_state_borrow_const — show whether the
     * device is currently running any agents. This makes the weather
     * scene situationally aware ("don't bother me with weather while
     * I'm coding") without giving the plugin write access to anything. */
    plugin_agent_state_t snap;
    if (!plugin_agent_state_borrow_const(&snap)) {
        lv_label_set_text(st->agent_hint, "");
        return;
    }
    if (snap.running > 0) {
        lv_label_set_text_fmt(st->agent_hint,
                              "(%d agent%s busy)",
                              snap.running,
                              snap.running == 1 ? "" : "s");
    } else if (!snap.ever_received) {
        lv_label_set_text(st->agent_hint, "(no bridge yet)");
    } else {
        lv_label_set_text(st->agent_hint, "(agents idle)");
    }
}


/* ----------------------- console handlers ------------------------------ */

static weather_state_t *g_state;   /* console handlers don't get scene* */

static int cmd_set_city(const plugin_console_args_t *args)
{
    if (!args || args->argc < 1 || !args->argv[0]) {
        plugin_reply_err(PLUG, "set_city: missing city argument");
        return -1;
    }
    if (!g_state) {
        plugin_reply_err(PLUG, "set_city: scene not initialised");
        return -1;
    }
    strncpy(g_state->city_name, args->argv[0], CITY_MAX - 1);
    g_state->city_name[CITY_MAX - 1] = '\0';
    lv_label_set_text(g_state->city, g_state->city_name);
    plugin_reply_ok(PLUG, "city=%s", g_state->city_name);
    return 0;
}

static int cmd_refresh(const plugin_console_args_t *args)
{
    (void)args;
    if (!g_state) {
        plugin_reply_err(PLUG, "refresh: scene not initialised");
        return -1;
    }
    render_strip(g_state);
    render_agent_hint(g_state);
    plugin_reply_ok(PLUG, "refreshed (stub data — see scene_weather.c "
                          "STUB DATA CONTRACT)");
    return 0;
}


/* ----------------------- lifecycle hooks ------------------------------- */

static void on_init(scene_t *s, lv_obj_t *parent)
{
    weather_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;
    g_state = st;

    strncpy(st->city_name, "Tokyo", CITY_MAX - 1);
    memcpy(st->forecast, FAKE_FORECAST, sizeof(FAKE_FORECAST));

    /* Title */
    st->title = lv_label_create(parent);
    lv_label_set_text(st->title, "Weather");
    lv_obj_align(st->title, LV_ALIGN_TOP_MID, 0, 16);

    /* City */
    st->city = lv_label_create(parent);
    lv_label_set_text(st->city, st->city_name);
    lv_obj_align(st->city, LV_ALIGN_TOP_MID, 0, 44);

    /* 4-day strip */
    st->strip = lv_obj_create(parent);
    lv_obj_remove_style_all(st->strip);
    lv_obj_set_size(st->strip, lv_pct(96), 120);
    lv_obj_align(st->strip, LV_ALIGN_CENTER, 0, 0);
    render_strip(st);

    /* Agent hint */
    st->agent_hint = lv_label_create(parent);
    lv_label_set_text(st->agent_hint, "");
    lv_obj_align(st->agent_hint, LV_ALIGN_BOTTOM_MID, 0, -16);
    render_agent_hint(st);

    /* Console verbs — must be in on_init, not on_show. */
    plugin_console_register(PLUG, "set_city", cmd_set_city,
                            "plugin.weather.set_city <name>");
    plugin_console_register(PLUG, "refresh", cmd_refresh,
                            "plugin.weather.refresh — redraw from stub data");

    plugin_log(PLUG, PLUGIN_LOG_INFO,
               "initialised with %d-day fake forecast for %s",
               DAYS, st->city_name);
}

static void on_show(scene_t *s)
{
    weather_state_t *st = (weather_state_t *)s->user_data;
    if (!st) return;
    render_agent_hint(st);   /* refresh on each show */
}

static void on_hide(scene_t *s)
{
    (void)s;
}

static void on_tick(scene_t *s, uint32_t t_ms)
{
    weather_state_t *st = (weather_state_t *)s->user_data;
    if (!st) return;
    /* Refresh agent hint at ~1 Hz (every 1024 ms ish) — cheap. */
    static uint32_t last_ms;
    if (t_ms - last_ms > 1024u) {
        render_agent_hint(st);
        last_ms = t_ms;
    }
}

static int on_dash(scene_t *s, const char *verb,
                   const plugin_console_args_t *args)
{
    (void)s; (void)args;
    if (!verb) return -1;

    /* `dash plugin weather refresh ...` is also handled directly by
     * cmd_refresh via plugin_console_register; we keep on_dash here
     * as the demonstrative example of the alternative dispatch path. */
    if (strcmp(verb, "refresh") == 0) {
        return cmd_refresh(args);
    }
    if (strcmp(verb, "set_city") == 0) {
        return cmd_set_city(args);
    }
    plugin_log(PLUG, PLUGIN_LOG_WARN,
               "on_dash: unknown verb '%s'", verb);
    return -1;
}


/* ----------------------- registration ---------------------------------- */

PLUGIN_SCENE_REGISTER(scene_weather,
    .id           = "weather",
    .display_name = "Weather",
    .accent       = LV_COLOR_MAKE(0x4D, 0xA6, 0xFF),
    .description  = "Four-day weather forecast (stub data; see header).",
    .tags         = "plugin,weather,example",
    .init         = on_init,
    .on_show      = on_show,
    .on_hide      = on_hide,
    .frame        = on_tick,
    .on_dash      = on_dash);
