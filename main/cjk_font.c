#include "cjk_font.h"
#include "esp_log.h"

#include <stddef.h>   /* NULL — the tiny_ttf-less fallback returns it */

void cjk_utf8_lcpy(char *dst, const char *src, unsigned cap)
{
    if (!dst || cap == 0) return;
    unsigned out = 0;
    if (src) {
        while (src[out]) {
            unsigned char c = (unsigned char)src[out];
            unsigned len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
            if (out + len > cap - 1) break;      /* whole char wouldn't fit */
            for (unsigned i = 0; i < len; ++i) {
                if (!src[out + i]) { dst[out] = '\0'; return; }  /* input cut short */
                dst[out + i] = src[out + i];
            }
            out += len;
        }
    }
    dst[out] = '\0';
}

#if LV_USE_TINY_TTF

/* Embedded SimHei GB2312 subset (main/zh.ttf via EMBED_FILES). IDF names the
 * symbols after the file: zh.ttf → _binary_zh_ttf_{start,end}. */
extern const uint8_t zh_ttf_start[] asm("_binary_zh_ttf_start");
extern const uint8_t zh_ttf_end[]   asm("_binary_zh_ttf_end");

/* tiny_ttf fonts are per-size; cache every size the scenes/overlays ask for.
 * Sizes in play: status-bar 18, banner 16/14, awaiting 22/20, prompt 22/14, …
 * Keep the cap comfortably above the distinct-size count so the per-event
 * push banner never misses the cache and leaks a font per notification. */
#define CJK_CACHE_MAX 12
static lv_font_t *s_font[CJK_CACHE_MAX];
static int        s_px[CJK_CACHE_MAX];

const lv_font_t *cjk_font(int px)
{
    if (px <= 0) return NULL;
    for (int i = 0; i < CJK_CACHE_MAX; ++i)
        if (s_font[i] && s_px[i] == px) return s_font[i];

    size_t sz = (size_t)(zh_ttf_end - zh_ttf_start);
    lv_font_t *f = lv_tiny_ttf_create_data((const void *)zh_ttf_start, sz, px);
    if (!f) {
        ESP_LOGE("cjk", "tiny_ttf create FAILED px=%d sz=%u ptr=%p b0=%02x b1=%02x",
                 px, (unsigned)sz, (const void *)zh_ttf_start,
                 zh_ttf_start[0], zh_ttf_start[1]);
        return NULL;
    }
    for (int i = 0; i < CJK_CACHE_MAX; ++i)
        if (!s_font[i]) { s_font[i] = f; s_px[i] = px; return f; }
    return f;   /* cache full (won't happen with so few sizes) — leak-free enough */
}

/* UI fonts: Consolas subsets (see header). Same pipeline; each font
 * instance gets the same-size CJK font wired as its LVGL fallback so
 * mixed Latin/Chinese labels render both. */
extern const uint8_t ui_ttf_start[]      asm("_binary_ui_ttf_start");
extern const uint8_t ui_ttf_end[]        asm("_binary_ui_ttf_end");
extern const uint8_t ui_bold_ttf_start[] asm("_binary_ui_bold_ttf_start");
extern const uint8_t ui_bold_ttf_end[]   asm("_binary_ui_bold_ttf_end");

#define UI_CACHE_MAX 12

static const lv_font_t *ui_font_cached(const uint8_t *data, size_t sz,
                                       lv_font_t **cache, int *cache_px,
                                       int px)
{
    if (px <= 0) return NULL;
    for (int i = 0; i < UI_CACHE_MAX; ++i)
        if (cache[i] && cache_px[i] == px) return cache[i];

    lv_font_t *f = lv_tiny_ttf_create_data((const void *)data, sz, px);
    if (!f) {
        ESP_LOGE("cjk", "ui tiny_ttf create FAILED px=%d", px);
        return NULL;
    }
    /* CJK fallback: anything Consolas lacks (hanzi) falls through to the
     * SimHei subset at the same size. */
    f->fallback = cjk_font(px);
    for (int i = 0; i < UI_CACHE_MAX; ++i)
        if (!cache[i]) { cache[i] = f; cache_px[i] = px; return f; }
    return f;
}

const lv_font_t *ui_font(int px)
{
    static lv_font_t *s_cache[UI_CACHE_MAX];
    static int        s_px[UI_CACHE_MAX];
    return ui_font_cached(ui_ttf_start,
                          (size_t)(ui_ttf_end - ui_ttf_start),
                          s_cache, s_px, px);
}

const lv_font_t *ui_font_bold(int px)
{
    static lv_font_t *s_cache[UI_CACHE_MAX];
    static int        s_px[UI_CACHE_MAX];
    return ui_font_cached(ui_bold_ttf_start,
                          (size_t)(ui_bold_ttf_end - ui_bold_ttf_start),
                          s_cache, s_px, px);
}

/* Clock digits: rounded subset for scene_clock (see header). Same
 * tiny_ttf pipeline, its own tiny cache — the face wants one size but a
 * follow-up (e.g. seconds readout) may add another. */
extern const uint8_t clock_ttf_start[] asm("_binary_clock_digits_ttf_start");
extern const uint8_t clock_ttf_end[]   asm("_binary_clock_digits_ttf_end");

#define CLOCK_CACHE_MAX 4
static lv_font_t *s_clock_font[CLOCK_CACHE_MAX];
static int        s_clock_px[CLOCK_CACHE_MAX];

const lv_font_t *clock_font(int px)
{
    if (px <= 0) return NULL;
    for (int i = 0; i < CLOCK_CACHE_MAX; ++i)
        if (s_clock_font[i] && s_clock_px[i] == px) return s_clock_font[i];

    size_t sz = (size_t)(clock_ttf_end - clock_ttf_start);
    lv_font_t *f = lv_tiny_ttf_create_data((const void *)clock_ttf_start, sz, px);
    if (!f) {
        ESP_LOGE("cjk", "clock tiny_ttf create FAILED px=%d sz=%u",
                 px, (unsigned)sz);
        return NULL;
    }
    for (int i = 0; i < CLOCK_CACHE_MAX; ++i)
        if (!s_clock_font[i]) { s_clock_font[i] = f; s_clock_px[i] = px; return f; }
    return f;
}

#else  /* tiny_ttf not built — callers fall back to the Latin font */

const lv_font_t *cjk_font(int px) { (void)px; return NULL; }
const lv_font_t *clock_font(int px) { (void)px; return NULL; }
const lv_font_t *ui_font(int px) { (void)px; return NULL; }
const lv_font_t *ui_font_bold(int px) { (void)px; return NULL; }

#endif
