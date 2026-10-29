/* Google Material Symbols for VoidOS: every icon on the watch comes from
   here. Baked by tools/gen_assets.py from the PNGs in assets/micons (rendered
   from Google's Material Symbols Outlined font by tools/render_micons.py)
   into A8 images that LVGL recolours, so one bitmap serves any colour.

   mi_img()  the baked image for an icon at (or nearest to) a pixel size
   mi_draw() draw it centred on (cx, cy) into a canvas/draw layer
   mi_obj()  an lv_image widget showing it, top-left at (x, y) */
#ifndef VOIDOS_MICONS_H
#define VOIDOS_MICONS_H

#include "lvgl.h"
#include "gen/micons.h"

extern const lv_image_dsc_t *const micon_tab[MI_COUNT][MI_NSIZES];

/* the baked size closest to px (ties go to the larger one) */
static inline const lv_image_dsc_t *mi_img(int id, int px)
{
    const lv_image_dsc_t *best = NULL;
    int bd = 1 << 30;
    if (id < 0 || id >= MI_COUNT)
        return NULL;
    for (int i = 0; i < MI_NSIZES; i++) {
        const lv_image_dsc_t *d = micon_tab[id][i];
        if (!d)
            continue;
        int diff = (int)mi_sizes[i] - px;
        if (diff < 0)
            diff = -diff * 2 + 1;     /* prefer a bigger icon to a smaller */
        else
            diff *= 2;
        if (diff < bd) {
            bd = diff;
            best = d;
        }
    }
    return best;
}

static inline void mi_draw_opa(lv_layer_t *layer, int id, int cx, int cy,
                               int px, lv_color_t c, lv_opa_t opa)
{
    const lv_image_dsc_t *img = mi_img(id, px);
    if (!img)
        return;
    int w = img->header.w, h = img->header.h;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = img;
    d.recolor = c;
    d.recolor_opa = LV_OPA_COVER;
    d.opa = opa;
    lv_area_t a = {cx - w / 2, cy - h / 2, cx - w / 2 + w - 1,
                   cy - h / 2 + h - 1};
    lv_draw_image(layer, &d, &a);
}

static inline void mi_draw(lv_layer_t *layer, int id, int cx, int cy, int px,
                           lv_color_t c)
{
    mi_draw_opa(layer, id, cx, cy, px, c, LV_OPA_COVER);
}

static inline lv_obj_t *mi_obj(lv_obj_t *parent, int id, int px,
                               lv_color_t c, int x, int y)
{
    lv_obj_t *img = lv_image_create(parent);
    const lv_image_dsc_t *d = mi_img(id, px);
    if (d)
        lv_image_set_src(img, d);
    lv_obj_set_style_image_recolor(img, c, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(img, x, y);
    return img;
}

/* the battery_N_bar icon for a charge level (battery_alert under 5 %) */
static inline int mi_battery(int pct, int charging)
{
    if (charging)
        return MI_BATT_CHARGING;
    if (pct < 0)
        return MI_BATT_ALERT;
    if (pct < 5)
        return MI_BATT_ALERT;
    if (pct >= 95)
        return MI_BATT_FULL;
    return MI_BATT_0 + (pct * 7) / 100;     /* 0..6 bars */
}

#endif
