#include "cjk_font.h"
#include "esp_log.h"

#if LV_USE_TINY_TTF

/* Embedded SimHei GB2312 subset (main/zh.ttf via EMBED_FILES). IDF names the
 * symbols after the file: zh.ttf → _binary_zh_ttf_{start,end}. */
extern const uint8_t zh_ttf_start[] asm("_binary_zh_ttf_start");
extern const uint8_t zh_ttf_end[]   asm("_binary_zh_ttf_end");

/* tiny_ttf fonts are per-size; cache the handful of sizes the scenes ask for. */
#define CJK_CACHE_MAX 6
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

#else  /* tiny_ttf not built — callers fall back to the Latin font */

const lv_font_t *cjk_font(int px) { (void)px; return NULL; }

#endif
