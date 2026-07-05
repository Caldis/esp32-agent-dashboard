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
    /* tiny_ttf reports ~1.16-1.2 em line height for these faces; budget
     * 1.2 em rounded up so stacked layout math never clips a glyph. */
    return (s_tier_px[clamp_tier(t)] * 12 + 9) / 10;
}
