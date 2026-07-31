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
 * v6.0: the AWAITING takeover scene is retired — the dashboard IS the
 * "your turn" view at every agent count. When a turn comes back,
 * scene_auto_switch_cb pulls the display here (rising edge, one-shot)
 * instead of switching to a near-identical dedicated page.
 *
 * All four rows are pre-created in init() and shown/hidden per tick; row
 * geometry is recomputed only when the visible count changes.
 */

#include "scenes.h"
#include "agent_state.h"
#include "theme.h"
#include "status_bar.h"
#include "cjk_font.h"   /* cjk_utf8_lcpy */
#include "ui_type.h"
#include "scene_trans.h"
#include "anim/apple_ease.h"

#include <stdio.h>
#include <string.h>

#include "ui_screen.h"
#include "lvgl.h"

/* 屏宽 = 坐标空间。v7.4 之前这里硬写 466，而 466 从来不是面板宽度
 * （见 CLAUDE.md 的 Panel geometry）——凡是靠它【算】出来的居中都会
 * 左偏 7px。用 LV_ALIGN_*_MID 对齐的元素不受影响，那是相对父容器的。 */
#define SCREEN_W     UI_LV_W
#define COL_BG         0x0B0A09
#define COL_SURFACE    0x1C1814
#define COL_SURFACE_HI 0x26201A   /* waiting-row surface, one step lighter */
#define COL_TEXT       0xF3EEE2
#define COL_TEXT_DIM   0x8A807A
#define COL_MUTE       0x5A514A
#define COL_TEAL       0x2BB3B1
#define COL_GOLD       0xB89020
#define COL_GOLD_HI    0xE0B43C   /* urgent (approve/clarify) accent */

/* Fleet rows live in the shared safe band between the 48-px clock and
 * the footer numbers (ui_type.h anchors). */
#define ROW_W        UI_CONTENT_W
#define ROW_X        ((SCREEN_W - ROW_W) / 2)
#define ROW_AREA_TOP UI_BAND_TOP
#define ROW_AREA_BOT UI_BAND_BOT
#define ROW_GAP       12
#define ROW_H_MAX    126
/* Rows tall enough get a second line (activity); short rows show only
 * name + meta so the name stays at full BODY size instead of shrinking. */
#define ROW_TWOLINE_MIN_H 100

/* Ambient cluster anchor. v6.0: the awaiting takeover scene is retired
 * (user call: two near-identical gold pages a key press flipped
 * between). The dashboard gold pose IS the "your turn" view now: ring +
 * TITLE greeting word + project chip. TITLE (52) for ALL three states —
 * the word never changes size again; "loud" is carried by colour alone
 * (gold = your move, the device-wide contract).
 *
 * Two poses, both ink-centred in the chrome band (clock ink ends 112,
 * footer chrome starts at UI_CHROME_BOT 382 → centre 247):
 *   ring+word       ink 12..164, centre  88 → y 160 (idle / thinking)
 *   ring+word+chip  ink 12..215, centre 114 → y 133 (gold, chip shown)
 * ambient_slide_to() glides between them (the old avoidance-slide
 * machinery, revived). */
#define AMBIENT_Y_CENTERED 160
#define AMBIENT_Y_CHIP     133
#define AMBIENT_SLIDE_MS   450
/* ring ink ends ~84; +28 gap below it. Chip sits under the word's
 * line box (112+63) + 14. */
#define AMBIENT_WORD_Y  112
#define AMBIENT_CHIP_Y  189

/* ── 转场演员布局 (v6.2) ──────────────────────────────────────────
 * footer 四件套（共享 key，与 clock 之间原地不动）+ ambient 簇整组 +
 * 每张 fleet 卡片。ambient 与 rows 互斥（HIDDEN），框架自动跳过隐藏的
 * 那一半，所以同一张演员表覆盖两种密度。 */
#define DASH_A_FOOTER0  0
#define DASH_A_AMBIENT  (DASH_A_FOOTER0 + STATUS_BAR_TRANS_ACTORS)
#define DASH_A_ROW0     (DASH_A_AMBIENT + 1)
#define DASH_ACTOR_N    (DASH_A_ROW0 + AGENT_SLOT_MAX)
/* ambient 簇 224 高、静止 y 最高 133 → 下沉 360 才让环的墨完全出屏。 */
#define DASH_AMBIENT_OUT  360
#define DASH_ROW_OUT      500

typedef struct {
    lv_obj_t *card;
    lv_obj_t *dot;
    lv_obj_t *name_lbl;
    lv_obj_t *meta_lbl;
    lv_obj_t *act_lbl;
    int16_t   rest_y;    /* layout_rows 排出的静止 y（转场 rest 姿态） */
} fleet_row_t;

typedef struct {
    status_bar_t sb;             /* shared top time + bottom active/tokens */
    /* ambient (single-agent) group */
    lv_obj_t *ambient_grp;
    /* v5.3: the pet mascot is retired (user: the panel's ONE job is
     * "agent finished a turn, your move" — the creature was charm, not
     * signal). The 96px slot now holds the same breathing pulse ring
     * the awaiting takeover uses: ONE visual language device-wide.
     * Colour carries the state: teal=running, gold=your turn, dim=idle. */
    lv_obj_t *pulse_ring;        /* 72px outline ring */
    lv_obj_t *pulse_dot;         /* breathing inner dot */
    uint32_t  pulse_color;       /* cached — restyle only on change */
    lv_obj_t *ambient_lbl;       /* status word */
    /* (v5.8: ambient_proj / ambient_act retired — grey metadata lines
     * under the word carried no glanceable signal.) */
    /* v6.0: project chip ("cc esp32-agent-dashboard"), gold-state only —
     * inherited from the retired awaiting takeover: it answers "WHO is
     * waiting on me". Cached to avoid re-invalidating tiny_ttf labels
     * every 500ms tick. */
    lv_obj_t *ambient_chip;
    char      cached_word[32];
    char      cached_chip[64];
    int       ambient_target_y;  /* last slide target; 0 = not yet placed */
    /* fleet (multi-agent) rows */
    fleet_row_t rows[AGENT_SLOT_MAX];
    int       last_layout_n;     /* row count last laid out; -1 forces layout */
    bool      rows_twoline;      /* current layout mode (set by layout_rows) */
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

/* Chinese primary words (v4.4): 3 hanzi at HERO/TITLE tier subtend 22'
 * at 1 m — readable across the desk, where the 9-char English originals
 * at any feasible size were not. All chars verified in the GB2312 font
 * subset. */
static const char *awaiting_headline(awaiting_kind_t k)
{
    switch (k) {
        case AWAITING_CONTINUE: return "该你了";
        case AWAITING_APPROVE:  return "请批准";
        case AWAITING_PICK:     return "请选择";
        case AWAITING_TYPE:     return "请输入";
        case AWAITING_CLARIFY:  return "需澄清";
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
        /* CONTINUE shows the rotating greeting (the same index the takeover
         * uses, so device + detail agree); other kinds keep the fixed
         * instructional headline. */
        const char *head = (s->awaiting_kind == AWAITING_CONTINUE)
            ? agent_awaiting_greeting(s->awaiting_greeting_idx)
            : awaiting_headline(s->awaiting_kind);
        if (s->awaiting_summary[0]) {
            /* "·" (U+00B7) — proven renderable in the device font subset
             * (status_bar uses it); em dash is NOT in the subset. */
            snprintf(buf, cap, "%s \xC2\xB7 ", head);
            size_t used = strlen(buf);
            if (used < cap) {
                cjk_utf8_lcpy(buf + used, s->awaiting_summary, (unsigned)(cap - used));
            }
        } else {
            snprintf(buf, cap, "%s", head);
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
    snprintf(buf, cap, "%s", s->status == AGENT_STATUS_RUNNING ? "运行中" : "-");
}

/* ── ambient (single-agent) mode ─────────────────────────────────── */

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
    /* 转场进行中：簇正在场外飞，别在这条 y 上开第二条动画抢方向盘。
     * 新的 target 已记下，dash_sync_rest 会把它交给入场的落点。 */
    if (scene_trans_busy()) return;
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

/* 呼吸点 exec（同旧 awaiting glyph 的呼吸）：CENTER 对齐是持久属性，
 * 尺寸变化后自动保持居中。 */
static void dash_pulse_breath(void *obj, int32_t v)
{
    lv_obj_set_size((lv_obj_t *)obj, v, v);
}

/* 呼吸：14↔28 px、2 s、无限往复。抽成函数是因为探针要能停/起它
 * （lv_anim_delete 停掉之后只能重建）。 */
static void pulse_breath_start(dash_t *d)
{
    lv_anim_t pa;
    lv_anim_init(&pa);
    lv_anim_set_var(&pa, d->pulse_dot);
    lv_anim_set_values(&pa, 14, 28);
    lv_anim_set_time(&pa, 2000);
    lv_anim_set_playback_time(&pa, 2000);
    lv_anim_set_repeat_count(&pa, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&pa, apple_ease_out);
    lv_anim_set_exec_cb(&pa, dash_pulse_breath);
    lv_anim_start(&pa);
}

/* ── ?dashprobe：ambient 簇的成本分解探针 ─────────────────────────
 * 纯测量仪器，不是特性开关：逐位摘掉簇里的一个元素，用 trans_bench 的
 * render_avg 差值给它标价，据此决定优化打哪儿。
 *
 * 关键设计：0x2（藏点）与 0x10（冻结呼吸）要分开——藏点同时去掉了
 * "画"和"每帧改半径"，冻结只去掉后者。若两者差值接近，成本就在
 * 半径churn（LVGL 的圆形 mask 缓存只有 4 项，呼吸点每帧换一个半径，
 * 有理由怀疑它把环的 mask 也一起挤出去了）；若藏点远大于冻结，
 * 成本就在画本身。一次刷机答完两个问题。 */
static uint32_t s_probe = 0;
void scene_dashboard_set_probe(uint32_t mask) { s_probe = mask; }
uint32_t scene_dashboard_get_probe(void)      { return s_probe; }

#define PROBE_NO_RING   0x1u
#define PROBE_NO_DOT    0x2u
#define PROBE_NO_WORD   0x4u
#define PROBE_NO_CHIP   0x8u
#define PROBE_NO_BREATH 0x10u

static void probe_show(lv_obj_t *o, bool show)
{
    if (show) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* console task 只翻 flag；DOM 在 tick（LVGL task）上应用。 */
static void probe_apply(dash_t *d)
{
    static uint32_t applied = 0;
    if (applied == s_probe) return;
    uint32_t was = applied;
    applied = s_probe;

    probe_show(d->pulse_ring, !(s_probe & PROBE_NO_RING));
    probe_show(d->pulse_dot,  !(s_probe & PROBE_NO_DOT));
    probe_show(d->ambient_lbl, !(s_probe & PROBE_NO_WORD));
    /* chip 的显隐由 render_single 按状态管；探针只做单向压制。 */
    if (s_probe & PROBE_NO_CHIP) lv_obj_add_flag(d->ambient_chip, LV_OBJ_FLAG_HIDDEN);

    if ((s_probe ^ was) & PROBE_NO_BREATH) {
        if (s_probe & PROBE_NO_BREATH) lv_anim_delete(d->pulse_dot, dash_pulse_breath);
        else                           pulse_breath_start(d);
    }
}

/* The single-agent pose (v6.0). Word + colour + chip all derive from
 * the slot: gold = your move (rotating greeting for CONTINUE, fixed
 * instructional word for approve/pick/type/clarify) + project chip;
 * teal = thinking; dim = idle. This absorbs the retired awaiting
 * takeover scene — same information, ONE page, word always TITLE. */
static void render_single(dash_t *d, const agent_state_t *st,
                          const agent_slot_t *one)
{
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        lv_obj_add_flag(d->rows[i].card, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(d->ambient_grp, LV_OBJ_FLAG_HIDDEN);
    d->last_layout_n = -1;   /* force row re-layout when fleet returns */

    const char *word = "空闲";
    uint32_t pc = 0x5A514A;                       /* idle dim */
    bool your_move = false;
    if (one) {
        if (one->awaiting_kind != AWAITING_NONE) {
            word = (one->awaiting_kind == AWAITING_CONTINUE)
                 ? agent_awaiting_greeting(one->awaiting_greeting_idx)
                 : awaiting_headline(one->awaiting_kind);
            your_move = true;
        } else if (one->status == AGENT_STATUS_WAITING) {
            word = "该你了";
            your_move = true;
        } else if (one->status == AGENT_STATUS_RUNNING) {
            word = "思考中";
            pc = 0x2BB3B1;                        /* teal — thinking */
        }
    } else if (st->running > 0) {                 /* totals without slots */
        word = "思考中";
        pc = 0x2BB3B1;
    } else if (st->waiting > 0) {
        word = "该你了";
        your_move = true;
    }
    if (your_move) pc = 0xB89020;                 /* gold — your move */

    if (strncmp(word, d->cached_word, sizeof(d->cached_word)) != 0) {
        snprintf(d->cached_word, sizeof(d->cached_word), "%s", word);
        lv_label_set_text(d->ambient_lbl, word);
    }

    if (pc != d->pulse_color) {
        d->pulse_color = pc;
        lv_obj_set_style_border_color(d->pulse_ring, lv_color_hex(pc), 0);
        lv_obj_set_style_bg_color(d->pulse_dot, lv_color_hex(pc), 0);
        lv_obj_set_style_text_color(d->ambient_chip, lv_color_hex(pc), 0);
    }

    /* Project chip — gold pose only: WHO is waiting on me. Project name
     * from cwd (human-readable); session id "abcd:9f" (4 head + 2 tail,
     * the v2.7.0 uniqueness format) only as fallback. */
    if (your_move && one) {
        char chip[64];
        const char *base = cwd_basename(one->cwd);
        if (base) {
            char basetrunc[27];   /* UTF-8-safe: never split a CJK folder name */
            cjk_utf8_lcpy(basetrunc, base, sizeof(basetrunc));
            snprintf(chip, sizeof(chip), "%s  %s", short_kind(one->kind), basetrunc);
        } else {
            const char *sid = one->session_id;
            size_t sid_len = strlen(sid);
            if (sid_len <= 6) {
                snprintf(chip, sizeof(chip), "%s  %s", short_kind(one->kind),
                         sid[0] ? sid : "agent");
            } else {
                snprintf(chip, sizeof(chip), "%s  %.4s:%s", short_kind(one->kind),
                         sid, sid + sid_len - 2);
            }
        }
        if (strncmp(chip, d->cached_chip, sizeof(d->cached_chip)) != 0) {
            snprintf(d->cached_chip, sizeof(d->cached_chip), "%s", chip);
            lv_label_set_text(d->ambient_chip, chip);
        }
        lv_obj_clear_flag(d->ambient_chip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(d->ambient_chip, LV_OBJ_FLAG_HIDDEN);
    }

    ambient_slide_to(d, (your_move && one) ? AMBIENT_Y_CHIP : AMBIENT_Y_CENTERED,
                     !st->motion_reduced);
}

static void render_ambient(dash_t *d, const agent_state_t *st)
{
    /* Pick the slot the pose should speak for, by NEED — not by slot
     * order. v7.3: with background conversations the slot list is mostly
     * finished turns, and taking the first in_use slot made the panel
     * announce "空闲" while another agent was still thinking. Awaiting
     * outranks running outranks anything else. */
    const agent_slot_t *one = NULL;
    int best = -1;
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        const agent_slot_t *s = &st->slots[i];
        if (!s->in_use) continue;
        int rank = (s->awaiting_kind != AWAITING_NONE) ? 2
                 : (s->status == AGENT_STATUS_RUNNING) ? 1 : 0;
        if (rank > best) { best = rank; one = s; }
    }
    render_single(d, st, one);
}

/* ── fleet (multi-agent) mode ────────────────────────────────────── */

static void layout_rows(dash_t *d, int n)
{
    int h = (ROW_AREA_BOT - ROW_AREA_TOP - (n - 1) * ROW_GAP) / n;
    if (h > ROW_H_MAX) h = ROW_H_MAX;
    int total = n * h + (n - 1) * ROW_GAP;
    int y = ROW_AREA_TOP + (ROW_AREA_BOT - ROW_AREA_TOP - total) / 2;

    /* Two-line rows (name + activity) only when the row is tall enough
     * to keep both at their full tier size; otherwise a single BODY
     * name line vertically centered. Never shrink the font to fit. */
    d->rows_twoline = (h >= ROW_TWOLINE_MIN_H);
    int name_h = ui_type_line(UI_T_BODY);    /* 44 */
    int act_h  = ui_type_line(UI_T_LABEL);   /* 32 */
    int meta_h = ui_type_line(UI_T_LABEL);
    int name_y = d->rows_twoline
               ? (h - name_h - UI_GAP_SM - act_h) / 2
               : (h - name_h) / 2;
    int act_y  = name_y + name_h + UI_GAP_SM;
    int meta_y = name_y + (name_h - meta_h) / 2 + 2;  /* optically on the name line */

    for (int i = 0; i < n; ++i) {
        fleet_row_t *r = &d->rows[i];
        r->rest_y = (int16_t)y;
        lv_obj_set_pos(r->card, ROW_X, y);
        lv_obj_set_size(r->card, ROW_W, h);
        lv_obj_set_pos(r->dot, 20, name_y + (name_h - 16) / 2);
        lv_obj_set_pos(r->name_lbl, 50, name_y);
        /* Fixed height = one line: LONG_DOT only ellipsizes when the label
         * can't grow — width alone lets long names wrap onto the activity
         * line (seen on-device with "esp32-agent-dashboard"). */
        lv_obj_set_size(r->name_lbl, ROW_W - 50 - 16 - 96, name_h);
        lv_obj_align(r->meta_lbl, LV_ALIGN_TOP_RIGHT, -18, meta_y);
        lv_obj_set_pos(r->act_lbl, 50, act_y);
        lv_obj_set_size(r->act_lbl, ROW_W - 50 - 20, act_h);
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
        /* v7.3: teal means THINKING. A finished (done) or untouched (idle)
         * agent gets the muted ink instead — the row stays informative
         * without claiming the agent is busy or wants you. */
        uint32_t accent = waiting ? (urgent ? COL_GOLD_HI : COL_GOLD)
                        : (s->status == AGENT_STATUS_RUNNING) ? COL_TEAL
                        : COL_MUTE;

        lv_obj_clear_flag(r->card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(r->card,
            lv_color_hex(waiting ? COL_SURFACE_HI : COL_SURFACE), 0);
        lv_obj_set_style_border_width(r->card, waiting ? 1 : 0, 0);
        lv_obj_set_style_border_color(r->card, lv_color_hex(accent), 0);

        lv_obj_set_style_bg_color(r->dot, lv_color_hex(accent), 0);

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

        /* Activity line only exists in two-line layouts; hidden rows
         * would otherwise paint over the next card. */
        if (d->rows_twoline) {
            char act[112];
            compose_activity(s, act, sizeof(act));
            lv_label_set_text(r->act_lbl, act);
            lv_obj_set_style_text_color(r->act_lbl,
                lv_color_hex(waiting ? accent : COL_TEXT_DIM), 0);
            lv_obj_clear_flag(r->act_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(r->act_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ── tick ────────────────────────────────────────────────────────── */

static void tick(lv_timer_t *t)
{
    dash_t *d = (dash_t *)lv_timer_get_user_data(t);
    if (!d) return;

    probe_apply(d);

    agent_state_lock();
    agent_state_t *st = agent_state_get();
    status_bar_update(&d->sb, st);
    const agent_slot_t *focus = NULL;
    if (st->focused_slot >= 0 && st->focused_slot < AGENT_SLOT_MAX
        && st->slots[st->focused_slot].in_use) {
        focus = &st->slots[st->focused_slot];
    }
    /* Key3 focus pins the single-agent pose to one slot of a 2+ fleet;
     * the word/chip derive from the pinned slot, not aggregate totals.
     * v7.3: the split is decided by how many agents actually WANT
     * something (agent_state_active_count), not by how many sessions
     * exist. Background conversations that finished their turn are
     * `done` — listed, but they no longer drag the display into the
     * multi-row "everyone needs you" view. */
    int active = agent_state_active_count();
    if (active >= 2 && focus) render_single(d, st, focus);
    else if (active >= 2)     render_fleet(d, st);
    else                      render_ambient(d, st);
    agent_state_unlock();
}

/* ── 转场 profile (v6.2) ─────────────────────────────────────────── */

/* 两处静止姿态是"活"的：ambient 簇在 chip/无 chip 两个 pose 间滑动，
 * fleet 卡片的 y 随行数重排。转场前把当前值交给框架，否则入场会把元素
 * 弹回 bind 那一刻的旧位置。 */
static void dash_sync_rest(scene_t *s);

static trans_actor_t s_dash_actors[DASH_ACTOR_N];
static trans_profile_t s_dash_profile = {
    .actors    = s_dash_actors,
    .actor_n   = DASH_ACTOR_N,
    /* 时间已在共识姿态（顶部中央 48px 小钟）→ 无需变形回调。 */
    .sync_rest = dash_sync_rest,
};

static void dash_sync_rest(scene_t *s)
{
    dash_t *d = (dash_t *)s->user_data;
    if (!d) return;
    s_dash_actors[DASH_A_AMBIENT].rest_pos = (int16_t)
        (d->ambient_target_y ? d->ambient_target_y : AMBIENT_Y_CENTERED);
    for (int i = 0; i < AGENT_SLOT_MAX; ++i)
        s_dash_actors[DASH_A_ROW0 + i].rest_pos = d->rows[i].rest_y;
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

    /* ambient group (single-agent mode). 224 tall = ring box (96) + 28
     * gap + word line (63) + 14 gap + chip line (32); even at the chip
     * pose's y 133 the transparent container ends at 357, clear of the
     * footer numbers (392). */
    d->ambient_grp = lv_obj_create(parent);
    lv_obj_remove_style_all(d->ambient_grp);
    lv_obj_set_size(d->ambient_grp, SCREEN_W, 224);
    lv_obj_align(d->ambient_grp, LV_ALIGN_TOP_MID, 0, AMBIENT_Y_CENTERED);
    lv_obj_clear_flag(d->ambient_grp, LV_OBJ_FLAG_SCROLLABLE);

    /* v5.3: the breathing pulse ring (awaiting's glyph, device-wide
     * visual language) in the same 96x96 slot the pet used to hold. */
    lv_obj_t *pbox = lv_obj_create(d->ambient_grp);
    lv_obj_remove_style_all(pbox);
    lv_obj_set_size(pbox, 96, 96);
    lv_obj_align(pbox, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(pbox, LV_OBJ_FLAG_SCROLLABLE);

    d->pulse_ring = lv_obj_create(pbox);
    lv_obj_remove_style_all(d->pulse_ring);
    lv_obj_set_size(d->pulse_ring, 72, 72);
    lv_obj_align(d->pulse_ring, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(d->pulse_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(d->pulse_ring, lv_color_hex(0x5A514A), 0);
    lv_obj_set_style_border_width(d->pulse_ring, 2, 0);
    lv_obj_set_style_radius(d->pulse_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(d->pulse_ring, LV_OBJ_FLAG_SCROLLABLE);

    d->pulse_dot = lv_obj_create(d->pulse_ring);
    lv_obj_remove_style_all(d->pulse_dot);
    lv_obj_set_size(d->pulse_dot, 18, 18);
    lv_obj_align(d->pulse_dot, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(d->pulse_dot, lv_color_hex(0x5A514A), 0);
    lv_obj_set_style_bg_opa(d->pulse_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(d->pulse_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(d->pulse_dot, LV_OBJ_FLAG_SCROLLABLE);
    d->pulse_color = 0x5A514A;

    /* Same breath as the awaiting glyph: 14↔28 px, 2 s, infinite. A
     * small solid dot — cheap per-frame redraw, no big-label overlap. */
    pulse_breath_start(d);

    /* Status word — the scene's primary fact after the pet: TITLE tier
     * (52 px ≈ 22' of visual angle at 0.6 m). */
    d->ambient_lbl = lv_label_create(d->ambient_grp);
    lv_obj_set_style_text_color(d->ambient_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(d->ambient_lbl, ui_type_bold(UI_T_TITLE), 0);
    lv_label_set_text(d->ambient_lbl, "空闲");
    lv_obj_align(d->ambient_lbl, LV_ALIGN_TOP_MID, 0, AMBIENT_WORD_Y);

    /* Project chip (v6.0, from the retired takeover) — LABEL tier under
     * the word, gold-only, hidden until a slot wants the user. */
    d->ambient_chip = lv_label_create(d->ambient_grp);
    lv_obj_set_style_text_color(d->ambient_chip, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_text_font(d->ambient_chip, ui_type(UI_T_LABEL), 0);
    lv_label_set_text(d->ambient_chip, "");
    lv_obj_align(d->ambient_chip, LV_ALIGN_TOP_MID, 0, AMBIENT_CHIP_Y);
    lv_obj_add_flag(d->ambient_chip, LV_OBJ_FLAG_HIDDEN);

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
        lv_obj_set_size(r->dot, 16, 16);
        lv_obj_set_style_radius(r->dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(r->dot, lv_color_hex(COL_TEAL), 0);
        lv_obj_set_style_bg_opa(r->dot, LV_OPA_COVER, 0);

        r->name_lbl = lv_label_create(r->card);
        lv_obj_set_style_text_color(r->name_lbl, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_text_font(r->name_lbl, ui_type_bold(UI_T_BODY), 0);
        lv_label_set_long_mode(r->name_lbl, LV_LABEL_LONG_DOT);
        lv_label_set_text(r->name_lbl, "");

        r->meta_lbl = lv_label_create(r->card);
        lv_obj_set_style_text_color(r->meta_lbl, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_style_text_font(r->meta_lbl, ui_type(UI_T_LABEL), 0);
        lv_label_set_text(r->meta_lbl, "");

        r->act_lbl = lv_label_create(r->card);
        lv_obj_set_style_text_color(r->act_lbl, lv_color_hex(COL_TEXT_DIM), 0);
        lv_obj_set_style_text_font(r->act_lbl, ui_type(UI_T_LABEL), 0);
        lv_label_set_long_mode(r->act_lbl, LV_LABEL_LONG_DOT);
        lv_label_set_text(r->act_lbl, "");
    }

    d->timer = lv_timer_create(tick, 500, d);
    lv_timer_pause(d->timer);

    /* ── 转场演员表 ──
     * footer：来自 status_bar 的规范定义（共享 key）。
     * ambient：整簇一个演员，从底部弹入/沉出——不要拆成环/词/chip 三个
     *   演员各自动，那是三份大 tiny_ttf 重绘，也会跟 ambient_slide_to
     *   争同一条 y。TROPA_NONE：位移已经把它整个送出屏幕，没必要再给
     *   TITLE 52 的词加逐帧 text_opa。
     * rows：每张卡错峰 50ms，后进先出地退场。
     *
     * 不开 .bake（v6.4 实测）：烘焙让 dashboard 转场从 21.8ms 变成
     * 26.6ms，慢 23%。规律是烘焙只在【内容成本/面积】比值高时划算——
     * ambient 簇是 466x224 的容器，里面只有一个 96px 圆环和两行字，
     * 绝大部分是空像素；烘焙它等于每帧从 PSRAM 搬 417KB 几乎全透明的
     * ARGB8888，去替代画三个廉价对象。weather 的演员正相反（30 条
     * 抗锯齿线段、15 个标签），那边烘焙省 17%。 */
    status_bar_trans_actors(&d->sb, &s_dash_actors[DASH_A_FOOTER0]);
    s_dash_actors[DASH_A_AMBIENT] = (trans_actor_t){
        .obj = d->ambient_grp, .dir = TRANS_FROM_BOTTOM, .ch = TROPA_NONE,
        .out_dist = DASH_AMBIENT_OUT, .delay_ms = 90 };
    for (int i = 0; i < AGENT_SLOT_MAX; ++i) {
        s_dash_actors[DASH_A_ROW0 + i] = (trans_actor_t){
            .obj = d->rows[i].card, .dir = TRANS_FROM_BOTTOM, .ch = TROPA_NONE,
            .out_dist = DASH_ROW_OUT, .delay_ms = (uint16_t)(50 * i) };
    }
    scene_trans_bind("dashboard", &s_dash_profile);
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
