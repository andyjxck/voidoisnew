#include <stdint.h>
#include "ui_deco.h"
#include "phone.h"
#include <math.h>
#include "lvgl.h"
#include "voidos/hal.h"
#include "board.h"
#include "assets.h"

extern const lv_font_t kenney9, kenney12, kenney16, kenney24, kenney56;
int sensors_bpm(void);
int ui_steps(void);
int sensors_ppg(int i, int *out_y100);
int sensors_hrs_ok(void);
int hal_clock_synced(void);
int hal_clock_restored(void);
int ui_face_digital(void);
int phone_take_face_dirty(void);
void hal_clock_date(int *y, int *mo, int *d, int *dow);

/* Analog face: black field bleeding into the bezel, edge ticks,
   three hands with a smooth sub-second second sweep. Small
   complications: brand + steps under 12, date at 3, HR subdial at 6. */
#define CX 120
#define CY 120

static lv_obj_t *dial_obj;
static lv_obj_t *lbl_bpm, *lbl_steps, *lbl_date, *heart_obj, *fp_obj;
static lv_obj_t *d_lbl_time, *d_lbl_date, *d_lbl_steps, *d_lbl_bpm;
static int is_digital;
static uint32_t frame_ms;

/* ---- breathing pulse ------------------------------------------------------
   one tiny opacity anim on the BPM readout: 60% <-> 100% over a ~4 s
   cycle. Only the label's pixels get dirtied each frame, and
   lv_timer_handler doesn't run while the panel sleeps, so it costs
   nothing in the dark. The anim MUST be deleted before the label is
   (rebuild's lv_obj_clean frees it) — hence ui_face_breath_stop(). */
static lv_obj_t *breath_obj;
static void breath_exec(void *o, int32_t v)
{
    lv_obj_set_style_opa(o, (lv_opa_t)v, 0);
}
static void breath_start(lv_obj_t *o)
{
    breath_obj = o;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, breath_exec);
    lv_anim_set_values(&a, LV_OPA_60, LV_OPA_COVER);
    lv_anim_set_duration(&a, 2000);
    lv_anim_set_reverse_duration(&a, 2000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}
void ui_face_breath_stop(void)
{
    if (breath_obj) {
        lv_anim_delete(breath_obj, breath_exec);  /* compares, no deref */
        breath_obj = NULL;
    }
}
/* label caches live at file scope so ui_face_build can reset them —
   otherwise a rebuilt label keeps its placeholder until the value moves */
static int last_bpm = -1, last_steps = -1, last_synced = -1, last_dom = -1;
static int last_sec = -1, last_min = -1;
static lv_area_t prev_hand;

static void draw_line(lv_layer_t *layer, int x0, int y0, int x1, int y1,
                      lv_color_t color, int width, int round_end)
{
    const lv_area_t *clip = &layer->_clip_area;
    if ((x0 < clip->x1 - width && x1 < clip->x1 - width) ||
        (x0 > clip->x2 + width && x1 > clip->x2 + width) ||
        (y0 < clip->y1 - width && y1 < clip->y1 - width) ||
        (y0 > clip->y2 + width && y1 > clip->y2 + width))
        return;
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.p1.x = x0;
    dsc.p1.y = y0;
    dsc.p2.x = x1;
    dsc.p2.y = y1;
    dsc.color = color;
    dsc.width = width;
    dsc.round_end = round_end;
    lv_draw_line(layer, &dsc);
}

/* tapered hand: filled triangle pivot->tip plus a short tail */
static void hand(lv_layer_t *layer, float deg, int len, int tail,
                 int base_w, uint32_t color)
{
    float a = (deg - 90.0f) * 0.0174533f;
    float dx = cosf(a), dy = sinf(a);
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = lv_color_hex(color);
    dsc.p[0].x = CX + (int)(dx * len);
    dsc.p[0].y = CY + (int)(dy * len);
    dsc.p[1].x = CX - (int)(dy * base_w);
    dsc.p[1].y = CY + (int)(dx * base_w);
    dsc.p[2].x = CX + (int)(dy * base_w);
    dsc.p[2].y = CY - (int)(dx * base_w);
    lv_draw_triangle(layer, &dsc);
    if (tail)
        draw_line(layer, CX, CY, CX - (int)(dx * tail),
                  CY - (int)(dy * tail), lv_color_hex(color), 4, 1);
}

static void dial_draw(lv_event_t *ev)
{
    lv_layer_t *layer = lv_event_get_layer(ev);
    lv_draw_fill_dsc_t fill;
    lv_draw_fill_dsc_init(&fill);

    /* VoidOS signature: the seconds arc repaints the rim in a new
       colour every lap, overwriting the previous lap's colour */
    static const uint32_t lap_cols[6] = {
        0x5AC8D2, 0x3CC850, 0xEBC828, 0xE66C9B, 0x9B6CE6, 0xE68C3C
    };
    int s = hal_clock_second();
    int ms = hal_clock_millis();
    int m_now = hal_clock_minute() + hal_clock_hour() * 60;
    float sweep = (s + ms / 1000.0f) * 6.0f;   /* degrees, 0 = 12 */
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.center.x = CX;
    arc.center.y = CY;
    arc.radius = 114;
    arc.width = 3;
    arc.rounded = 1;
    /* previous lap's colour, full ring, underneath */
    arc.color = lv_color_hex(lap_cols[(m_now + 5) % 6]);
    arc.start_angle = -90;
    arc.end_angle = 270;
    arc.rounded = 0;
    lv_draw_arc(layer, &arc);
    /* current lap paints over it as it sweeps */
    arc.color = lv_color_hex(lap_cols[m_now % 6]);
    arc.start_angle = -90;
    arc.end_angle = -90 + (int32_t)sweep;
    arc.rounded = 1;
    lv_draw_arc(layer, &arc);

    /* indices: rounded squares at hours (ours, not PineTime's sticks),
       dim dots for the other minute positions */
    for (int i = 0; i < 60; i++) {
        float a = i * 6.0f * 0.0174533f - 1.5707963f;
        int ix = CX + (int)(cosf(a) * 102);
        int iy = CY + (int)(sinf(a) * 102);
        if (i % 5 == 0) {
            lv_area_t sq = {ix - 4, iy - 4, ix + 4, iy + 4};
            fill.color = lv_color_hex(0xB8C4D8);
            fill.radius = 3;
            lv_draw_fill(layer, &fill, &sq);
        } else {
            int mx = CX + (int)(cosf(a) * 110);
            int my = CY + (int)(sinf(a) * 110);
            lv_area_t dot = {mx - 1, my - 1, mx + 1, my + 1};
            fill.color = lv_color_hex(0x2A3344);
            fill.radius = 1;
            lv_draw_fill(layer, &fill, &dot);
        }
    }

    int h = hal_clock_hour() % 12;
    int m = hal_clock_minute();

    /* second hand is a plain thin line — a triangle stretched it into
       a malformed blob; minute and hour keep the tapered profile */
    float sa = (sweep - 90.0f) * 0.0174533f;
    draw_line(layer, CX - (int)(cosf(sa) * 18), CY - (int)(sinf(sa) * 18),
              CX + (int)(cosf(sa) * 88), CY + (int)(sinf(sa) * 88),
              lv_color_hex(0x5AC8D2), 2, 1);
    hand(layer, (m + s / 60.0f) * 6.0f, 76, 12, 5, 0xD8E0EC);
    hand(layer, (h + m / 60.0f) * 30.0f, 52, 12, 7, 0xFFFFFF);

    /* pivot cap */
    lv_area_t cap = {CX - 7, CY - 7, CX + 7, CY + 7};
    fill.color = lv_color_hex(0x5AC8D2);
    fill.radius = 7;
    lv_draw_fill(layer, &fill, &cap);
    lv_area_t dot = {CX - 3, CY - 3, CX + 3, CY + 3};
    fill.color = lv_color_hex(0x0A0E15);
    fill.radius = 3;
    lv_draw_fill(layer, &fill, &dot);

}

static void heart_draw(lv_event_t *ev)
{
    static const char shape[6][8] = {
        ".XX.XX.", "XXXXXXX", "XXXXXXX", ".XXXXX.", "..XXX..", "...X..."
    };
    lv_layer_t *layer = lv_event_get_layer(ev);
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target(ev), &area);
    /* pulse at the measured rate; shimmer when no lock */
    int rate = sensors_bpm();
    int lit;
    if (rate > 0) {
        int period = 60000 / rate;
        lit = (int)(frame_ms % (uint32_t)period) < 220;
    } else {
        lit = ((int)(frame_ms / 110) % 8) < 2;
    }
    lv_draw_fill_dsc_t fill;
    lv_draw_fill_dsc_init(&fill);
    fill.color = lv_color_hex(lit ? 0xFF9898 : 0xE63C3C);
    for (int y = 0; y < 6; y++)
        for (int x = 0; x < 7; x++) {
            if (shape[y][x] != 'X') continue;
            lv_area_t pixel = {area.x1 + x * 2, area.y1 + y * 2,
                               area.x1 + x * 2 + 1, area.y1 + y * 2 + 1};
            lv_draw_fill(layer, &fill, &pixel);
        }
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font,
                       uint32_t color, int x, int y, int width, const char *value)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, width);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_text(obj, value);
    return obj;
}

/* ---- digital face, 2.1 layout ------------------------------------------
   y   8..49   weather chip (left, tap -> weather)   date block (right)
   y  54..117  big time
   y 124..164  now-playing chip while music plays (tap -> music),
               else a hex rule
   y 170..232  steps pill (cyan) + heart-rate pill (red), each 112x62 so
               a 5-digit step count / 3-digit BPM sits on its own row
               under the label instead of on top of it */
#define PILL_Y 170
#define PILL_H 63
#define PILL_W 112
#define PILL_LX 5
#define PILL_RX 123
#define CHIP_Y 8
#define CHIP_H 42
#define MUS_Y 123
#define MUS_H 42

static lv_obj_t *d_wx_hit, *d_wx_temp, *d_wx_city, *d_mus_hit, *d_mus_lbl;
static lv_obj_t *d_lbl_dow;
static lv_obj_t *art_obj;
static int d_wx_icon = -2, d_mus_on = -1, d_mus_playing = -1;
static uint32_t d_wx_rx;
void ui_nav_open(int which);       /* ui_nav.c: 1 weather, 2 music */
void ui_music_ascii(char *dst, const char *src, int cap);

static void digi_draw(lv_event_t *ev)
{
    lv_layer_t *l = lv_event_get_layer(ev);
    /* weather chip */
    int wxok = d_wx_icon >= 0;
    dk_oct(l, 6, CHIP_Y, 108, CHIP_H, 9, wxok ? DK_CYAN : DK_DIM, 1,
           wxok ? 28 : 0);
    dk_wx_icon(l, wxok ? d_wx_icon : -1, 28, CHIP_Y + CHIP_H / 2, 12,
               lv_color_white());
    if (wxok) {
        lv_obj_update_layout(d_wx_temp);
        int tx = lv_obj_get_x(d_wx_temp);
        lv_point_t sz;
        lv_text_get_size(&sz, lv_label_get_text(d_wx_temp), &kenney16, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        dk_deg(l, tx + sz.x + 2, CHIP_Y + 8, 3, DK_CYAN);
    }
    /* date block: right-aligned, hex tick on the far edge */
    dk_line(l, 228, CHIP_Y + 4, 228, CHIP_Y + CHIP_H - 4, DK_DIM, 2);
    /* middle: music chip or hex rule */
    if (d_mus_on > 0) {
        dk_oct(l, 18, MUS_Y, 204, MUS_H, 10, DK_RED, 2, 30);
        dk_hex(l, 42, MUS_Y + MUS_H / 2, 13, DK_RED, 2);
        dk_music(l, d_mus_playing > 0 ? DK_M_PLAY : DK_M_PAUSE, 43,
                 MUS_Y + MUS_H / 2, 5, lv_color_white());
    } else {
        dk_rule(l, MUS_Y + MUS_H / 2, DK_CYAN);
    }
    /* pills */
    dk_oct(l, PILL_LX, PILL_Y, PILL_W, PILL_H, 11, DK_CYAN, 2, 22);
    dk_oct(l, PILL_RX, PILL_Y, PILL_W, PILL_H, 11, DK_RED, 2, 22);
    dk_line(l, PILL_LX + 14, PILL_Y + 30, PILL_LX + PILL_W - 15, PILL_Y + 30,
            DK_DIM, 1);
    dk_line(l, PILL_RX + 14, PILL_Y + 30, PILL_RX + PILL_W - 15, PILL_Y + 30,
            DK_DIMR, 1);
}

static void chip_cb(lv_event_t *e)
{
    ui_nav_open((int)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *hit(lv_obj_t *scr, int x, int y, int w, int h, int which)
{
    lv_obj_t *o = lv_obj_create(scr);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o, chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)which);
    return o;
}

static lv_obj_t *label_l(lv_obj_t *parent, const lv_font_t *font,
                         uint32_t color, int x, int y, int width,
                         lv_text_align_t al, const char *value)
{
    lv_obj_t *o = label(parent, font, color, x, y, width, value);
    lv_obj_set_style_text_align(o, al, 0);
    return o;
}

/* refresh weather + music chips; returns 1 when the art must repaint */
static int face_phone_refresh(void)
{
    int dirty = 0;
    const struct wx *w = wx_get();
    int icon = w ? w->icon : -1;
    uint32_t rx = w ? w->rx_ms : 0;
    if (icon != d_wx_icon || rx != d_wx_rx) {
        char b[24];
        if (w) {
            int t = w->temp >= 0 ? (w->temp + 50) / 100 : -((-w->temp + 50) / 100);
            lv_snprintf(b, sizeof b, "%d", t);
            lv_label_set_text(d_wx_temp, b);
            lv_obj_set_style_text_color(d_wx_temp, lv_color_white(), 0);
            /* the icon already says the sky; the 12 px line has room
               for today's range, not "FEW CLOUDS" */
            int hi = w->tmax >= 0 ? (w->tmax + 50) / 100 : -((-w->tmax + 50) / 100);
            int lo = w->tmin >= 0 ? (w->tmin + 50) / 100 : -((-w->tmin + 50) / 100);
            lv_snprintf(b, sizeof b, "H%d L%d", hi, lo);
            lv_label_set_text(d_wx_city, b);
        } else {
            lv_label_set_text(d_wx_temp, "--");
            lv_obj_set_style_text_color(d_wx_temp, lv_color_hex(0x5A6478), 0);
            lv_label_set_text(d_wx_city, "NO DATA");
        }
        d_wx_icon = icon;
        d_wx_rx = rx;
        dirty = 1;
    }
    int on = music_active();
    const struct music *m = music_get();
    if (on != d_mus_on || (on && m->playing != d_mus_playing) ||
        (on && phone_take_face_dirty())) {
        if (on) {
            char t[44];
            ui_music_ascii(t, m->track[0] ? m->track : "PLAYING", sizeof t);
            lv_label_set_text(d_mus_lbl, t);
            lv_obj_remove_flag(d_mus_hit, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(d_mus_hit, LV_OBJ_FLAG_HIDDEN);
        }
        d_mus_on = on;
        d_mus_playing = m->playing;
        dirty = 1;
    }
    return dirty;
}

static void build_digital(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_obj_t *art = lv_obj_create(scr);
    art_obj = art;
    lv_obj_set_pos(art, 0, 0);
    lv_obj_set_size(art, 240, 240);
    lv_obj_set_style_bg_opa(art, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(art, 0, 0);
    lv_obj_set_style_pad_all(art, 0, 0);
    lv_obj_remove_flag(art, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(art, digi_draw, LV_EVENT_DRAW_MAIN, NULL);

    /* weather chip (tap target = the whole chip, 108x42) */
    d_wx_hit = hit(scr, 6, CHIP_Y, 108, CHIP_H, 1);
    d_wx_temp = label_l(scr, &kenney16, 0xFFFFFF, 46, CHIP_Y + 6, 60,
                        LV_TEXT_ALIGN_LEFT, "--");
    d_wx_city = label_l(scr, &kenney12, 0x8A94A8, 46, CHIP_Y + 24, 66,
                        LV_TEXT_ALIGN_LEFT, "NO DATA");
    lv_obj_remove_flag(d_wx_temp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(d_wx_city, LV_OBJ_FLAG_CLICKABLE);

    /* date block */
    d_lbl_dow = label_l(scr, &kenney12, 0x46D8E0, 118, CHIP_Y + 4, 104,
                        LV_TEXT_ALIGN_RIGHT, "---");
    d_lbl_date = label_l(scr, &kenney16, 0xD8E0EC, 104, CHIP_Y + 21, 118,
                         LV_TEXT_ALIGN_RIGHT, "SYNC TIME");

    d_lbl_time = label(scr, &kenney56, 0xFFFFFF, 0, 54, 240, "--:--");

    /* now-playing chip, only while music plays + phone linked */
    d_mus_hit = hit(scr, 18, MUS_Y, 204, MUS_H, 2);
    d_mus_lbl = label_l(d_mus_hit, &kenney12, 0xFFFFFF, 46, 7, 150,
                        LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_long_mode(d_mus_lbl, LV_LABEL_LONG_DOT);
    lv_obj_t *np = label_l(d_mus_hit, &kenney12, 0xF0586E, 46, 22, 150,
                           LV_TEXT_ALIGN_LEFT, "NOW PLAYING");
    lv_obj_remove_flag(np, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(d_mus_hit, LV_OBJ_FLAG_HIDDEN);

    /* steps pill: footprint + STEPS label row, value row below */
    static lv_image_dsc_t fp_dsc;
    fp_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    fp_dsc.header.w = 24;
    fp_dsc.header.h = 24;
    fp_dsc.header.cf = LV_COLOR_FORMAT_A8;
    fp_dsc.header.stride = 24;
    fp_dsc.data = icon_alpha[14];
    fp_dsc.data_size = 24 * 24;
    fp_obj = lv_image_create(scr);
    lv_image_set_src(fp_obj, &fp_dsc);
    lv_image_set_scale(fp_obj, 160);          /* ~15 px */
    lv_obj_set_pos(fp_obj, PILL_LX + 12, PILL_Y + 3);
    lv_obj_set_style_image_recolor(fp_obj, lv_color_hex(0x46D8E0), 0);
    lv_obj_set_style_image_recolor_opa(fp_obj, LV_OPA_COVER, 0);
    label_l(scr, &kenney12, 0x8A94A8, PILL_LX + 40, PILL_Y + 9, 62,
            LV_TEXT_ALIGN_LEFT, "STEPS");
    d_lbl_steps = label(scr, &kenney24, 0x46D8E0, PILL_LX + 3, PILL_Y + 33,
                        PILL_W - 6, "0");

    heart_obj = lv_obj_create(scr);
    lv_obj_set_pos(heart_obj, PILL_RX + 18, PILL_Y + 10);
    lv_obj_set_size(heart_obj, 14, 12);
    lv_obj_set_style_bg_opa(heart_obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(heart_obj, 0, 0);
    lv_obj_set_style_pad_all(heart_obj, 0, 0);
    lv_obj_remove_flag(heart_obj,
                       LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(heart_obj, heart_draw, LV_EVENT_DRAW_MAIN, NULL);
    label_l(scr, &kenney12, 0x8A94A8, PILL_RX + 40, PILL_Y + 9, 62,
            LV_TEXT_ALIGN_LEFT, "BPM");
    d_lbl_bpm = label(scr, &kenney24, 0xF0586E, PILL_RX + 3, PILL_Y + 33,
                      PILL_W - 6, "--");
    breath_start(d_lbl_bpm);

    last_bpm = last_steps = last_synced = last_dom = last_min = -1;
    d_wx_icon = -2;
    d_mus_on = d_mus_playing = -1;
    face_phone_refresh();
}

void ui_face_build(lv_obj_t *scr)
{
    is_digital = ui_face_digital();
    if (is_digital) {
        build_digital(scr);
        return;
    }
    /* true black: the dead bezel disappears into the dial edge */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* dial layer under everything: ticks, hands, PPG trace */
    dial_obj = lv_obj_create(scr);
    lv_obj_set_pos(dial_obj, 0, 0);
    lv_obj_set_size(dial_obj, 240, 240);
    lv_obj_set_style_bg_opa(dial_obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dial_obj, 0, 0);
    lv_obj_set_style_pad_all(dial_obj, 0, 0);
    lv_obj_remove_flag(dial_obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(dial_obj, dial_draw, LV_EVENT_DRAW_MAIN, NULL);

    /* numerals at 12/6/9 only — minimal, ours */
    static const int quads[3] = {12, 6, 9};
    for (int i = 0; i < 3; i++) {
        float a = quads[i] * 30.0f * 0.0174533f - 1.5707963f;
        int x = CX + (int)(cosf(a) * 80) - 14;
        int y = CY + (int)(sinf(a) * 80) - 10;
        char num[3];
        lv_snprintf(num, sizeof num, "%d", quads[i]);
        label(scr, &kenney16, 0xD8E0EC, x, y, 28, num);
    }

    /* complications */
    label(scr, &kenney9, 0x59647B, 90, 50, 60, "VOIDOS");
    /* steps complication: footprint icon + bare number, no "STEPS";
       the pair recentres itself in the tick as digits change */
    static lv_image_dsc_t fp_dsc;
    fp_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    fp_dsc.header.w = 24;
    fp_dsc.header.h = 24;
    fp_dsc.header.cf = LV_COLOR_FORMAT_A8;
    fp_dsc.header.stride = 24;
    fp_dsc.data = icon_alpha[14];
    fp_dsc.data_size = 24 * 24;
    fp_obj = lv_image_create(scr);
    lv_image_set_src(fp_obj, &fp_dsc);
    lv_obj_set_pos(fp_obj, 88, 62);
    lv_obj_set_style_image_recolor(fp_obj, lv_color_hex(0x8890A0), 0);
    lv_obj_set_style_image_recolor_opa(fp_obj, LV_OPA_COVER, 0);
    lbl_steps = label(scr, &kenney9, 0x8890A0, 116, 66, 60, "--");
    lv_obj_set_width(lbl_steps, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(lbl_steps, LV_TEXT_ALIGN_LEFT, 0);
    lbl_date = label(scr, &kenney9, 0x8890A0, 160, 112, 56, "");

    last_bpm = last_steps = last_synced = last_dom = last_min = -1;
    prev_hand.x1 = prev_hand.x2 = 0;
    heart_obj = lv_obj_create(scr);
    lv_obj_set_pos(heart_obj, 96, 172);
    lv_obj_set_size(heart_obj, 14, 12);
    lv_obj_set_style_bg_opa(heart_obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(heart_obj, 0, 0);
    lv_obj_set_style_pad_all(heart_obj, 0, 0);
    lv_obj_remove_flag(heart_obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(heart_obj, heart_draw, LV_EVENT_DRAW_MAIN, NULL);
    lbl_bpm = label(scr, &kenney9, 0xD8E0EC, 114, 173, 46, "--");
    lv_obj_set_style_text_align(lbl_bpm, LV_TEXT_ALIGN_LEFT, 0);
    breath_start(lbl_bpm);
}


/* bounding box of the second hand + its rim-arc tip at a given angle */
static void sec_area(float deg, lv_area_t *a)
{
    float r = (deg - 90.0f) * 0.0174533f;
    int tx = CX + (int)(cosf(r) * 88), ty = CY + (int)(sinf(r) * 88);
    int bx = CX - (int)(cosf(r) * 18), by = CY - (int)(sinf(r) * 18);
    int ax = CX + (int)(cosf(r) * 114), ay = CY + (int)(sinf(r) * 114);
    int x1 = tx, x2 = tx, y1 = ty, y2 = ty;
    const int px[3] = {bx, ax, CX}, py[3] = {by, ay, CY};
    for (int i = 0; i < 3; i++) {
        if (px[i] < x1) x1 = px[i];
        if (px[i] > x2) x2 = px[i];
        if (py[i] < y1) y1 = py[i];
        if (py[i] > y2) y2 = py[i];
    }
    a->x1 = x1 - 6; a->y1 = y1 - 6; a->x2 = x2 + 6; a->y2 = y2 + 6;
}

void ui_face_tick(void)
{
    frame_ms = hal_millis();
    char buf[24];

    if (is_digital) {
        int bpm = sensors_bpm();
        if (bpm != last_bpm) {
            lv_snprintf(buf, sizeof buf, "%d", bpm > 0 ? bpm : 0);
            lv_label_set_text(d_lbl_bpm, bpm > 0 ? buf : "--");
            last_bpm = bpm;
        }
        int steps = ui_steps();
        if (steps != last_steps) {
            if (steps < 0)
                lv_snprintf(buf, sizeof buf, "--");
            else if (steps >= 100000)
                lv_snprintf(buf, sizeof buf, "%dK", steps / 1000);
            else
                lv_snprintf(buf, sizeof buf, "%d", steps);
            lv_label_set_text(d_lbl_steps, buf);
            last_steps = steps;
        }
        int sec = hal_clock_second(), min = hal_clock_minute();
        /* 1 synced, 2 restored after a reboot/flash (shown grey, never
           trusted), 0 never set */
        int synced = hal_clock_synced() ? 1 : hal_clock_restored() ? 2 : 0;
        if (min != last_min || sec != last_sec || synced != last_synced) {
            if (synced)
                lv_snprintf(buf, sizeof buf, "%02d:%02d", hal_clock_hour(), min);
            else
                lv_snprintf(buf, sizeof buf, "--:--");
            lv_label_set_text(d_lbl_time, buf);
            lv_obj_set_style_text_color(d_lbl_time,
                lv_color_hex(synced == 2 ? 0x8A93A0 : 0xFFFFFF), 0);
            last_sec = sec;
        }
        if (min != last_min || synced != last_synced) {
            last_min = min;
            last_synced = synced;
            int y, mo, d, dow;
            hal_clock_date(&y, &mo, &d, &dow);
            /* 2.0.1 left the build-time placeholder "FRI, OCT 24" up until
               the phone synced the clock, which read as a wrong date */
            if (synced == 2) {
                lv_label_set_text(d_lbl_dow, "SYNCING TIME");
                lv_label_set_text(d_lbl_date, "OPEN PHONE");
            } else if (synced) {
                static const char *dn[8] = {"", "MONDAY", "TUESDAY",
                                            "WEDNESDAY", "THURSDAY",
                                            "FRIDAY", "SATURDAY", "SUNDAY"};
                static const char *mn[13] = {"", "JAN", "FEB", "MAR",
                                             "APR", "MAY", "JUN", "JUL",
                                             "AUG", "SEP", "OCT", "NOV",
                                             "DEC"};
                lv_label_set_text(d_lbl_dow,
                                  dow >= 1 && dow <= 7 ? dn[dow] : "---");
                lv_snprintf(buf, sizeof buf, "%d %s", d,
                            mo >= 1 && mo <= 12 ? mn[mo] : "???");
                lv_label_set_text(d_lbl_date, buf);
            } else {
                lv_label_set_text(d_lbl_dow, "TIME NOT SET");
                lv_label_set_text(d_lbl_date, "OPEN PHONE");
            }
        }
        static uint32_t last_phone;
        if (frame_ms - last_phone >= 500) {
            last_phone = frame_ms;
            if (face_phone_refresh())
                lv_obj_invalidate(art_obj);
        }
        static uint32_t last_heart;
        if (frame_ms - last_heart >= 120) {
            last_heart = frame_ms;
            lv_obj_invalidate(heart_obj);
        }
        return;
    }

    int bpm = sensors_bpm();
    if (bpm != last_bpm) {
        lv_snprintf(buf, sizeof buf, "%d", bpm > 0 ? bpm : 0);
        lv_label_set_text(lbl_bpm, bpm > 0 ? buf : "--");
        last_bpm = bpm;
    }
    int steps = ui_steps();
    if (steps != last_steps) {
        lv_snprintf(buf, sizeof buf, "%d", steps >= 0 ? steps : 0);
        lv_label_set_text(lbl_steps, steps >= 0 ? buf : "--");
        /* keep icon+number centred as a unit under VOIDOS */
        lv_obj_update_layout(lbl_steps);
        int tw = lv_obj_get_width(lbl_steps);
        int gx = 120 - (28 + tw) / 2;
        lv_obj_set_pos(fp_obj, gx, 62);
        lv_obj_set_x(lbl_steps, gx + 28);
        last_steps = steps;
    }
    int synced = hal_clock_synced();
    int y, mo, d, dow;
    hal_clock_date(&y, &mo, &d, &dow);
    if (synced != last_synced || d != last_dom) {
        if (synced) {
            static const char *dn[8] = {"", "MON", "TUE", "WED", "THU",
                                        "FRI", "SAT", "SUN"};
            lv_snprintf(buf, sizeof buf, "%s %d",
                        dow >= 1 && dow <= 7 ? dn[dow] : "???", d);
            lv_label_set_text(lbl_date, buf);
        } else {
            lv_label_set_text(lbl_date, "");
        }
        last_synced = synced;
        last_dom = d;
    }
    /* Repaint only what moved. A full 240x240 invalidate is 115 KB over
       SPI per frame (~115 ms at 8 MHz) — the source of the jerky hand.
       The second hand + arc tip get a tight union of old and new boxes;
       the minute/hour hands only move once a minute, so a full dial
       repaint then is cheap. */
    int sec = hal_clock_second(), min = hal_clock_minute();
    if (min != last_min) {
        last_min = min;
        lv_obj_invalidate(dial_obj);
    } else {
        lv_area_t now_a;
        sec_area((sec + hal_clock_millis() / 1000.0f) * 6.0f, &now_a);
        lv_area_t u = now_a;
        if (prev_hand.x2 > prev_hand.x1) {
            if (prev_hand.x1 < u.x1) u.x1 = prev_hand.x1;
            if (prev_hand.y1 < u.y1) u.y1 = prev_hand.y1;
            if (prev_hand.x2 > u.x2) u.x2 = prev_hand.x2;
            if (prev_hand.y2 > u.y2) u.y2 = prev_hand.y2;
        }
        lv_obj_invalidate_area(dial_obj, &u);
        prev_hand = now_a;
    }
    (void)last_sec;
    last_sec = sec;
    /* the heart only needs ~8 Hz */
    static uint32_t last_heart;
    if (frame_ms - last_heart >= 120) {
        last_heart = frame_ms;
        lv_obj_invalidate(heart_obj);
    }
}
