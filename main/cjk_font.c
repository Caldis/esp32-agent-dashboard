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

#else  /* tiny_ttf not built — callers fall back to the Latin font */

const lv_font_t *cjk_font(int px) { (void)px; return NULL; }

#endif
