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

/* ── 行盒规范化 (v6.3) ───────────────────────────────────────────────
 * Bug: 大字号汉字顶部被切掉一条（TITLE 52 上约 6px）。
 *
 * 根因不在中文字体，在 fallback 链的行盒归属：混排标签的行盒由【主字体】
 * 决定，而主字体是 Consolas。lv_tiny_ttf 用 hhea 度量算行盒——
 *   基线以上 = ascent/upem，基线以下 = (lineGap - descent)/upem
 * 实测（直接读字体表，见 git log）：
 *   ui/ui_bold.ttf  upem 2048  asc 1521 desc -527 gap 350
 *                   → 基线以上 0.7427 em，以下 0.4282 em
 *   zh.ttf          upem  256  asc  220 desc  -36 gap  36
 *                   → 汉字墨迹需要基线以上 0.8594 em（yMax=asc，顶满）
 * 0.8594 - 0.7427 = 0.1167 em 的墨被行盒切掉，字号越大切得越多。
 *
 * 修法是【重新分配】而不是加高行盒：Consolas 基线以下有 0.4282 em，而它
 * 自己最低的字形只要 0.3022 em——把富余的挪到上面去。行高从 1.1709 em
 * 变成 1.19 em，仍然在 ui_type_line() 预算的 1.2 em 之内，所以所有堆叠
 * 布局的行距不受影响。副作用：墨迹在行盒内整体下移约 0.117 em——原本那
 * 部分是被切掉看不见的，所以视觉上是"字变完整"而不是"字挪位"。
 *
 * 不动 clock_digits.ttf：它基线以上有 1.075 em、墨迹只要 0.74 em，本来就
 * 宽裕，而大钟的档位变形布局是按它现有度量调出来的。 */
#define LINE_ABOVE_PERMIL  880   /* 基线以上 0.880 em > 汉字需要的 0.8594 */
#define LINE_BELOW_PERMIL  310   /* 基线以下 0.310 em > 拉丁需要的 0.3022 */

static void normalise_line_box(lv_font_t *f, int px)
{
    if (!f) return;
    int32_t below = (px * LINE_BELOW_PERMIL + 500) / 1000;
    int32_t above = (px * LINE_ABOVE_PERMIL + 500) / 1000;
    f->base_line   = below;
    f->line_height = above + below;
}

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
    normalise_line_box(f, px);
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
    normalise_line_box(f, px);
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
 * tiny_ttf pipeline, its own cache. v5.1: the scene-transition "true
 * morph" steps the clock through size RUNGS (48/66/84/102/120/135 for
 * the big face, 26/33/40/48 for weather's corner clock) instead of
 * transform_scale (the retired 12-fps plan A), so the cache must hold
 * every rung — 12 covers both ladders with headroom. Each instance is
 * tiny (12-glyph subset, ~2.4 KB source). */
extern const uint8_t clock_ttf_start[] asm("_binary_clock_digits_ttf_start");
extern const uint8_t clock_ttf_end[]   asm("_binary_clock_digits_ttf_end");

#define CLOCK_CACHE_MAX 12
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
