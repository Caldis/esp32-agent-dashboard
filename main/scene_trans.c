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
#include "perf_mon.h"

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

/* ── 实测反例之二：转场期间整屏合并 ────────────────────────────────
 * 试过并撤销：在 LV_EVENT_REFR_START 里 lv_obj_invalidate(screen)，把一帧
 * 里 20+ 个互不重叠的脏矩形折成一个整屏区域。动机是"每帧脏像素 34~83 万
 * vs 整屏 21.7 万，整屏画一次应该更便宜"。
 * 结果反而更慢：render 14.7→29.7ms（clock）、20.9→32.6ms（weather），
 * overrun 1→17。
 *
 * 教训——渲染成本不与"脏区面积"成正比，而与"要重新生成的内容"成正比。
 * 脏矩形小的时候，落在里面的对象少；整屏失效等于强制把屏幕上每一个对象
 * 都重画一遍，包括天气那 30 条抗锯齿线段、圆环、圆角卡片这些昂贵图元。
 * 面积只是账单的一半，另一半是"这块面积上压着多少个要重新光栅化的东西"。
 *
 * 这条反例直接指向了正解：贵的是【重新生成内容】，那就别每帧重新生成
 * ——把运动中的演员烤成位图（见下面的 ghost 精灵）。 */

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

/* ── 精灵烘焙 (v6.3) ───────────────────────────────────────────────
 * 运动开始前把演员（含其所有子对象）光栅化成一张位图，转场期间移动这张
 * 位图，结束后换回本体。
 *
 * 为什么有效：实测证明每帧的成本来自【重新生成内容】——天气插画的 30
 * 条抗锯齿线段、脉冲圆环、fleet 卡片的圆角、TITLE 52 的汉字，每一帧、
 * 每一个缓冲分块都要重画一遍。烤成位图之后每帧只剩一次图像 blit：没有
 * 几何、没有抗锯齿、没有圆角遮罩，纯内存搬运。同样的画面、同样的运动
 * 曲线，成本降一个数量级。这就是掌机时代 sprite 的老办法。
 *
 * 一次性成本：转场开始时每个演员一次快照（约等于它一帧的渲染成本），
 * 发生在元素还静止的时候，之后 20+ 帧全部受益。
 *
 * 内存：RGB565A8（3 B/px）。保留 alpha 是为了不赌背景——不透明 RGB565
 * 会把容器的透明区域烤成纯黑方块，在 AMOLED 上纯黑与背景 0x0B0A09 的
 * 差别肉眼可见，而且演员之间有重叠（weather 的星群与大温度）。大块自动
 * 落在 PSRAM（>16KB 的分配走 SPIRAM），weather 全部演员合计约 400KB。
 *
 * 失败即降级：快照失败（内存不足）就返回 false，该演员走常规路径。 */

static lv_obj_t *actor_target(const trans_actor_t *a)
{
    return a->ghost ? a->ghost : a->obj;
}

static bool s_bake_on = true;

void scene_trans_set_bake(bool on) { s_bake_on = on; }
bool scene_trans_get_bake(void)    { return s_bake_on; }

static bool ghost_begin(trans_actor_t *a)
{
    if (!s_bake_on) return false;
    if (!a->bake || a->ghost || a->ch != TROPA_NONE) return false;
    if (!a->obj || lv_obj_has_flag(a->obj, LV_OBJ_FLAG_HIDDEN)) return false;

    /* 格式必须同时在两张白名单里：
     *   lv_snapshot_take_to_draw_buf 的 switch（lv_snapshot.c）
     *   CONFIG_LV_DRAW_SW_SUPPORT_* （渲染器能不能画）
     * RGB565A8 只满足第二张——snapshot 直接返回 INVALID。v6.3 用的就是它，
     * 于是每次烘焙都静默失败降级，"精灵烘焙"整个特性从未真正运行过，而
     * ?bake 的 A/B 两条臂跑的是同一份代码路径（那 5~15% 的"收益"是漂移）。
     * ARGB8888 两张白名单都在。代价是 4 B/px 而不是 3。 */
    lv_draw_buf_t *buf = lv_snapshot_take(a->obj, LV_COLOR_FORMAT_ARGB8888);
    if (!buf) {
        /* WARN, not DEBUG: silent fallback is how this feature spent a
         * whole release doing nothing while an A/B "measured" its win. */
        ESP_LOGW(TAG, "bake failed (snapshot returned NULL) — live actor");
        return false;
    }
    lv_obj_t *img = lv_image_create(lv_obj_get_parent(a->obj));
    if (!img) { lv_draw_buf_destroy(buf); return false; }
    lv_obj_remove_style_all(img);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_image_set_src(img, buf);
    /* 与本体同姿态：align 是持久属性，x/y 是它上面的偏移，两者都照抄。 */
    lv_obj_set_style_align(img, lv_obj_get_style_align(a->obj, LV_PART_MAIN), 0);
    lv_obj_set_pos(img, lv_obj_get_style_x(a->obj, LV_PART_MAIN),
                        lv_obj_get_style_y(a->obj, LV_PART_MAIN));
    lv_obj_add_flag(a->obj, LV_OBJ_FLAG_HIDDEN);

    a->ghost     = img;
    a->ghost_buf = buf;
    return true;
}

/* 替身落幕：把它的最终姿态交还本体，再销毁。本体在整个转场里没动过，
 * 所以这一步就是"位置交接"——观众看到的仍是同一个东西。 */
static void ghost_end(trans_actor_t *a)
{
    if (!a->ghost) return;
    int32_t x = lv_obj_get_style_x(a->ghost, LV_PART_MAIN);
    int32_t y = lv_obj_get_style_y(a->ghost, LV_PART_MAIN);
    lv_obj_delete(a->ghost);
    lv_draw_buf_destroy(a->ghost_buf);
    a->ghost     = NULL;
    a->ghost_buf = NULL;
    lv_obj_set_pos(a->obj, x, y);
    lv_obj_clear_flag(a->obj, LV_OBJ_FLAG_HIDDEN);
}

static void profile_ghosts_end(trans_profile_t *p)
{
    if (!p) return;
    for (int i = 0; i < p->actor_n; ++i) ghost_end(&p->actors[i]);
}

/* anim exec：var = 演员条目（static 数组，指针恒有效）。目标可能是替身。 */
static void anim_actor_pos(void *var, int32_t v)
{
    trans_actor_t *a = (trans_actor_t *)var;
    lv_obj_t *t = actor_target(a);
    if (is_x_axis(a)) lv_obj_set_x(t, v);
    else              lv_obj_set_y(t, v);
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
    ghost_end(a);                     /* 共享元素不动，替身没有存在的理由 */
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
            if (!animate) {
                ghost_end(a);
                anim_actor_pos(a, out_pos_of(a));
                actor_set_opa(a, LV_OPA_TRANSP);
                continue;
            }
            /* 先烤再读姿态：替身生于本体当前位置，读谁都一样；重定向
             * 打断时替身已存在，ghost_begin 让位，出场直接接手它。 */
            ghost_begin(a);
            lv_obj_t *tgt = actor_target(a);
            int32_t live_pos = is_x_axis(a) ? lv_obj_get_style_x(tgt, LV_PART_MAIN)
                                            : lv_obj_get_style_y(tgt, LV_PART_MAIN);
            uint32_t d = maxd - a->delay_ms;      /* 反转：后到的先走 */
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
                ghost_end(a);
                anim_actor_pos(a, a->rest_pos);
                actor_set_opa(a, a->base_opa);
                continue;
            }
            ghost_begin(a);
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

/* ── 动态刷新率 (v6.3) ─────────────────────────────────────────────
 * 静止低刷省电、运动高刷保顺滑——两种取向，两个档位，跟着转场状态机走。
 *
 * 为什么必须做：实测转场期间 render ≈ 20ms，而刷新周期是 33ms。也就是
 * 每帧画完还要空等 13ms 才允许画下一帧——帧率被封在 30fps 并不是因为
 * 画不动，是因为没人让它画。把转场窗口的周期压到 REFR_MS_ACTIVE，帧就
 * 以渲染器的真实速度连续产出。
 *
 * 反过来，静止时屏幕上只有呼吸点/冒号这种低频动画，33ms 是纯粹的浪费：
 * 每次刷新周期到点 LVGL 都要走一遍脏区检查。IDLE 档把它拉长，代价只是
 * 呼吸动画的时间分辨率——2s 一个呼吸周期在 66ms 步进下仍有 30 级，肉眼
 * 看不出台阶。
 *
 * 档位在 IDLE<->转场之间切换，不做更细的分级：唯一的高刷需求就是转场
 * 和它带动的锚点变形，而那正好由这个状态机界定。 */
#define REFR_MS_ACTIVE  16          /* 转场：~60Hz 上限，实际由渲染器决定 */
static uint32_t s_refr_idle_ms = 66;   /* 静止：~15Hz，可用 ?refr 调 */

static void refr_rate(uint32_t ms)
{
    lv_display_t *d = lv_display_get_default();
    if (!d) return;
    lv_timer_t *t = lv_display_get_refr_timer(d);
    if (t) lv_timer_set_period(t, ms);
    perf_mon_set_period(ms);        /* overrun 阈值要跟着档位走 */
}

void scene_trans_set_idle_refr(uint32_t ms)
{
    if (ms < 8) ms = 8;
    s_refr_idle_ms = ms;
    if (s_state == ST_IDLE) refr_rate(ms);
}

uint32_t scene_trans_get_idle_refr(void) { return s_refr_idle_ms; }

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
    if (target < 0) { s_state = ST_IDLE; s_from_p = NULL;
                      refr_rate(s_refr_idle_ms); return; }

    trans_profile_t *from_p = s_from_p;
    s_from_p = NULL;

    /* 出场结束：替身在黑幕瞬切【之前】落幕，把离屏姿态交还本体，并释放
     * 像素。晚一步就会随场景容器一起被隐藏，缓冲泄漏在 PSRAM 里。 */
    profile_ghosts_end(from_p);

    scene_fw_show_instant(target);
    const scene_t *sc = scene_fw_current();

    bool anim = motion_ok();
    uint32_t in_total = play_intro((scene_t *)sc, anim, from_p);
    if (anim && in_total > 0) {
        s_state = ST_INTRO;
        arm_step(in_total + STEP_GUARD_MS);
    } else {
        profile_ghosts_end(profile_of(sc));
        s_state = ST_IDLE;
        refr_rate(s_refr_idle_ms);
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
            /* 入场收尾：替身落幕，本体回到 rest 并重新接管（呼吸、闪烁、
             * 内容刷新都要作用在真身上）。 */
            profile_ghosts_end(profile_of(scene_fw_current()));
            s_state = ST_IDLE;
            refr_rate(s_refr_idle_ms);
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
        /* 抬到高刷【在出场动画开始之前】——晚一帧就是可见的第一帧卡顿。 */
        refr_rate(REFR_MS_ACTIVE);
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

    /* 转场中：覆盖目标。INTRO 阶段由 step_cb 在收尾时发现 pending 并
     * 立刻反向出场。 */
    int prev_pending = s_pending;
    s_pending = target_idx;
    ESP_LOGI(TAG, "retarget to %d (state=%d)", target_idx, s_state);

    /* OUTRO 阶段重定向时，held 判定是按【旧目标】算的，必须重算。
     * 具体症状：clock→dashboard 途中改判去 weather——footer 与 dashboard
     * 共享所以正原地待命，而 weather 根本没有 footer 层级，于是它在黑幕
     * 瞬切那一刻凭空消失，而不是滑出去。只在快速连按时出现，一帧，但它
     * 是"共享元素"契约的破口：元素要么滑出去，要么留下，不能蒸发。
     * 重跑 outro 即可：play_outro 会杀掉旧动画并从当前实际位置重新起步，
     * 已经在飞的演员只是换了个起点继续飞，新失去 held 的演员开始滑出。 */
    if (s_state == ST_OUTRO && target_idx != prev_pending) {
        scene_t *cur = (scene_t *)scene_fw_current();
        bool anim = motion_ok();
        uint32_t out_total = play_outro(cur, anim,
                                        profile_of(scene_fw_get(target_idx)));
        arm_step((out_total ? out_total : 0) + STEP_GUARD_MS);
    }
}
