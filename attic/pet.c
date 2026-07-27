/*
 * pet — see pet.h.
 *
 * Object tree:
 *
 *   root (96x96, transparent)          ← bounce / sway / breath anims
 *    ├─ spin  (72x72, centered)
 *    │   └─ screen (Codex terminal face) / disc (generic)
 *    ├─ clawd (Claude: pixel crab — body + arms ×2 + legs ×4;
 *    │         direct root child: wider than spin, would clip there)
 *    └─ face  (centered, above all)    ← Codex eye-scan anim
 *        ├─ eye_l, eye_r
 *        └─ mouth
 *
 * Clawd is Claude Code's classic pixel mascot, traced from the official
 * spritesheet onto a 3px-unit grid of plain rects (radius 0) — no
 * canvas, no image assets, so it costs nothing in flash and stays crisp
 * on the round AMOLED. Eyes live in the face layer as dark punch-outs
 * drawn over the body; while working the legs scuttle in alternating
 * pairs.
 */

#include "pet.h"
#include "theme.h"

#include <string.h>

#define PET_SIZE      96
#define BODY_SIZE     72
#define COL_EYE       0x0B0A09   /* noir bg — eyes punch through the body */
#define COL_SCREEN    0x141110   /* codex terminal face fill */

#define EYE_W          10
#define EYE_H_OPEN     10
#define EYE_H_WIDE     13        /* waiting: big expectant eyes */
#define EYE_H_CLOSED    2
#define CODEX_EYE_W     6
#define CODEX_EYE_H    16

/* Clawd pixel grid, traced from the official spritesheet (walk-cycle
 * row): unit u = 3px. Body 20u×15u, arms 5u×5u poking out mid-body,
 * eyes 2u squares set high, legs 2u×5u in two pairs (outer legs flush
 * with the body's edges). */
#define CLAWD_U         3
#define CLAWD_W        (CLAWD_U * 30)             /* arms included: 90 */
#define CLAWD_H        (CLAWD_U * 20)             /* body + legs: 60 */
#define CLAWD_LEG_Y    (CLAWD_U * 15)             /* legs' resting row */
#define CLAWD_LEG_LIFT (CLAWD_U * 1)              /* scuttle lift, px */
#define CLAWD_EYE_W    (CLAWD_U * 2)              /* small square eyes */
#define CLAWD_EYE_H    (CLAWD_U * 2)
#define CLAWD_EYE_WIDE (CLAWD_U * 3)

typedef enum { KIND_GENERIC = 0, KIND_CLAUDE, KIND_CODEX } pet_kind_t;

struct pet {
    lv_obj_t   *root;
    lv_obj_t   *spin;                /* body layer (kept unrotated) */
    lv_obj_t   *clawd;               /* claude pixel-crab container */
    lv_obj_t   *legs[4];             /* clawd scuttle targets */
    lv_obj_t   *screen;              /* codex terminal body */
    lv_obj_t   *disc;                /* generic round body */
    lv_obj_t   *face;                /* eye group (codex scan target) */
    lv_obj_t   *eye_l, *eye_r, *mouth;
    lv_timer_t *blink;
    pet_kind_t  kind;
    pet_mood_t  mood;
    bool        eyes_open;
    bool        inited;
};

/* ── anim exec helpers ───────────────────────────────────────────── */

static void a_y(void *o, int32_t v)      { lv_obj_set_y((lv_obj_t *)o, v); }
static void a_x(void *o, int32_t v)      { lv_obj_set_x((lv_obj_t *)o, v); }
static void a_rot(void *o, int32_t v)    { lv_obj_set_style_transform_rotation((lv_obj_t *)o, v, 0); }
static void a_scale(void *o, int32_t v)  { lv_obj_set_style_transform_scale((lv_obj_t *)o, v, 0); }
static void a_eye_h(void *o, int32_t v)  { lv_obj_set_height((lv_obj_t *)o, v); }

static void anim_loop(lv_obj_t *obj, lv_anim_exec_xcb_t cb,
                      int32_t from, int32_t to, uint32_t ms, bool yoyo)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    if (yoyo) lv_anim_set_playback_time(&a, ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

/* ── eyes ────────────────────────────────────────────────────────── */

static int eye_open_h(const pet_t *p)
{
    if (p->kind == KIND_CODEX) {
        return (p->mood == PET_MOOD_WAITING) ? CODEX_EYE_H + 4 : CODEX_EYE_H;
    }
    if (p->kind == KIND_CLAUDE) {
        return (p->mood == PET_MOOD_WAITING) ? CLAWD_EYE_WIDE : CLAWD_EYE_H;
    }
    return (p->mood == PET_MOOD_WAITING) ? EYE_H_WIDE : EYE_H_OPEN;
}

static void eyes_set(pet_t *p, bool open)
{
    p->eyes_open = open;
    int h = open ? eye_open_h(p) : EYE_H_CLOSED;
    lv_obj_set_height(p->eye_l, h);
    lv_obj_set_height(p->eye_r, h);
}

/* Quick blink: close fast, spring back. One-shot; the repeating timer
 * retriggers it. */
static void blink_cb(lv_timer_t *t)
{
    pet_t *p = (pet_t *)lv_timer_get_user_data(t);
    if (!p || !p->eyes_open) return;
    int h = eye_open_h(p);
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *eye = i ? p->eye_r : p->eye_l;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, eye);
        lv_anim_set_exec_cb(&a, a_eye_h);
        lv_anim_set_values(&a, h, EYE_H_CLOSED);
        lv_anim_set_time(&a, 90);
        lv_anim_set_playback_time(&a, 140);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }
}

/* ── shape builders (all created once, shown per kind) ───────────── */

static void build(pet_t *p, lv_obj_t *parent)
{
    p->root = lv_obj_create(parent);
    lv_obj_remove_style_all(p->root);
    lv_obj_set_size(p->root, PET_SIZE, PET_SIZE);
    lv_obj_clear_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_transform_pivot_x(p->root, PET_SIZE / 2, 0);
    lv_obj_set_style_transform_pivot_y(p->root, PET_SIZE / 2, 0);

    p->spin = lv_obj_create(p->root);
    lv_obj_remove_style_all(p->spin);
    lv_obj_set_size(p->spin, BODY_SIZE, BODY_SIZE);
    lv_obj_center(p->spin);
    lv_obj_clear_flag(p->spin, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_transform_pivot_x(p->spin, BODY_SIZE / 2, 0);
    lv_obj_set_style_transform_pivot_y(p->spin, BODY_SIZE / 2, 0);

    /* Clawd: Claude Code's pixel mascot, traced from the spritesheet's
     * walk cycle — flat rectangular body, one stubby arm out each side
     * at mid-height, four legs in two pairs. Sharp corners everywhere:
     * it's pixel art. Parented to root (not spin): the arms overflow
     * spin's 72px bounds and would get clipped there. */
    p->clawd = lv_obj_create(p->root);
    lv_obj_remove_style_all(p->clawd);
    lv_obj_set_size(p->clawd, CLAWD_W, CLAWD_H);
    lv_obj_center(p->clawd);
    lv_obj_clear_flag(p->clawd, LV_OBJ_FLAG_SCROLLABLE);
    static const struct { int16_t x, y, w, h; } clawd_px[] = {
        { CLAWD_U *  5, 0,           CLAWD_U * 20, CLAWD_U * 15 }, /* body      */
        { 0,            CLAWD_U * 5, CLAWD_U *  5, CLAWD_U *  5 }, /* left arm  */
        { CLAWD_U * 25, CLAWD_U * 5, CLAWD_U *  5, CLAWD_U *  5 }, /* right arm */
    };
    for (size_t i = 0; i < sizeof clawd_px / sizeof clawd_px[0]; ++i) {
        lv_obj_t *r = lv_obj_create(p->clawd);
        lv_obj_remove_style_all(r);
        lv_obj_set_pos(r, clawd_px[i].x, clawd_px[i].y);
        lv_obj_set_size(r, clawd_px[i].w, clawd_px[i].h);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    }
    static const int16_t leg_x[4] = {
        CLAWD_U * 5, CLAWD_U * 10, CLAWD_U * 18, CLAWD_U * 23,
    };
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *leg = lv_obj_create(p->clawd);
        lv_obj_remove_style_all(leg);
        lv_obj_set_pos(leg, leg_x[i], CLAWD_LEG_Y);
        lv_obj_set_size(leg, CLAWD_U * 2, CLAWD_U * 5);
        lv_obj_set_style_bg_opa(leg, LV_OPA_COVER, 0);
        p->legs[i] = leg;
    }

    /* Codex terminal face: rounded screen with an accent bezel. */
    p->screen = lv_obj_create(p->spin);
    lv_obj_remove_style_all(p->screen);
    lv_obj_set_size(p->screen, 62, 50);
    lv_obj_center(p->screen);
    lv_obj_set_style_radius(p->screen, 12, 0);
    lv_obj_set_style_bg_color(p->screen, lv_color_hex(COL_SCREEN), 0);
    lv_obj_set_style_bg_opa(p->screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p->screen, 2, 0);
    lv_obj_set_style_border_opa(p->screen, LV_OPA_COVER, 0);

    /* Generic round buddy. */
    p->disc = lv_obj_create(p->spin);
    lv_obj_remove_style_all(p->disc);
    lv_obj_set_size(p->disc, 56, 56);
    lv_obj_center(p->disc);
    lv_obj_set_style_radius(p->disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(p->disc, LV_OPA_COVER, 0);

    /* Face layer (never rotates with spin). */
    p->face = lv_obj_create(p->root);
    lv_obj_remove_style_all(p->face);
    lv_obj_set_size(p->face, BODY_SIZE, BODY_SIZE);
    lv_obj_center(p->face);
    lv_obj_clear_flag(p->face, LV_OBJ_FLAG_SCROLLABLE);

    p->eye_l = lv_obj_create(p->face);
    p->eye_r = lv_obj_create(p->face);
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *e = i ? p->eye_r : p->eye_l;
        lv_obj_remove_style_all(e);
        lv_obj_set_size(e, EYE_W, EYE_H_OPEN);
        lv_obj_set_style_radius(e, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(e, lv_color_hex(COL_EYE), 0);
        lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
    }
    /* eye position/size is per-kind — set in apply_kind() */

    p->mouth = lv_obj_create(p->face);
    lv_obj_remove_style_all(p->mouth);
    lv_obj_set_size(p->mouth, 14, 3);
    lv_obj_set_style_radius(p->mouth, 2, 0);
    lv_obj_set_style_bg_color(p->mouth, lv_color_hex(COL_EYE), 0);
    lv_obj_set_style_bg_opa(p->mouth, LV_OPA_COVER, 0);
    lv_obj_align(p->mouth, LV_ALIGN_CENTER, 0, 14);

    p->blink = lv_timer_create(blink_cb, 3300, p);
    lv_timer_pause(p->blink);
}

/* ── kind styling ────────────────────────────────────────────────── */

static pet_kind_t kind_of(const char *kind)
{
    if (kind && strcmp(kind, "claude-code") == 0) return KIND_CLAUDE;
    if (kind && strcmp(kind, "codex") == 0)       return KIND_CODEX;
    return KIND_GENERIC;
}

static void apply_kind(pet_t *p, pet_kind_t k, uint32_t accent)
{
    /* Classic Clawd salmon = the kind accent lifted toward white (rust
     * lands on the brand coral; mono theme stays mono). */
    lv_color_t clawd_col = lv_color_mix(lv_color_white(), lv_color_hex(accent), 80);
    uint32_t n = lv_obj_get_child_count(p->clawd);
    for (uint32_t i = 0; i < n; ++i)
        lv_obj_set_style_bg_color(lv_obj_get_child(p->clawd, i), clawd_col, 0);
    if (k == KIND_CLAUDE) lv_obj_clear_flag(p->clawd, LV_OBJ_FLAG_HIDDEN);
    else                  lv_obj_add_flag(p->clawd, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_style_border_color(p->screen, lv_color_hex(accent), 0);
    if (k == KIND_CODEX) lv_obj_clear_flag(p->screen, LV_OBJ_FLAG_HIDDEN);
    else                 lv_obj_add_flag(p->screen, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_style_bg_color(p->disc, lv_color_hex(accent), 0);
    if (k == KIND_GENERIC) lv_obj_clear_flag(p->disc, LV_OBJ_FLAG_HIDDEN);
    else                   lv_obj_add_flag(p->disc, LV_OBJ_FLAG_HIDDEN);

    /* Codex eyes are accent-on-dark terminal cursors; Claude eyes are
     * square pixel punch-outs; generic eyes round punch-outs. */
    uint32_t eye_col = (k == KIND_CODEX) ? accent : COL_EYE;
    int eye_w = (k == KIND_CODEX) ? CODEX_EYE_W
              : (k == KIND_CLAUDE) ? CLAWD_EYE_W : EYE_W;
    int eye_r = (k == KIND_CODEX) ? 2
              : (k == KIND_CLAUDE) ? 0 : LV_RADIUS_CIRCLE;
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *e = i ? p->eye_r : p->eye_l;
        lv_obj_set_style_bg_color(e, lv_color_hex(eye_col), 0);
        lv_obj_set_width(e, eye_w);
        lv_obj_set_style_radius(e, eye_r, 0);
    }
    /* Clawd's small eyes sit high and wide on the body (centers at
     * ±9u per the spritesheet); other kinds keep the compact face. */
    int eye_dx = (k == KIND_CLAUDE) ? CLAWD_U * 6 : 11;
    int eye_dy = (k == KIND_CLAUDE) ? -(CLAWD_U * 6) : -2;
    lv_obj_align(p->eye_l, LV_ALIGN_CENTER, -eye_dx, eye_dy);
    lv_obj_align(p->eye_r, LV_ALIGN_CENTER,  eye_dx, eye_dy);
    lv_obj_set_style_bg_color(p->mouth, lv_color_hex(eye_col), 0);
    if (k == KIND_CODEX) lv_obj_clear_flag(p->mouth, LV_OBJ_FLAG_HIDDEN);
    else                 lv_obj_add_flag(p->mouth, LV_OBJ_FLAG_HIDDEN);
}

/* ── mood animation sets ─────────────────────────────────────────── */

static void reset_transforms(pet_t *p)
{
    lv_anim_delete(p->root, NULL);
    lv_anim_delete(p->spin, NULL);
    lv_anim_delete(p->face, NULL);
    lv_anim_delete(p->eye_l, NULL);
    lv_anim_delete(p->eye_r, NULL);
    for (int i = 0; i < 4; ++i) {
        lv_anim_delete(p->legs[i], NULL);
        lv_obj_set_y(p->legs[i], CLAWD_LEG_Y);
    }
    lv_obj_set_style_transform_rotation(p->root, 0, 0);
    lv_obj_set_style_transform_scale(p->root, 256, 0);
    lv_obj_set_style_transform_rotation(p->spin, 0, 0);
    lv_obj_set_y(p->root, 0);
    lv_obj_set_x(p->face, 0);
    lv_obj_center(p->spin);
    lv_obj_center(p->face);
}

static void apply_mood(pet_t *p)
{
    reset_transforms(p);
    switch (p->mood) {
        case PET_MOOD_WORKING:
            eyes_set(p, true);
            /* busy bounce */
            anim_loop(p->root, a_y, 0, -6, 650, true);
            if (p->kind == KIND_CLAUDE) {
                /* legs scuttle in alternating pairs, like it's pacing
                 * in place while it works */
                for (int i = 0; i < 4; ++i) {
                    bool lifted = i & 1;   /* odd legs start mid-step */
                    anim_loop(p->legs[i], a_y,
                              lifted ? CLAWD_LEG_Y - CLAWD_LEG_LIFT : CLAWD_LEG_Y,
                              lifted ? CLAWD_LEG_Y : CLAWD_LEG_Y - CLAWD_LEG_LIFT,
                              280, true);
                }
            } else if (p->kind == KIND_CODEX) {
                /* eyes scan left-right like it's reading a diff */
                anim_loop(p->face, a_x, -4, 4, 1100, true);
            }
            lv_timer_resume(p->blink);
            break;
        case PET_MOOD_WAITING:
            eyes_set(p, true);
            /* expectant sway (±7°) */
            anim_loop(p->root, a_rot, -70, 70, 800, true);
            lv_timer_resume(p->blink);
            break;
        case PET_MOOD_IDLE:
        default:
            eyes_set(p, false);          /* asleep */
            lv_timer_pause(p->blink);
            /* slow breathing */
            anim_loop(p->root, a_scale, 256, 240, 2400, true);
            break;
    }
}

/* ── public API ──────────────────────────────────────────────────── */

pet_t *pet_create(lv_obj_t *parent)
{
    pet_t *p = lv_malloc_zeroed(sizeof(pet_t));
    build(p, parent);
    p->kind = KIND_GENERIC;
    p->mood = PET_MOOD_IDLE;
    apply_kind(p, p->kind, theme_accent_for_kind(NULL));
    apply_mood(p);
    p->inited = true;
    return p;
}

lv_obj_t *pet_obj(pet_t *p) { return p ? p->root : NULL; }

void pet_set(pet_t *p, const char *kind, pet_mood_t mood)
{
    if (!p) return;
    pet_kind_t k = kind_of(kind);
    if (p->inited && k == p->kind && mood == p->mood) return;
    bool kind_changed = (k != p->kind);
    p->kind = k;
    p->mood = mood;
    if (kind_changed) apply_kind(p, k, theme_accent_for_kind(kind));
    apply_mood(p);
}
