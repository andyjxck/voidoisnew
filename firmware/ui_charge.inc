/* ui_charge.inc — battery / charging screen (2.2.4), included by ui_nav.c.

   Plugging the watch in wakes the screen and shows it for 6 s: a
   chamfered battery whose eight cells fill to the current level, then a
   chase climbs the remaining cells while it charges, with Google's
   "bolt" Material Symbol on top. On the charger but no longer charging
   it reads CHARGED with every cell lit. Tapping the BATT hub in the
   control centre opens the same screen on battery power (static cells).
   A tap, a swipe or the side button closes it; so does sleeping,
   unplugging, an alarm or a timer.

   Cost: it sits on the top layer, so showing it is one full redraw; the
   chase then invalidates only the cell column (~48x110 px) every 140 ms,
   ~10 ms of SPI each. */

#define CHG_SHOW_MS    6000u
#define CHG_STEP_MS    140u
#define CHG_DEBOUNCE   250u
#define CHG_CELLS      8

/* battery body geometry (screen coords; the box is full-screen) */
#define CHG_BX   88
#define CHG_BY   46
#define CHG_BW   64
#define CHG_BH   128
#define CHG_CELL_X1 (CHG_BX + 8)
#define CHG_CELL_X2 (CHG_BX + CHG_BW - 9)
#define CHG_CELL_H  11
#define CHG_CELL_GAP 3
#define CHG_CELL_Y2(i) (CHG_BY + CHG_BH - 9 - (i) * (CHG_CELL_H + CHG_CELL_GAP))

static lv_obj_t *chg_box;
static uint32_t chg_until, chg_t0, chg_last_step;
static int chg_step;

static int chg_visible(void)
{
    return chg_box && !lv_obj_has_flag(chg_box, LV_OBJ_FLAG_HIDDEN);
}

static int chg_level_cells(int pct)
{
    if (pct <= 0)
        return 0;
    int n = (pct * CHG_CELLS + 99) / 100;      /* any charge lights one */
    return n > CHG_CELLS ? CHG_CELLS : n;
}

static void chg_draw(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    int pct = hal_battery_pct();
    int plugged = hal_on_charger(), charging = hal_charging();
    int full = plugged && !charging;
    int base = full ? CHG_CELLS : chg_level_cells(pct);
    lv_color_t col = full || plugged ? C_GOOD : pct < 20 ? C_WARN : C_ACCENT;
    lv_color_t dim = lv_color_hex(0x1A222E);

    /* body + terminal cap */
    dk_oct(l, CHG_BX, CHG_BY, CHG_BW, CHG_BH, 8, col, 2, 0);
    dk_rect(l, CHG_BX + 20, CHG_BY - 8, CHG_BX + CHG_BW - 21, CHG_BY - 3,
            col, LV_OPA_COVER);

    /* cells: the level, then (charging) a chase up the rest */
    int lit = base;
    if (charging) {
        int span = CHG_CELLS - base;           /* cells left to fill */
        if (span > 0)
            lit = base + chg_step % (span + 1);
        else
            lit = (chg_step & 1) ? CHG_CELLS : CHG_CELLS - 1;  /* topping */
    }
    for (int i = 0; i < CHG_CELLS; i++) {
        int y2 = CHG_CELL_Y2(i);
        lv_color_t c = i < base ? col
                     : i < lit ? lv_color_mix(col, lv_color_black(), 150)
                               : dim;
        dk_rect(l, CHG_CELL_X1, y2 - CHG_CELL_H + 1, CHG_CELL_X2, y2, c,
                LV_OPA_COVER);
    }
    if (plugged)   /* Material Symbols "bolt" */
        mi_draw(l, MI_BOLT, CHG_BX + CHG_BW / 2, CHG_BY + CHG_BH / 2, 36,
                lv_color_white());

    char v[8];
    lv_snprintf(v, sizeof v, "%d%%", pct < 0 ? 0 : pct);
    draw_txt_c(l, &kenney24, C_WHITE, 120, CHG_BY + CHG_BH + 10, v);
    draw_txt_c(l, &kenney12, plugged ? col : C_MUTED, 120,
               CHG_BY + CHG_BH + 40,
               full ? "CHARGED" : charging ? "CHARGING" :
               plugged ? "ON CHARGER" : "ON BATTERY");
}

static void chg_hide(void)
{
    if (chg_visible())
        lv_obj_add_flag(chg_box, LV_OBJ_FLAG_HIDDEN);
    chg_until = 0;
}

static void chg_tap(lv_event_t *e)
{
    (void)e;
    chg_hide();
}

static void chg_show(uint32_t now, uint32_t ms)
{
    if (!chg_box) {
        chg_box = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(chg_box);
        lv_obj_set_size(chg_box, 240, 240);
        lv_obj_set_pos(chg_box, 0, 0);
        lv_obj_set_style_bg_color(chg_box, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(chg_box, LV_OPA_COVER, 0);
        lv_obj_remove_flag(chg_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(chg_box, LV_OBJ_FLAG_CLICKABLE);  /* eats taps */
        lv_obj_add_event_cb(chg_box, chg_draw, LV_EVENT_DRAW_MAIN, NULL);
        lv_obj_add_event_cb(chg_box, chg_tap, LV_EVENT_CLICKED, NULL);
    }
    chg_step = 0;
    chg_t0 = chg_last_step = now;
    chg_until = now + ms;
    lv_obj_remove_flag(chg_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(chg_box);
    lv_obj_move_foreground(chg_box);
}

static void chg_open(void)
{
    chg_show(hal_millis(), CHG_SHOW_MS);
}

/* every ui_tick, asleep or not */
static void chg_tick(uint32_t now)
{
    /* debounced charger edge: a cable seats with some bounce */
    static int stable = -1, raw_last = -1;
    static uint32_t raw_since;
    int raw = hal_on_charger();
    if (raw != raw_last) {
        raw_last = raw;
        raw_since = now;
    }
    if (now - raw_since >= CHG_DEBOUNCE && raw != stable) {
        int was = stable;
        stable = raw;
        if (was == 0 && raw == 1 && !alarm_ringing && !timer_done) {
            if (asleep)
                ui_wake();
            else
                last_input_ms = now;
            buzz(40);
            chg_show(now, CHG_SHOW_MS);
        } else if (was == 1 && raw == 0) {
            chg_hide();
        }
        /* (was == -1: boot, already plugged or not: no screen) */
    }
    if (!chg_visible())
        return;
    if (asleep || alarm_ringing || timer_done ||
        (chg_until && (int32_t)(now - chg_until) >= 0)) {
        chg_hide();
        return;
    }
    last_input_ms = now;           /* never sleep under it */
    if (hal_charging() && now - chg_last_step >= CHG_STEP_MS) {
        chg_last_step = now;
        chg_step++;
        lv_area_t a;
        a.x1 = CHG_CELL_X1;
        a.x2 = CHG_CELL_X2;
        a.y1 = CHG_CELL_Y2(CHG_CELLS - 1) - CHG_CELL_H + 1;
        a.y2 = CHG_CELL_Y2(0);
        lv_obj_invalidate_area(chg_box, &a);
    }
}
