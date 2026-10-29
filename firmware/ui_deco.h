/* VoidOS visual language, shared by the face and every app screen:
 * neon outlines on black — chamfered "pill" cards, pointy-top hexes,
 * cyan (0x46D8E0) for the main accent and red (0xF0586E) for the hot
 * one — plus the line-art icons the new weather/music screens need.
 * Header-only (static) so the host renderer (tests/uisnap) and the
 * firmware build it the same way. */
#ifndef VOIDOS_UI_DECO_H
#define VOIDOS_UI_DECO_H
#include "lvgl.h"

#define DK_CYAN  lv_color_hex(0x46D8E0)
#define DK_RED   lv_color_hex(0xF0586E)
#define DK_DIM   lv_color_hex(0x1E4E56)    /* cyan at ~35 % */
#define DK_DIMR  lv_color_hex(0x5A2430)    /* red at ~35 % */
#define DK_GREY  lv_color_hex(0x8C96A8)
#define DK_AMBER lv_color_hex(0xF0B43C)

static inline void dk_line(lv_layer_t *l, int x1, int y1, int x2, int y2,
                           lv_color_t c, int w)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = w;
    d.round_start = d.round_end = 1;
    d.p1.x = x1; d.p1.y = y1; d.p2.x = x2; d.p2.y = y2;
    lv_draw_line(l, &d);
}

static inline void dk_tri(lv_layer_t *l, int x0, int y0, int x1, int y1,
                          int x2, int y2, lv_color_t c, lv_opa_t o)
{
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = c;
    t.opa = o;
    t.p[0].x = x0; t.p[0].y = y0;
    t.p[1].x = x1; t.p[1].y = y1;
    t.p[2].x = x2; t.p[2].y = y2;
    lv_draw_triangle(l, &t);
}

static inline void dk_rect(lv_layer_t *l, int x1, int y1, int x2, int y2,
                           lv_color_t c, lv_opa_t o)
{
    if (x2 < x1 || y2 < y1)
        return;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = c;
    r.bg_opa = o;
    lv_area_t a = {x1, y1, x2, y2};
    lv_draw_rect(l, &r, &a);
}

/* chamfered card: optional tinted fill + neon stroke */
static inline void dk_oct(lv_layer_t *l, int x, int y, int w, int h, int cut,
                          lv_color_t c, int lw, lv_opa_t fill)
{
    int x2 = x + w - 1, y2 = y + h - 1;
    if (fill) {
        dk_rect(l, x + cut, y, x2 - cut, y2, c, fill);
        dk_rect(l, x, y + cut, x + cut - 1, y2 - cut, c, fill);
        dk_rect(l, x2 - cut + 1, y + cut, x2, y2 - cut, c, fill);
        dk_tri(l, x + cut, y, x + cut, y + cut, x, y + cut, c, fill);
        dk_tri(l, x2 - cut, y, x2, y + cut, x2 - cut, y + cut, c, fill);
        dk_tri(l, x, y2 - cut, x + cut, y2 - cut, x + cut, y2, c, fill);
        dk_tri(l, x2 - cut, y2 - cut, x2, y2 - cut, x2 - cut, y2, c, fill);
    }
    const int px[8] = {x + cut, x2 - cut, x2, x2, x2 - cut, x + cut, x, x};
    const int py[8] = {y, y, y + cut, y2 - cut, y2, y2, y2 - cut, y + cut};
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = lw;
    for (int i = 0; i < 8; i++) {
        d.p1.x = px[i]; d.p1.y = py[i];
        d.p2.x = px[(i + 1) % 8]; d.p2.y = py[(i + 1) % 8];
        lv_draw_line(l, &d);
    }
}

/* pointy-top hexagon outline, r = centre to vertex */
static inline void dk_hex(lv_layer_t *l, int cx, int cy, int r, lv_color_t c,
                          int lw)
{
    int hw = r * 866 / 1000;
    const int px[6] = {cx, cx + hw, cx + hw, cx, cx - hw, cx - hw};
    const int py[6] = {cy - r, cy - r / 2, cy + r / 2, cy + r, cy + r / 2,
                       cy - r / 2};
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = lw;
    for (int i = 0; i < 6; i++) {
        d.p1.x = px[i]; d.p1.y = py[i];
        d.p2.x = px[(i + 1) % 6]; d.p2.y = py[(i + 1) % 6];
        lv_draw_line(l, &d);
    }
}

static inline void dk_arc(lv_layer_t *l, int cx, int cy, int r, int w,
                          int a0, int a1, lv_color_t c)
{
    lv_draw_arc_dsc_t a;
    lv_draw_arc_dsc_init(&a);
    a.center.x = cx;
    a.center.y = cy;
    a.radius = r;
    a.width = w;
    a.color = c;
    a.start_angle = a0;
    a.end_angle = a1;
    a.rounded = 1;
    lv_draw_arc(l, &a);
}

/* hex-tick header rule: —⬡— under a screen title */
static inline void dk_rule(lv_layer_t *l, int y, lv_color_t c)
{
    dk_line(l, 20, y, 104, y, DK_DIM, 1);
    dk_line(l, 136, y, 220, y, DK_DIM, 1);
    dk_hex(l, 120, y, 6, c, 2);
}

/* degree ring for temperatures (the pixel font has no °) */
static inline void dk_deg(lv_layer_t *l, int x, int y, int r, lv_color_t c)
{
    dk_arc(l, x + r, y + r, r, 2, 0, 360, c);
}

/* ---- weather glyphs: line art inside a (2s x 2s) box ---------------- */
static inline void dk_cloud(lv_layer_t *l, int cx, int cy, int s,
                            lv_color_t c, int lw)
{
    dk_arc(l, cx - s / 3, cy, s / 3, lw, 90, 270, c);
    dk_arc(l, cx - s / 8, cy - s / 4, s / 3, lw, 180, 330, c);
    dk_arc(l, cx + s / 3, cy - s / 8, s * 3 / 8, lw, 240, 90, c);
    dk_line(l, cx - s / 3, cy + s / 3, cx + s / 3, cy + s / 4 + s / 8 - 1, c, lw);
}

static inline void dk_sun(lv_layer_t *l, int cx, int cy, int s, lv_color_t c,
                          int lw)
{
    int r = s * 4 / 10;
    dk_arc(l, cx, cy, r, lw, 0, 360, c);
    static const int dx[8] = {1000, 707, 0, -707, -1000, -707, 0, 707};
    static const int dy[8] = {0, 707, 1000, 707, 0, -707, -1000, -707};
    for (int i = 0; i < 8; i++)
        dk_line(l, cx + dx[i] * (r + s / 6) / 1000, cy + dy[i] * (r + s / 6) / 1000,
                cx + dx[i] * (s - 1) / 1000, cy + dy[i] * (s - 1) / 1000, c, lw);
}

static inline void dk_wx_icon(lv_layer_t *l, int icon, int cx, int cy, int s,
                              lv_color_t c)
{
    int lw = s >= 20 ? 3 : 2;
    lv_color_t sun = DK_AMBER;
    switch (icon) {
    case 0:                                   /* clear */
        dk_sun(l, cx, cy, s, sun, lw);
        break;
    case 1:                                   /* few clouds */
        dk_sun(l, cx - s / 3, cy - s / 3, s * 2 / 3, sun, lw > 2 ? 2 : lw);
        dk_cloud(l, cx + s / 6, cy + s / 5, s * 3 / 4, c, lw);
        break;
    case 2:                                   /* clouds */
        dk_cloud(l, cx, cy, s, c, lw);
        break;
    case 3:                                   /* heavy clouds */
        dk_cloud(l, cx - s / 5, cy - s / 4, s * 2 / 3, DK_GREY, lw);
        dk_cloud(l, cx + s / 6, cy + s / 6, s * 3 / 4, c, lw);
        break;
    case 4:                                   /* showers */
    case 5:                                   /* rain */
        dk_cloud(l, cx, cy - s / 4, s * 3 / 4, c, lw);
        for (int i = -1; i <= 1; i++) {
            if (icon == 4 && i == 0)
                continue;
            int x = cx + i * s / 3;
            dk_line(l, x, cy + s / 3, x - s / 8, cy + s - 2, DK_CYAN, lw);
        }
        break;
    case 6:                                   /* thunderstorm */
        dk_cloud(l, cx, cy - s / 4, s * 3 / 4, c, lw);
        dk_line(l, cx + s / 8, cy + s / 4, cx - s / 6, cy + s * 3 / 5, sun, lw);
        dk_line(l, cx - s / 6, cy + s * 3 / 5, cx + s / 8, cy + s * 3 / 5, sun, lw);
        dk_line(l, cx + s / 8, cy + s * 3 / 5, cx - s / 8, cy + s - 1, sun, lw);
        break;
    case 7:                                   /* snow */
        dk_cloud(l, cx, cy - s / 4, s * 3 / 4, c, lw);
        for (int i = -1; i <= 1; i++) {
            int x = cx + i * s / 3, y = cy + s * 2 / 3;
            int r = s / 7 + 1;
            dk_line(l, x - r, y, x + r, y, lv_color_white(), 2);
            dk_line(l, x, y - r, x, y + r, lv_color_white(), 2);
        }
        break;
    case 8:                                   /* mist */
        for (int i = 0; i < 4; i++) {
            int y = cy - s * 3 / 5 + i * s * 2 / 5;
            int o = (i & 1) ? s / 4 : 0;
            dk_line(l, cx - s + o + 2, y, cx + s - (s / 4 - o) - 2, y, c, lw);
        }
        break;
    default:                                  /* no data: dashed hex */
        dk_hex(l, cx, cy, s, DK_DIM, 2);
        dk_line(l, cx - s / 3, cy, cx + s / 3, cy, DK_GREY, 2);
        break;
    }
}

/* ---- music glyphs ----------------------------------------------------- */
enum { DK_M_PLAY, DK_M_PAUSE, DK_M_NEXT, DK_M_PREV, DK_M_VOLUP, DK_M_VOLDN,
       DK_M_NOTE };

static inline void dk_music(lv_layer_t *l, int k, int cx, int cy, int s,
                            lv_color_t c)
{
    switch (k) {
    case DK_M_PLAY:
        dk_tri(l, cx - s * 2 / 3, cy - s, cx - s * 2 / 3, cy + s, cx + s, cy,
               c, LV_OPA_COVER);
        break;
    case DK_M_PAUSE:
        dk_rect(l, cx - s * 3 / 4, cy - s, cx - s / 4, cy + s, c, LV_OPA_COVER);
        dk_rect(l, cx + s / 4, cy - s, cx + s * 3 / 4, cy + s, c, LV_OPA_COVER);
        break;
    case DK_M_NEXT:
    case DK_M_PREV: {
        int d = k == DK_M_NEXT ? 1 : -1;
        dk_tri(l, cx - d * s, cy - s * 3 / 4, cx - d * s, cy + s * 3 / 4,
               cx, cy, c, LV_OPA_COVER);
        dk_tri(l, cx, cy - s * 3 / 4, cx, cy + s * 3 / 4, cx + d * s * 3 / 4,
               cy, c, LV_OPA_COVER);
        int bx = cx + d * s * 3 / 4;
        dk_rect(l, d > 0 ? bx : bx - 2, cy - s * 3 / 4, d > 0 ? bx + 2 : bx,
                cy + s * 3 / 4, c, LV_OPA_COVER);
        break;
    }
    case DK_M_VOLUP:
    case DK_M_VOLDN:
        dk_rect(l, cx - s, cy - s / 3, cx - s / 2, cy + s / 3, c, LV_OPA_COVER);
        dk_tri(l, cx - s / 2, cy - s / 3, cx - s / 2, cy + s / 3, cx, cy - s * 3 / 4,
               c, LV_OPA_COVER);
        dk_tri(l, cx - s / 2, cy + s / 3, cx, cy + s * 3 / 4, cx, cy - s * 3 / 4,
               c, LV_OPA_COVER);
        dk_line(l, cx + s / 3, cy, cx + s, cy, c, 2);
        if (k == DK_M_VOLUP)
            dk_line(l, cx + s * 2 / 3, cy - s / 3, cx + s * 2 / 3, cy + s / 3, c, 2);
        break;
    case DK_M_NOTE:
        dk_arc(l, cx - s / 3, cy + s / 2, s / 3, s / 3 + 1, 0, 360, c);
        dk_line(l, cx, cy + s / 2, cx, cy - s, c, 2);
        dk_line(l, cx, cy - s, cx + s * 2 / 3, cy - s * 2 / 3, c, 2);
        break;
    }
}

#endif
