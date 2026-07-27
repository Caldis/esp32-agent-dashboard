#include "ui_type.h"
#include "cjk_font.h"

/* The only place in the app that touches raw pixel sizes. Keep the set
 * small: every distinct size costs a tiny_ttf instance in the ui /
 * ui_bold / cjk caches (12 slots each — 5 tiers fits three times over). */
static const int s_tier_px[UI_T_COUNT] = { 20, 26, 36, 52, 88 };

/* Built-in Montserrat fallbacks (only reachable when tiny_ttf is out of
 * the build). Limited to sizes the sdkconfig already enables. */
static const lv_font_t *tier_fallback(ui_tier_t t)
{
    switch (t) {
        case UI_T_CAPTION: return &lv_font_montserrat_20;
        case UI_T_LABEL:   return &lv_font_montserrat_22;
        case UI_T_BODY:    return &lv_font_montserrat_36;
        case UI_T_TITLE:   return &lv_font_montserrat_48;
        case UI_T_HERO:    return &lv_font_montserrat_48;
        default:           return &lv_font_montserrat_20;
    }
}

static ui_tier_t clamp_tier(ui_tier_t t)
{
    return (t >= UI_T_COUNT) ? UI_T_CAPTION : t;
}

const lv_font_t *ui_type(ui_tier_t t)
{
    t = clamp_tier(t);
    const lv_font_t *f = ui_font(s_tier_px[t]);
    return f ? f : tier_fallback(t);
}

const lv_font_t *ui_type_bold(ui_tier_t t)
{
    t = clamp_tier(t);
    const lv_font_t *f = ui_font_bold(s_tier_px[t]);
    return f ? f : tier_fallback(t);
}

int ui_type_px(ui_tier_t t)
{
    return s_tier_px[clamp_tier(t)];
}

int ui_type_line(ui_tier_t t)
{
    /* 1.2 em rounded up. Must stay >= the ACTUAL line box cjk_font builds
     * (LINE_ABOVE_PERMIL + LINE_BELOW_PERMIL, see cjk_font.h) or stacked
     * layout math starts clipping glyphs. */
    return (s_tier_px[clamp_tier(t)] * 12 + 9) / 10;
}

/* The two are tuned independently in different files and the smallest tier
 * has ZERO slack (CAPTION 20: box 24 px, budget 24 px), so a one-permil
 * nudge to either side would silently start clipping the smallest text on
 * the device — the exact failure mode the v6.3 font fix was chasing, and
 * one that a 320 px screenshot cannot show. Fail the build instead. */
#define UI_TYPE_LINE_OF(px)   (((px) * 12 + 9) / 10)
#define UI_TYPE_BOX_OF(px)    (CJK_LINE_ABOVE_OF(px) + CJK_LINE_BELOW_OF(px))
#define UI_TYPE_ASSERT_TIER(px) \
    _Static_assert(UI_TYPE_LINE_OF(px) >= UI_TYPE_BOX_OF(px), \
        /* ASCII only: gcc escapes non-ASCII bytes in assertion text, and a \
           diagnostic you have to decode is a diagnostic you ignore. */ \
        "ui_type_line(" #px ") is smaller than the cjk_font line box: " \
        "text at this tier will be clipped. Raise the 1.2 em budget in " \
        "ui_type_line() or lower CJK_LINE_ABOVE/BELOW_PERMIL.")
UI_TYPE_ASSERT_TIER(20);
UI_TYPE_ASSERT_TIER(26);
UI_TYPE_ASSERT_TIER(36);
UI_TYPE_ASSERT_TIER(52);
UI_TYPE_ASSERT_TIER(88);
