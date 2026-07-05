/*
 * scene_overview — cross-agent rollup (v4).
 *
 * The middle stop of the BOOT view cycle (dashboard → overview → clock).
 * Where the dashboard renders each agent, this scene aggregates them:
 *
 *   agents present → big live-agent count, "N running · M waiting",
 *                    token totals (today + cumulative), kind mix line
 *                    ("cc x2 · cx x1").
 *   no agents      → the original zZz breathing empty state ("no
 *                    agents" / "agent just stopped").
 *
 * The wire id stays "idle" — `dash idle`, stress/profile tooling and
 * NVS default_scene values predate the rename and keep working.
 *
 * Both widget groups are pre-created in init(); the tick toggles
 * hidden flags on the group containers and only rewrites labels whose
 * text actually changed. State is copied out under agent_state_lock and
 * the lock released before any widget mutation (repo convention).
 */

#include "scenes.h"
#include "agent_state.h"
#include "theme.h"
#include "status_bar.h"
#include "cjk_font.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#define PERIOD_MS   2400u

#define SCREEN_W       466
#define COL_TEXT       0xF3EEE2
#define COL_TEXT_DIM   0x8A807A
#define COL_MUTE       0x5A514A
#define COL_TEAL       0x2BB3B1   /* the v2 breathing-ring teal */
#define COL_GOLD       0xB89020

/* Rollup cluster anchors: the band between the 48pt clock (~112) and
 * the footer numbers (~408). */
#define ROLL_Y        128   /* group top */
#define ROLL_NUM_Y      0   /* big count, inside group */
#define ROLL_STATE_Y  124
#define ROLL_TOKENS_Y 162
#define ROLL_KINDS_Y  196

typedef struct {
    status_bar_t sb;              /* shared top time + bottom active/tokens */
    /* rollup group (agents present) */
    lv_obj_t   *roll_grp;
    lv_obj_t   *roll_ring_out;    /* outer teal ring, breathing */
    lv_obj_t   *roll_ring_in;     /* inner teal ring, breathing */
    lv_obj_t   *roll_num;         /* big live-agent count — the "core" */
    lv_obj_t   *roll_state;       /* "N running · M waiting" */
    lv_obj_t   *roll_tokens;      /* "today 12.3k · total 4.5M" */
    lv_obj_t   *roll_kinds;       /* "cc x2 · cx x1" */
    /* Caches mirror the 80-byte compose buffer 1:1 so snprintf can never
     * truncate (format-truncation is -Werror on this toolchain). */
    char        cached_num[80];
    char        cached_state[80];
    char        cached_tokens[80];
    char        cached_kinds[80];
    /* zZz group (empty state) — teal ring + breathing dot, the original
     * v2 ambient design (recovered from pre-v3 scene_dashboard). */
    lv_obj_t   *ring;
    lv_obj_t   *dot;
    lv_obj_t   *zzz_a;
    lv_obj_t   *zzz_b;
    lv_obj_t   *zzz_c;
    lv_obj_t   *sub;
    lv_obj_t   *zzz_grp;
    lv_timer_t *timer;
    uint32_t    t0_ms;
    int         last_sub_state;   /* 0 = "no agents", 1 = "just stopped" */
} overview_state_t;

/* ── the recovered v2 breathing (teal ring + nested dot) ─────────────
 * Verbatim pattern from pre-v3 scene_dashboard (git 42af936^): a 96px
 * teal outline ring with a solid teal dot inside animating 16↔34 px,
 * 1.5s ease-in-out, infinite playback. The rollup variant breathes its
 * two rings around the big count instead. Armed once at init; LVGL
 * skips rendering while the owning group is hidden. */

static void anim_breath_size(void *obj, int32_t v)
{
    lv_obj_set_size((lv_obj_t *)obj, v, v);
    lv_obj_center((lv_obj_t *)obj);
}

static void arm_breath_size(lv_obj_t *obj, int32_t from, int32_t to,
                            uint32_t ms)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    lv_anim_set_playback_time(&a, ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, anim_breath_size);
    lv_anim_start(&a);
}

/* Ring sizes must re-center around a fixed point: keep them children of
 * a fixed-size wrapper and lv_obj_center in the exec cb (as above). */
static lv_obj_t *mk_teal_ring(lv_obj_t *parent, int size, lv_opa_t opa)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, size, size);
    lv_obj_center(r);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(COL_TEAL), 0);
    lv_obj_set_style_border_width(r, 2, 0);
    lv_obj_set_style_border_opa(r, opa, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}

/* Same table as scene_dashboard's short_kind (static there). */
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

/* k/M formatting lifted from the old scene_sessions rollup. */
static void fmt_tokens(char *buf, size_t cap, uint64_t v)
{
    if (v < 1000)            snprintf(buf, cap, "%u", (unsigned)v);
    else if (v < 1000000ULL) snprintf(buf, cap, "%.1fk", (double)v / 1000.0);
    else                     snprintf(buf, cap, "%.1fM", (double)v / 1000000.0);
}

/* Update a label only when its text changed — keeps the 60ms tick from
 * invalidating four static labels every frame. */
static void set_if_changed(lv_obj_t *lbl, char *cache, size_t cap,
                           const char *text)
{
    if (strncmp(cache, text, cap) == 0) return;
    snprintf(cache, cap, "%s", text);
    lv_label_set_text(lbl, text);
}

/* ── zZz empty state (carried over from scene_idle v1) ───────────── */

static lv_opa_t letter_opa(uint32_t phase_ms, int letter_idx)
{
    /* Each letter is offset by 200 ms; full cycle 2400 ms; each letter
     * is "on" for the middle 40% of its slot. */
    uint32_t local = (phase_ms + (uint32_t)letter_idx * 200u) % PERIOD_MS;
    if (local < PERIOD_MS * 30 / 100) {
        /* fade-in */
        return (lv_opa_t)((local * 255u) / (PERIOD_MS * 30 / 100));
    } else if (local < PERIOD_MS * 70 / 100) {
        return 255;
    } else {
        uint32_t remaining = PERIOD_MS - local;
        uint32_t span = PERIOD_MS * 30 / 100;
        if (span == 0) return 0;
        return (lv_opa_t)((remaining * 255u) / span);
    }
}

static bool recently_stopped(uint32_t *out_seconds)
{
    /* Look at the most-recent last_active_unix across agents. If we have
     * host clock and (now - last_active) < 30s, return true. Only called
     * when no slot is live (the rollup owns the populated case). */
    uint32_t epoch = 0;
    uint32_t clk_received_ms = 0;
    uint32_t latest = 0;
    agent_state_lock();
    agent_state_t *s = agent_state_get();
    epoch = s->host_epoch_unix;
    clk_received_ms = s->host_clock_received_ms;
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        if (!s->slots[i].in_use) continue;
        if (s->slots[i].last_active_unix > latest)
            latest = s->slots[i].last_active_unix;
    }
    /* fallback: even if a slot was pruned, the last_snapshot_ms tells us
     * something happened "now-ish". We treat any snapshot inside 30s as
     * "active". */
    uint32_t last_snap = s->last_snapshot_ms;
    bool ever = s->ever_received;
    agent_state_unlock();

    if (epoch && latest) {
        uint32_t now_epoch = epoch + (lv_tick_get() - clk_received_ms) / 1000u;
        if (now_epoch >= latest && now_epoch - latest < 30) {
            if (out_seconds) *out_seconds = now_epoch - latest;
            return true;
        }
    }
    if (ever) {
        uint32_t age = (lv_tick_get() - last_snap) / 1000u;
        if (age < 30) {
            if (out_seconds) *out_seconds = age;
            return true;
        }
    }
    return false;
}

static void render_zzz(overview_state_t *st, uint32_t phase)
{
    const theme_palette_t *pal = theme_current();

    /* The teal ring + dot breathe on their own lv_anim (armed at init);
     * only the letters and subtitle are driven from the tick. */
    lv_obj_set_style_text_opa(st->zzz_a, letter_opa(phase, 0), 0);
    lv_obj_set_style_text_opa(st->zzz_b, letter_opa(phase, 1), 0);
    lv_obj_set_style_text_opa(st->zzz_c, letter_opa(phase, 2), 0);
    lv_obj_set_style_text_color(st->zzz_a, lv_color_hex(pal->text), 0);
    lv_obj_set_style_text_color(st->zzz_b, lv_color_hex(pal->text), 0);
    lv_obj_set_style_text_color(st->zzz_c, lv_color_hex(pal->text), 0);

    int desired = recently_stopped(NULL) ? 1 : 0;
    if (desired != st->last_sub_state) {
        lv_label_set_text(st->sub, desired ? "agent just stopped" : "no agents");
        st->last_sub_state = desired;
    }
    lv_obj_set_style_text_color(st->sub, lv_color_hex(pal->text_dim), 0);
}

/* ── rollup ──────────────────────────────────────────────────────── */

static void render_rollup(overview_state_t *st, int slot_count,
                          int running, int waiting,
                          uint64_t tok_today, uint64_t tok_cum,
                          const char kinds[][AGENT_KIND_MAX],
                          const bool inuse[])
{
    char buf[80];

    snprintf(buf, sizeof(buf), "%d", slot_count);
    set_if_changed(st->roll_num, st->cached_num, sizeof(st->cached_num), buf);

    /* "·" (U+00B7) is proven renderable in the device font subset. */
    snprintf(buf, sizeof(buf), "%d running \xC2\xB7 %d waiting",
             running, waiting);
    set_if_changed(st->roll_state, st->cached_state,
                   sizeof(st->cached_state), buf);
    /* Gold when someone needs the user, calm dim otherwise. */
    lv_obj_set_style_text_color(st->roll_state,
        lv_color_hex(waiting > 0 ? COL_GOLD : COL_TEXT_DIM), 0);

    char today[16], cum[16];
    fmt_tokens(today, sizeof(today), tok_today);
    fmt_tokens(cum, sizeof(cum), tok_cum);
    snprintf(buf, sizeof(buf), "today %s \xC2\xB7 total %s", today, cum);
    set_if_changed(st->roll_tokens, st->cached_tokens,
                   sizeof(st->cached_tokens), buf);

    /* Kind mix, stable slot order: "cc x2 · cx x1". */
    const char *seen_sk[AGENT_SLOT_MAX];
    int seen_n[AGENT_SLOT_MAX];
    int uniq = 0;
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        if (!inuse[i]) continue;
        const char *sk = short_kind(kinds[i]);
        int j = 0;
        for (; j < uniq; ++j) {
            if (strcmp(seen_sk[j], sk) == 0) { seen_n[j]++; break; }
        }
        if (j == uniq) { seen_sk[uniq] = sk; seen_n[uniq] = 1; uniq++; }
    }
    size_t used = 0;
    buf[0] = '\0';
    for (int j = 0; j < uniq && used < sizeof(buf); ++j) {
        used += (size_t)snprintf(buf + used, sizeof(buf) - used,
                                 "%s%s x%d", j ? " \xC2\xB7 " : "",
                                 seen_sk[j], seen_n[j]);
    }
    set_if_changed(st->roll_kinds, st->cached_kinds,
                   sizeof(st->cached_kinds), buf);
}

/* ── tick ────────────────────────────────────────────────────────── */

static void overview_tick(lv_timer_t *t)
{
    overview_state_t *st = (overview_state_t *)lv_timer_get_user_data(t);
    if (!st) return;

    int slot_count, running, waiting;
    uint64_t tok_today, tok_cum;
    char kinds[AGENT_SLOT_MAX][AGENT_KIND_MAX];
    bool inuse[AGENT_SLOT_MAX];

    agent_state_lock();
    agent_state_t *s = agent_state_get();
    status_bar_update(&st->sb, s);
    slot_count = s->slot_count;
    running    = s->running;
    waiting    = s->waiting;
    tok_today  = s->tokens_today;
    tok_cum    = s->tokens_cumulative;
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        inuse[i] = s->slots[i].in_use;
        memcpy(kinds[i], s->slots[i].kind, AGENT_KIND_MAX);
    }
    agent_state_unlock();

    if (slot_count > 0) {
        lv_obj_add_flag(st->zzz_grp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(st->roll_grp, LV_OBJ_FLAG_HIDDEN);
        render_rollup(st, slot_count, running, waiting,
                      tok_today, tok_cum, kinds, inuse);
    } else {
        lv_obj_add_flag(st->roll_grp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(st->zzz_grp, LV_OBJ_FLAG_HIDDEN);
        render_zzz(st, lv_tick_get() - st->t0_ms);
    }
}

/* ── init / lifecycle ────────────────────────────────────────────── */

static lv_obj_t *mk_group(lv_obj_t *parent, int y, int h)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, SCREEN_W, h);
    lv_obj_align(g, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);
    return g;
}

static void overview_init(scene_t *s, lv_obj_t *parent)
{
    overview_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;
    st->t0_ms = lv_tick_get();
    st->last_sub_state = -1;

    status_bar_create(parent, &st->sb);

    /* rollup group */
    st->roll_grp = mk_group(parent, ROLL_Y, 240);

    /* Nested teal rings around the big count (created first: z-under).
     * The count is the "core"; the rings breathe in size like the v2
     * dot did. A fixed wrapper keeps them centred while resizing. */
    lv_obj_t *ringbox = lv_obj_create(st->roll_grp);
    lv_obj_remove_style_all(ringbox);
    lv_obj_set_size(ringbox, 200, 200);
    lv_obj_align(ringbox, LV_ALIGN_TOP_MID, 0, ROLL_NUM_Y - 40);
    lv_obj_clear_flag(ringbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ringbox, LV_OBJ_FLAG_CLICKABLE);
    st->roll_ring_out = mk_teal_ring(ringbox, 178, LV_OPA_20);
    st->roll_ring_in  = mk_teal_ring(ringbox, 148, LV_OPA_40);
    arm_breath_size(st->roll_ring_in,  144, 158, 1500);
    arm_breath_size(st->roll_ring_out, 172, 186, 1500);

    st->roll_num = lv_label_create(st->roll_grp);
    lv_obj_set_style_text_font(st->roll_num,
                               ui_font_bold_or(96, &lv_font_montserrat_48), 0);
    lv_obj_set_style_text_color(st->roll_num, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(st->roll_num, "0");
    lv_obj_align(st->roll_num, LV_ALIGN_TOP_MID, 0, ROLL_NUM_Y);

    const lv_font_t *cf = ui_font_or(18, &lv_font_montserrat_16);
    st->roll_state = lv_label_create(st->roll_grp);
    lv_obj_set_style_text_font(st->roll_state, cf, 0);
    lv_obj_set_style_text_color(st->roll_state, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(st->roll_state, "");
    lv_obj_align(st->roll_state, LV_ALIGN_TOP_MID, 0, ROLL_STATE_Y);

    st->roll_tokens = lv_label_create(st->roll_grp);
    lv_obj_set_style_text_font(st->roll_tokens, cf, 0);
    lv_obj_set_style_text_color(st->roll_tokens, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(st->roll_tokens, "");
    lv_obj_align(st->roll_tokens, LV_ALIGN_TOP_MID, 0, ROLL_TOKENS_Y);

    st->roll_kinds = lv_label_create(st->roll_grp);
    lv_obj_set_style_text_font(st->roll_kinds, cf, 0);
    lv_obj_set_style_text_color(st->roll_kinds, lv_color_hex(COL_MUTE), 0);
    lv_label_set_text(st->roll_kinds, "");
    lv_obj_align(st->roll_kinds, LV_ALIGN_TOP_MID, 0, ROLL_KINDS_Y);

    lv_obj_add_flag(st->roll_grp, LV_OBJ_FLAG_HIDDEN);

    /* zZz group (empty state) — the recovered v2 ambient: 96px teal
     * outline ring + solid teal dot breathing 16↔34 inside it. */
    st->zzz_grp = mk_group(parent, 0, 466);

    st->ring = mk_teal_ring(st->zzz_grp, 96, LV_OPA_40);
    lv_obj_align(st->ring, LV_ALIGN_CENTER, 0, -24);

    st->dot = lv_obj_create(st->ring);
    lv_obj_remove_style_all(st->dot);
    lv_obj_set_size(st->dot, 18, 18);
    lv_obj_center(st->dot);
    lv_obj_set_style_bg_color(st->dot, lv_color_hex(COL_TEAL), 0);
    lv_obj_set_style_bg_opa(st->dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(st->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(st->dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(st->dot, LV_OBJ_FLAG_CLICKABLE);
    arm_breath_size(st->dot, 16, 34, 1500);

    /* Three independent z letters so we can opa them separately —
     * moved below the ring (they used to sit on the old grey disc). */
    st->zzz_a = lv_label_create(st->zzz_grp);
    st->zzz_b = lv_label_create(st->zzz_grp);
    st->zzz_c = lv_label_create(st->zzz_grp);
    { const lv_font_t *zf = ui_font_or(22, &lv_font_montserrat_22);
      lv_obj_set_style_text_font(st->zzz_a, zf, 0);
      lv_obj_set_style_text_font(st->zzz_b, zf, 0);
      lv_obj_set_style_text_font(st->zzz_c, zf, 0); }
    lv_label_set_text(st->zzz_a, "z");
    lv_label_set_text(st->zzz_b, "Z");
    lv_label_set_text(st->zzz_c, "z");
    lv_obj_align(st->zzz_a, LV_ALIGN_CENTER, -22, 62);
    lv_obj_align(st->zzz_b, LV_ALIGN_CENTER,   0, 62);
    lv_obj_align(st->zzz_c, LV_ALIGN_CENTER,  22, 62);

    st->sub = lv_label_create(st->zzz_grp);
    lv_obj_set_style_text_font(st->sub, ui_font_or(14, &lv_font_montserrat_14), 0);
    lv_obj_set_style_text_opa(st->sub, LV_OPA_70, 0);
    lv_label_set_text(st->sub, "no agents");
    lv_obj_align(st->sub, LV_ALIGN_CENTER, 0, 120);

    st->timer = lv_timer_create(overview_tick, 60, st);
    lv_timer_pause(st->timer);
    overview_tick(st->timer);
}

static void overview_on_show(scene_t *s)
{
    overview_state_t *st = (overview_state_t *)s->user_data;
    if (!st) return;
    st->t0_ms = lv_tick_get();
    if (st->timer) {
        lv_timer_resume(st->timer);
        overview_tick(st->timer);
    }
}

static void overview_on_hide(scene_t *s)
{
    overview_state_t *st = (overview_state_t *)s->user_data;
    if (st && st->timer) lv_timer_pause(st->timer);
}

scene_t scene_overview = {
    .id           = "idle",   /* wire-compat: dash idle / NVS default_scene */
    .display_name = "Overview",
    .accent       = LV_COLOR_MAKE(0x6B, 0x6F, 0x7A),
    .description  = "Cross-agent rollup: live count, running/waiting, token "
                    "totals, kind mix; zZz empty state when no agents.",
    .tags         = "agent,overview,rollup,idle",
    .init         = overview_init,
    .on_show      = overview_on_show,
    .on_hide      = overview_on_hide,
};
