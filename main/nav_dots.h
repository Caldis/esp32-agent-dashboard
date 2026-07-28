#pragma once

/*
 * nav_dots — 顶部三个按键指示点 (v6.6)。
 *
 * 设备有三个物理键，正好对应三个界面（BOOT=总览 / USER=天气 / PWR=时间）。
 * 三个点按键的物理顺序从左到右排在屏幕顶部，当前所在界面那一点高亮。
 * 它回答两个问题：现在在哪、以及"还有几个界面、分别归哪个键管"。
 *
 * 放在 lv_layer_top()：跨场景、且必须画在场景内容之上。挂在场景根上就要
 * 复制三份，还会被场景切换连带隐藏。
 *
 * 更新时机是场景变化（scene_fw_set_change_listener），不是每帧——三个
 * 小圆点每帧重绘毫无意义，而每次重绘都要连带重画它们下面那块。
 */

/* app_main 里创建一次（需要 LVGL 已初始化，且在场景注册之后调用一次
 * nav_dots_set_scene 给初值）。 */
void nav_dots_init(void);

/* 高亮 scene_id 对应的那一点；未知 id 则全部置暗。 */
void nav_dots_set_scene(const char *scene_id);
