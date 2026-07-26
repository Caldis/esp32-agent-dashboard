#include "status_bar.h"

#include <stdio.h>
#include <time.h>

#include "lvgl.h"
#include "cjk_font.h"   /* clock_font — the rounded digits face */
#include "ui_type.h"

#define COL_TEXT       0xF3EEE2
#define COL_TEXT_DIM   0x8A807A
#define COL_TEAL       0x2BB3B1
#define COL_WARN       0xE0A030   /* amber — "waiting for host" */
#define COL_DANGER     0xE0503C   /* red   — "host disconnected" */

/* Connection health: snapshots (with the bridge's 10s keepalive) should always
 * be recent. If none has arrived for STALE_MS, the host/bridge link is down —
 * say so on-screen instead of freezing on stale data. 12s = one keepalive
 * period + 2s slack: one missed keepalive means the link IS down (the bridge
 * detects port loss in <1s and reconnect-pushes immediately, so a healthy
 * link never goes this quiet); the old 25s made an unplugged device lie
 * about live agents for close to half a minute. */
#define CONN_STALE_MS  12000u
enum { CONN_OK = 0, CONN_WAITING, CONN_STALE };

/* Geometry (v4.4 type-scale pass): footer numbers moved to BODY(36)
 * and captions to CAPTION(20) — the 28/12 originals measured 9'/4' of
 * visual angle at 0.6 m, below every legibility floor. The columns sit
 * lower and wider apart to make room.
 *
 * v4.5: numbers now align TOP_MID over their caption (a number and its
 * caption share the column's dx, so the number centres over the
 * caption instead of left-hanging off it).
 *
 * v6.1: the column pair is SYMMETRIC about the screen mid (dx = ±77,
 * same 154px separation). The old -92/+62 was v4.5 drift ("don't move
 * the captions") — its midpoint sat at x218, 15px left of centre,
 * which read as the whole footer leaning left (user call), worst in
 * idle where both numbers are a lone "0". */
#define HEADER_Y        56   /* time, top-center */
#define CONN_DOT_D      16   /* connection dot diameter (~1.3 mm @305 ppi) */
#define CONN_DOT_Y      24   /* sits in the band the old text pill used,
                              * clear of the 48 px clock at HEADER_Y */
#define FOOTER_Y       392   /* numbers row (36 px line ends ≈435) */
#define FOOTER_CAP_Y   438   /* captions row (20 px line ends ≈462) */
#define FOOTER_LEFT_DX  (-77)   /* active column centre (x156) */
#define FOOTER_RIGHT_DX  (77)   /* tokens column centre (x310) */

void status_bar_format_time(char *buf, size_t cap, const agent_state_t *st)
{
    if (st->host_epoch_unix > 0) {
        uint32_t now = st->host_epoch_unix
                     + (lv_tick_get() - st->host_clock_received_ms) / 1000;
        int32_t tz_now = (int32_t)now + st->host_tz_offset_seconds;
        time_t tt = (time_t)tz_now;
        struct tm tmv;
        gmtime_r(&tt, &tmv);
        snprintf(buf, cap, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    } else {
        snprintf(buf, cap, "--:--");
    }
}

static void fmt_tokens(char *buf, size_t cap, uint64_t tok)
{
    if (tok < 1000) {
        snprintf(buf, cap, "%u", (unsigned)tok);
    } else if (tok < 100000) {
        snprintf(buf, cap, "%.1fk", (double)tok / 1000.0);
    } else {
        snprintf(buf, cap, "%uk", (unsigned)(tok / 1000));
    }
}

/* dx = horizontal offset from screen mid. TOP_MID alignment persists
 * across lv_label_set_text, so a number stays centred over its caption
 * as its digit count changes (same idiom as the top clock). */
static lv_obj_t *mk(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                    int dx, int y, const char *init)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_text(l, init);
    lv_obj_align(l, LV_ALIGN_TOP_MID, dx, y);
    return l;
}

void status_bar_create(lv_obj_t *parent, status_bar_t *sb)
{
    /* Top clock: the rounded digits font (same family as scene_clock's
     * big face — "the small clock becomes the big clock"). */
    sb->time_lbl = lv_label_create(parent);
    lv_obj_set_style_text_color(sb->time_lbl, lv_color_hex(COL_TEXT), 0);
    { const lv_font_t *tf = clock_font(48);
      lv_obj_set_style_text_font(sb->time_lbl,
                                 tf ? tf : &lv_font_montserrat_48, 0); }
    lv_label_set_text(sb->time_lbl, "--:--");
    lv_obj_align(sb->time_lbl, LV_ALIGN_TOP_MID, 0, HEADER_Y);

    sb->active_num = mk(parent, ui_type_bold(UI_T_BODY),
                        COL_TEAL, FOOTER_LEFT_DX, FOOTER_Y, "0");
    sb->active_cap = mk(parent, ui_type(UI_T_CAPTION),
                        COL_TEXT_DIM, FOOTER_LEFT_DX, FOOTER_CAP_Y, "active");
    sb->token_num  = mk(parent, ui_type_bold(UI_T_BODY),
                        COL_TEXT, FOOTER_RIGHT_DX, FOOTER_Y, "0");
    sb->token_cap  = mk(parent, ui_type(UI_T_CAPTION),
                        COL_TEXT_DIM, FOOTER_RIGHT_DX, FOOTER_CAP_Y, "tokens");

    /* Connection-health dot, top-center above the clock. Deliberately
     * quiet — the link state is secondary info; the CONN_DOT_D disc
     * (≈1.3 mm at 305 ppi) reads as a status LED at desk distance
     * without shouting text at the user. Amber = waiting for host,
     * red = host disconnected. Hidden while healthy. */
    sb->conn_dot = lv_obj_create(parent);
    lv_obj_remove_style_all(sb->conn_dot);
    lv_obj_set_size(sb->conn_dot, CONN_DOT_D, CONN_DOT_D);
    lv_obj_set_style_radius(sb->conn_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(sb->conn_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sb->conn_dot, lv_color_hex(COL_DANGER), 0);
    lv_obj_clear_flag(sb->conn_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(sb->conn_dot, LV_ALIGN_TOP_MID, 0, CONN_DOT_Y);
    lv_obj_add_flag(sb->conn_dot, LV_OBJ_FLAG_HIDDEN);
    sb->conn_state = -1;   /* force first update to apply */
}

void status_bar_update(status_bar_t *sb, const agent_state_t *st)
{
    char buf[16];
    status_bar_format_time(buf, sizeof(buf), st);
    lv_label_set_text(sb->time_lbl, buf);

    char left[16];
    snprintf(left, sizeof(left), "%d", st->running + st->waiting);
    lv_label_set_text(sb->active_num, left);

    fmt_tokens(buf, sizeof(buf), st->tokens_today);
    lv_label_set_text(sb->token_num, buf);

    /* Connection health. Only touch the label when the state changes, so we
     * don't re-invalidate every tick. */
    int conn;
    if (!st->ever_received) {
        conn = CONN_WAITING;
    } else if ((lv_tick_get() - st->last_snapshot_ms) > CONN_STALE_MS) {
        conn = CONN_STALE;
    } else {
        conn = CONN_OK;
    }
    if (conn != sb->conn_state) {
        sb->conn_state = conn;
        if (conn == CONN_OK) {
            lv_obj_add_flag(sb->conn_dot, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_style_bg_color(sb->conn_dot,
                lv_color_hex(conn == CONN_WAITING ? COL_WARN : COL_DANGER), 0);
            lv_obj_clear_flag(sb->conn_dot, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
