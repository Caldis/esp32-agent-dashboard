#pragma once
/* 最小 scene 替身:数据层只用 id-based 查找/切换/读当前 id。
 * 真实 scene_t 含 LVGL 字段;此替身仅保留 id。 */
typedef struct scene { const char *id; } scene_t;
int            scene_fw_find_by_id(const char *id);
void           scene_fw_show(int idx);
const scene_t *scene_fw_current(void);
