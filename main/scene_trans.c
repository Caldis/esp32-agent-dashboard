/*
 * scene_trans — implementation. See scene_trans.h.
 *
 * 状态机：IDLE → OUTRO → (黑幕瞬切) → INTRO → IDLE。
 * 推进用一次性 lv_timer；所有入口都在 display 锁 / LVGL task 上。
 * 演员动画：位移走 spring_disp（入场）/ apple_ease_in（出场），
 * 透明度走 spring_opa / apple_ease_in。HIDDEN 的演员整段跳过。
 *
 * v6.2 连续性层：出场/入场前先求 (from, to) 两侧演员的共享交集
 * （mark_held）。同 key 同姿态的演员双方都不动，只被钉在 rest 上——
 * 元素"原地等待"换页，而不是飞出去再飞回来。详见 scene_trans.h。
 */

#include "scene_trans.h"
#include "agent_state.h"
#include "anim/spring.h"
#include "anim/apple_ease.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "scene_trans";

#define OUT_MS        240   /* 出场（加速离屏） */
#define IN_MS         520   /* 入场（弹簧展开） */
#define IN_OPA_MS     380   /* 入场淡入比位移先到位——先"显形"后"落座" */
#define STEP_GUARD_MS  40   /* 每阶段完成后的推进余量 */

#define MAX_PROFILES  12

enum { ST_IDLE = 0, ST_OUTRO, ST_INTRO };

static struct {
    const char      *id;
    trans_profile_t *p;
} s_profiles[MAX_PROFILES];
static int s_profile_n = 0;

static int         s_state = ST_IDLE;
static int         s_pending = -1;     /* 转场目标（可被覆盖） */
static lv_timer_t *s_step = NULL;      /* 一次性推进器 */
/* 出场那一侧的 profile：入场时拿它求共享元素交集（对称判定）。 */
static trans_profile_t *s_from_p = NULL;

/* ── profile 查找 ──────────────────────────────────────────────────── */

static trans_profile_t *profile_of(const scene_t *sc)
{
    if (!sc || !sc->id) return NULL;
    for (int i = 0; i < s_profile_n; ++i)
        if (strcmp(s_profiles[i].id, sc->id) == 0) return s_profiles[i].p;
    return NULL;
}

void scene_trans_bind(const char *scene_id, trans_profile_t *profile)
{
    if (!scene_id || !profile || s_profile_n >= MAX_PROFILES) return;
    /* rest 位置快照：对象此刻处于 align 后的静止姿态。 */
    for (int i = 0; i < profile->actor_n; ++i) {
        trans_actor_t *a = &profile->actors[i];
        a->held = 0;
        if (!a->obj) continue;
        if (a->dir == TRANS_FROM_LEFT || a->dir == TRANS_FROM_RIGHT)
            a->rest_pos = (int16_t)lv_obj_get_style_x(a->obj, LV_PART_MAIN);
        else
            a->rest_pos = (int16_t)lv_obj_get_style_y(a->obj, LV_PART_MAIN);
    }
    s_profiles[s_profile_n].id = scene_id;
    s_profiles[s_profile_n].p  = profile;
    s_profile_n++;
}

/* ── 演员低层：位移/透明度读写 ─────────────────────────────────────── */

static int32_t out_pos_of(const trans_actor_t *a)
{
    switch (a->dir) {
    case TRANS_FROM_TOP:    return a->rest_pos - a->out_dist;
    case TRANS_FROM_BOTTOM: return a->rest_pos + a->out_dist;
    case TRANS_FROM_LEFT:   return a->rest_pos - a->out_dist;
    case TRANS_FROM_RIGHT:  return a->rest_pos + a->out_dist;
    default:                return a->rest_pos;
    }
}

static bool is_x_axis(const trans_actor_t *a)
{
    return a->dir == TRANS_FROM_LEFT || a->dir == TRANS_FROM_RIGHT;
}

/* ── 共享元素：求 (from, to) 的交集 ────────────────────────────────
 * key 相同只是"开发者说这是同一个东西"；姿态全等才是可验证的护栏。
 * 两轴都比：动画轴用 rest_pos（对象可能正停在场外，屏幕坐标不可信），
 * 另一轴转场从不改写，当前 style 值即静止值。 */

static bool same_pose(const trans_actor_t *a, const trans_actor_t *b)
{
    if (a->dir != b->dir || a->ch != b->ch) return false;
    if (a->base_opa != b->base_opa)         return false;
    if (a->rest_pos != b->rest_pos)         return false;
    if (lv_obj_get_style_align(a->obj, LV_PART_MAIN) !=
        lv_obj_get_style_align(b->obj, LV_PART_MAIN)) return false;
    int32_t ax = is_x_axis(a) ? lv_obj_get_style_y(a->obj, LV_PART_MAIN)
                              : lv_obj_get_style_x(a->obj, LV_PART_MAIN);
    int32_t bx = is_x_axis(b) ? lv_obj_get_style_y(b->obj, LV_PART_MAIN)
                              : lv_obj_get_style_x(b->obj, LV_PART_MAIN);
    return ax == bx;
}

/* 本次转场里，把与对侧同 key 同姿态的演员标为 held（原地待命）。
 * 返回 held 的个数——日志里报出来，"共享判定是否命中"才是可观测的
 * （姿态不等会静默退化成正常进出场，不看计数根本发现不了漂移）。 */
static int mark_held(trans_profile_t *p, trans_profile_t *other)
{
    int n_held = 0;
    if (!p) return 0;
    for (int i = 0; i < p->actor_n; ++i) {
        trans_actor_t *a = &p->actors[i];
        a->held = 0;
        if (!a->obj || !a->key || !other) continue;
        if (lv_obj_has_flag(a->obj, LV_OBJ_FLAG_HIDDEN)) continue;
        for (int j = 0; j < other->actor_n; ++j) {
            trans_actor_t *b = &other->actors[j];
            if (!b->obj || !b->key || strcmp(a->key, b->key) != 0) continue;
            if (lv_obj_has_flag(b->obj, LV_OBJ_FLAG_HIDDEN)) break;
            if (same_pose(a, b)) { a->held = 1; n_held++; }
            break;                       /* key 唯一：命中即定论 */
        }
    }
    return n_held;
}

static void actor_set_opa(const trans_actor_t *a, lv_opa_t v)
{
    switch (a->ch) {
    case TROPA_TEXT:
        lv_obj_set_style_text_opa(a->obj, v, 0);
        break;
    case TROPA_GROUP_TEXT: {
        uint32_t n = lv_obj_get_child_count(a->obj);
        for (uint32_t i = 0; i < n; ++i)
            lv_obj_set_style_text_opa(lv_obj_get_child(a->obj, i), v, 0);
        break;
    }
    case TROPA_BG:
        lv_obj_set_style_bg_opa(a->obj, v, 0);
        break;
    case TROPA_BORDER:
        lv_obj_set_style_border_opa(a->obj, v, 0);
        break;
    default:
        break;
    }
}

static lv_opa_t actor_get_opa(const trans_actor_t *a)
{
    switch (a->ch) {
    case TROPA_TEXT:
        return lv_obj_get_style_text_opa(a->obj, LV_PART_MAIN);
    case TROPA_GROUP_TEXT: {
        lv_obj_t *c = lv_obj_get_child(a->obj, 0);
        return c ? lv_obj_get_style_text_opa(c, LV_PART_MAIN) : LV_OPA_COVER;
    }
    case TROPA_BG:
        return lv_obj_get_style_bg_opa(a->obj, LV_PART_MAIN);
    case TROPA_BORDER:
        return lv_obj_get_style_border_opa(a->obj, LV_PART_MAIN);
    default:
        return LV_OPA_COVER;
    }
}

/* anim exec：var = 演员条目（static 数组，指针恒有效）。 */
static void anim_actor_pos(void *var, int32_t v)
{
    trans_actor_t *a = (trans_actor_t *)var;
    if (is_x_axis(a)) lv_obj_set_x(a->obj, v);
    else              lv_obj_set_y(a->obj, v);
}

static void anim_actor_opa(void *var, int32_t v)
{
    actor_set_opa((trans_actor_t *)var, (lv_opa_t)v);
}

static void actor_kill_anims(trans_actor_t *a)
{
    lv_anim_delete(a, anim_actor_pos);
    lv_anim_delete(a, anim_actor_opa);
}

static void start_actor_anim(trans_actor_t *a, lv_anim_exec_xcb_t cb,
                             int32_t from, int32_t to, uint32_t ms,
                             uint32_t delay, lv_anim_path_cb_t path)
{
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, a);
    lv_anim_set_time(&an, ms);
    lv_anim_set_delay(&an, delay);
    lv_anim_set_path_cb(&an, path);
    lv_anim_set_values(&an, from, to);
    lv_anim_set_exec_cb(&an, cb);
    lv_anim_start(&an);
}

/* 只统计真正会动的演员：held / HIDDEN 的不参与错峰基准，否则一屏全是
 * 共享元素时还要白等一个 maxd。 */
static uint16_t max_delay_of(const trans_profile_t *p)
{
    uint16_t m = 0;
    for (int i = 0; i < p->actor_n; ++i) {
        const trans_actor_t *a = &p->actors[i];
        if (!a->obj || a->held) continue;
        if (lv_obj_has_flag(a->obj, LV_OBJ_FLAG_HIDDEN)) continue;
        if (a->delay_ms > m) m = a->delay_ms;
    }
    return m;
}

/* held 演员：杀掉残留动画并钉死在 rest 姿态。瞬切帧上两侧像素级重合，
 * 交接才不可见——这是整个共享元素机制的交接点。 */
static void actor_park_at_rest(trans_actor_t *a)
{
    actor_kill_anims(a);
    anim_actor_pos(a, a->rest_pos);
    if (a->ch != TROPA_NONE) actor_set_opa(a, a->base_opa);
}

/* ── 出场 / 入场编排 ──────────────────────────────────────────────── */

/* 出场：从 live 值加速离屏（后进先出：入场 delay 大的先走）。
 * 返回全部完成所需的毫秒数。 */
static uint32_t play_outro(scene_t *sc, bool animate, trans_profile_t *other)
{
    trans_profile_t *p = profile_of(sc);
    uint32_t total = 0;

    if (p) {
        if (p->sync_rest) p->sync_rest(sc);
        int n_held = mark_held(p, other);
        ESP_LOGI(TAG, "outro %s: %d/%d held", sc->id, n_held, p->actor_n);
        uint16_t maxd = max_delay_of(p);
        bool any = false;
        for (int i = 0; i < p->actor_n; ++i) {
            trans_actor_t *a = &p->actors[i];
            if (!a->obj) continue;
            if (a->held) { actor_park_at_rest(a); continue; }
            actor_kill_anims(a);
            if (lv_obj_has_flag(a->obj, LV_OBJ_FLAG_HIDDEN)) continue;
            int32_t live_pos = is_x_axis(a) ? lv_obj_get_style_x(a->obj, LV_PART_MAIN)
                                            : lv_obj_get_style_y(a->obj, LV_PART_MAIN);
            uint32_t d = maxd - a->delay_ms;      /* 反转：后到的先走 */
            if (!animate) {
                anim_actor_pos(a, out_pos_of(a));
                actor_set_opa(a, LV_OPA_TRANSP);
                continue;
            }
            any = true;
            if (a->dir != TRANS_FADE_ONLY)
                start_actor_anim(a, anim_actor_pos, live_pos, out_pos_of(a),
                                 OUT_MS, d, apple_ease_in);
            if (a->ch != TROPA_NONE)
                start_actor_anim(a, anim_actor_opa, actor_get_opa(a),
                                 LV_OPA_TRANSP, OUT_MS, d, apple_ease_in);
        }
        if (animate && any) total = OUT_MS + maxd;
        if (p->clock_to_consensus) {
            p->clock_to_consensus(sc, animate ? OUT_MS : 0);
            /* 时间锚点也是"会动的东西"：全员 held 时它得独自撑住时长。 */
            if (animate && total < OUT_MS) total = OUT_MS;
        }
    }
    return total;
}

/* 入场：先摆到场外，再弹簧滑入。透明度先到位（IN_OPA_MS < IN_MS），
 * 元素"显形中落座"。 */
static uint32_t play_intro(scene_t *sc, bool animate, trans_profile_t *other)
{
    trans_profile_t *p = profile_of(sc);
    uint32_t total = 0;

    if (p) {
        if (p->sync_rest) p->sync_rest(sc);
        int n_held = mark_held(p, other);
        ESP_LOGI(TAG, "intro %s: %d/%d held", sc->id, n_held, p->actor_n);
        uint16_t maxd = max_delay_of(p);
        bool any = false;
        for (int i = 0; i < p->actor_n; ++i) {
            trans_actor_t *a = &p->actors[i];
            if (!a->obj) continue;
            /* 共享：上一场景的同位对象刚刚就停在这个姿态上，瞬间钉住
             * 即可——它在观众眼里"根本没离开过屏幕"。 */
            if (a->held) { actor_park_at_rest(a); continue; }
            actor_kill_anims(a);
            if (lv_obj_has_flag(a->obj, LV_OBJ_FLAG_HIDDEN)) continue;
            if (!animate) {
                anim_actor_pos(a, a->rest_pos);
                actor_set_opa(a, a->base_opa);
                continue;
            }
            any = true;
            if (a->dir != TRANS_FADE_ONLY) {
                anim_actor_pos(a, out_pos_of(a));
                start_actor_anim(a, anim_actor_pos, out_pos_of(a), a->rest_pos,
                                 IN_MS, a->delay_ms, spring_disp);
            }
            if (a->ch != TROPA_NONE) {
                actor_set_opa(a, LV_OPA_TRANSP);
                start_actor_anim(a, anim_actor_opa, LV_OPA_TRANSP, a->base_opa,
                                 IN_OPA_MS, a->delay_ms, spring_opa);
            }
        }
        if (animate && any) total = IN_MS + maxd;
        if (p->clock_from_consensus)
            p->clock_from_consensus(sc, animate ? IN_MS : 0);
        if (p->clock_from_consensus && animate && IN_MS > total) total = IN_MS;
    }
    return total;
}

/* ── 状态机推进 ───────────────────────────────────────────────────── */

static bool motion_ok(void)
{
    bool ok;
    agent_state_lock();
    ok = !agent_state_get()->motion_reduced;
    agent_state_unlock();
    return ok;
}

static void arm_step(uint32_t ms);

/* OUTRO 完成：黑幕瞬切 + 播新场景 INTRO。 */
static void do_switch_and_intro(void)
{
    int target = s_pending;
    s_pending = -1;
    if (target < 0) { s_state = ST_IDLE; s_from_p = NULL; return; }

    trans_profile_t *from_p = s_from_p;
    s_from_p = NULL;

    scene_fw_show_instant(target);
    const scene_t *sc = scene_fw_current();

    bool anim = motion_ok();
    uint32_t in_total = play_intro((scene_t *)sc, anim, from_p);
    if (anim && in_total > 0) {
        s_state = ST_INTRO;
        arm_step(in_total + STEP_GUARD_MS);
    } else {
        s_state = ST_IDLE;
    }
}

static void step_cb(lv_timer_t *t)
{
    (void)t;
    lv_timer_pause(s_step);
    if (s_state == ST_OUTRO) {
        do_switch_and_intro();
    } else if (s_state == ST_INTRO) {
        /* INTRO 收尾。期间若有新目标（连按），立刻转 OUTRO。 */
        if (s_pending >= 0) {
            scene_t *cur = (scene_t *)scene_fw_current();
            bool anim = motion_ok();
            s_from_p = profile_of(cur);
            uint32_t out_total = play_outro(cur, anim,
                                            profile_of(scene_fw_get(s_pending)));
            s_state = ST_OUTRO;
            arm_step(out_total + STEP_GUARD_MS);
        } else {
            s_state = ST_IDLE;
        }
    }
}

static void arm_step(uint32_t ms)
{
    if (!s_step) {
        s_step = lv_timer_create(step_cb, ms, NULL);
        lv_timer_set_repeat_count(s_step, -1);
    }
    lv_timer_set_period(s_step, ms);
    lv_timer_reset(s_step);
    lv_timer_resume(s_step);
}

bool scene_trans_busy(void)
{
    return s_state != ST_IDLE;
}

void scene_trans_switch(int target_idx)
{
    int n = scene_fw_count();
    if (n <= 0) return;
    target_idx = ((target_idx % n) + n) % n;

    if (s_state == ST_IDLE) {
        if (target_idx == scene_fw_current_index()) return;
        s_pending = target_idx;
        scene_t *cur = (scene_t *)scene_fw_current();
        bool anim = motion_ok();
        s_from_p = profile_of(cur);
        uint32_t out_total = play_outro(cur, anim,
                                        profile_of(scene_fw_get(target_idx)));
        if (anim && out_total > 0) {
            s_state = ST_OUTRO;
            arm_step(out_total + STEP_GUARD_MS);
        } else {
            /* 无演员/无动画：出场即刻完成，直接瞬切。 */
            s_state = ST_OUTRO;
            do_switch_and_intro();
        }
        return;
    }

    /* 转场中：覆盖目标。OUTRO 阶段自然转向新目标；INTRO 阶段由
     * step_cb 在收尾时发现 pending 并立刻反向出场。 */
    s_pending = target_idx;
    ESP_LOGI(TAG, "retarget to %d (state=%d)", target_idx, s_state);
}
