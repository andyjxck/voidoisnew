/* ui_power.inc — power menu (2.4.1), included by ui_nav.c.

   Hold the side button for 3 s: the watch buzzes and this menu opens
   over whatever is on screen (it wakes the screen first). POWER OFF
   saves everything and puts the nRF52 into System OFF; a press of the
   side button starts it again. RESTART saves everything, the clock
   marked fresh so the alarm keeps working without the phone (alarm.h),
   and resets. CANCEL, a short press of the button, sleeping or 10 s
   without a tap close it.

   Keep holding past 3 s and main.c still resets the watch at 8 s with
   the button down, so the bootloader's own button-held recovery works
   as it did with the old 3 s reset; a frozen main loop is still caught
   by the watchdog. Not offered mid-DFU. */

#define PM_HOLD_MS     3000u
#define PM_SHOW_MS     10000u

void hal_power_off(void);
void hal_system_reset(void);
void ui_save_before_restart(void);
void ui_save_before_off(void);

/* One object, drawn by hand, created on open and deleted on close: the
   LVGL heap is small and the screen underneath stays alive. */
static lv_obj_t *pm_box;
static const char *pm_msg;           /* set: the box shows only this */
static uint32_t pm_until;

#define PM_ROW_X 20
#define PM_ROW_W 200
#define PM_ROW_H 50
static const int pm_row_y[3] = {46, 106, 166};
static const char *const pm_row_txt[3] = {"POWER OFF", "RESTART", "CANCEL"};
static const uint32_t pm_row_col[3] = {0xF0586E, 0x46D8E0, 0x2A3444};
static const uint32_t pm_txt_col[3] = {0xF0586E, 0xFFFFFF, 0x8A94A8};

static int pm_visible(void)
{
    return pm_box != NULL;
}

static void pm_hide(void)
{
    if (pm_box) {
        lv_obj_delete(pm_box);
        pm_box = NULL;
        lv_obj_invalidate(lv_screen_active());
    }
    pm_msg = NULL;
    pm_until = 0;
}

static void pm_draw(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    if (pm_msg) {
        draw_txt_c(l, &kenney16, lv_color_white(), 120, 112, pm_msg);
        return;
    }
    draw_txt_c(l, &kenney16, lv_color_hex(0x46D8E0), 120, 16, "POWER");
    for (int i = 0; i < 3; i++) {
        lv_color_t c = lv_color_hex(pm_row_col[i]);
        int y = pm_row_y[i];
        dk_oct(l, PM_ROW_X, y, PM_ROW_W, PM_ROW_H, 9, c, 2,
               i < 2 ? 40 : 0);
        draw_txt_c(l, &kenney16, lv_color_hex(pm_txt_col[i]), 120,
                   y + (PM_ROW_H - 18) / 2, pm_row_txt[i]);
    }
}

/* the last thing on screen before the action: drawn at once, since
   neither action returns to the main loop */
static void pm_say(const char *s)
{
    pm_msg = s;
    lv_obj_invalidate(pm_box);
    lv_refr_now(NULL);
}

static void pm_click(lv_event_t *e)
{
    (void)e;
    if (pm_msg)
        return;
    lv_point_t p;
    lv_indev_t *in = lv_indev_active();
    if (!in)
        return;
    lv_indev_get_point(in, &p);
    int row = -1;
    for (int i = 0; i < 3; i++)
        if (p.y >= pm_row_y[i] - 4 && p.y < pm_row_y[i] + PM_ROW_H + 4 &&
            p.x >= PM_ROW_X - 10 && p.x < PM_ROW_X + PM_ROW_W + 10)
            row = i;
    if (row == 0) {
        pm_say("POWERING OFF");
        ui_save_before_off();
        hal_power_off();             /* does not return */
    } else if (row == 1) {
        pm_say("RESTARTING");
        ui_save_before_restart();
        hal_system_reset();          /* does not return */
    } else if (row == 2) {
        pm_hide();
    }
}

static void pm_show(uint32_t now)
{
    if (!pm_box) {
        pm_box = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(pm_box);
        lv_obj_set_size(pm_box, 240, 240);
        lv_obj_set_pos(pm_box, 0, 0);
        lv_obj_set_style_bg_color(pm_box, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(pm_box, LV_OPA_COVER, 0);
        lv_obj_remove_flag(pm_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(pm_box, LV_OBJ_FLAG_CLICKABLE);   /* eats taps */
        lv_obj_add_event_cb(pm_box, pm_draw, LV_EVENT_DRAW_MAIN, NULL);
        lv_obj_add_event_cb(pm_box, pm_click, LV_EVENT_CLICKED, NULL);
    }
    pm_msg = NULL;
    pm_until = now + PM_SHOW_MS;
    lv_obj_invalidate(pm_box);
    lv_obj_move_foreground(pm_box);
}

/* every ui_tick, after the button is read. Returns 1 while a hold that
   opened the menu is still down or just ended: the caller drops that
   press, so letting go doesn't also navigate. */
static int pm_tick(uint32_t now, uint32_t held)
{
    extern int ble_dfu_busy(void);
    static int latched;
    int swallow = 0;
    if (held >= PM_HOLD_MS && !latched) {
        latched = 1;
        if (!ble_dfu_busy()) {
            if (asleep)
                ui_wake();
            chg_hide();
            goal_hide();
            buzz(60);
            pm_show(now);
        }
    }
    if (latched) {
        swallow = 1;
        if (!held)
            latched = 0;
    }
    if (!pm_visible())
        return swallow;
    if (latched)
        pm_until = now + PM_SHOW_MS;
    if (asleep || (pm_until && (int32_t)(now - pm_until) >= 0)) {
        pm_hide();
        return swallow;
    }
    last_input_ms = now;             /* never sleep under it */
    return swallow;
}
