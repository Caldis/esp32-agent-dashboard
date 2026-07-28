/*
 * ui_motion — 见 ui_motion.h。
 */

#include "ui_motion.h"
#include "perf_mon.h"

#include "esp_log.h"

static const char *TAG = "ui_motion";

#define REFR_MS_ACTIVE  16      /* 高刷档：~60Hz 上限，实际由渲染器决定 */

static uint32_t s_idle_ms = 66;
static int      s_holders = 0;
static bool     s_batching = false;

static void apply_period(uint32_t ms)
{
    lv_display_t *d = lv_display_get_default();
    if (!d) return;
    lv_timer_t *t = lv_display_get_refr_timer(d);
    if (t) lv_timer_set_period(t, ms);
    /* overrun 的判定阈值要跟着档位走，否则低刷档会被按高刷的预算误判成
     * 满屏掉帧。 */
    perf_mon_set_period(ms);
}

void ui_motion_init(uint32_t idle_period_ms)
{
    if (idle_period_ms >= 8) s_idle_ms = idle_period_ms;
    s_holders = 0;
    apply_period(s_idle_ms);
}

void ui_motion_hold(void)
{
    if (s_holders++ == 0) apply_period(REFR_MS_ACTIVE);
}

void ui_motion_release(void)
{
    if (s_holders == 0) {
        /* 多放一次就会把设备永久锁在高刷上（或更糟，锁在低刷）。宁可
         * 大声报错也不要静默吞掉——这类不配对最难事后追查。 */
        ESP_LOGE(TAG, "release without hold — check for an unbalanced path");
        return;
    }
    if (--s_holders == 0) apply_period(s_idle_ms);
}

int ui_motion_holders(void) { return s_holders; }

void ui_motion_set_idle_period(uint32_t ms)
{
    if (ms < 8) ms = 8;
    s_idle_ms = ms;
    if (s_holders == 0) apply_period(ms);
}

uint32_t ui_motion_get_idle_period(void) { return s_idle_ms; }

/* ── 批量样式写入 ─────────────────────────────────────────────────── */

void ui_motion_batch_begin(void)
{
    if (s_batching) {
        /* 嵌套会让内层的 end 提前恢复刷新，外层剩下的写入又各自失效——
         * 批量化悄悄失效，只表现为"某个动画忽然变慢"。 */
        ESP_LOGE(TAG, "nested batch — not supported");
        return;
    }
    s_batching = true;
    lv_obj_enable_style_refresh(false);
}

void ui_motion_batch_end(lv_obj_t *anchor, const lv_area_t *areas, int n)
{
    if (!s_batching) return;
    lv_obj_enable_style_refresh(true);
    s_batching = false;
    if (!anchor) return;
    if (!areas || n <= 0) { lv_obj_invalidate(anchor); return; }
    for (int i = 0; i < n; ++i) lv_obj_invalidate_area(anchor, &areas[i]);
}
