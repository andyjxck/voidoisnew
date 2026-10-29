/* VoidOS visual language, shared by the face and every app screen:
 * neon outlines on black — chamfered "pill" cards, pointy-top hexes,
 * cyan (0x46D8E0) for the main accent and red (0xF0586E) for the hot
 * one — plus the line-art icons the music screen needs.
 * Header-only (static) so the host renderer (tests/uisnap) and the
 * firmware build it the same way. */
#ifndef VOIDOS_UI_DECO_H
#define VOIDOS_UI_DECO_H
#include "lvgl.h"
#include "micons.h"

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

/* host renderer (tests/uisnap) records every card, hex and bar so its
   audit can fail text that sits half on a shape's outline */
#ifdef UISNAP_SHAPES
void uisnap_shape(int kind, int a, int b, int c, int d, int e);
#define DK_SHAPE(k, a, b, c, d, e) uisnap_shape(k, a, b, c, d, e)
#else
#define DK_SHAPE(k, a, b, c, d, e) ((void)0)
#endif

static inline void dk_rect_raw(lv_layer_t *l, int x1, int y1, int x2, int y2,
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

static inline void dk_rect(lv_layer_t *l, int x1, int y1, int x2, int y2,
                           lv_color_t c, lv_opa_t o)
{
    DK_SHAPE(3, x1, y1, x2, y2, 0);
    dk_rect_raw(l, x1, y1, x2, y2, c, o);
}

/* chamfered card: optional tinted fill + neon stroke */
static inline void dk_oct(lv_layer_t *l, int x, int y, int w, int h, int cut,
                          lv_color_t c, int lw, lv_opa_t fill)
{
    int x2 = x + w - 1, y2 = y + h - 1;
    DK_SHAPE(1, x, y, w, h, cut);
    if (fill) {
        dk_rect_raw(l, x + cut, y, x2 - cut, y2, c, fill);
        dk_rect_raw(l, x, y + cut, x + cut - 1, y2 - cut, c, fill);
        dk_rect_raw(l, x2 - cut + 1, y + cut, x2, y2 - cut, c, fill);
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
    DK_SHAPE(2, cx, cy, r, 0, 0);
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


/* ---- music glyphs: Google Material Symbols (micons.h) ------------------
   s is the old half-size; a Material icon's art spans 20/24 of its box */
enum { DK_M_PLAY, DK_M_PAUSE, DK_M_NEXT, DK_M_PREV, DK_M_VOLUP, DK_M_VOLDN,
       DK_M_NOTE };

static inline void dk_music(lv_layer_t *l, int k, int cx, int cy, int s,
                            lv_color_t c)
{
    static const unsigned char ico[] = {
        [DK_M_PLAY] = MI_PLAY, [DK_M_PAUSE] = MI_PAUSE,
        [DK_M_NEXT] = MI_SKIP_NEXT, [DK_M_PREV] = MI_SKIP_PREV,
        [DK_M_VOLUP] = MI_VOLUME_UP, [DK_M_VOLDN] = MI_VOLUME_DOWN,
        [DK_M_NOTE] = MI_MUSIC_NOTE,
    };
    if (k < 0 || k > DK_M_NOTE)
        return;
    mi_draw(l, ico[k], cx, cy, (s * 12 + 2) / 5, c);
}

#endif
