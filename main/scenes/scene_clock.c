/*
 * scene_clock — StandBy-style big clock (v4).
 *
 * The third stop of the BOOT view cycle (dashboard → overview → clock).
 * iPhone-landscape-charging vibe: nothing on screen but a large
 * centered HH:MM plus the shared active/tokens footer. The status
 * bar's own top clock is hidden — the face IS the clock — while its
 * connection-health pill keeps working (a stale link must be visible
 * on every environment scene).
 *
 * Time source is status_bar_format_time(), the exact math the top
 * clock uses (host epoch + tick delta + tz), so the two can never
 * disagree; "--:--" until the host pushes `dash time`.
 *
 * The face is rendered by tiny_ttf at CLOCK_PX from the embedded
 * M PLUS Rounded 1c Black digit subset (clock_font() — rounded, heavy,
 * colon side bearings pre-tightened; the SF-Rounded look StandBy has).
 * Falls back to Montserrat when tiny_ttf is unavailable.
 *
 * v4.5: the colon BLINKS ~1 Hz like a standard digital clock. The font
 * has no space glyph and tabular digits (adv 660), so the face is three
 * separate labels — hours / colon / minutes — inside a container. Only
 * the colon's text_opa toggles (from the scene tick, deduped — never a
 * per-frame anim over a big tiny_ttf label, per CLAUDE.md). Because the
 * digits are tabular, the three fixed-offset label boxes sit edge-to-
 * edge exactly like the old single "HH:MM" string and never shift.
 *
 * Entrance (v4 M4, plan B): on every show the group starts at the top
 * small-clock position, fully transparent, and glides down to center
 * while fading in — apple_ease_out on both tracks. Plan A additionally
 * animated transform_scale (small→full size), but scaling a 135px
 * tiny_ttf label re-renders it through an intermediate layer every
 * frame and measured 12.5-13.4 fps against the panel's 30 (visible
 * stutter); y+fade keeps the "top clock becomes the big clock" story
 * at full frame rate. motion_reduced skips straight to the resting
 * pose (and holds the colon solid — a blink is motion).
 */

#include "scenes.h"
#include "agent_state.h"
#include "status_bar.h"
#include "cjk_font.h"
#include "ui_type.h"
#include "anim/apple_ease.h"
#include "anim/spring.h"
#include "scene_trans.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "ui_screen.h"
#include "lvgl.h"

/* 屏宽 = 坐标空间。v7.4 之前这里硬写 466，而 466 从来不是面板宽度
 * （见 CLAUDE.md 的 Panel geometry）——凡是靠它【算】出来的居中都会
 * 左偏 7px。用 LV_ALIGN_*_MID 对齐的元素不受影响，那是相对父容器的。 */
#define SCREEN_W     UI_LV_W
#define CLOCK_PX    135   /* was 150; pulled to ~90% on user feedback */
#define COL_TEXT    0xF3EEE2
#define COL_DIM     0x8A807A

/* Horizontal offset of the hours / minutes labels from the centred
 * colon: half the sum of a two-digit advance (2×660) and the colon
 * advance (286), scaled from the font's 1000 units/em. This places the
 * three label boxes edge-to-edge, reproducing the old single-string
 * layout pixel-for-pixel; tabular digits keep it stable as time
 * changes. = 803*135/1000 = 108 px. */
#define COLON_DX    (((1320 + 286) / 2 * CLOCK_PX) / 1000)

/* Face group is tall enough to hold the 135px glyphs without clipping,
 * full-width so the ±108 labels never clip horizontally. */
#define FACE_GRP_H  200

/* v6.7: 时间+天气行整体上移 4% 屏高（466*0.04≈19px）。
 *
 * 目的不是"把它放高"，而是把【视觉重心】拉回正中：大钟本来是几何居中的，
 * 但 v6.5 在它下面加了一行常驻天气，墨迹重心因此整体下沉，读起来偏低。
 * 补的量约等于那行墨迹给重心带来的位移，所以 4% 而不是更多——15% 试过，
 * 那是把整块内容搬上去，底部会空出一大块。
 *
 * face_grp 是 CENTER 对齐，所以 rest 的 y 偏移不再是 0 而是 -FACE_RISE；
 * 天气行同步上移相同量，两者作为一个视觉整体一起动。 */
#define FACE_RISE    19
#define FACE_REST_Y  (-FACE_RISE)

/* 常驻天气行。原 322：face_grp 居中 200 高 -> 大钟墨迹约 166..300，footer
 * 数字从 392 起，322 落在两者之间。上移后同步减 FACE_RISE。 */
#define WX_LINE_Y   (322 - FACE_RISE)

/* Colon blink: half-period decoupled from the tick rate. 1000 ms half
 * = 1 s on / 1 s off = 0.5 Hz — a calm pulse (1 Hz read as too fast).
 * The tick stays quicker so the minute rollover and connection pill
 * update promptly; the blink phase is derived from elapsed time, so it
 * toggles exactly on the half-period regardless of tick rate. */
#define CLOCK_TICK_MS  500
#define BLINK_MS      1000

/* Entrance geometry. The group is CENTER-aligned, so y is an offset
 * from the vertical middle (233 on this 466px panel). The top clock
 * renders at TOP_MID y=56 with a 48pt font — its visual center sits at
 * ≈56+29=85, i.e. offset 85-233 = -148 from screen center. */
#define ENTRY_Y      (-148)
#define ENTRY_MS     550

/* ── push subsystem: agent start/end notifications ──────────────────
 * While the clock is the visible scene, an agent entering RUN (task
 * start) or leaving RUN to idle/removed (task end) briefly "pushes" a
 * card in the centre. The big face retreats to the top-clock slot via
 * the plan-B morph — slide up (ENTRY_Y) + text_opa fade while the status
 * bar's own 48pt top clock fades IN at exactly that spot; the two never
 * scale, so the "big clock shrank to the corner" reads at full frame
 * rate (scaling the 135px tiny_ttf face measured 12-13 fps — the retired
 * clock plan A). A running/finished glyph + headline + project chip then
 * appear; after PUSH_HOLD_MS everything reverses back to the pure face.
 * Coalesces: a fresh event inside the window swaps the card content and
 * restarts the countdown rather than re-morphing. */
#define PUSH_START        0
#define PUSH_END          1
#define PUSH_HOLD_MS   3000     /* dwell before auto-restore to the face */
#define PUSH_MORPH_MS   440     /* face → top-clock retreat (restore = ENTRY_MS) */
#define PUSH_CARD_MS    360     /* card enter / exit */
#define PUSH_CARD_DELAY 120     /* card lags the retreat so the face clears first */
#define PUSH_ENTER_DY    24     /* card rises this many px as it fades in */
#define PUSH_GLYPH       92     /* glyph zone (orbit ring+dot, or the √) */
#define PUSH_RING_SZ     76
#define PUSH_DOT_SZ      14
#define PUSH_ORBIT_R     30     /* dot orbit radius about the glyph centre */
#define PUSH_ORBIT_MS  1200     /* one revolution */
#define PUSH_LABEL_MAX   28     /* UTF-8-safe project name, like scene_awaiting */
#define COL_TEAL     0x2BB3B1   /* running accent (palette.md) */
#define COL_TRACK    0x2A4A49   /* faint orbit ring */

typedef struct {
    status_bar_t sb;          /* footer + conn pill; top clock hidden */
    lv_obj_t   *face_grp;     /* container: entrance animates THIS */
    lv_obj_t   *hh;           /* hours "HH" */
    lv_obj_t   *colon;        /* ":" — only its text_opa toggles */
    lv_obj_t   *mm;           /* minutes "MM" */
    int         face_px;      /* current size rung of the face (morph) */
    int32_t     morph_v;      /* live morph factor 0..1000 (1000 = big) */
    lv_timer_t *timer;
    char        cached[16];   /* last rendered time string */
    uint32_t    show_ms;      /* on_show tick — blink phase + entrance gate */
    bool        motion_ok;    /* mirror of !motion_reduced from on_show */
    int         colon_on;     /* last applied colon state; -1 forces apply */
    lv_obj_t   *wx_lbl;       /* v6.5: 常驻天气行（大钟下方） */
    char        cached_wx[96];

    /* ── push subsystem ── */
    lv_obj_t   *push_grp;     /* centre card container (fade/slide as one) */
    lv_obj_t   *push_glyph;   /* 92×92 zone holding the ring/dot/√ */
    lv_obj_t   *push_ring;    /* faint orbit track (START) / settle ring (END) */
    lv_obj_t   *push_dot;     /* bright dot orbiting the ring (START) */
    lv_obj_t   *push_check;   /* LV_SYMBOL_OK (END) */
    lv_obj_t   *push_head;    /* headline: 开始运行 / 运行结束 */
    lv_obj_t   *push_chip;    /* "cc  <project>" */
    lv_timer_t *push_hold;    /* one-shot dwell timer (paused between pushes) */
    bool        push_active;  /* a card is currently up */
    int         push_kind;    /* PUSH_START / PUSH_END of the current card */

    /* running-set edge detection: the set of (kind, sid) that were
     * RUNNING at the previous tick, with each one's project label cached
     * so an END push can still name an agent whose slot was pruned. */
    struct {
        char kind[AGENT_KIND_MAX];
        char sid[AGENT_SESSION_ID_MAX];
        char label[PUSH_LABEL_MAX];
    } run_prev[AGENT_SLOT_MAX];
    int  run_prev_n;
    bool run_seeded;          /* false on show → first tick only seeds */
} clock_state_t;

/* Apply one text_opa to every child of the group (hh/colon/mm). Used by
 * the entrance fade — text_opa does NOT inherit from a container, and
 * the code deliberately avoids widget/style opa (it composites through
 * an intermediate layer at 9-15 fps; text_opa is a plain per-pixel
 * alpha applied while blitting the glyphs — no layer). */
static void set_group_text_opa(lv_obj_t *grp, lv_opa_t opa)
{
    uint32_t n = lv_obj_get_child_count(grp);
    for (uint32_t i = 0; i < n; ++i)
        lv_obj_set_style_text_opa(lv_obj_get_child(grp, i), opa, 0);
}

/* 天气行的淡入淡出。它跟着大钟一起让位给 push 卡：卡片墨迹到 y~338，
 * 天气行在 252..283，两者重叠——大钟退到顶部而天气行留在原地的话，卡片
 * 会直接压在它上面（用户报的"日期概要和切过去的界面重叠"）。 */
static void anim_wxline_opa(void *obj, int32_t v)
{
    lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void anim_grp_y(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

static void anim_grp_opa(void *obj, int32_t v)
{
    set_group_text_opa((lv_obj_t *)obj, (lv_opa_t)v);
}

/* Play (or skip) the top-to-center entrance. Called from on_show on the
 * LVGL task; always resets to a deterministic start pose first so a
 * re-entry mid-animation can't compound. */
static void clock_entrance(clock_state_t *st, bool motion_ok)
{
    lv_anim_delete(st->face_grp, anim_grp_y);
    lv_anim_delete(st->face_grp, anim_grp_opa);

    lv_anim_delete(st->wx_lbl, anim_wxline_opa);
    if (!motion_ok) {
        lv_obj_set_y(st->face_grp, FACE_REST_Y);
        set_group_text_opa(st->face_grp, LV_OPA_COVER);
        lv_obj_set_style_text_opa(st->wx_lbl, LV_OPA_COVER, 0);
        return;
    }

    lv_obj_set_y(st->face_grp, ENTRY_Y);
    set_group_text_opa(st->face_grp, LV_OPA_TRANSP);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, st->face_grp);
    lv_anim_set_time(&a, ENTRY_MS);
    lv_anim_set_path_cb(&a, apple_ease_out);

    lv_anim_set_values(&a, ENTRY_Y, FACE_REST_Y);
    lv_anim_set_exec_cb(&a, anim_grp_y);
    lv_anim_start(&a);

    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a, anim_grp_opa);
    lv_anim_start(&a);

    /* 天气行与大钟同进同退（见 anim_wxline_opa）。 */
    lv_anim_delete(st->wx_lbl, anim_wxline_opa);
    lv_obj_set_style_text_opa(st->wx_lbl, LV_OPA_TRANSP, 0);
    lv_anim_set_var(&a, st->wx_lbl);
    lv_anim_set_exec_cb(&a, anim_wxline_opa);
    lv_anim_start(&a);
}

/* ── push subsystem helpers ─────────────────────────────────────────
 * Everything below drives the transient agent start/end card. All
 * fades use per-object opa the same way the entrance does: text_opa on
 * labels, border/bg_opa on the shapes — never widget/style opa on a
 * text-bearing container (that composites through a layer). */

/* Same kind → 2-letter abbreviation table as scene_awaiting's chip. */
static const char *short_kind_of(const char *kind)
{
    if (strcmp(kind, "claude-code") == 0) return "cc";
    if (strcmp(kind, "codex") == 0)       return "cx";
    if (strcmp(kind, "cursor") == 0)      return "cu";
    if (strcmp(kind, "aider") == 0)       return "ai";
    if (strcmp(kind, "windsurf") == 0)    return "ws";
    if (strcmp(kind, "copilot") == 0)     return "cp";
    if (strcmp(kind, "qwen-code") == 0)   return "qw";
    return "ag";
}

/* Project = last path segment of cwd (UTF-8-safe trunc; never splits a
 * hanzi). Empty string when cwd is unknown. */
static void project_label(const char *cwd, char *out, size_t cap)
{
    if (!cap) return;
    const char *base = cwd;
    for (const char *p = cwd; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    if (base && base[0]) cjk_utf8_lcpy(out, base, cap);
    else                 out[0] = '\0';
}

/* Snapshot the current RUNNING set into run_prev. Caller holds the
 * agent_state lock (reads slots). */
static void snapshot_running(clock_state_t *st, agent_state_t *s)
{
    st->run_prev_n = 0;
    for (int i = 0; i < AGENT_SLOT_MAX && st->run_prev_n < AGENT_SLOT_MAX; ++i) {
        agent_slot_t *sl = &s->slots[i];
        if (!sl->in_use || sl->status != AGENT_STATUS_RUNNING) continue;
        int k = st->run_prev_n++;
        strncpy(st->run_prev[k].kind, sl->kind, AGENT_KIND_MAX - 1);
        st->run_prev[k].kind[AGENT_KIND_MAX - 1] = '\0';
        strncpy(st->run_prev[k].sid, sl->session_id, AGENT_SESSION_ID_MAX - 1);
        st->run_prev[k].sid[AGENT_SESSION_ID_MAX - 1] = '\0';
        project_label(sl->cwd, st->run_prev[k].label, PUSH_LABEL_MAX);
    }
}

/* Fade helper: one alpha applied across every visible piece of the card. */
static void push_set_opa(clock_state_t *st, lv_opa_t v)
{
    lv_obj_set_style_text_opa(st->push_head, v, 0);
    lv_obj_set_style_text_opa(st->push_chip, v, 0);
    lv_obj_set_style_text_opa(st->push_check, v, 0);
    lv_obj_set_style_border_opa(st->push_ring, v, 0);
    lv_obj_set_style_bg_opa(st->push_dot, v, 0);
}

static void anim_push_y(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

static void anim_push_opa(void *obj, int32_t v)
{
    clock_state_t *st = lv_obj_get_user_data((lv_obj_t *)obj);
    if (st) push_set_opa(st, (lv_opa_t)v);
}
/* Exit variant: hides the card once fully faded — same trick the scene
 * framework uses (hide inside the exec at 0) instead of a completed_cb. */
static void anim_push_opa_out(void *obj, int32_t v)
{
    clock_state_t *st = lv_obj_get_user_data((lv_obj_t *)obj);
    if (st) push_set_opa(st, (lv_opa_t)v);
    if (v == 0) lv_obj_add_flag((lv_obj_t *)obj, LV_OBJ_FLAG_HIDDEN);
}

static void anim_topclock_in(void *obj, int32_t v)
{
    lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}
static void anim_topclock_out(void *obj, int32_t v)
{
    lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
    if (v == 0) lv_obj_add_flag((lv_obj_t *)obj, LV_OBJ_FLAG_HIDDEN);
}


/* Orbit: the dot walks a circle about the glyph-zone centre. deg10 is
 * tenths of a degree; −90° start puts it at 12 o'clock. A single small
 * lv_obj position update per frame — no layer, no big-label re-raster. */
static void anim_orbit(void *obj, int32_t deg10)
{
    float a = ((float)deg10 / 10.0f - 90.0f) * 3.14159265f / 180.0f;
    int c = PUSH_GLYPH / 2;
    int x = c + (int)lroundf(PUSH_ORBIT_R * cosf(a)) - PUSH_DOT_SZ / 2;
    int y = c + (int)lroundf(PUSH_ORBIT_R * sinf(a)) - PUSH_DOT_SZ / 2;
    lv_obj_set_pos((lv_obj_t *)obj, x, y);
}

static void start_orbit(clock_state_t *st)
{
    lv_anim_delete(st->push_dot, anim_orbit);
    if (!st->motion_ok) { anim_orbit(st->push_dot, 0); return; }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, st->push_dot);
    lv_anim_set_time(&a, PUSH_ORBIT_MS);
    lv_anim_set_values(&a, 0, 3600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_exec_cb(&a, anim_orbit);
    lv_anim_start(&a);
}

/* Big face → top-clock slot: slide up + text_opa fade, the 48pt top
 * clock fading in at the same centre. Zero scaling. */
static void retreat_clock(clock_state_t *st)
{
    lv_anim_delete(st->face_grp, anim_grp_y);
    lv_anim_delete(st->face_grp, anim_grp_opa);
    lv_obj_clear_flag(st->sb.time_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_anim_delete(st->sb.time_lbl, anim_topclock_in);
    lv_anim_delete(st->sb.time_lbl, anim_topclock_out);

    lv_anim_delete(st->wx_lbl, anim_wxline_opa);
    if (!st->motion_ok) {
        lv_obj_set_y(st->face_grp, ENTRY_Y);
        set_group_text_opa(st->face_grp, LV_OPA_TRANSP);
        lv_obj_set_style_text_opa(st->sb.time_lbl, LV_OPA_COVER, 0);
        lv_obj_set_style_text_opa(st->wx_lbl, LV_OPA_TRANSP, 0);
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, st->face_grp);
    lv_anim_set_time(&a, PUSH_MORPH_MS);
    lv_anim_set_path_cb(&a, apple_ease_out);
    lv_anim_set_values(&a, lv_obj_get_y(st->face_grp), ENTRY_Y);
    lv_anim_set_exec_cb(&a, anim_grp_y);
    lv_anim_start(&a);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_exec_cb(&a, anim_grp_opa);
    lv_anim_start(&a);

    lv_anim_set_var(&a, st->wx_lbl);
    lv_anim_set_exec_cb(&a, anim_wxline_opa);
    lv_anim_start(&a);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, st->sb.time_lbl);
    lv_anim_set_time(&b, PUSH_MORPH_MS);
    lv_anim_set_path_cb(&b, apple_ease_out);
    lv_anim_set_values(&b, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&b, anim_topclock_in);
    lv_anim_start(&b);
}

static void enter_push(clock_state_t *st)
{
    lv_anim_delete(st->push_grp, anim_push_y);
    lv_anim_delete(st->push_grp, anim_push_opa);
    lv_anim_delete(st->push_grp, anim_push_opa_out);
    lv_obj_clear_flag(st->push_grp, LV_OBJ_FLAG_HIDDEN);

    if (!st->motion_ok) {
        lv_obj_set_y(st->push_grp, 0);
        push_set_opa(st, LV_OPA_COVER);
        return;
    }

    lv_obj_set_y(st->push_grp, PUSH_ENTER_DY);
    push_set_opa(st, LV_OPA_TRANSP);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, st->push_grp);
    lv_anim_set_time(&a, PUSH_CARD_MS);
    lv_anim_set_delay(&a, PUSH_CARD_DELAY);
    lv_anim_set_path_cb(&a, apple_ease_out);
    lv_anim_set_values(&a, PUSH_ENTER_DY, 0);
    lv_anim_set_exec_cb(&a, anim_push_y);
    lv_anim_start(&a);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a, anim_push_opa);
    lv_anim_start(&a);
}

/* Reverse the whole thing: card fades/slides out, the face glides back
 * down + in (reusing clock_entrance), the top clock fades away. */
static void clock_push_dismiss(clock_state_t *st)
{
    if (!st->push_active) return;
    st->push_active = false;
    if (st->push_hold) lv_timer_pause(st->push_hold);
    lv_anim_delete(st->push_dot, anim_orbit);

    lv_anim_delete(st->push_grp, anim_push_y);
    lv_anim_delete(st->push_grp, anim_push_opa);
    lv_anim_delete(st->push_grp, anim_push_opa_out);
    if (!st->motion_ok) {
        push_set_opa(st, LV_OPA_TRANSP);
        lv_obj_add_flag(st->push_grp, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, st->push_grp);
        lv_anim_set_time(&a, PUSH_CARD_MS);
        lv_anim_set_path_cb(&a, apple_ease_in);
        lv_anim_set_values(&a, 0, PUSH_ENTER_DY);
        lv_anim_set_exec_cb(&a, anim_push_y);
        lv_anim_start(&a);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_exec_cb(&a, anim_push_opa_out);
        lv_anim_start(&a);
    }

    /* Face back in; re-sync the colon blink from now. */
    st->show_ms = lv_tick_get();
    st->colon_on = -1;
    clock_entrance(st, st->motion_ok);

    lv_anim_delete(st->sb.time_lbl, anim_topclock_in);
    lv_anim_delete(st->sb.time_lbl, anim_topclock_out);
    if (!st->motion_ok) {
        lv_obj_set_style_text_opa(st->sb.time_lbl, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(st->sb.time_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_anim_t b;
        lv_anim_init(&b);
        lv_anim_set_var(&b, st->sb.time_lbl);
        lv_anim_set_time(&b, ENTRY_MS);
        lv_anim_set_path_cb(&b, apple_ease_in);
        lv_anim_set_values(&b, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_exec_cb(&b, anim_topclock_out);
        lv_anim_start(&b);
    }
}

static void push_hold_cb(lv_timer_t *t)
{
    clock_state_t *st = (clock_state_t *)lv_timer_get_user_data(t);
    if (st) clock_push_dismiss(st);
}

/* Fire (or coalesce into) a card. kind = PUSH_START / PUSH_END. */
static void clock_push_trigger(clock_state_t *st, int kind,
                               const char *agent_kind, const char *label)
{
    lv_label_set_text(st->push_head, kind == PUSH_END ? "运行结束" : "开始运行");
    char chip[64];
    const char *sk = short_kind_of(agent_kind);
    if (label && label[0]) snprintf(chip, sizeof chip, "%s  %s", sk, label);
    else                   snprintf(chip, sizeof chip, "%s", sk);
    lv_label_set_text(st->push_chip, chip);

    if (kind == PUSH_END) {
        lv_anim_delete(st->push_dot, anim_orbit);
        lv_obj_add_flag(st->push_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(st->push_check, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(st->push_check, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(st->push_dot, LV_OBJ_FLAG_HIDDEN);
    }
    st->push_kind = kind;

    if (st->push_active) {
        /* Coalesce: content already swapped above — just refresh the
         * running motion and restart the dwell countdown. */
        if (kind == PUSH_START) start_orbit(st);
        if (st->push_hold) lv_timer_reset(st->push_hold);
        return;
    }

    st->push_active = true;
    retreat_clock(st);
    enter_push(st);
    if (kind == PUSH_START) start_orbit(st);
    if (st->push_hold) { lv_timer_reset(st->push_hold); lv_timer_resume(st->push_hold); }
}

static void clock_tick(lv_timer_t *t)
{
    clock_state_t *st = (clock_state_t *)lv_timer_get_user_data(t);
    if (!st) return;

    char buf[16];
    int  event = 0;                    /* 0 none, PUSH_START+1, PUSH_END+1 */
    char ev_kind[AGENT_KIND_MAX] = "";
    char ev_label[PUSH_LABEL_MAX] = "";

    agent_state_lock();
    agent_state_t *s = agent_state_get();
    status_bar_update(&st->sb, s);
    status_bar_format_time(buf, sizeof(buf), s);

    /* Running-set edge detection (data-driven; only ever runs while the
     * clock is the visible scene, so a push means the event happened
     * WHILE the user was watching). The first tick after a show merely
     * seeds — agents already running when we arrive don't push. */
    if (!st->run_seeded) {
        snapshot_running(st, s);
        st->run_seeded = true;
    } else {
        /* END: a previously-running (kind,sid) that is no longer running,
         * UNLESS it went to WAITING — that's an awaiting hand-off, not a
         * finished task (scene_awaiting / the fleet row own that). */
        for (int i = 0; i < st->run_prev_n; ++i) {
            agent_slot_t *sl = agent_state_find_slot(st->run_prev[i].kind,
                                                     st->run_prev[i].sid);
            bool running = sl && sl->in_use && sl->status == AGENT_STATUS_RUNNING;
            bool waiting = sl && sl->in_use && sl->status == AGENT_STATUS_WAITING;
            if (!running && !waiting) {
                event = PUSH_END + 1;
                strncpy(ev_kind, st->run_prev[i].kind, sizeof ev_kind - 1);
                ev_kind[sizeof ev_kind - 1] = '\0';
                strncpy(ev_label, st->run_prev[i].label, sizeof ev_label - 1);
                ev_label[sizeof ev_label - 1] = '\0';
            }
        }
        /* START: a running (kind,sid) absent from the previous set.
         * Detected after END so a simultaneous start+end shows the more
         * salient "new activity". */
        for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
            agent_slot_t *sl = &s->slots[i];
            if (!sl->in_use || sl->status != AGENT_STATUS_RUNNING) continue;
            bool known = false;
            for (int j = 0; j < st->run_prev_n; ++j) {
                if (strcmp(st->run_prev[j].kind, sl->kind) == 0 &&
                    strcmp(st->run_prev[j].sid, sl->session_id) == 0) {
                    known = true; break;
                }
            }
            if (!known) {
                event = PUSH_START + 1;
                strncpy(ev_kind, sl->kind, sizeof ev_kind - 1);
                ev_kind[sizeof ev_kind - 1] = '\0';
                project_label(sl->cwd, ev_label, sizeof ev_label);
            }
        }
        snapshot_running(st, s);       /* run_prev := current running set */
    }
    agent_state_unlock();

    /* LVGL mutations happen OUTSIDE the agent_state lock. */
    if (event) clock_push_trigger(st, event - 1, ev_kind, ev_label);

    /* Rewrite hours/minutes only on minute change — every set_text
     * re-rasterises the big tiny_ttf glyphs. buf is always 5 chars
     * ("HH:MM" / "--:--"): [0..1]=hh, [2]=':', [3..4]=mm. */
    if (strcmp(buf, st->cached) != 0) {
        snprintf(st->cached, sizeof(st->cached), "%s", buf);
        char hh[3] = { buf[0], buf[1], '\0' };
        char mm[3] = { buf[3], buf[4], '\0' };
        lv_label_set_text(st->hh, hh);
        lv_label_set_text(st->mm, mm);
    }

    /* Colon blink. Leave the colon to the entrance fade until it
     * finishes; then toggle it 1 Hz. Hold it solid when motion is
     * reduced or before the host time syncs (buf[0]=='-'). Deduped so
     * we only invalidate the tiny colon region on an actual change. */
    /* 常驻天气行。缓存比较后再写：tiny_ttf 标签一次 set_text 就是一次
     * 失效 + 重新排版，而这行每分钟最多变一次。 */
    char wxline[96];
    scene_weather_mini_line(wxline, sizeof(wxline));
    if (strcmp(wxline, st->cached_wx) != 0) {
        snprintf(st->cached_wx, sizeof(st->cached_wx), "%s", wxline);
        lv_label_set_text(st->wx_lbl, wxline);
    }

    /* v6.3: 转场期间不闪。冒号是 135px 的 tiny_ttf 字形，一次 opa 翻转
     * 就是一次大字形重绘；而大钟这时正在做尺寸档位变形，每一分渲染预算
     * 都要留给它。转场结束后 colon_on=-1 会强制重新应用相位。 */
    uint32_t elapsed = lv_tick_get() - st->show_ms;
    if (!st->push_active && elapsed >= ENTRY_MS && !scene_trans_busy()) {
        bool blink = st->motion_ok && (buf[0] != '-');
        int want = (!blink || ((elapsed / BLINK_MS) % 2 == 0)) ? 1 : 0;
        if (want != st->colon_on) {
            st->colon_on = want;
            lv_obj_set_style_text_opa(st->colon,
                want ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
    }
}

/* ── v5.1 转场接入：时间真变形 + footer 演员 ────────────────────────
 * 时间是跨转场的固定锚点（用户契约），且必须是【同一实体】的真位移
 * + 真缩放——不允许小钟/大钟交叉淡化的障眼法。实现：face_grp 三标签
 * 全程唯一实体（status_bar 的 time_lbl 在本场景恒隐）；
 *   位移  face_grp 的 y 从共识槽位(-148，ink 中心=顶钟位 85)连续滑到
 *         中央，spring_disp 真弹簧（出场反向 apple_ease_in 加速）。
 *   缩放  字号走档位阶梯 48→66→84→102→120→135：transform_scale 是
 *         红线（plan A 实测 12-13 fps），而档位步进每档只做一次原生
 *         tiny_ttf 栅格化（12-glyph 子集，毫秒级），帧间保持快路径。
 *         三标签 CENTER 对齐，字号切换自动居中，只需按档位重算
 *         hh/mm 的 ±COLON_DX 让三个标签盒继续边贴边。
 * 共识端(48px@顶部中央)与其它场景的 time_lbl 逐像素等价（同字体、
 * tabular 数字、同拼装公式），瞬切帧时间纹丝不动。
 * footer 两列是演员：从底部屏幕外弹入/沉出。 */

static const int16_t FACE_RUNGS[] = { 48, 66, 84, 102, 120, 135 };
#define FACE_RUNG_N     6
#define FACE_PX_MIN     48
#define CONSENSUS_GRP_Y ENTRY_Y     /* -148: face ink 中心落在顶钟槽位 */

/* 应用一个字号档位：换字体 + 重算三标签盒的边贴边偏移。 */
static void face_apply_rung(clock_state_t *st, int px)
{
    const lv_font_t *f = clock_font(px);
    if (!f) return;
    int dx = ((1320 + 286) / 2 * px) / 1000;
    lv_obj_set_style_text_font(st->hh,    f, 0);
    lv_obj_set_style_text_font(st->colon, f, 0);
    lv_obj_set_style_text_font(st->mm,    f, 0);
    lv_obj_align(st->hh,    LV_ALIGN_CENTER, -dx, 0);
    lv_obj_align(st->colon, LV_ALIGN_CENTER,   0, 0);
    lv_obj_align(st->mm,    LV_ALIGN_CENTER,  dx, 0);
}

/* 变形驱动：v = 0(共识小钟) .. 1000(中央大钟)。y 连续；字号就近档位，
 * 档位变化才触发重栅格化。spring 过冲(v≈1055)被"就近档位"天然钳制在
 * 135，y 的过冲则表现为轻微下坠回弹——期望的弹簧手感。 */
static void anim_face_morph(void *var, int32_t v)
{
    clock_state_t *st = (clock_state_t *)lv_obj_get_user_data((lv_obj_t *)var);
    if (!st) return;
    st->morph_v = v;
    /* v=0 共识槽位, v=1000 静止姿态。静止不再是 0——大钟整体上移了
     * FACE_RISE，插值终点必须跟着走，否则变形会把它拉回旧位置。 */
    lv_obj_set_y(st->face_grp, CONSENSUS_GRP_Y
                 + (int32_t)(FACE_REST_Y - CONSENSUS_GRP_Y) * v / 1000);
    int px = FACE_PX_MIN + (CLOCK_PX - FACE_PX_MIN) * v / 1000;
    int best = FACE_RUNGS[0];
    for (int i = 1; i < FACE_RUNG_N; ++i)
        if (LV_ABS(FACE_RUNGS[i] - px) < LV_ABS(best - px))
            best = FACE_RUNGS[i];
    if (best != st->face_px) {
        st->face_px = best;
        face_apply_rung(st, best);
    }
}

static void face_kill_morph(clock_state_t *st)
{
    lv_anim_delete(st->face_grp, anim_face_morph);
    lv_anim_delete(st->face_grp, anim_grp_y);
    lv_anim_delete(st->face_grp, anim_grp_opa);
    lv_anim_delete(st->sb.time_lbl, anim_topclock_in);
    lv_anim_delete(st->sb.time_lbl, anim_topclock_out);
}

static void face_start_morph(clock_state_t *st, int32_t to, uint32_t ms,
                             lv_anim_path_cb_t path)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, st->face_grp);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, path);
    lv_anim_set_values(&a, st->morph_v, to);
    lv_anim_set_exec_cb(&a, anim_face_morph);
    lv_anim_start(&a);
}

static void clock_trans_to_consensus(scene_t *s, uint32_t ms)
{
    clock_state_t *st = (clock_state_t *)s->user_data;
    if (!st) return;
    face_kill_morph(st);
    /* 本场景时间实体=face；小钟标签恒隐（共识形态由 face@48 呈现）。 */
    lv_obj_add_flag(st->sb.time_lbl, LV_OBJ_FLAG_HIDDEN);
    set_group_text_opa(st->face_grp, LV_OPA_COVER);
    st->colon_on = -1;                     /* blink 熄相不带进变形 */

    /* push 卡把 face 退去顶部+透明的罕见边界：直接落共识端态。 */
    if (st->push_active || ms == 0) {
        anim_face_morph(st->face_grp, 0);
        return;
    }
    face_start_morph(st, 0, ms, apple_ease_in);
}

static void clock_trans_from_consensus(scene_t *s, uint32_t ms)
{
    clock_state_t *st = (clock_state_t *)s->user_data;
    if (!st) return;
    face_kill_morph(st);
    lv_obj_add_flag(st->sb.time_lbl, LV_OBJ_FLAG_HIDDEN);
    set_group_text_opa(st->face_grp, LV_OPA_COVER);

    st->show_ms = lv_tick_get();     /* 冒号闪从变形结束后接管 */
    st->colon_on = -1;

    if (ms == 0) {
        anim_face_morph(st->face_grp, 1000);
        return;
    }
    /* 瞬切帧：face@48 恰在共识槽位 = 上一场景小钟的位置。 */
    anim_face_morph(st->face_grp, 0);
    face_start_morph(st, 1000, ms, spring_disp);
}

#define CLOCK_A_WX  STATUS_BAR_TRANS_ACTORS
#define CLOCK_ACTOR_N (STATUS_BAR_TRANS_ACTORS + 1)
static trans_actor_t s_clock_actors[CLOCK_ACTOR_N];
static trans_profile_t s_clock_profile = {
    .actors               = s_clock_actors,
    .actor_n              = CLOCK_ACTOR_N,
    .clock_to_consensus   = clock_trans_to_consensus,
    .clock_from_consensus = clock_trans_from_consensus,
};

static void clock_init(scene_t *s, lv_obj_t *parent)
{
    clock_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;
    st->colon_on = -1;

    status_bar_create(parent, &st->sb);
    /* The big face replaces the 48pt top clock. status_bar_update keeps
     * set_text-ing the hidden label; harmless. */
    lv_obj_add_flag(st->sb.time_lbl, LV_OBJ_FLAG_HIDDEN);

    /* Container the entrance animates as one unit. user_data carries st
     * for the morph exec (anim_face_morph). */
    st->face_grp = lv_obj_create(parent);
    lv_obj_remove_style_all(st->face_grp);
    lv_obj_set_size(st->face_grp, SCREEN_W, FACE_GRP_H);
    lv_obj_align(st->face_grp, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(st->face_grp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(st->face_grp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(st->face_grp, st);
    st->face_px = CLOCK_PX;
    st->morph_v = 1000;

    const lv_font_t *bf = clock_font(CLOCK_PX);
    if (!bf) bf = &lv_font_montserrat_48;

    st->hh = lv_label_create(st->face_grp);
    lv_obj_set_style_text_font(st->hh, bf, 0);
    lv_obj_set_style_text_color(st->hh, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(st->hh, "--");
    lv_obj_align(st->hh, LV_ALIGN_CENTER, -COLON_DX, 0);

    st->colon = lv_label_create(st->face_grp);
    lv_obj_set_style_text_font(st->colon, bf, 0);
    lv_obj_set_style_text_color(st->colon, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(st->colon, ":");
    lv_obj_align(st->colon, LV_ALIGN_CENTER, 0, 0);

    st->mm = lv_label_create(st->face_grp);
    lv_obj_set_style_text_font(st->mm, bf, 0);
    lv_obj_set_style_text_color(st->mm, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(st->mm, "--");
    lv_obj_align(st->mm, LV_ALIGN_CENTER, COLON_DX, 0);

    /* v6.5 常驻天气行。以前这行只在 scene_weather 的 MODE_CLOCK 姿态里
     * 出现（刻钟膨胀那 30 秒），所以"时钟下面有没有天气"取决于你当时在
     * 哪个场景——两块屏幕长得都像大钟，行为却不同，而且大钟的实现被写
     * 了两份。现在它常驻在这里，MODE_CLOCK 整套已从 scene_weather 删除。
     * 内容由 scene_weather_mini_line() 组装：同一个事实只有一份翻译表。 */
    st->wx_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(st->wx_lbl, ui_type(UI_T_LABEL), 0);
    lv_obj_set_style_text_color(st->wx_lbl, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(st->wx_lbl, "");
    lv_obj_align(st->wx_lbl, LV_ALIGN_TOP_MID, 0, WX_LINE_Y);

    /* ── push card (agent start/end), on top of the face, hidden until
     * clock_push_trigger fires. Sized to its exact content height so the
     * stack sits centred; user_data carries st for the opa exec_cbs. */
    int glyph_zone = PUSH_GLYPH;
    int head_line  = ui_type_line(UI_T_TITLE);
    int chip_line  = ui_type_line(UI_T_LABEL);
    int card_h = glyph_zone + UI_GAP_MD + head_line + UI_GAP_SM + chip_line;

    st->push_grp = lv_obj_create(parent);
    lv_obj_remove_style_all(st->push_grp);
    lv_obj_set_size(st->push_grp, SCREEN_W, card_h);
    lv_obj_align(st->push_grp, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(st->push_grp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(st->push_grp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(st->push_grp, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(st->push_grp, st);

    st->push_glyph = lv_obj_create(st->push_grp);
    lv_obj_remove_style_all(st->push_glyph);
    lv_obj_set_size(st->push_glyph, PUSH_GLYPH, PUSH_GLYPH);
    lv_obj_align(st->push_glyph, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(st->push_glyph, LV_OBJ_FLAG_SCROLLABLE);

    st->push_ring = lv_obj_create(st->push_glyph);
    lv_obj_remove_style_all(st->push_ring);
    lv_obj_set_size(st->push_ring, PUSH_RING_SZ, PUSH_RING_SZ);
    lv_obj_align(st->push_ring, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(st->push_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(st->push_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(st->push_ring, lv_color_hex(COL_TRACK), 0);
    lv_obj_set_style_border_width(st->push_ring, 3, 0);
    lv_obj_clear_flag(st->push_ring, LV_OBJ_FLAG_SCROLLABLE);

    st->push_dot = lv_obj_create(st->push_glyph);
    lv_obj_remove_style_all(st->push_dot);
    lv_obj_set_size(st->push_dot, PUSH_DOT_SZ, PUSH_DOT_SZ);
    lv_obj_set_style_radius(st->push_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(st->push_dot, lv_color_hex(COL_TEAL), 0);
    lv_obj_set_style_bg_opa(st->push_dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(st->push_dot, LV_OBJ_FLAG_SCROLLABLE);
    anim_orbit(st->push_dot, 0);            /* park at 12 o'clock */

    st->push_check = lv_label_create(st->push_glyph);
    lv_label_set_text(st->push_check, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(st->push_check, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(st->push_check, lv_color_hex(COL_TEAL), 0);
    lv_obj_align(st->push_check, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(st->push_check, LV_OBJ_FLAG_HIDDEN);

    st->push_head = lv_label_create(st->push_grp);
    lv_obj_set_style_text_font(st->push_head, ui_type_bold(UI_T_TITLE), 0);
    lv_obj_set_style_text_color(st->push_head, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(st->push_head, "开始运行");
    lv_obj_align(st->push_head, LV_ALIGN_TOP_MID, 0, glyph_zone + UI_GAP_MD);

    st->push_chip = lv_label_create(st->push_grp);
    lv_obj_set_style_text_font(st->push_chip, ui_type(UI_T_LABEL), 0);
    lv_obj_set_style_text_color(st->push_chip, lv_color_hex(COL_TEAL), 0);
    lv_label_set_text(st->push_chip, "");
    lv_obj_align(st->push_chip, LV_ALIGN_TOP_MID, 0,
                 glyph_zone + UI_GAP_MD + head_line + UI_GAP_SM);

    /* One-shot dwell timer: paused between pushes, reset+resumed on
     * trigger, paused again by push_hold_cb after it fires once. */
    st->push_hold = lv_timer_create(push_hold_cb, PUSH_HOLD_MS, st);
    lv_timer_pause(st->push_hold);

    /* Tick drives the minute dirty-check + conn pill; the colon blink
     * phase is time-derived (BLINK_MS), independent of this rate. */
    st->timer = lv_timer_create(clock_tick, CLOCK_TICK_MS, st);
    lv_timer_pause(st->timer);
    clock_tick(st->timer);

    /* v5.0 转场演员：footer 两列（定义由 status_bar 统一提供，v6.2 起
     * 带共享 key —— 与 dashboard 之间 footer 原地不动）。大钟面不进
     * 演员表——它是时间锚点，由 to/from_consensus 变形。 */
    status_bar_trans_actors(&st->sb, s_clock_actors);
    /* 天气行随 footer 一起从底部进出；它没有共享 key，因为只有
     * 这个场景有它。 */
    s_clock_actors[CLOCK_A_WX] = (trans_actor_t){
        .obj = st->wx_lbl, .dir = TRANS_FROM_BOTTOM, .ch = TROPA_TEXT,
        .base_opa = 255, .out_dist = 180, .delay_ms = 40 };
    scene_trans_bind("clock", &s_clock_profile);
}

static void clock_on_show(scene_t *s)
{
    clock_state_t *st = (clock_state_t *)s->user_data;
    if (!st) return;

    bool motion_reduced;
    agent_state_lock();
    motion_reduced = agent_state_get()->motion_reduced;
    agent_state_unlock();
    st->motion_ok = !motion_reduced;
    st->show_ms = lv_tick_get();
    st->colon_on = -1;   /* re-apply on the first post-entrance tick */

    /* No stale card from a prior show; re-seed the running set so agents
     * already running when we arrive don't fire a spurious push. */
    st->push_active = false;
    st->run_seeded = false;
    if (st->push_hold) lv_timer_pause(st->push_hold);
    lv_anim_delete(st->push_dot, anim_orbit);
    lv_obj_add_flag(st->push_grp, LV_OBJ_FLAG_HIDDEN);
    push_set_opa(st, LV_OPA_TRANSP);
    lv_obj_add_flag(st->sb.time_lbl, LV_OBJ_FLAG_HIDDEN);  /* face owns the time */

    /* v5.0：入场变形归 scene_trans（from_consensus 回调）。on_show 只
     * 摆静置姿态（morph 端态 1000 = 中央 135px，含字号档位复位）——
     * 非转场路径也保证画面完整；转场路径会紧接着重摆再播弹簧。 */
    anim_face_morph(st->face_grp, 1000);
    set_group_text_opa(st->face_grp, LV_OPA_COVER);

    if (st->timer) {
        lv_timer_resume(st->timer);
        clock_tick(st->timer);
    }
}

static void clock_on_hide(scene_t *s)
{
    clock_state_t *st = (clock_state_t *)s->user_data;
    if (!st) return;
    /* Tear down any in-flight push first: stop the dwell timer + orbit,
     * kill the card/top-clock anims, hide the card, and put the top
     * clock back to its hidden resting state. */
    st->push_active = false;
    if (st->push_hold) lv_timer_pause(st->push_hold);
    lv_anim_delete(st->push_dot, anim_orbit);
    lv_anim_delete(st->push_grp, anim_push_y);
    lv_anim_delete(st->push_grp, anim_push_opa);
    lv_anim_delete(st->push_grp, anim_push_opa_out);
    push_set_opa(st, LV_OPA_TRANSP);
    lv_obj_add_flag(st->push_grp, LV_OBJ_FLAG_HIDDEN);
    lv_anim_delete(st->sb.time_lbl, anim_topclock_in);
    lv_anim_delete(st->sb.time_lbl, anim_topclock_out);
    lv_obj_set_style_text_opa(st->sb.time_lbl, LV_OPA_COVER, 0);
    lv_obj_add_flag(st->sb.time_lbl, LV_OBJ_FLAG_HIDDEN);

    /* Kill an in-flight entrance/morph and park at the resting pose so
     * the next show never inherits a mid-flight face. */
    face_kill_morph(st);
    anim_face_morph(st->face_grp, 1000);
    set_group_text_opa(st->face_grp, LV_OPA_COVER);
    st->colon_on = -1;
    if (st->timer) lv_timer_pause(st->timer);
}

scene_t scene_clock = {
    .id           = "clock",
    .display_name = "Clock",
    .accent       = LV_COLOR_MAKE(0xF3, 0xEE, 0xE2),
    .description  = "StandBy-style big centered clock with the shared "
                    "active/tokens footer.",
    .tags         = "clock,standby,time",
    .init         = clock_init,
    .on_show      = clock_on_show,
    .on_hide      = clock_on_hide,
};
