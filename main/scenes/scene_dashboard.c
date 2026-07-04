/*
 * scene_dashboard — adaptive fleet view (v3.0).
 *
 * The resting/working screen. Density adapts to how many agents are live:
 *
 *   1 agent   → ambient breathing pulse + status word (kept from v2) plus
 *               the project name and a live activity line, so a lone agent
 *               still shows WHAT it is doing, not just that it exists.
 *   2-4 agents → per-agent rows ("fleet"): status dot / kind chip + project /
 *               activity line / right meta. Running rows are teal-on-surface;
 *               waiting rows glow gold with a hairline border so "who needs
 *               me" is readable across the room. Urgent kinds (approve /
 *               clarify) get the brighter gold.
 *
 * With 2+ agents the AWAITING takeover is suppressed (see
 * scene_auto_switch_cb in esp32_agent_dashboard_main.c) — the fleet IS the
 * multi-agent view; a full-screen takeover would hide every other agent,
 * which was the v2 complaint.
 *
 * All four rows are pre-created in init() and shown/hidden per tick; row
 * geometry is recomputed only when the visible count changes.
 */

#include "scenes.h"
#include "agent_state.h"
#include "theme.h"
#include "status_bar.h"
#include "cjk_font.h"
#include "anim/apple_ease.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#define SCREEN_W       466
#define COL_BG         0x0B0A09
#define COL_SURFACE    0x1C1814
#define COL_SURFACE_HI 0x26201A   /* waiting-row surface, one step lighter */
#define COL_TEXT       0xF3EEE2
#define COL_TEXT_DIM   0x8A807A
#define COL_MUTE       0x5A514A
#define COL_TEAL       0x2BB3B1
#define COL_GOLD       0xB89020
#define COL_GOLD_HI    0xE0B43C   /* urgent (approve/clarify) accent */

/* Fleet rows live between the 48pt clock and the footer. Width 380 matches
 * scene_awaiting's content width — verified safe against the panel edges. */
#define ROW_W        380
#define ROW_X        ((SCREEN_W - ROW_W) / 2)
#define ROW_AREA_TOP 128
#define ROW_AREA_BOT 396
#define ROW_GAP       10
#define ROW_H_MAX    104

/* Ambient cluster vertical anchors. The usable band runs from the clock's
 * bottom (~112) to the footer numbers (~408). Cluster = ring(96) + status
 * word(≈34+gap) ≈ 152 px; with the project + activity lines the content
 * grows to ≈ 218 px. Centering each case in the band gives:
 *   no info   → y 184 (ring center ≈ 232 ≈ panel center)
 *   with info → y 151 (cluster slides UP to make room below)
 * The transition animates with Apple's standard ease (apple_ease_out). */
#define AMBIENT_Y_CENTERED 184
#define AMBIENT_Y_INFO     151
#define AMBIENT_SLIDE_MS   450

typedef struct {
    lv_obj_t *card;
    lv_obj_t *dot;
    lv_obj_t *kind_lbl;
    lv_obj_t *name_lbl;
    lv_obj_t *meta_lbl;
    lv_obj_t *act_lbl;
} fleet_row_t;

typedef struct {
    status_bar_t sb;             /* shared top time + bottom active/tokens */
    /* ambient (single-agent) group */
    lv_obj_t *ambient_grp;
    lv_obj_t *ambient_ring;
    lv_obj_t *ambient_dot;       /* breathing inner dot */
    lv_obj_t *ambient_lbl;       /* status word */
    lv_obj_t *ambient_proj;      /* "cc  <project>" */
    lv_obj_t *ambient_act;       /* live activity line */
    int       ambient_target_y;  /* last slide target; 0 = not yet placed */
    bool      breath_armed;
    /* fleet (multi-agent) rows */
    fleet_row_t rows[AGENT_SLOT_MAX];
    int       last_layout_n;     /* row count last laid out; -1 forces layout */
    lv_timer_t *timer;
} dash_t;

/* ── helpers ─────────────────────────────────────────────────────── */

static const char *short_kind(const char *kind)
{
    if (kind == NULL) return "ag";
    if (strcmp(kind, "claude-code") == 0) return "cc";
    if (strcmp(kind, "codex") == 0)       return "cx";
    if (strcmp(kind, "cursor") == 0)      return "cu";
    if (strcmp(kind, "aider") == 0)       return "ai";
    if (strcmp(kind, "windsurf") == 0)    return "ws";
    if (strcmp(kind, "copilot") == 0)     return "cp";
    if (strcmp(kind, "qwen-code") == 0)   return "qw";
    return "ag";
}

static const char *awaiting_headline(awaiting_kind_t k)
{
    switch (k) {
        case AWAITING_CONTINUE: return "your turn";
        case AWAITING_APPROVE:  return "approve?";
        case AWAITING_PICK:     return "pick one";
        case AWAITING_TYPE:     return "type a reply";
        case AWAITING_CLARIFY:  return "clarify";
        default:                return "";
    }
}

static const char *cwd_basename(const char *cwd)
{
    const char *base = cwd;
    for (const char *p = cwd; p && *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return (base && base[0]) ? base : NULL;
}

static void fmt_tokens_short(char *buf, size_t cap, uint64_t tok)
{
    if (tok < 1000)        snprintf(buf, cap, "%u", (unsigned)tok);
    else if (tok < 100000) snprintf(buf, cap, "%.1fk", (double)tok / 1000.0);
    else                   snprintf(buf, cap, "%uk", (unsigned)(tok / 1000));
}

static void fmt_dur_s(char *buf, size_t cap, uint32_t s)
{
    if (s < 60)        snprintf(buf, cap, "%us", (unsigned)s);
    else if (s < 3600) snprintf(buf, cap, "%um", (unsigned)(s / 60));
    else               snprintf(buf, cap, "%uh%um",
                                (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
}

/* How long has this slot been awaiting? Prefer the HOST-side timestamp
 * (awaiting_since_unix vs host clock) so the duration survives device
 * reboots and bridge reconnects; fall back to the device-local tick. */
static uint32_t awaiting_elapsed_s(const agent_state_t *st, const agent_slot_t *s)
{
    if (s->awaiting_since_unix && st->host_epoch_unix) {
        uint32_t now_epoch = st->host_epoch_unix
                           + (lv_tick_get() - st->host_clock_received_ms) / 1000u;
        if (now_epoch >= s->awaiting_since_unix) {
            return now_epoch - s->awaiting_since_unix;
        }
    }
    if (s->awaiting_entered_ms) {
        return (lv_tick_get() - s->awaiting_entered_ms) / 1000u;
    }
    return 0;
}

/* Compose the one-line activity for a slot. Waiting slots show the agent's
 * own summary (dash-state) or the awaiting headline; running slots show the
 * latest transcript entry (tool + text), falling back to the user's prompt. */
static void compose_activity(const agent_slot_t *s, char *buf, size_t cap)
{
    if (s->awaiting_kind != AWAITING_NONE) {
        if (s->awaiting_summary[0]) {
            /* "·" (U+00B7) — proven renderable in the device font subset
             * (status_bar uses it); em dash is NOT in the subset. */
            snprintf(buf, cap, "%s \xC2\xB7 ", awaiting_headline(s->awaiting_kind));
            size_t used = strlen(buf);
            if (used < cap) {
                cjk_utf8_lcpy(buf + used, s->awaiting_summary, (unsigned)(cap - used));
            }
        } else {
            snprintf(buf, cap, "%s", awaiting_headline(s->awaiting_kind));
        }
        return;
    }
    if (s->entry_count > 0 && s->entries[0].text[0]) {
        if (s->entries[0].tool[0]) {
            snprintf(buf, cap, "%s  ", s->entries[0].tool);
            size_t used = strlen(buf);
            if (used < cap) {
                cjk_utf8_lcpy(buf + used, s->entries[0].text, (unsigned)(cap - used));
            }
        } else {
            cjk_utf8_lcpy(buf, s->entries[0].text, (unsigned)cap);
        }
        return;
    }
    if (s->msg[0]) { cjk_utf8_lcpy(buf, s->msg, (unsigned)cap); return; }
    snprintf(buf, cap, "%s", s->status == AGENT_STATUS_RUNNING ? "working" : "-");
}

/* ── ambient (single-agent) mode ─────────────────────────────────── */

static void anim_breath_size(void *obj, int32_t v)
{
    lv_obj_set_size((lv_obj_t *)obj, v, v);
    lv_obj_center((lv_obj_t *)obj);
}

static void arm_breath(dash_t *d)
{
    if (d->breath_armed || !d->ambient_dot) return;
    d->breath_armed = 1;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, d->ambient_dot);
    lv_anim_set_values(&a, 16, 34);
    lv_anim_set_time(&a, 1500);
    lv_anim_set_playback_time(&a, 1500);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, anim_breath_size);
    lv_anim_start(&a);
}

static void anim_ambient_y(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

/* Slide the ambient cluster between its two anchors: centered when there is
 * no detail below it, shifted up (avoidance) when project/activity lines
 * appear. Apple standard ease; instant when motion is reduced or on the
 * very first placement. */
static void ambient_slide_to(dash_t *d, int target, bool motion_ok)
{
    if (target == d->ambient_target_y) return;
    bool first = (d->ambient_target_y == 0);
    d->ambient_target_y = target;
    lv_anim_delete(d->ambient_grp, anim_ambient_y);
    if (first || !motion_ok) {
        lv_obj_set_y(d->ambient_grp, target);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, d->ambient_grp);
    lv_anim_set_values(&a, lv_obj_get_style_y(d->ambient_grp, LV_PART_MAIN),
                       target);
    lv_anim_set_time(&a, AMBIENT_SLIDE_MS);
    lv_anim_set_path_cb(&a, apple_ease_out);
    lv_anim_set_exec_cb(&a, anim_ambient_y);
    lv_anim_start(&a);
}

static void render_ambient(dash_t *d, const agent_state_t *st)
{
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        lv_obj_add_flag(d->rows[i].card, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(d->ambient_grp, LV_OBJ_FLAG_HIDDEN);
    d->last_layout_n = -1;   /* force row re-layout when fleet returns */

    int active_now = st->running + st->waiting;
    const char *verb = (st->running > 0) ? "thinking" :
                       (active_now > 0)  ? "your turn" : "idle";
    lv_label_set_text(d->ambient_lbl, verb);

    /* Find the (single) live slot for project + activity detail. */
    const agent_slot_t *one = NULL;
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        if (st->slots[i].in_use) { one = &st->slots[i]; break; }
    }
    ambient_slide_to(d, one ? AMBIENT_Y_INFO : AMBIENT_Y_CENTERED,
                     !st->motion_reduced);
    if (one) {
        char proj[64];
        const char *base = cwd_basename(one->cwd);
        char basetrunc[40];
        if (base) {
            cjk_utf8_lcpy(basetrunc, base, sizeof(basetrunc));
            snprintf(proj, sizeof(proj), "%s  %s", short_kind(one->kind), basetrunc);
        } else {
            snprintf(proj, sizeof(proj), "%s", short_kind(one->kind));
        }
        lv_label_set_text(d->ambient_proj, proj);

        char act[112];
        compose_activity(one, act, sizeof(act));
        lv_label_set_text(d->ambient_act, act);
    } else {
        lv_label_set_text(d->ambient_proj, "");
        lv_label_set_text(d->ambient_act, "");
    }
}

/* ── fleet (multi-agent) mode ────────────────────────────────────── */

static void layout_rows(dash_t *d, int n)
{
    int h = (ROW_AREA_BOT - ROW_AREA_TOP - (n - 1) * ROW_GAP) / n;
    if (h > ROW_H_MAX) h = ROW_H_MAX;
    int total = n * h + (n - 1) * ROW_GAP;
    int y = ROW_AREA_TOP + (ROW_AREA_BOT - ROW_AREA_TOP - total) / 2;

    int line1_y = h * 30 / 100;    /* centerline of name row */
    int line2_y = h * 68 / 100;    /* centerline of activity row */

    for (int i = 0; i < n; ++i) {
        fleet_row_t *r = &d->rows[i];
        lv_obj_set_pos(r->card, ROW_X, y);
        lv_obj_set_size(r->card, ROW_W, h);
        lv_obj_set_pos(r->dot, 18, line1_y - 7);
        lv_obj_set_pos(r->kind_lbl, 42, line1_y - 7);
        lv_obj_set_pos(r->name_lbl, 68, line1_y - 13);
        /* Fixed height = one line: LONG_DOT only ellipsizes when the label
         * can't grow — width alone lets long names wrap onto the activity
         * line (seen on-device with "esp32-agent-dashboard"). */
        lv_obj_set_size(r->name_lbl, ROW_W - 68 - 16 - 72, 28);
        lv_obj_align(r->meta_lbl, LV_ALIGN_TOP_RIGHT, -16, line1_y - 9);
        lv_obj_set_pos(r->act_lbl, 22, line2_y - 11);
        lv_obj_set_size(r->act_lbl, ROW_W - 44, 24);
        y += h + ROW_GAP;
    }
}

static void render_fleet(dash_t *d, const agent_state_t *st)
{
    lv_obj_add_flag(d->ambient_grp, LV_OBJ_FLAG_HIDDEN);

    /* Collect visible slots in stable slot order. */
    const agent_slot_t *vis[AGENT_SLOT_MAX];
    int n = 0;
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        if (st->slots[i].in_use) vis[n++] = &st->slots[i];
    }
    if (n < 2) return;   /* caller guards; belt-and-braces */

    if (n != d->last_layout_n) {
        layout_rows(d, n);
        d->last_layout_n = n;
    }

    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        fleet_row_t *r = &d->rows[i];
        if (i >= n) {
            lv_obj_add_flag(r->card, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const agent_slot_t *s = vis[i];
        bool waiting = (s->awaiting_kind != AWAITING_NONE)
                    || (s->status == AGENT_STATUS_WAITING);
        bool urgent  = (s->awaiting_kind == AWAITING_APPROVE)
                    || (s->awaiting_kind == AWAITING_CLARIFY);
        uint32_t accent = waiting ? (urgent ? COL_GOLD_HI : COL_GOLD) : COL_TEAL;

        lv_obj_clear_flag(r->card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(r->card,
            lv_color_hex(waiting ? COL_SURFACE_HI : COL_SURFACE), 0);
        lv_obj_set_style_border_width(r->card, waiting ? 1 : 0, 0);
        lv_obj_set_style_border_color(r->card, lv_color_hex(accent), 0);

        lv_obj_set_style_bg_color(r->dot, lv_color_hex(accent), 0);

        lv_label_set_text(r->kind_lbl, short_kind(s->kind));

        const char *base = cwd_basename(s->cwd);
        char name[40];
        if (base) cjk_utf8_lcpy(name, base, sizeof(name));
        else {
            /* fall back to a short session id: first 4 + ':' + last 2 */
            size_t len = strlen(s->session_id);
            if (len > 6) snprintf(name, sizeof(name), "%.4s:%s",
                                  s->session_id, s->session_id + len - 2);
            else         snprintf(name, sizeof(name), "%s",
                                  s->session_id[0] ? s->session_id : "agent");
        }
        lv_label_set_text(r->name_lbl, name);

        char meta[16];
        if (waiting && s->awaiting_kind != AWAITING_NONE) {
            fmt_dur_s(meta, sizeof(meta), awaiting_elapsed_s(st, s));
        } else if (waiting) {
            snprintf(meta, sizeof(meta), "wait");
        } else {
            fmt_tokens_short(meta, sizeof(meta), s->tokens_today);
        }
        lv_label_set_text(r->meta_lbl, meta);
        lv_obj_set_style_text_color(r->meta_lbl,
            lv_color_hex(waiting ? accent : COL_TEXT_DIM), 0);

        char act[112];
        compose_activity(s, act, sizeof(act));
        lv_label_set_text(r->act_lbl, act);
        lv_obj_set_style_text_color(r->act_lbl,
            lv_color_hex(waiting ? accent : COL_TEXT_DIM), 0);
    }
}

/* ── tick ────────────────────────────────────────────────────────── */

static void tick(lv_timer_t *t)
{
    dash_t *d = (dash_t *)lv_timer_get_user_data(t);
    if (!d) return;

    agent_state_lock();
    agent_state_t *st = agent_state_get();
    status_bar_update(&d->sb, st);
    if (st->slot_count >= 2) render_fleet(d, st);
    else                     render_ambient(d, st);
    agent_state_unlock();
}

/* ── init ────────────────────────────────────────────────────────── */

static void init(scene_t *s, lv_obj_t *parent)
{
    s->container = parent;
    dash_t *d = lv_malloc(sizeof(dash_t));
    memset(d, 0, sizeof(dash_t));
    d->last_layout_n = -1;
    s->user_data = d;

    const theme_palette_t *pal = theme_current();
    lv_obj_set_style_bg_color(parent, lv_color_hex(pal ? pal->bg : COL_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    status_bar_create(parent, &d->sb);

    /* ambient group (single-agent mode) */
    d->ambient_grp = lv_obj_create(parent);
    lv_obj_remove_style_all(d->ambient_grp);
    lv_obj_set_size(d->ambient_grp, SCREEN_W, 260);
    lv_obj_align(d->ambient_grp, LV_ALIGN_TOP_MID, 0, AMBIENT_Y_CENTERED);
    lv_obj_clear_flag(d->ambient_grp, LV_OBJ_FLAG_SCROLLABLE);

    d->ambient_ring = lv_obj_create(d->ambient_grp);
    lv_obj_set_size(d->ambient_ring, 96, 96);
    lv_obj_align(d->ambient_ring, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(d->ambient_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(d->ambient_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(d->ambient_ring, lv_color_hex(COL_TEAL), 0);
    lv_obj_set_style_border_width(d->ambient_ring, 2, 0);
    lv_obj_set_style_border_opa(d->ambient_ring, LV_OPA_40, 0);
    lv_obj_clear_flag(d->ambient_ring, LV_OBJ_FLAG_SCROLLABLE);

    d->ambient_dot = lv_obj_create(d->ambient_ring);
    lv_obj_set_size(d->ambient_dot, 18, 18);
    lv_obj_center(d->ambient_dot);
    lv_obj_set_style_bg_color(d->ambient_dot, lv_color_hex(COL_TEAL), 0);
    lv_obj_set_style_bg_opa(d->ambient_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d->ambient_dot, 0, 0);
    lv_obj_set_style_radius(d->ambient_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(d->ambient_dot, LV_OBJ_FLAG_SCROLLABLE);

    d->ambient_lbl = lv_label_create(d->ambient_grp);
    lv_obj_set_style_text_color(d->ambient_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(d->ambient_lbl, &lv_font_montserrat_28, 0);
    lv_label_set_text(d->ambient_lbl, "idle");
    lv_obj_align(d->ambient_lbl, LV_ALIGN_TOP_MID, 0, 118);

    d->ambient_proj = lv_label_create(d->ambient_grp);
    lv_obj_set_style_text_color(d->ambient_proj, lv_color_hex(COL_TEXT_DIM), 0);
    { const lv_font_t *zf = cjk_font(18);
      lv_obj_set_style_text_font(d->ambient_proj, zf ? zf : &lv_font_montserrat_16, 0); }
    lv_label_set_text(d->ambient_proj, "");
    lv_obj_align(d->ambient_proj, LV_ALIGN_TOP_MID, 0, 164);

    d->ambient_act = lv_label_create(d->ambient_grp);
    lv_obj_set_size(d->ambient_act, 340, 24);   /* one line; DOT-truncate */
    lv_obj_set_style_text_align(d->ambient_act, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(d->ambient_act, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(d->ambient_act, lv_color_hex(COL_MUTE), 0);
    { const lv_font_t *zf = cjk_font(18);
      lv_obj_set_style_text_font(d->ambient_act, zf ? zf : &lv_font_montserrat_16, 0); }
    lv_label_set_text(d->ambient_act, "");
    lv_obj_align(d->ambient_act, LV_ALIGN_TOP_MID, 0, 194);

    /* fleet rows (multi-agent mode) — created hidden */
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        fleet_row_t *r = &d->rows[i];
        r->card = lv_obj_create(parent);
        lv_obj_remove_style_all(r->card);
        lv_obj_set_style_radius(r->card, 14, 0);
        lv_obj_set_style_bg_color(r->card, lv_color_hex(COL_SURFACE), 0);
        lv_obj_set_style_bg_opa(r->card, LV_OPA_COVER, 0);
        lv_obj_clear_flag(r->card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r->card, LV_OBJ_FLAG_HIDDEN);

        r->dot = lv_obj_create(r->card);
        lv_obj_remove_style_all(r->dot);
        lv_obj_set_size(r->dot, 14, 14);
        lv_obj_set_style_radius(r->dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(r->dot, lv_color_hex(COL_TEAL), 0);
        lv_obj_set_style_bg_opa(r->dot, LV_OPA_COVER, 0);

        r->kind_lbl = lv_label_create(r->card);
        lv_obj_set_style_text_color(r->kind_lbl, lv_color_hex(COL_MUTE), 0);
        lv_obj_set_style_text_font(r->kind_lbl, &lv_font_montserrat_12, 0);
        lv_label_set_text(r->kind_lbl, "");

        r->name_lbl = lv_label_create(r->card);
        lv_obj_set_style_text_color(r->name_lbl, lv_color_hex(COL_TEXT), 0);
        { const lv_font_t *zf = cjk_font(22);
          lv_obj_set_style_text_font(r->name_lbl, zf ? zf : &lv_font_montserrat_22, 0); }
        lv_label_set_long_mode(r->name_lbl, LV_LABEL_LONG_DOT);
        lv_label_set_text(r->name_lbl, "");

        r->meta_lbl = lv_label_create(r->card);
        lv_obj_set_style_text_color(r->meta_lbl, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_style_text_font(r->meta_lbl, &lv_font_montserrat_16, 0);
        lv_label_set_text(r->meta_lbl, "");

        r->act_lbl = lv_label_create(r->card);
        lv_obj_set_style_text_color(r->act_lbl, lv_color_hex(COL_TEXT_DIM), 0);
        { const lv_font_t *zf = cjk_font(18);
          lv_obj_set_style_text_font(r->act_lbl, zf ? zf : &lv_font_montserrat_16, 0); }
        lv_label_set_long_mode(r->act_lbl, LV_LABEL_LONG_DOT);
        lv_label_set_text(r->act_lbl, "");
    }

    arm_breath(d);

    d->timer = lv_timer_create(tick, 500, d);
    lv_timer_pause(d->timer);
}

static void on_show(scene_t *s)
{
    dash_t *d = (dash_t *)s->user_data;
    if (!d) return;
    if (d->timer) {
        lv_timer_resume(d->timer);
        tick(d->timer);
    }
}

static void on_hide(scene_t *s)
{
    dash_t *d = (dash_t *)s->user_data;
    if (d && d->timer) lv_timer_pause(d->timer);
}

scene_t scene_dashboard = {
    .id           = "dashboard",
    .display_name = "Dashboard",
    .accent       = LV_COLOR_MAKE(0x2B, 0xB3, 0xB1),
    .description  = "Adaptive fleet view: ambient pulse for one agent, per-agent rows for 2-4.",
    .tags         = "dashboard,fleet,ambient,home",
    .init         = init,
    .on_show      = on_show,
    .on_hide      = on_hide,
};
