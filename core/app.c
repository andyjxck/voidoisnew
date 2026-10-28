#include <math.h>
#include <stdio.h>
#include <string.h>
#include "voidos/app.h"
#include "font.h"

#define BACK_X 10
#define BACK_Y 28
#define BACK_W 26
#define BACK_H 20

#define TILE_W 105
#define TILE_H 55
#define TILE_X0 15
#define TILE_X1 120
#define TILE_Y0 55
#define TILE_DY 62

static int in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void key_pos(int i, int *x, int *y);
static void stat_pt_pos(const app_t *app, int i, int *x, int *y);
static void hex_btn(int cx, int cy, float R, hal_color_t tint);

static int in_circle(int x, int y, int cx, int cy, int r)
{
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

static void rect(int x, int y, int w, int h, hal_color_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > VOIDOS_WIDTH) w = VOIDOS_WIDTH - x;
    if (y + h > VOIDOS_HEIGHT) h = VOIDOS_HEIGHT - y;
    if (w > 0 && h > 0)
        hal_draw_rect(x, y, w, h, c);
}

/* all UI text renders through the TTF path (Kenney Future in the sim,
   a converted bitmap strike on device). scale 1..4 -> pixel size */
static int scale_px(int scale)
{
    static const int px_map[5] = {9, 9, 16, 24, 32};
    if (scale < 0)
        scale = 0;
    if (scale > 4)
        scale = 4;
    return px_map[scale];
}

static void text(int x, int y, const char *s, hal_color_t c, int scale)
{
    hal_face_draw_text(x, y, s, c, scale_px(scale));
}

static void text_center(int y, const char *s, hal_color_t c, int scale)
{
    int w = 0, h = 0;
    hal_face_text_size(s, scale_px(scale), &w, &h);
    text((VOIDOS_WIDTH - w) / 2, y, s, c, scale);
}

static int back_tapped(int x, int y)
{
    return in_rect(x, y, BACK_X, BACK_Y, BACK_W, BACK_H);
}

/* launcher hexes: 2-per-row honeycomb, alternating rows staggered */
static void launcher_pos(int i, int *cx, int *cy)
{
    int row = i / 2;
    *cx = ((row & 1) ? 96 : 70) + (i % 2) * 80;
    *cy = 64 + row * 54;
}

int app_launcher_tile_at(int x, int y)
{
    for (int i = 0; i < 6; i++) {
        int cx, cy;
        launcher_pos(i, &cx, &cy);
        if (in_rect(x, y, cx - 30, cy - 30, 60, 60))
            return i;
    }
    return -1;
}

static void go_back(app_t *app)
{
    switch (app->screen) {
    case SCR_FACE:
        app->screen = SCR_LAUNCHER;
        break;
    case SCR_LAUNCHER:
    case SCR_QUICK:
    case SCR_NOTIF:
    case SCR_STATS:
    case SCR_NUTRITION:
        app->screen = SCR_FACE;
        break;
    case SCR_SETTINGS:
        app->screen = app->settings_parent;
        break;
    case SCR_WORKOUT:
    case SCR_SLEEP:
    case SCR_WATER:
    case SCR_CLOCK:
        app->screen = SCR_LAUNCHER;
        break;
    case SCR_WORKOUT_RUN:
        app->screen = SCR_LAUNCHER;
        break;
    case SCR_REPLY:
        app->screen = SCR_NOTIF;
        break;
    case SCR_STAT_DETAIL:
        app->screen = SCR_STATS;
        break;
    case SCR_SCAN:
        app->screen = SCR_NUTRITION;
        break;
    case SCR_WORKOUT_DONE:
    case SCR_SLEEP_DONE:
        app->screen = SCR_FACE;
        break;
    default:
        break;
    }
}

static void tap_face(app_t *app, int x, int y)
{
    (void)x;
    (void)y;
    app->screen = SCR_LAUNCHER;
}

static void tap_launcher(app_t *app, int x, int y)
{
    if (back_tapped(x, y)) {
        go_back(app);
        return;
    }
    switch (app_launcher_tile_at(x, y)) {
    case 0: app->screen = app->workout_active ? SCR_WORKOUT_RUN : SCR_WORKOUT;
        break;
    case 1: app->screen = SCR_SLEEP; break;
    case 2: app->screen = SCR_WATER; break;
    case 3: app->screen = SCR_STATS; break;
    case 4:
        app->settings_parent = SCR_LAUNCHER;
        app->screen = SCR_SETTINGS;
        break;
    case 5: app->screen = SCR_CLOCK; break;
    default: break;
    }
}

static void tap_workout(app_t *app, int x, int y)
{
    if (back_tapped(x, y)) {
        go_back(app);
        return;
    }
    int start = 0;
    app->workout_routine = -1;
    for (int i = 0; i < 3; i++)
        if (in_rect(x, y, 30, 48 + i * 24, 180, 20)) {
            app->workout_routine = i;
            start = 1;
        }
    if (in_rect(x, y, 30, 124, 180, 54))
        start = 1;
    if (start) {
        app->screen = SCR_WORKOUT_RUN;
        app->workout_active = 1;
        app->sets_done = 0;
        app->workout_paused = 0;
        app->paused_total_ms = 0;
        app->rest_end_ms = 0;
        app->workout_start_ms = hal_millis();
    }
}

static void tap_workout_run(app_t *app, int x, int y)
{
    if (in_rect(x, y, 20, 170, 95, 45)) {
        uint32_t now = hal_millis();
        if (app->workout_paused) {
            uint32_t delta = now - app->pause_start_ms;
            app->paused_total_ms += delta;
            if (app->rest_end_ms)
                app->rest_end_ms += delta;
            app->workout_paused = 0;
        } else {
            app->pause_start_ms = now;
            app->workout_paused = 1;
        }
        return;
    }
    if (in_rect(x, y, 125, 170, 95, 45)) {   /* END — finish early */
        app->last_workout_ms =
            hal_millis() - app->workout_start_ms - app->paused_total_ms;
        app->workout_active = 0;
        app->screen = SCR_WORKOUT_DONE;
        return;
    }
    if (app->workout_paused)
        return;
    if (in_rect(x, y, 20, 100, 200, 55)) {
        if (app->sets_done < APP_WORKOUT_SETS) {
            app->sets_done++;
            if (app->sets_done < APP_WORKOUT_SETS)
                app->rest_end_ms = hal_millis() + APP_REST_MS;
            else
                app->rest_end_ms = 0;
        } else {
            uint32_t now = hal_millis();
            app->last_workout_ms = now - app->workout_start_ms - app->paused_total_ms;
            app->workout_active = 0;
            app->screen = SCR_WORKOUT_DONE;
        }
    }
}

static void tap_sleep(app_t *app, int x, int y)
{
    if (back_tapped(x, y)) {
        go_back(app);
        return;
    }
    if (in_rect(x, y, 40, 178, 160, 26)) {
        app->sleep_track ^= 1;
        return;
    }
    if (!app->sleep_track)
        return;
    if (in_rect(x, y, 40, 110, 160, 55)) {
        if (app->in_bed) {
            app->last_in_bed_ms = hal_millis() - app->bed_start_ms;
            app->in_bed = 0;
            app->screen = SCR_SLEEP_DONE;
        } else {
            app->in_bed = 1;
            app->bed_start_ms = hal_millis();
        }
    }
}

static void tap_water(app_t *app, int x, int y)
{
    if (back_tapped(x, y)) {
        go_back(app);
        return;
    }
    if (in_rect(x, y, 15, 198, 100, 32))
        app->water_ml += 250;
    else if (in_rect(x, y, 125, 198, 100, 32))
        app->water_ml += 500;
}

static void tap_quick(app_t *app, int x, int y)
{
    if (back_tapped(x, y)) {
        go_back(app);
        return;
    }
    if (in_rect(x, y, 184, 12, 44, 90)) {
        int b = (94 - y) * 100 / 68;
        app->brightness = b < 0 ? 0 : b > 100 ? 100 : b;
        return;
    }
    /* honeycomb cells (pointy-top hexes, apothem ~28) */
    if (in_circle(x, y, 91, 82, 26))   { app->dnd_on ^= 1;        return; }
    if (in_circle(x, y, 149, 82, 26))  { app->airplane_on ^= 1;   return; }
    if (in_circle(x, y, 63, 132, 26))  { app->bt_on ^= 1;         return; }
    if (!(app->cc_hide & 1) && in_circle(x, y, 120, 132, 26)) {
        app->torch_on ^= 1;
        return;
    }
    if (!(app->cc_hide & 4) && in_circle(x, y, 177, 132, 26)) {
        app->ping_t0 = hal_millis();
        return;
    }
    if (!(app->cc_hide & 2) && in_circle(x, y, 91, 182, 26)) {
        app->water_lock_on ^= 1;
        return;
    }
    if (in_circle(x, y, 149, 182, 26)) {
        app->settings_parent = SCR_QUICK;
        app->screen = SCR_SETTINGS;
    }
}

static void tap_notif(app_t *app, int x, int y)
{
    if (back_tapped(x, y)) {
        go_back(app);
        return;
    }
    if (in_rect(x, y, 70, 188, 100, 26)) {
        app->notif_cleared = 1;
        return;
    }
    if (!app->notif_cleared && in_rect(x, y, 24, 56, 192, 68))
        app->screen = SCR_REPLY;
}

/* reply keyboard: honeycomb letter cells + DEL/SPC/SEND row */
static void tap_reply(app_t *app, int x, int y)
{
    static const char keys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.,";
    for (int i = 0; i < 28; i++) {
        int kx, ky;
        key_pos(i, &kx, &ky);
        if (in_circle(x, y, kx, ky, 13)) {
            char c = keys[i];
            /* long press on punctuation yields the shifted mark */
            if (hal_millis() - app->touch_down_ms >= 500) {
                if (c == '.')
                    c = '?';
                else if (c == ',')
                    c = '!';
            }
            if (app->reply_len < (int)sizeof(app->reply_buf) - 1) {
                app->reply_buf[app->reply_len++] = c;
                app->reply_buf[app->reply_len] = '\0';
            }
            return;
        }
    }
    if (in_rect(x, y, 20, 198, 58, 26)) {          /* DEL */
        if (app->reply_len > 0)
            app->reply_buf[--app->reply_len] = '\0';
        return;
    }
    if (in_rect(x, y, 88, 198, 58, 26)) {          /* SPC */
        if (app->reply_len < (int)sizeof(app->reply_buf) - 1) {
            app->reply_buf[app->reply_len++] = ' ';
            app->reply_buf[app->reply_len] = '\0';
        }
        return;
    }
    if (in_rect(x, y, 156, 198, 64, 26)) {         /* SEND */
        app->reply_len = 0;
        app->reply_buf[0] = '\0';
        app->notif_cleared = 1;
        app->screen = SCR_NOTIF;
        return;
    }
    if (in_rect(x, y, 192, 24, 30, 26)) {          /* cancel ✕ */
        app->reply_len = 0;
        app->reply_buf[0] = '\0';
        app->screen = SCR_NOTIF;
    }
}

static void tap_stats(app_t *app, int x, int y)
{
    if (back_tapped(x, y)) {
        go_back(app);
        return;
    }
    for (int i = 0; i < 6; i++) {
        int cx = (i & 1) ? 126 : 18;
        int cy = 44 + (i / 2) * 50;
        if (in_rect(x, y, cx, cy, 96, 44)) {
            app->stat_sel = i;
            app->stat_range = 0;
            app->stat_pt = -1;
            app->screen = SCR_STAT_DETAIL;
            return;
        }
    }
}

static void tap_stat_detail(app_t *app, int x, int y)
{
    if (back_tapped(x, y)) {
        go_back(app);
        return;
    }
    if (in_rect(x, y, 76, 42, 44, 20)) {
        app->stat_range = 0;
        app->stat_pt = -1;
        return;
    }
    if (in_rect(x, y, 122, 42, 44, 20)) {
        app->stat_range = 1;
        app->stat_pt = -1;
        return;
    }
    /* chart tap: nearest point selects, second tap clears */
    if (in_rect(x, y, 20, 60, 200, 128)) {
        int n = app->stat_range ? 7 : 24;
        int best = -1, bd = 1000000;
        for (int i = 0; i < n; i++) {
            int px, py;
            stat_pt_pos(app, i, &px, &py);
            int dx = x - px, dy = y - py;
            int d = dx * dx + dy * dy;
            if (d < bd) { bd = d; best = i; }
        }
        app->stat_pt = (best == app->stat_pt) ? -1 : best;
    }
}

static void tap_nutrition(app_t *app, int x, int y)
{
    if (back_tapped(x, y)) {
        go_back(app);
        return;
    }
    if (in_rect(x, y, 26, 130, 188, 38))
        app->water_ml += 250;
    if (in_rect(x, y, 70, 178, 100, 22)) {
        app->screen = SCR_SCAN;
        app->scan_t0 = 0;
        app->scan_logged = 0;
        app->barcode_len = 0;
        app->barcode[0] = 0;
    }
}

static int scan_state(const app_t *app);

/* fake barcode catalogue — keyed by EAN, like gym-void's Open Food Facts hit */
typedef struct {
    const char *barcode, *name;
    int kcal, p, c, f, ml;
} scan_food_t;
static const scan_food_t scan_foods[] = {
    {"5060292301713", "GREEK YOGURT", 120, 10, 4, 0, 0},
    {"5060335631058", "PROTEIN BAR", 210, 20, 22, 7, 0},
    {"5060667440012", "WHEY SHAKE", 160, 24, 8, 2, 300},
    {"5010029000519", "BANANA", 90, 1, 23, 0, 0},
    {"5000159484695", "OAT LATTE", 130, 4, 14, 5, 250},
};
#define SCAN_FOODS_N ((int)(sizeof scan_foods / sizeof scan_foods[0]))

static int scan_lookup(const char *barcode)
{
    for (int i = 0; i < SCAN_FOODS_N; i++)
        if (strcmp(scan_foods[i].barcode, barcode) == 0)
            return i;
    return -1;
}

static void tap_scan(app_t *app, int x, int y)
{
    int st = scan_state(app);
    if (st == 1) {                       /* found card — dismiss to hub */
        app->screen = SCR_NUTRITION;
        return;
    }
    if (st == 2) {                       /* miss — retry entry */
        app->scan_t0 = 0;
        app->barcode_len = 0;
        app->barcode[0] = 0;
        return;
    }
    for (int i = 0; i < 12; i++) {
        int kx = 32 + (i % 3) * 60;
        int ky = 84 + (i / 3) * 32;
        if (!in_rect(x, y, kx, ky, 52, 26))
            continue;
        if (i == 9) {                    /* DEL */
            if (app->barcode_len > 0)
                app->barcode[--app->barcode_len] = 0;
            return;
        }
        if (i == 11) {                   /* GO — barcode lookup */
            app->barcode[app->barcode_len] = 0;
            int hit = scan_lookup(app->barcode);
            if (hit < 0) {
                app->scan_t0 = 1;        /* flags the not-found card */
                return;
            }
            const scan_food_t *f = &scan_foods[hit];
            app->scan_idx = hit;
            app->scan_logged = 1;
            app->kcal += f->kcal;
            app->protein_g += f->p;
            app->carbs_g += f->c;
            app->fat_g += f->f;
            app->water_ml += f->ml;
            snprintf(app->last_food, sizeof app->last_food, "%s", f->name);
            return;
        }
        if (app->barcode_len < 13) {
            app->barcode[app->barcode_len++] = (char)('0' + (i == 10 ? 0 : i + 1));
            app->barcode[app->barcode_len] = 0;
        }
        return;
    }
}

static void tap_settings(app_t *app, int x, int y)
{
    if (back_tapped(x, y)) {
        go_back(app);
        return;
    }
    for (int i = 0; i < 9; i++) {
        if (!in_rect(x, y, 24, 50 + i * 19, 192, 17))
            continue;
        switch (i) {
        case 0: app->dnd_on ^= 1; break;
        case 1: app->airplane_on ^= 1; break;
        case 2: app->bt_on ^= 1; break;
        case 3: app->torch_on ^= 1; break;
        case 4: app->water_lock_on ^= 1; break;
        case 5:
            app->brightness = app->brightness >= 100 ? 25
                                                     : app->brightness + 25;
            break;
        default: app->cc_hide ^= 1u << (i - 6); break;
        }
        return;
    }
}

static void tap_clock(app_t *app, int x, int y)
{
    if (back_tapped(x, y)) {
        go_back(app);
        return;
    }
    if (in_rect(x, y, 20, 48, 62, 22)) { app->clock_tab = 0; return; }
    if (in_rect(x, y, 89, 48, 62, 22)) { app->clock_tab = 1; return; }
    if (in_rect(x, y, 158, 48, 62, 22)) { app->clock_tab = 2; return; }
    if (app->clock_tab == 0) {
        if (in_rect(x, y, 30, 140, 84, 44)) {
            if (app->sw_running) {
                app->sw_acc += hal_millis() - app->sw_t0;
                app->sw_running = 0;
            } else {
                app->sw_t0 = hal_millis();
                app->sw_running = 1;
            }
        } else if (in_rect(x, y, 126, 140, 84, 44)) {
            app->sw_running = 0;
            app->sw_acc = 0;
        }
    } else if (app->clock_tab == 1) {
        static const int mins[3] = {1, 5, 10};
        if (app->timer_done) {
            if (in_rect(x, y, 60, 140, 120, 44))
                app->timer_done = 0;
            return;
        }
        if (app->timer_end_ms) {
            if (in_rect(x, y, 60, 140, 120, 44))
                app->timer_end_ms = 0;
            return;
        }
        for (int i = 0; i < 3; i++)
            if (in_rect(x, y, 24 + i * 66, 108, 60, 34)) {
                app->timer_end_ms = hal_millis() + (uint32_t)mins[i] * 60000u;
                app->timer_done = 0;
                return;
            }
    } else if (in_rect(x, y, 60, 140, 120, 44)) {
        app->alarm_on ^= 1;
    }
}

static void tap_back_only(app_t *app, int x, int y)
{
    if (back_tapped(x, y))
        go_back(app);
}

static void handle_tap(app_t *app, int x, int y)
{
    switch (app->screen) {
    case SCR_FACE: tap_face(app, x, y); break;
    case SCR_LAUNCHER: tap_launcher(app, x, y); break;
    case SCR_QUICK: tap_quick(app, x, y); break;
    case SCR_NUTRITION: tap_nutrition(app, x, y); break;
    case SCR_SCAN: tap_scan(app, x, y); break;
    case SCR_NOTIF: tap_notif(app, x, y); break;
    case SCR_REPLY: tap_reply(app, x, y); break;
    case SCR_STATS: tap_stats(app, x, y); break;
    case SCR_STAT_DETAIL: tap_stat_detail(app, x, y); break;
    case SCR_SETTINGS: tap_settings(app, x, y); break;
    case SCR_CLOCK: tap_clock(app, x, y); break;
    case SCR_WORKOUT_DONE:
    case SCR_SLEEP_DONE:
        tap_back_only(app, x, y);
        break;
    case SCR_WORKOUT: tap_workout(app, x, y); break;
    case SCR_WORKOUT_RUN: tap_workout_run(app, x, y); break;
    case SCR_SLEEP: tap_sleep(app, x, y); break;
    case SCR_WATER: tap_water(app, x, y); break;
    }
}

static void handle_swipe(app_t *app, app_swipe_t sw)
{
    /* control centre dismisses with a swipe up, like watchOS */
    if (app->screen == SCR_QUICK) {
        if (sw == SWIPE_UP)
            app->screen = SCR_FACE;
        return;
    }
    /* notifications: swipe left back to the face, swipe right clears */
    if (app->screen == SCR_NOTIF) {
        if (sw == SWIPE_LEFT)
            app->screen = SCR_FACE;
        else if (sw == SWIPE_RIGHT)
            app->notif_cleared = 1;
        return;
    }
    /* reply composer: swipe left back to the notification */
    if (app->screen == SCR_REPLY) {
        if (sw == SWIPE_LEFT)
            app->screen = SCR_NOTIF;
        return;
    }
    /* launcher: swipe right back to the face */
    if (app->screen == SCR_LAUNCHER) {
        if (sw == SWIPE_RIGHT)
            app->screen = SCR_FACE;
        return;
    }
    /* nutrition was entered with a swipe left — exit right, scan backs out */
    if (app->screen == SCR_NUTRITION) {
        if (sw == SWIPE_RIGHT)
            app->screen = SCR_FACE;
        return;
    }
    if (app->screen == SCR_SCAN) {
        app->screen = SCR_NUTRITION;
        return;
    }
    /* stats exits downward (it was entered with a swipe up) */
    if (app->screen == SCR_STATS) {
        if (sw == SWIPE_DOWN)
            app->screen = SCR_FACE;
        return;
    }
    if (app->screen == SCR_STAT_DETAIL) {
        if (sw == SWIPE_DOWN || sw == SWIPE_LEFT)
            app->screen = SCR_STATS;
        return;
    }
    if (app->screen != SCR_FACE)
        return;
    switch (sw) {
    case SWIPE_UP: app->screen = SCR_STATS; break;
    case SWIPE_DOWN: app->screen = SCR_QUICK; break;
    case SWIPE_LEFT: app->screen = SCR_NUTRITION; break;
    case SWIPE_RIGHT: app->screen = SCR_NOTIF; break;
    default: break;
    }
}

static app_swipe_t classify_swipe(int dx, int dy)
{
    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;
    if (ax <= APP_TAP_MAX_DIST && ay <= APP_TAP_MAX_DIST)
        return SWIPE_NONE;
    if (ax >= APP_SWIPE_MIN_DIST && ax > ay)
        return dx > 0 ? SWIPE_RIGHT : SWIPE_LEFT;
    if (ay >= APP_SWIPE_MIN_DIST && ay > ax)
        return dy > 0 ? SWIPE_DOWN : SWIPE_UP;
    return SWIPE_NONE;
}

void app_init(app_t *app)
{
    *app = (app_t){0};
    app->screen = SCR_FACE;
    app->settings_parent = SCR_LAUNCHER;
    app->brightness = 80;
    app->ping_t0 = 0xFFFFFFFFu;
    app->stat_pt = -1;
    app->sleep_track = 1;
    app->workout_routine = -1;
    hal_mark_all_dirty();
}

void app_handle_event(app_t *app, const hal_event_t *ev)
{
    /* any input may change the screen or state — repaint it all once.
       (Device HAL uses this to pick dirty tiles; sim ignores it.) */
    hal_mark_all_dirty();
    switch (ev->type) {
    case HAL_EV_QUIT:
        app->quit = 1;
        break;
    case HAL_EV_BUTTON:
        go_back(app);
        break;
    case HAL_EV_TOUCH_DOWN:
        app->touch_active = 1;
        app->touch_moved = 0;
        app->touch_start_x = app->touch_last_x = ev->x;
        app->touch_start_y = app->touch_last_y = ev->y;
        app->touch_down_ms = hal_millis();
        app->slider_drag = app->screen == SCR_QUICK &&
                           in_rect(ev->x, ev->y, 184, 12, 44, 90);
        break;
    case HAL_EV_TOUCH_MOVE:
        if (app->touch_active) {
            int dx = ev->x - app->touch_start_x;
            int dy = ev->y - app->touch_start_y;
            app->touch_last_x = ev->x;
            app->touch_last_y = ev->y;
            if (app->slider_drag) {
                int b = (94 - ev->y) * 100 / 68;
                app->brightness = b < 0 ? 0 : b > 100 ? 100 : b;
            }
            if (dx > APP_TAP_MAX_DIST || dx < -APP_TAP_MAX_DIST ||
                dy > APP_TAP_MAX_DIST || dy < -APP_TAP_MAX_DIST)
                app->touch_moved = 1;
        }
        break;
    case HAL_EV_TOUCH_UP:
        if (app->touch_active) {
            int dx = ev->x - app->touch_start_x;
            int dy = ev->y - app->touch_start_y;
            int ax = dx < 0 ? -dx : dx;
            int ay = dy < 0 ? -dy : dy;
            app->touch_active = 0;
            /* press-and-hold on the face is a peek gesture, not a nav tap;
               a slider drag never becomes a tap */
            int peek = app->screen == SCR_FACE &&
                       (int32_t)(hal_millis() - app->touch_down_ms) > 500;
            int skip = peek || app->slider_drag;
            app->slider_drag = 0;
            app_swipe_t sw = classify_swipe(dx, dy);
            if (!skip && sw != SWIPE_NONE)
                handle_swipe(app, sw);
            else if (!skip && !app->touch_moved &&
                     ax <= APP_TAP_MAX_DIST && ay <= APP_TAP_MAX_DIST)
                handle_tap(app, ev->x, ev->y);
        }
        break;
    default:
        break;
    }
}

static void fmt_mmss(char *buf, unsigned ms)
{
    unsigned s = ms / 1000;
    snprintf(buf, 12, "%u:%02u", s / 60, s % 60);
}

#define DIAL_CX 120
#define DIAL_CY 120
#define DIAL_R 113
#define NUM_R 94
#define NUM_PX 22
#define FACE_BG HAL_RGB565(8, 10, 18)
#define FACE_TICK HAL_RGB565(70, 78, 96)

static const int16_t sin60[60] = {
    0, 107, 213, 316, 416, 512, 602, 685, 761, 828,
    887, 935, 974, 1002, 1018, 1024, 1018, 1002, 974, 935,
    887, 828, 761, 685, 602, 512, 416, 316, 213, 107,
    0, -107, -213, -316, -416, -512, -602, -685, -761, -828,
    -887, -935, -974, -1002, -1018, -1024, -1018, -1002, -974, -935,
    -887, -828, -761, -685, -602, -512, -416, -316, -213, -107
};

/* Rounded-rectangle perimeter point: flat edges, circular corner arcs.
   R is the half-size; the corner radius shrinks as R is inset so inner
   tracks stay concentric. pos is in dial units, 60/rev, 0 = top. */
#define DIAL_SQ_R 118
#define SQ_CORNER 34.0f
void app_squircle_point(float pos, float R, int *x, int *y)
{
    float a = pos * 0.104719755f;
    float dx = sinf(a), dy = -cosf(a);         /* ray dir, y down */
    float rr = SQ_CORNER - (DIAL_SQ_R - R);
    if (rr < 4.0f) rr = 4.0f;
    if (rr > R)    rr = R;
    float iw = R - rr, ih = R - rr;            /* straight-run half-len */
    float sx = fabsf(dx) > 1e-5f ? R / fabsf(dx) : 1e9f;
    float sy = fabsf(dy) > 1e-5f ? R / fabsf(dy) : 1e9f;
    float s = sx < sy ? sx : sy;
    float px = dx * s, py = dy * s;
    if (fabsf(px) > iw && fabsf(py) > ih) {    /* ray lands in a corner */
        float ccx = px > 0.0f ? iw : -iw;
        float ccy = py > 0.0f ? ih : -ih;
        float ca = atan2f(py - ccy, px - ccx);
        px = ccx + rr * cosf(ca);
        py = ccy + rr * sinf(ca);
    }
    *x = DIAL_CX + (int)px;
    *y = DIAL_CY + (int)py;
}

/* arc-length lookup: pos is in dial units (60/rev). Evenly-spaced
   elements (ticks, numerals) must be placed by perimeter distance,
   not angle — on a squircle equal angles bunch up at the corners. */
#define SQ_LEN_STEPS 600
static float sq_len_tab[SQ_LEN_STEPS + 1];
static float sq_len_total;
static int sq_len_built;

static void sq_len_build(void)
{
    if (sq_len_built)
        return;
    int px = 0, py = 0;
    sq_len_tab[0] = 0.0f;
    for (int i = 1; i <= SQ_LEN_STEPS; i++) {
        int x, y;
        app_squircle_point((float)i * 60.0f / SQ_LEN_STEPS,
                           100.0f, &x, &y);
        float dx = (float)(x - px), dy = (float)(y - py);
        sq_len_tab[i] = sq_len_tab[i - 1] + sqrtf(dx * dx + dy * dy);
        px = x;
        py = y;
    }
    sq_len_total = sq_len_tab[SQ_LEN_STEPS];
    sq_len_built = 1;
}

/* dial pos (0..60) that sits at fraction f of the squircle perimeter */
float app_sq_pos_at(float f)
{
    sq_len_build();
    float target = f * sq_len_total;
    int lo = 0, hi = SQ_LEN_STEPS;
    while (lo + 1 < hi) {
        int mid = (lo + hi) / 2;
        if (sq_len_tab[mid] < target) lo = mid; else hi = mid;
    }
    float span = sq_len_tab[hi] - sq_len_tab[lo];
    float t = span > 0.001f ? (target - sq_len_tab[lo]) / span : 0.0f;
    return ((float)lo + t) * 60.0f / SQ_LEN_STEPS;
}

static int in_squircle(int x, int y, float R)
{
    float rr = SQ_CORNER - (DIAL_SQ_R - R);
    if (rr < 4.0f) rr = 4.0f;
    float iw = R - rr, ih = R - rr;
    float ax = fabsf((float)(x - DIAL_CX)), ay = fabsf((float)(y - DIAL_CY));
    if (ax > R || ay > R)
        return 0;
    float qx = ax - iw > 0.0f ? ax - iw : 0.0f;
    float qy = ay - ih > 0.0f ? ay - ih : 0.0f;
    return qx * qx + qy * qy <= rr * rr;
}

/* alpha pixel that only lands inside the dial squircle */
static void sq_alpha(int x, int y, int w, int h, hal_color_t c, unsigned a)
{
    if (in_squircle(x + w / 2, y + h / 2, DIAL_SQ_R - 1.0f))
        hal_draw_rect_a(x, y, w, h, c, a);
}

void app_dial_point_f(float pos, int radius, int *x, int *y)
{
    float a = pos * 0.104719755f;
    *x = DIAL_CX + (int)(sinf(a) * radius);
    *y = DIAL_CY - (int)(cosf(a) * radius);
}

void app_dial_point(int pos, int radius, int *x, int *y)
{
    pos = ((pos % 60) + 60) % 60;
    int s = sin60[pos];
    int c = sin60[(15 - pos + 60) % 60];
    *x = DIAL_CX + s * radius / 1024;
    *y = DIAL_CY - c * radius / 1024;
}

/* Filled convex quad with anti-aliased edges: interior emitted as solid
   scanline runs, boundary pixels get a coverage alpha (3x3 supersample).
   On hardware the same calls composite into the line buffer — no display
   readback needed. */
static void fill_quad(float x0, float y0, float x1, float y1,
                      float x2, float y2, float x3, float y3, hal_color_t c)
{
    const float xs[4] = {x0, x1, x2, x3};
    const float ys[4] = {y0, y1, y2, y3};
    int x_min = (int)floorf(fminf(fminf(x0, x1), fminf(x2, x3)));
    int x_max = (int)ceilf(fmaxf(fmaxf(x0, x1), fmaxf(x2, x3)));
    int y_min = (int)floorf(fminf(fminf(y0, y1), fminf(y2, y3)));
    int y_max = (int)ceilf(fmaxf(fmaxf(y0, y1), fmaxf(y2, y3)));
    for (int y = y_min; y < y_max; y++) {
        int run = -1;
        for (int x = x_min; x <= x_max; x++) {
            int hits = 0;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++) {
                    float px = x + (2.0f * sx + 1.0f) / 6.0f;
                    float py = y + (2.0f * sy + 1.0f) / 6.0f;
                    int side = 0, in = 1;
                    for (int e = 0; e < 4; e++) {
                        float cr = (xs[(e + 1) & 3] - xs[e]) * (py - ys[e])
                                 - (ys[(e + 1) & 3] - ys[e]) * (px - xs[e]);
                        if (cr < 0.0f)      { if (side > 0) { in = 0; break; } side = -1; }
                        else if (cr > 0.0f) { if (side < 0) { in = 0; break; } side =  1; }
                    }
                    hits += in;
                }
            if (hits == 9) {
                if (run < 0) run = x;
            } else {
                if (run >= 0) {
                    hal_draw_rect(run, y, x - run, 1, c);
                    run = -1;
                }
                if (hits > 0)
                    hal_draw_rect_a(x, y, 1, 1, c, hits * 255u / 9u);
            }
        }
        if (run >= 0)
            hal_draw_rect(run, y, x_max - run + 1, 1, c);
    }
}

static void fill_disc(int cx, int cy, int r, hal_color_t c)
{
    for (int y = -r - 1; y <= r + 1; y++) {
        int run = -1;
        for (int x = -r - 1; x <= r + 1; x++) {
            float dx = x + 0.5f, dy = y + 0.5f;
            float cov = (float)r + 0.5f - sqrtf(dx * dx + dy * dy);
            if (cov >= 1.0f) {
                if (run < 0) run = x;
            } else {
                if (run >= 0) {
                    rect(cx + run, cy + y, x - run, 1, c);
                    run = -1;
                }
                if (cov > 0.0f)
                    hal_draw_rect_a(cx + x, cy + y, 1, 1, c,
                                    (unsigned)(cov * 255.0f));
            }
        }
        if (run >= 0)
            rect(cx + run, cy + y, r + 2 - run, 1, c);
    }
}

/* Tapered hand: quad from radius r0 (full width w0) to tip r1 (width w1).
   pos is in dial units (60 per revolution) and may be fractional so hands
   can sweep continuously instead of stepping. */
static void hand(float pos, int r0, int r1, float w0, float w1, hal_color_t c)
{
    float a = pos * 0.104719755f;                       /* 2*pi/60 */
    float d = sinf(a);                                  /* dir.x = sin */
    float e = cosf(a);                                  /* dir.y = -cos */
    float x0 = DIAL_CX + d * r0, y0 = DIAL_CY - e * r0;
    float x1 = DIAL_CX + d * r1, y1 = DIAL_CY - e * r1;
    float px = e * 0.5f, py = d * 0.5f;                 /* perpendicular */
    /* dark keyline under the body so it reads machined, not flat */
    fill_quad(x0 - px * (w0 + 2.0f), y0 - py * (w0 + 2.0f),
              x0 + px * (w0 + 2.0f), y0 + py * (w0 + 2.0f),
              x1 + px * (w1 + 1.0f), y1 + py * (w1 + 1.0f),
              x1 - px * (w1 + 1.0f), y1 - py * (w1 + 1.0f),
              HAL_RGB565(6, 8, 14));
    fill_quad(x0 - px * w0, y0 - py * w0,
              x0 + px * w0, y0 + py * w0,
              x1 + px * w1, y1 + py * w1,
              x1 - px * w1, y1 - py * w1, c);
}

static hal_color_t dim565(hal_color_t c, float f);

static void face_num(int n, float pulse)
{
    /* cardinal numerals sit dead-centre on each squircle edge and
       throb with the heartbeat */
    static const int pos[4][2] = {
        {DIAL_CX, 28},                  /* 12 top    */
        {VOIDOS_WIDTH - 28, DIAL_CY},   /* 3  right  */
        {DIAL_CX, VOIDOS_HEIGHT - 28},  /* 6  bottom */
        {28, DIAL_CY},                  /* 9  left   */
    };
    int i = n == 12 ? 0 : n == 3 ? 1 : n == 6 ? 2 : 3;
    char buf[4];
    int w, h;
    snprintf(buf, sizeof buf, "%d", n);
    int x = pos[i][0], y = pos[i][1];
    hal_face_text_size(buf, NUM_PX, &w, &h);
    hal_face_draw_text(x - w / 2, y - h / 2, buf,
                       dim565(HAL_COLOR_WHITE, 0.80f + 0.20f * pulse),
                       NUM_PX);
}

/* small centred face label (minute markers, PM) */
static void face_small(int cx, int cy, const char *s)
{
    int w, h;
    hal_face_text_size(s, 9, &w, &h);
    hal_face_draw_text(cx - w / 2, cy - h / 2, s, HAL_COLOR_MUTED, 9);
}

static void draw_heart(int x, int y, int cell)
{
    static const char rows[6][8] = {
        ".XX.XX.",
        "XXXXXXX",
        "XXXXXXX",
        ".XXXXX.",
        "..XXX..",
        "...X..."
    };
    for (int r = 0; r < 6; r++)
        for (int c = 0; c < 7; c++)
            if (rows[r][c] == 'X')
                hal_draw_rect(x + c * cell, y + r * cell, cell, cell,
                              HAL_COLOR_RED);
}

/* scattered dim stars in the glass — static night sky */
static void draw_stars(void)
{
    if (!hal_render_static())
        return;
    for (int i = 0; i < 40; i++) {
        unsigned h = (unsigned)i * 2654435761u + 0x9E3779B9u;
        int x = (int)((h >> 3) % 220) + 10;
        int y = (int)((h >> 13) % 130) + 10;
        if (!in_squircle(x, y, DIAL_SQ_R - 14.0f))
            continue;
        int s = ((h >> 23) & 3u) == 3 ? 2 : 1;
        sq_alpha(x, y, s, s, HAL_RGB565(150, 160, 200), 110);
    }
}

/* soft radial glow: alpha falls off as (1-d/R)^2 — no hard edges *//* soft radial glow: alpha falls off as (1-d/R)^2 — no hard edges */
static void glow_disc(int cx, int cy, int R, int step,
                      hal_color_t c, int peak)
{
    for (int y = cy - R; y < cy + R; y += step) {
        for (int x = cx - R; x < cx + R; x += step) {
            float dx = (float)(x - cx), dy = (float)(y - cy);
            float d = sqrtf(dx * dx + dy * dy) / (float)R;
            if (d >= 1.0f)
                continue;
            float f = 1.0f - d;
            hal_draw_rect_a(x, y, step, step, c,
                            (unsigned)(peak * f * f));
        }
    }
}

/* soft ellipse — squircle-clipped, for wispy cloud puffs */
static void soft_ellipse(int cx, int cy, int rx, int ry,
                         hal_color_t c, int peak)
{
    for (int y = cy - ry; y <= cy + ry; y++)
        for (int x = cx - rx; x <= cx + rx; x++) {
            float nx = (float)(x - cx) / rx, ny = (float)(y - cy) / ry;
            float d = nx * nx + ny * ny;
            if (d >= 1.0f)
                continue;
            float f = 1.0f - d;
            sq_alpha(x, y, 1, 1, c, (unsigned)(peak * f * f));
        }
}



/* specular band following the squircle perimeter: pos centre ± span,
   alpha falls off angularly and across the band thickness */
static void sq_spec(float mid_pos, float half_pos, int peak, hal_color_t c)
{
    for (float pos = mid_pos - half_pos; pos <= mid_pos + half_pos;
         pos += 0.04f) {
        float t = (pos - mid_pos) / half_pos;
        float w = 1.0f - t * t;
        w *= w;
        for (int k = 0; k < 6; k++) {
            float g = sinf((float)k / 5.0f * 3.14159265f);
            int al = (int)(peak * w * g);
            if (al < 3)
                continue;
            int x, y;
            app_squircle_point(pos, DIAL_SQ_R - 9.0f - k, &x, &y);
            sq_alpha(x, y, 1, 1, c, (unsigned)al);
        }
    }
}

/* liquid-glass backdrop on a squircle dial: gradient fill, inner glow,
   specular bands hugging the perimeter, rim lit from above */
static void draw_face_bg(void)
{
    if (!hal_render_static())
        return;
    int night = hal_clock_hour() < 6 || hal_clock_hour() >= 19;
    /* day reads as deep blue glass; night stays near-black navy */
    float tr = night ? 14.0f : 26.0f, tg = night ? 20.0f : 42.0f;
    float tb = night ? 34.0f : 84.0f;
    for (int y = 0; y < VOIDOS_HEIGHT; y++) {
        rect(0, y, VOIDOS_WIDTH, 1, HAL_RGB565(4, 6, 11));   /* sky */
        float ady = fabsf((float)(y - DIAL_CY));
        if (ady > DIAL_SQ_R)
            continue;
        float iw = DIAL_SQ_R - SQ_CORNER;
        float xw;
        if (ady <= iw)
            xw = DIAL_SQ_R;
        else {
            float q = ady - iw;
            xw = iw + sqrtf(SQ_CORNER * SQ_CORNER - q * q);
        }
        float t = (float)y / (float)VOIDOS_HEIGHT;
        rect(DIAL_CX - (int)xw, y, (int)(xw * 2.0f) + 1, 1,
             HAL_RGB565((int)(tr * (1.0f - 0.65f * t)),
                        (int)(tg * (1.0f - 0.65f * t)),
                        (int)(tb * (1.0f - 0.65f * t))));
    }

    /* light pooled in the upper half, feathered to nothing */
    for (int y = 10; y < 175; y += 3)
        for (int x = 20; x < 220; x += 3) {
            float dx = (float)(x - 96), dy = (float)(y - 55);
            float d = sqrtf(dx * dx + dy * dy) / 105.0f;
            if (d >= 1.0f)
                continue;
            float f = 1.0f - d;
            sq_alpha(x, y, 3, 3, HAL_RGB565(90, 115, 150),
                     (unsigned)(52.0f * f * f));
        }

    /* crisp reflection arcing across the top edge, faint bottom bounce */
    sq_spec(0.0f, 8.5f, 120, HAL_COLOR_WHITE);
    sq_spec(30.0f, 6.0f, 40, HAL_RGB565(140, 160, 190));

    /* inner shadow vignette pooling just inside the squircle edge */
    for (float pos = 0.0f; pos < 60.0f; pos += 0.06f)
        for (int k = 0; k < 7; k++) {
            int al = (int)(80.0f * (1.0f - (float)k / 7.0f));
            int x, y;
            app_squircle_point(pos, DIAL_SQ_R - 2.5f - k, &x, &y);
            sq_alpha(x, y, 1, 1, HAL_COLOR_BLACK, (unsigned)al);
        }

    /* squircle rim, lit from above */
    for (float pos = 0.0f; pos < 60.0f; pos += 0.025f) {
        int x, y;
        app_squircle_point(pos, DIAL_SQ_R - 1.5f, &x, &y);
        float l = 0.5f + 0.5f * cosf(pos * 0.104719755f);
        unsigned v = (unsigned)(34.0f + 96.0f * l);
        rect(x, y, 1, 1, HAL_RGB565(v, v + 6, v + 20));
    }
}

/* day sky pass: sun + clouds wrap continuously across the whole face
   and are drawn last, so they drift over the hands */
static void draw_day_sky(uint32_t ms)
{
    /* blurry sun — a soft warm bloom, no hard edge */
    glow_disc(190, 56, 46, 2, HAL_RGB565(250, 210, 120), 70);
    glow_disc(190, 56, 26, 2, HAL_RGB565(255, 225, 150), 130);
    soft_ellipse(190, 56, 12, 12, HAL_RGB565(255, 240, 190), 220);

    /* clouds wrap around the screen: each has its own speed/shape */
    static const int cy[4] = {58, 100, 176, 150};
    static const int rx[4] = {34, 22, 18, 42};
    static const int ry[4] = {12, 13, 8, 9};
    static const float sp[4] = {0.006f, 0.009f, 0.012f, 0.0075f};
    static const int ph[4] = {0, 150, 60, 220};
    for (int i = 0; i < 4; i++) {
        int span = VOIDOS_WIDTH + 2 * rx[i];
        int cx = (int)fmodf(ph[i] + (float)ms * sp[i], (float)span) - rx[i];
        soft_ellipse(cx, cy[i], rx[i], ry[i],
                     HAL_RGB565(150, 168, 195), 55);
        soft_ellipse(cx - rx[i] / 3, cy[i] - ry[i] / 2,
                     rx[i] * 2 / 3, ry[i],
                     HAL_RGB565(180, 198, 225), 60);
    }
}

/* digital face: one big Kenney time, lap-colour rim that fills once a
   minute, seconds and BPM. Deliberately cheap to draw — it has to run on
   a 64 MHz M4 over a 1-bit-per-clock SPI link. */
/* 2-px-thick straight segment, integer stepped */
static void seg_line(int x0, int y0, int x1, int y1, hal_color_t c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    if (steps <= 0) {
        rect(x0, y0, 2, 2, c);
        return;
    }
    for (int i = 0; i <= steps; i++)
        rect(x0 + dx * i / steps, y0 + dy * i / steps, 2, 2, c);
}

static void render_face_digital(app_t *app)
{
    (void)app;
    draw_face_bg();
    int hour = hal_clock_hour();
    int minute = hal_clock_minute();
    uint32_t ms = hal_millis();
    int night = hour < 6 || hour >= 19;
    if (night)
        draw_stars();

    /* perimeter lap ring, same colour cycle as the analogue face */
    static const hal_color_t lap[6] = {
        HAL_COLOR_RED,            HAL_RGB565(150, 60, 220),
        HAL_COLOR_ACCENT,         HAL_COLOR_GREEN,
        HAL_RGB565(235, 200, 40), HAL_RGB565(235, 110, 25),
    };
    int second = hal_clock_second();
    hal_color_t cur = lap[minute % 6], prev = lap[(minute + 5) % 6];
    int px0 = 0, py0 = 0, first = 1;
    for (float pos = 0.0f; pos <= 60.0f; pos += 0.1f) {
        int x, y;
        app_squircle_point(pos, DIAL_SQ_R - 7.5f, &x, &y);
        hal_color_t tc = pos <= (float)second ? cur : prev;
        if (first) {
            first = 0;
            px0 = x;
            py0 = y;
        } else {
            seg_line(px0, py0, x, y, tc);
            px0 = x;
            py0 = y;
        }
    }

    /* big time — digits drawn in fixed slots, the colon blinks in place
       so nothing slides in and out */
    char hh[4], mm[4];
    snprintf(hh, sizeof hh, "%02d", hour);
    snprintf(mm, sizeof mm, "%02d", minute);
    int wH = 0, wM = 0, wC = 0, th = 0;
    hal_face_text_size(hh, 56, &wH, &th);
    hal_face_text_size(mm, 56, &wM, &th);
    hal_face_text_size(":", 56, &wC, &th);
    int tx = (VOIDOS_WIDTH - wH - wC - wM) / 2, ty = 92 - th / 2;
    hal_face_draw_text(tx, ty, hh, HAL_COLOR_WHITE, 56);
    if (!((ms / 500) & 1))
        hal_face_draw_text(tx + wH, ty, ":", HAL_COLOR_WHITE, 56);
    hal_face_draw_text(tx + wH + wC, ty, mm, HAL_COLOR_WHITE, 56);

    /* seconds, small, under the time */
    char sstr[4];
    snprintf(sstr, sizeof sstr, "%02d", second);
    int sw = 0, sh = 0;
    hal_face_text_size(sstr, 24, &sw, &sh);
    hal_face_draw_text((VOIDOS_WIDTH - sw) / 2, 116, sstr,
                       HAL_COLOR_MUTED, 24);

    /* ECG trace: a heartbeat shape that sweeps the strip once per beat,
       R-spike hitting at the pulse */
    static const int8_t ecg[24] = {
        0, 0, -1, -2, -1, 0, 2, -8, 15, -5, 2, 0,
        1, 3, 4, 3, 1, 0, 0, 0, 0, 0, 0, 0
    };
    int bpm = 72 + (int)(5.0f * sinf((float)ms * 0.00043f));
    float beat = fmodf((float)ms * (float)bpm / 60000.0f, 1.0f);
    float pulse = beat < 0.25f ? sinf(beat / 0.25f * 3.14159265f) : 0.0f;
    int phase = (int)(beat * 24.0f);
    int ey = 154;
    int py = ey;
    for (int x = 58; x <= 182; x++) {
        int idx = (x / 3 + phase) % 24;
        if (idx < 0)
            idx += 24;
        int yy = ey - ecg[idx];
        int lo = py < yy ? py : yy, hi = py < yy ? yy : py;
        rect(x, lo, 2, hi - lo + 1, HAL_COLOR_ACCENT);
        py = yy;
    }

    /* BPM: halo centred on the heart, and the heart itself brightens
       with the beat */
    char bpm_str[16];
    snprintf(bpm_str, sizeof bpm_str, "%d BPM", bpm);
    int bw = 0, bh = 0;
    hal_face_text_size(bpm_str, 16, &bw, &bh);
    int bx = (VOIDOS_WIDTH - (16 + bw)) / 2;
    int by = 176;
    if (pulse > 0.01f)
        fill_disc(bx + 7, by + 6, 9 + (int)(4.0f * pulse),
                  HAL_RGB565(70, 14, 14));
    {
        int cell = 2;
        static const char rows[6][8] = {
            ".XX.XX.", "XXXXXXX", "XXXXXXX", ".XXXXX.", "..XXX..", "...X..."
        };
        hal_color_t hc = HAL_RGB565(230, (int)(60.0f + 90.0f * pulse),
                                    (int)(60.0f + 90.0f * pulse));
        for (int r = 0; r < 6; r++)
            for (int c = 0; c < 7; c++)
                if (rows[r][c] == 'X')
                    hal_draw_rect(bx + c * cell, by + r * cell, cell, cell, hc);
    }
    hal_face_draw_text(bx + 18, by, bpm_str, HAL_COLOR_WHITE, 16);

    /* steps line, small, under the complication */
    int steps = 3200 + (int)(ms / 6000);
    char step_str[16];
    snprintf(step_str, sizeof step_str, "%d STEPS", steps);
    int stw = 0, sth = 0;
    hal_face_text_size(step_str, 9, &stw, &sth);
    hal_face_draw_text((VOIDOS_WIDTH - stw) / 2, 198, step_str,
                       HAL_COLOR_MUTED, 9);
}

static void render_face_analog(app_t *app)
{
    draw_face_bg();
    int hour = hal_clock_hour();
    int minute = hal_clock_minute();
    uint32_t ms = hal_millis();
    int night = hour < 6 || hour >= 19;
    if (night)
        draw_stars();
    /* all three hands sweep continuously */
    float second = hal_clock_second() + hal_clock_millis() / 1000.0f;
    float min_f = (float)minute + second / 60.0f;
    float hp = (float)(hour % 12) * 5.0f + min_f / 12.0f;
    /* perimeter second ring — fills behind the second hand and resets
       each minute. The hue cycles red>purple>blue>green>yellow>orange
       once a minute; last lap's colour survives until overwritten */
    static const hal_color_t lap[6] = {
        HAL_COLOR_RED,            HAL_RGB565(150, 60, 220),
        HAL_COLOR_ACCENT,         HAL_COLOR_GREEN,
        HAL_RGB565(235, 200, 40), HAL_RGB565(235, 110, 25),
    };
    int lap_i = hal_clock_minute() % 6;
    hal_color_t cur = lap[lap_i], prev = lap[(lap_i + 5) % 6];
    for (float pos = 0.0f; pos < 60.0f; pos += 0.05f) {
        hal_color_t tc = pos <= second ? cur : prev;
        int x, y;
        app_squircle_point(pos, DIAL_SQ_R - 7.0f, &x, &y);
        rect(x, y, 1, 1, tc);
        app_squircle_point(pos, DIAL_SQ_R - 8.0f, &x, &y);
        rect(x, y, 1, 1, tc);
    }
    /* heartbeat pulse drives the numeral throb and the halo */
    int bpm_pre = 72 + (int)(5.0f * sinf((float)ms * 0.00043f));
    float beat = fmodf((float)ms * (float)bpm_pre / 60000.0f, 1.0f);
    float pulse = beat < 0.25f ? sinf(beat / 0.25f * 3.14159265f) : 0.0f;
    face_num(12, pulse);
    face_num(3, pulse);
    face_num(6, pulse);
    face_num(9, pulse);
    /* small minute labels on the inner side of each cardinal */
    face_small(DIAL_CX, 48, "60");                    /* below 12 */
    face_small(DIAL_CX, VOIDOS_HEIGHT - 48, "30");    /* above 6  */
    face_small(VOIDOS_WIDTH - 50, DIAL_CY, "15");     /* left of 3 */
    face_small(50, DIAL_CY, "45");                    /* right of 9 */
    if (hour >= 12) {                                 /* PM under 3/6/9 */
        face_small(VOIDOS_WIDTH - 28, DIAL_CY + 18, "PM");
        face_small(DIAL_CX, VOIDOS_HEIGHT - 26, "PM");
        face_small(28, DIAL_CY + 18, "PM");
    }

    /* press-and-hold greys the hands so the complication shows through */
    int peek = app->touch_active;
    hal_color_t hc = peek ? FACE_TICK : HAL_COLOR_WHITE;
    hal_color_t sc = peek ? HAL_RGB565(60, 90, 95) : HAL_COLOR_ACCENT;
    hand(hp, -10, 58, 10.0f, 1.5f, hc);
    hand(min_f, -12, 88, 6.0f, 1.0f, hc);
    hand(second, -20, 102, 2.4f, 0.6f, sc);
    int cwx = DIAL_CX - (int)(sinf(second * 0.104719755f) * 16.0f);
    int cwy = DIAL_CY + (int)(cosf(second * 0.104719755f) * 16.0f);
    fill_disc(cwx, cwy, 4, sc);
    fill_disc(DIAL_CX, DIAL_CY, 6, sc);
    fill_disc(DIAL_CX, DIAL_CY, 3, FACE_BG);
    fill_disc(DIAL_CX, DIAL_CY, 1, HAL_COLOR_WHITE);

    /* centred complication; keeps clear of the hour hand's half of the dial */
    int minutes = (hour % 12) * 60 + minute;
    int bpm_y = minutes >= 180 && minutes <= 540 ? 54 : 168;

    int bpm = bpm_pre;

    char bpm_str[16];
    snprintf(bpm_str, sizeof bpm_str, "%d BPM", bpm);
    int bpm_tw = 0, bpm_th = 0;
    hal_face_text_size(bpm_str, 13, &bpm_tw, &bpm_th);
    int bpm_x = DIAL_CX - (16 + bpm_tw) / 2;
    int heart_y = bpm_y + (bpm_th - 12) / 2;

    /* pulse halo synced to the beat, then the heart glyph itself */
    if (pulse > 0.01f)
        fill_disc(bpm_x + 7, heart_y + 6, 9 + (int)(4.0f * pulse),
                  HAL_RGB565(70, 14, 14));
    draw_heart(bpm_x, heart_y, 2);
    hal_face_draw_text(bpm_x + 18, bpm_y, bpm_str, HAL_COLOR_WHITE, 13);

    if (!night)
        draw_day_sky(ms);
}

static void render_launcher(app_t *app)
{
    static const char *names[6] = {
        "WORKOUT", "SLEEP", "WATER", "STATS", "SETTINGS", "CLOCK"
    };
    (void)app;
    draw_face_bg();
    text_center(32, "APPS", HAL_COLOR_MUTED, 1);
    for (int i = 0; i < 6; i++) {
        int cx, cy;
        launcher_pos(i, &cx, &cy);
        hex_btn(cx, cy, 30, 0);
        int tw = font_text_width(names[i], 1);
        text(cx - tw / 2, cy - 4, names[i], HAL_COLOR_WHITE, 1);
    }
}

/* glassy round button: drop shadow, tinted base, sheen, lit rim */
/* pointy-top hexagon button: tessellates edge-to-edge in a honeycomb */
static void hex_btn(int cx, int cy, float R, hal_color_t tint)
{
    int on = tint != 0;
    hal_color_t base = on ? tint : HAL_RGB565(30, 36, 48);
    const float HW = R * 0.866f;            /* apothem */

    /* soft drop shadow beneath */
    glow_disc(cx, cy + (int)(R * 0.23f), (int)(R * 1.05f), 2,
              HAL_COLOR_BLACK, 110);

    /* fill, inset 1 px so the rim reads as a border */
    for (int dy = -(int)R + 1; dy <= (int)R - 1; dy++) {
        float ay = fabsf((float)dy) + 1.0f;
        float hw = ay <= R / 2.0f ? HW
                 : HW * (2.0f - 2.0f * ay / R);
        if (hw < 0.5f)
            continue;
        rect(cx - (int)hw, cy + dy, (int)(hw * 2.0f) + 1, 1, base);
    }

    /* faint sheen in the upper half */
    glow_disc(cx - (int)(R * 0.2f), cy - (int)(R * 0.47f), (int)(R * 0.45f),
              1, HAL_COLOR_WHITE, on ? 55 : 40);

    /* rim: outer edge lit from above + a dim inner edge for depth */
    const float vx[6] = {0, HW, HW, 0, -HW, -HW};
    const float vy[6] = {-R, -R / 2, R / 2, R, R / 2, -R / 2};
    for (int e = 0; e < 6; e++) {
        int n = (e + 1) % 6;
        float my = (vy[e] + vy[n]) * 0.5f;
        float l = 0.5f - my / R * 0.5f;      /* brighter toward the top */
        unsigned v = (unsigned)(50.0f + 170.0f * l * l);
        hal_color_t rc = HAL_RGB565(v, v + 8, v + 22);
        for (int i = 0; i <= 60; i++) {
            float t = (float)i / 60.0f;
            rect(cx + (int)(vx[e] + (vx[n] - vx[e]) * t),
                 cy + (int)(vy[e] + (vy[n] - vy[e]) * t), 1, 1, rc);
            rect(cx + (int)((vx[e] + (vx[n] - vx[e]) * t) * 0.93f),
                 cy + (int)((vy[e] + (vy[n] - vy[e]) * t) * 0.93f),
                 1, 1, dim565(rc, 0.55f));
        }
    }
}

/* half-width of a capsule row: flat middle, semicircular caps */
static float capw(float dy, float h, float r)
{
    if (dy < r)
        return sqrtf(r * r - (r - dy) * (r - dy));
    if (dy > h - r) {
        float d = dy - (h - r);
        return sqrtf(r * r - d * d);
    }
    return r;
}

/* vertical capsule pill: shell outline, dark interior, fill from bottom */
static void vcap(int cx, int y0, int w, int h, hal_color_t shell,
                 int pct, hal_color_t fc)
{
    float r = w * 0.5f;
    for (int i = 0; i < h; i++) {
        float hw = capw(i + 0.5f, (float)h, r);
        int iw = (int)ceilf(hw);
        rect(cx - iw, y0 + i, iw * 2, 1, shell);
    }
    for (int i = 1; i < h - 1; i++) {
        float hw = capw(i + 0.5f, (float)h, r) - 1.0f;
        if (hw <= 0.5f)
            continue;
        rect(cx - (int)hw, y0 + i, (int)(hw * 2.0f), 1,
             HAL_RGB565(12, 15, 22));
    }
    int fh = (h - 4) * pct / 100;
    for (int i = h - 2 - fh; i < h - 2; i++) {
        float hw = capw(i + 0.5f, (float)h, r) - 2.0f;
        if (hw <= 0.5f)
            continue;
        rect(cx - (int)hw, y0 + i, (int)(hw * 2.0f), 1, fc);
    }
}

static void render_quick(app_t *app)
{
    /* same squircle glass panel as the watch face */
    draw_face_bg();

    int batt = 87;
    char buf[16];
    snprintf(buf, sizeof buf, "%d%%", batt);

    /* matching capsule pills flanking the honeycomb's top rows */
    hal_color_t shell = HAL_RGB565(70, 78, 90);
    vcap(38, 22, 16, 72, shell, batt, HAL_RGB565(60, 200, 120));
    hal_draw_icon(HAL_ICON_BATTERY, 52, 34, 16, 220);
    text(72, 38, buf, HAL_COLOR_MUTED, 1);
    vcap(202, 22, 16, 72, shell, app->brightness, HAL_COLOR_ACCENT);
    hal_draw_icon(HAL_ICON_SUN, 168, 34, 16, 220);

    /* glass toggles — tinted when active, watchOS colours */
    hal_color_t dnd_c  = app->dnd_on        ? HAL_RGB565(110, 70, 200) : 0;
    hal_color_t air_c  = app->airplane_on   ? HAL_RGB565(220, 130, 30) : 0;
    hal_color_t bt_c   = app->bt_on         ? HAL_RGB565(40, 110, 220) : 0;
    hal_color_t tor_c  = app->torch_on      ? HAL_RGB565(200, 160, 30) : 0;
    hal_color_t wat_c  = app->water_lock_on ? HAL_RGB565(30, 150, 190) : 0;

    /* tessellating honeycomb: rows 50 apart, cells 57 apart, staggered 28 */
    hex_btn(91, 82, 30, dnd_c);
    hal_draw_icon(app->dnd_on ? HAL_ICON_DND : HAL_ICON_DND_OFF,
                  91 - 13, 82 - 13, 26, 255);
    hex_btn(149, 82, 30, air_c);
    hal_draw_icon(app->airplane_on ? HAL_ICON_AIRPLANE
                                   : HAL_ICON_AIRPLANE_OFF,
                  149 - 13, 82 - 13, 26, 255);

    hex_btn(63, 132, 30, bt_c);
    hal_draw_icon(app->bt_on ? HAL_ICON_BT : HAL_ICON_BT_OFF,
                  63 - 13, 132 - 13, 26, 255);
    if (!(app->cc_hide & 1)) {
        hex_btn(120, 132, 30, tor_c);
        hal_draw_icon(app->torch_on ? HAL_ICON_TORCH : HAL_ICON_TORCH_OFF,
                      120 - 13, 132 - 13, 26, 255);
    }
    if (!(app->cc_hide & 4)) {
        hex_btn(177, 132, 30, 0);
        hal_draw_icon(HAL_ICON_PHONE, 177 - 13, 132 - 13, 26, 255);
    }

    if (!(app->cc_hide & 2)) {
        hex_btn(91, 182, 30, wat_c);
        hal_draw_icon(HAL_ICON_WATER, 91 - 13, 182 - 13, 26, 255);
    }
    hex_btn(149, 182, 30, 0);
    hal_draw_icon(HAL_ICON_SETTINGS, 149 - 13, 182 - 13, 26, 255);

    /* find-phone ping: expanding accent ripple for ~900 ms */
    uint32_t pdt = hal_millis() - app->ping_t0;
    if (pdt < 900) {
        int r = 8 + (int)(pdt * 60 / 900);
        for (int i = 0; i < 360; i += 4) {
            float a = i * 0.0174533f;
            sq_alpha(177 + (int)(r * cosf(a)), 132 + (int)(r * sinf(a)),
                     1, 1, HAL_COLOR_ACCENT, 160 - (int)(pdt * 140 / 900));
        }
    }
}

/* rounded-rect fill rows (quarter-circle corners, radius r) */
static void rr_fill(int x, int y, int w, int h, int r, hal_color_t c)
{
    for (int i = 0; i < h; i++) {
        float hw = w * 0.5f;
        float d = i < r ? r - (i + 0.5f) : i >= h - r ? (i + 0.5f) - (h - r) : 0;
        if (d > 0.0f)
            hw = w * 0.5f - r + sqrtf((float)r * r - d * d);
        int iw = (int)hw;
        rect(x + w / 2 - iw, y + i, iw * 2, 1, c);
    }
}

/* glass notification card: shadow, body, top sheen */
static void notif_card(int x, int y, int w, int h)
{
    glow_disc(x + w / 2, y + h + 4, w / 2, 3, HAL_COLOR_BLACK, 70);
    rr_fill(x, y, w, h, 12, HAL_RGB565(30, 36, 48));
    for (int i = 2; i < w - 2; i++) {       /* lit top edge */
        float cxp = (float)i - w / 2.0f;
        unsigned a = (unsigned)(120.0f / (1.0f + cxp * cxp / 900.0f));
        hal_draw_rect_a(x + i, y + 1, 1, 1, HAL_COLOR_WHITE, a);
    }
}

static void render_notif(app_t *app)
{
    draw_face_bg();

    text_center(32, "NOTIFICATIONS", HAL_COLOR_MUTED, 1);

    if (app->notif_cleared) {
        text_center(104, "INBOX ZERO", HAL_COLOR_WHITE, 1);
        text_center(120, "ALL CAUGHT UP", HAL_COLOR_MUTED, 1);
        return;
    }

    notif_card(24, 56, 192, 68);
    fill_disc(38, 70, 3, HAL_COLOR_ACCENT);   /* unread dot */
    hal_draw_icon(HAL_ICON_PHONE, 50, 78, 24, 230);
    text(84, 66, "MESSAGES", HAL_COLOR_MUTED, 1);
    text(84, 82, "MUM: DINNER AT 7?", HAL_COLOR_WHITE, 1);
    text(84, 98, "2M AGO", HAL_COLOR_MUTED, 1);
    text(170, 66, "SWIPE >", HAL_COLOR_MUTED, 1);

    /* glass CLEAR pill at the bottom */
    rr_fill(70, 188, 100, 26, 13, HAL_RGB565(30, 36, 48));
    text(88, 197, "CLEAR", HAL_COLOR_WARN, 1);

}

/* honeycomb key position: rows of 6-5-6-5-6, short rows offset half-cell */
static void key_pos(int i, int *x, int *y)
{
    static const int row_len[5] = {6, 5, 6, 5, 6};
    int row = 0, col = i;
    while (row < 5 && col >= row_len[row]) {
        col -= row_len[row];
        row++;
    }
    *x = 48 + col * 27 + (row_len[row] == 5 ? 14 : 0);
    *y = 72 + row * 24;
}

/* glass action pill: shadow, body, lit top edge, centred label */
static void action_pill(int x, int y, int w, const char *label, int accent)
{
    hal_color_t base = accent ? HAL_COLOR_ACCENT : HAL_RGB565(30, 36, 48);
    glow_disc(x + w / 2, y + 14, w / 2, 2, HAL_COLOR_BLACK, 80);
    rr_fill(x, y, w, 22, 11, base);
    /* lit top edge */
    for (int i = 4; i < w - 4; i++) {
        float cxp = (float)i - w / 2.0f;
        unsigned a = (unsigned)(110.0f / (1.0f + cxp * cxp / 400.0f));
        hal_draw_rect_a(x + i, y + 1, 1, 1,
                        accent ? HAL_COLOR_BLACK : HAL_COLOR_WHITE,
                        accent ? a / 3 : a);
    }
    int tw = font_text_width(label, 1);
    text(x + (w - tw) / 2, y + 8, label,
         accent ? HAL_COLOR_BLACK : HAL_COLOR_MUTED, 1);
}

/* small glass back chip, top-left inside the bezel */
static void back_chip(void)
{
    rr_fill(BACK_X, BACK_Y, BACK_W, BACK_H, 9, HAL_RGB565(30, 36, 48));
    text(BACK_X + 9, BACK_Y + 6, "<", HAL_COLOR_MUTED, 1);
}

/* big glass menu button; accent=1 fills accent with a dark label */
static void menu_btn(int x, int y, int w, int h, const char *label, int accent)
{
    glow_disc(x + w / 2, y + h / 2 + 5, w / 3, 2, HAL_COLOR_BLACK, 70);
    rr_fill(x, y, w, h, h / 2 > 14 ? 14 : h / 2,
            accent ? HAL_COLOR_ACCENT : HAL_RGB565(30, 36, 48));
    for (int i = 8; i < w - 8; i++) {
        float cxp = (float)i - w / 2.0f;
        unsigned a = (unsigned)(90.0f / (1.0f + cxp * cxp / 700.0f));
        hal_draw_rect_a(x + i, y + 1, 1, 1,
                        accent ? HAL_COLOR_BLACK : HAL_COLOR_WHITE,
                        accent ? a / 3 : a);
    }
    int sc = h >= 46 ? 2 : 1;
    int tw = font_text_width(label, sc);
    text(x + (w - tw) / 2, y + (h - 7 * sc) / 2, label,
         accent ? HAL_COLOR_BLACK : HAL_COLOR_WHITE, sc);
}

/* deterministic fake series, one row per stat, values 0..1 */
static float stat_val_norm(int sel, int i)
{
    return 0.25f + 0.7f * fabsf(sinf(i * 0.71f + sel * 1.31f));
}

/* plausible full-scale per stat for the tooltip value */
static int stat_val_at(int sel, int i)
{
    static const int scale[6] = {700, 140, 70, 80, 9, 600};
    return (int)(stat_val_norm(sel, i) * scale[sel] + 0.5f);
}

static void stat_pt_pos(const app_t *app, int i, int *x, int *y)
{
    int n = app->stat_range ? 7 : 24;
    int bw = 192 / n;
    *x = 24 + i * bw + bw / 2;
    *y = 168 - 2 - (int)(stat_val_norm(app->stat_sel, i) * 92.0f);
}

/* thin line segment between two points */
static void seg(float x0, float y0, float x1, float y1, float w,
                hal_color_t c)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f)
        return;
    float px = -dy / len * w * 0.5f, py = dx / len * w * 0.5f;
    fill_quad(x0 - px, y0 - py, x0 + px, y0 + py,
              x1 + px, y1 + py, x1 - px, y1 - py, c);
}

/* stat detail: title, 24H/7D pills, tappable line chart */
static void render_stat_detail(app_t *app)
{
    static const char *names[6] = {
        "STEPS", "HEART", "RESTING HR", "HRV", "SLEEP", "ENERGY"
    };
    static const char *units[6] = {
        "", " BPM", " BPM", " MS", " HRS", " KCAL"
    };
    draw_face_bg();
    text_center(30, names[app->stat_sel], HAL_COLOR_MUTED, 1);

    action_pill(76, 42, 44, "24H", app->stat_range == 0);
    action_pill(122, 42, 44, "7D", app->stat_range == 1);

    int n = app->stat_range ? 7 : 24;

    /* gapless columns, each one = one period */
    int bw = 192 / n;
    for (int i = 0; i < n; i++) {
        int px, py;
        stat_pt_pos(app, i, &px, &py);
        hal_color_t c = i == app->stat_pt ? HAL_COLOR_WHITE
                        : dim565(HAL_COLOR_ACCENT,
                                 0.35f + 0.55f * stat_val_norm(app->stat_sel, i));
        hal_draw_rect(24 + i * bw, py, bw, 168 - py, c);
    }
    hal_draw_rect(24, 168, 192, 1, HAL_RGB565(50, 60, 75));

    /* axis labels */
    hal_color_t ax = HAL_RGB565(80, 92, 110);
    text(24, 176, app->stat_range ? "-7D" : "-24H", ax, 1);
    text_center(176, app->stat_range ? "-3D" : "-12H", ax, 1);
    text(216 - 18, 176, "NOW", ax, 1);

    /* floating tooltip over the selected point */
    if (app->stat_pt >= 0 && app->stat_pt < n) {
        int px, py;
        stat_pt_pos(app, app->stat_pt, &px, &py);
        int bx = px - 34;
        if (bx < 24)
            bx = 24;
        if (bx > 152)
            bx = 152;
        int by = py - 44;
        if (by < 64)
            by = py + 12;
        glow_disc(bx + 34, by + 34, 30, 2, HAL_COLOR_BLACK, 80);
        rr_fill(bx, by, 68, 30, 8, HAL_RGB565(36, 44, 58));
        seg((float)(bx + 34), (float)(by + 30), (float)px, (float)py,
            1.0f, HAL_RGB565(70, 80, 96));
        char l1[12], l2[16];
        if (app->stat_range)
            snprintf(l1, sizeof l1, app->stat_pt == n - 1
                     ? "TODAY" : "-%d DAYS", n - 1 - app->stat_pt);
        else
            snprintf(l1, sizeof l1, "%02d:00",
                     (hal_clock_hour() - (n - 1 - app->stat_pt) + 48) % 24);
        snprintf(l2, sizeof l2, "%d%s", stat_val_at(app->stat_sel,
                 app->stat_pt), units[app->stat_sel]);
        text(bx + 6, by + 4, l1, HAL_COLOR_MUTED, 1);
        text(bx + 6, by + 16, l2, HAL_COLOR_WHITE, 1);
    }

    text_center(200, "TAP A POINT", HAL_COLOR_MUTED, 1);
}

static void render_reply(app_t *app)
{
    static const char keys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.,";
    draw_face_bg();

    /* message field */
    rr_fill(20, 24, 200, 26, 10, HAL_RGB565(20, 25, 34));
    text(202, 33, "X", HAL_COLOR_MUTED, 1);   /* cancel */
    if (app->reply_len)
        text(28, 33, app->reply_buf, HAL_COLOR_WHITE, 1);
    else
        text(28, 33, "REPLY...", HAL_COLOR_MUTED, 1);

    /* honeycomb keys */
    for (int i = 0; i < 28; i++) {
        int kx, ky;
        key_pos(i, &kx, &ky);
        hex_btn(kx, ky, 15, 0);
        char ch[2] = {keys[i], '\0'};
        text(kx - 3, ky - 4, ch, HAL_COLOR_WHITE, 1);
    }

    /* action row: glass pills, send lit accent */
    action_pill(20, 200, 58, "DEL", 0);
    action_pill(88, 200, 58, "SPC", 0);
    action_pill(156, 200, 64, "SEND", 1);
}

/* small glass stat card: label above value */
static void stat_card(int x, int y, const char *label, const char *value,
                      hal_color_t vc)
{
    glow_disc(x + 48, y + 44, 44, 2, HAL_COLOR_BLACK, 60);
    rr_fill(x, y, 96, 44, 10, HAL_RGB565(30, 36, 48));
    for (int i = 6; i < 90; i++) {          /* lit top edge */
        float cxp = (float)i - 48.0f;
        unsigned a = (unsigned)(90.0f / (1.0f + cxp * cxp / 700.0f));
        hal_draw_rect_a(x + i, y + 1, 1, 1, HAL_COLOR_WHITE, a);
    }
    text(x + 8, y + 7, label, HAL_COLOR_MUTED, 1);
    text(x + 8, y + 23, value, vc, 1);
}

static void render_stats(app_t *app)
{
    char buf[24];
    draw_face_bg();
    text_center(32, "STATS", HAL_COLOR_MUTED, 1);

    /* same live BPM model as the watch face */
    uint32_t ms = hal_millis();
    int bpm = 72 + (int)(5.0f * sinf((float)ms * 0.00043f));

    /* steps accumulate with run time to fake the pedometer */
    int steps = 4321 + (int)(ms / 8000u);
    int kcal = 180 + steps * 4 / 100;

    snprintf(buf, sizeof buf, "%d", steps);
    stat_card(18, 44, "STEPS", buf, HAL_COLOR_WHITE);
    snprintf(buf, sizeof buf, "%d BPM", bpm);
    stat_card(126, 44, "HEART", buf, HAL_COLOR_RED);

    snprintf(buf, sizeof buf, "%d BPM", bpm - 14);
    stat_card(18, 94, "RESTING HR", buf, HAL_COLOR_WHITE);
    int hrv = 48 + (int)(14.0f * sinf((float)ms * 0.00031f));
    snprintf(buf, sizeof buf, "%d MS", hrv);
    stat_card(126, 94, "HRV", buf, HAL_COLOR_ACCENT);

    stat_card(18, 144, "SLEEP", "7H 24M", HAL_COLOR_WHITE);
    snprintf(buf, sizeof buf, "%d KCAL", kcal);
    stat_card(126, 144, "ENERGY", buf, HAL_COLOR_WARN);

    /* workout status strip */
    rr_fill(18, 196, 204, 26, 10, HAL_RGB565(30, 36, 48));
    if (app->workout_active) {
        snprintf(buf, sizeof buf, "WORKOUT LIVE - %d SETS", app->sets_done);
        text(28, 205, buf, HAL_COLOR_ACCENT, 1);
    } else {
        text(28, 205, "WORKOUT", HAL_COLOR_MUTED, 1);
        text(150, 205, "NO SAVE", HAL_COLOR_MUTED, 1);
    }
}

static void render_nutrition(app_t *app)
{
    char buf[24];
    draw_face_bg();
    text_center(30, "NUTRITION", HAL_COLOR_MUTED, 1);

    /* today's calories */
    snprintf(buf, sizeof buf, "%d", app->kcal);
    text_center(44, buf, HAL_COLOR_WHITE, 3);
    text_center(72, "KCAL TODAY", HAL_COLOR_MUTED, 1);

    /* macro cards P / C / F */
    static const char mlab[3] = {'P', 'C', 'F'};
    int mval[3] = {app->protein_g, app->carbs_g, app->fat_g};
    for (int i = 0; i < 3; i++) {
        int x = 26 + i * 65;
        rr_fill(x, 90, 58, 30, 9, HAL_RGB565(26, 33, 44));
        hal_draw_rect(x + 2, 90, 54, 1, HAL_RGB565(50, 60, 76));
        snprintf(buf, sizeof buf, "%c %d", mlab[i], mval[i]);
        text(x + 10, 101, buf, HAL_COLOR_WHITE, 1);
    }

    /* water card — tap adds 250 ml (liquids credit hydration, like gym-void) */
    rr_fill(26, 130, 188, 38, 10, HAL_RGB565(26, 33, 44));
    hal_draw_rect(28, 130, 184, 1, HAL_RGB565(50, 60, 76));
    hal_draw_icon(HAL_ICON_WATER, 38, 141, 16, 255);
    snprintf(buf, sizeof buf, "%d ML", app->water_ml);
    text(62, 145, buf, HAL_COLOR_WHITE, 1);
    text(174, 145, "+250", HAL_COLOR_ACCENT, 1);

    /* barcode entry — the watch has no camera, digits are typed */
    action_pill(70, 178, 100, "ADD FOOD", 1);
    text_center(212, app->last_food[0] ? app->last_food : "NOTHING LOGGED",
                HAL_COLOR_MUTED, 1);
}

/* barcode entry states: 0 typing, 1 found, 2 not in database */
static int scan_state(const app_t *app)
{
    if (app->scan_logged)
        return 1;
    return app->scan_t0 ? 2 : 0;
}

static void render_scan(app_t *app)
{
    char buf[32];
    draw_face_bg();
    text_center(30, "BARCODE", HAL_COLOR_MUTED, 1);
    int st = scan_state(app);

    if (st == 0) {
        /* digit entry field */
        rr_fill(30, 48, 180, 26, 8, HAL_RGB565(20, 25, 34));
        hal_draw_rect(32, 49, 176, 1, HAL_RGB565(50, 60, 76));
        if (app->barcode_len)
            text(120 - app->barcode_len * 3, 57, app->barcode,
                 HAL_COLOR_WHITE, 1);
        else
            text_center(57, "ENTER BARCODE", HAL_COLOR_MUTED, 1);

        /* numeric keypad, 3x4 glass keys */
        static const char *keys[12] = {
            "1", "2", "3", "4", "5", "6", "7", "8", "9", "DEL", "0", "GO"
        };
        for (int i = 0; i < 12; i++) {
            int kx = 32 + (i % 3) * 60;
            int ky = 84 + (i / 3) * 32;
            int go = i == 11;
            glow_disc(kx + 26, ky + 13, 28, 2, HAL_COLOR_BLACK, 70);
            rr_fill(kx, ky, 52, 26, 8,
                    go ? HAL_COLOR_ACCENT : HAL_RGB565(30, 38, 50));
            hal_draw_rect(kx + 2, ky + 1, 48, 1,
                          go ? dim565(HAL_COLOR_ACCENT, 0.5f)
                             : HAL_RGB565(58, 70, 88));
            int tw = font_text_width(keys[i], 1);
            text(kx + (52 - tw) / 2, ky + 10, keys[i],
                 go ? HAL_COLOR_BLACK : HAL_COLOR_WHITE, 1);
        }
        text_center(224, "SWIPE BACK TO CANCEL", HAL_COLOR_GRAY, 1);
        return;
    }

    if (st == 2) {
        rr_fill(30, 84, 180, 64, 12, HAL_RGB565(28, 36, 48));
        text_center(100, "NOT IN DATABASE", HAL_COLOR_WARN, 1);
        text_center(120, app->barcode, HAL_COLOR_MUTED, 1);
        text_center(180, "TAP TO RETRY", HAL_COLOR_GRAY, 1);
        return;
    }

    const scan_food_t *f = &scan_foods[app->scan_idx];
    glow_disc(120, 118, 92, 2, HAL_COLOR_BLACK, 80);
    rr_fill(30, 66, 180, 104, 12, HAL_RGB565(28, 36, 48));
    hal_draw_rect(32, 67, 176, 1, HAL_RGB565(60, 72, 90));
    text_center(80, f->name, HAL_COLOR_WHITE, 1);
    snprintf(buf, sizeof buf, "%d KCAL", f->kcal);
    text_center(98, buf, HAL_COLOR_ACCENT, 2);
    snprintf(buf, sizeof buf, "P%d  C%d  F%d", f->p, f->c, f->f);
    text_center(122, buf, HAL_COLOR_MUTED, 1);
    if (f->ml) {
        snprintf(buf, sizeof buf, "+%d ML WATER", f->ml);
        text_center(140, buf, HAL_RGB565(90, 140, 220), 1);
    }
    text_center(154, "LOGGED", HAL_RGB565(110, 220, 140), 1);
    text_center(196, "TAP TO CONTINUE", HAL_COLOR_GRAY, 1);
}

static const char *workout_names[3] = {"PUSH DAY", "PULL DAY", "LEGS"};

static void render_workout(app_t *app)
{
    (void)app;
    draw_face_bg();
    back_chip();
    text_center(30, "WORKOUT", HAL_COLOR_MUTED, 1);

    /* routine quick-starts — stand-ins for gym-void routine sync */
    for (int i = 0; i < 3; i++) {
        int y = 48 + i * 24;
        rr_fill(30, y, 180, 20, 8, HAL_RGB565(26, 33, 44));
        hal_draw_rect(32, y + 1, 176, 1, HAL_RGB565(50, 60, 76));
        text(42, y + 6, workout_names[i], HAL_COLOR_WHITE, 1);
        text(196, y + 6, ">", HAL_COLOR_MUTED, 1);
    }
    menu_btn(30, 124, 180, 54, "QUICK SESSION", 1);
    text_center(204, "GYM-VOID ROUTINES - SIM", HAL_COLOR_GRAY, 1);
}

static void render_workout_run(app_t *app)
{
    char buf[24];
    draw_face_bg();
    text_center(30, app->workout_routine >= 0
                    ? workout_names[app->workout_routine] : "QUICK SESSION",
                HAL_COLOR_MUTED, 1);

    uint32_t now = hal_millis();
    uint32_t elapsed = now - app->workout_start_ms - app->paused_total_ms;
    if (app->workout_paused)
        elapsed -= now - app->pause_start_ms;
    fmt_mmss(buf, elapsed);
    text_center(50, buf, HAL_COLOR_WHITE, 3);

    /* set pips */
    for (int i = 0; i < APP_WORKOUT_SETS; i++)
        rr_fill(70 + i * 36, 86, 28, 10, 5,
                i < app->sets_done ? HAL_COLOR_GREEN : HAL_RGB565(30, 36, 48));

    if (app->sets_done < APP_WORKOUT_SETS) {
        snprintf(buf, sizeof buf, "SET %d DONE", app->sets_done + 1);
        menu_btn(20, 100, 200, 55, buf, 1);
    } else {
        menu_btn(20, 100, 200, 55, "FINISH", 1);
    }
    menu_btn(20, 170, 95, 45, app->workout_paused ? "RESUME" : "PAUSE", 0);
    menu_btn(125, 170, 95, 45, "END", 0);

    uint32_t ref = app->workout_paused ? app->pause_start_ms : hal_millis();
    if (app->workout_paused)
        text_center(220, "PAUSED", HAL_COLOR_WARN, 1);
    else if (app->rest_end_ms && (int32_t)(app->rest_end_ms - ref) > 0) {
        unsigned rem = (app->rest_end_ms - ref) / 1000;
        snprintf(buf, sizeof buf, "REST %uS", rem);
        text_center(220, buf, HAL_COLOR_WARN, 1);
    }
}

static void render_workout_done(app_t *app)
{
    char buf[24];
    draw_face_bg();
    back_chip();
    text_center(30, "DONE", HAL_COLOR_MUTED, 1);
    glow_disc(120, 120, 92, 2, HAL_COLOR_BLACK, 70);
    rr_fill(30, 60, 180, 108, 12, HAL_RGB565(28, 36, 48));
    hal_draw_rect(32, 61, 176, 1, HAL_RGB565(60, 72, 90));
    text_center(74, "SESSION COMPLETE", HAL_RGB565(110, 220, 140), 1);
    text_center(92, app->workout_routine >= 0
                    ? workout_names[app->workout_routine] : "QUICK SESSION",
                HAL_COLOR_MUTED, 1);
    snprintf(buf, sizeof buf, "SETS  %d", app->sets_done);
    text_center(112, buf, HAL_COLOR_WHITE, 2);
    fmt_mmss(buf, app->last_workout_ms);
    text_center(138, buf, HAL_COLOR_WHITE, 2);
    text_center(196, "SIMULATED - NO SAVE", HAL_COLOR_GRAY, 1);
}

static void render_sleep(app_t *app)
{
    char buf[24];
    draw_face_bg();
    back_chip();
    text_center(30, "SLEEP", HAL_COLOR_MUTED, 1);

    /* last-night card with stage split */
    rr_fill(24, 48, 192, 52, 10, HAL_RGB565(26, 33, 44));
    hal_draw_rect(26, 49, 188, 1, HAL_RGB565(50, 60, 76));
    if (app->in_bed) {
        fmt_mmss(buf, hal_millis() - app->bed_start_ms);
        text(34, 58, "IN BED", HAL_COLOR_ACCENT, 1);
        text(34, 76, buf, HAL_COLOR_WHITE, 1);
    } else {
        text(34, 56, "LAST NIGHT", HAL_COLOR_MUTED, 1);
        text(34, 70, "7H 24M", HAL_COLOR_WHITE, 2);
        text(138, 60, "DEEP 1H52", HAL_COLOR_MUTED, 1);
        text(138, 74, "REM 1H20", HAL_COLOR_MUTED, 1);
    }

    if (app->sleep_track)
        menu_btn(40, 110, 160, 55, app->in_bed ? "WAKE UP" : "BEDTIME",
                 !app->in_bed);
    else
        text_center(128, "TRACKING OFF", HAL_COLOR_WARN, 1);

    /* tracking toggle */
    rr_fill(40, 178, 160, 26, 13, HAL_RGB565(30, 36, 48));
    text(54, 187, "TRACKING", HAL_COLOR_MUTED, 1);
    text(160, 187, app->sleep_track ? "ON" : "OFF",
         app->sleep_track ? HAL_COLOR_ACCENT : HAL_COLOR_MUTED, 1);
    text_center(216, "STAGES SIMULATED", HAL_COLOR_GRAY, 1);
}

static void render_sleep_done(app_t *app)
{
    char buf[24];
    draw_face_bg();
    back_chip();
    text_center(30, "MORNING", HAL_COLOR_MUTED, 1);
    glow_disc(120, 116, 92, 2, HAL_COLOR_BLACK, 70);
    rr_fill(30, 60, 180, 104, 12, HAL_RGB565(28, 36, 48));
    hal_draw_rect(32, 61, 176, 1, HAL_RGB565(60, 72, 90));
    text_center(76, "TIME IN BED", HAL_COLOR_ACCENT, 1);
    fmt_mmss(buf, app->last_in_bed_ms);
    text_center(96, buf, HAL_COLOR_WHITE, 3);
    text_center(132, "DEEP 1H52  REM 1H20", HAL_COLOR_MUTED, 1);
    text_center(196, "STAGES SIMULATED", HAL_COLOR_GRAY, 1);
}

static void render_water(app_t *app)
{
    char buf[24];
    draw_face_bg();
    back_chip();
    text_center(30, "WATER", HAL_COLOR_MUTED, 1);

    /* bottle gauge filling toward the 2 l goal */
    int pct = app->water_ml * 100 / 2000;
    if (pct > 100)
        pct = 100;
    rr_fill(96, 48, 48, 66, 10, HAL_RGB565(40, 48, 62));
    int fh = 60 * pct / 100;
    uint32_t ms = hal_millis();
    for (int i = 0; i < fh; i++) {
        int y = 108 - i;
        /* wobble the surface rows */
        int wob = i > fh - 4
            ? (int)(sinf((float)(ms / 90 + i) * 0.6f) * 2.0f) : 0;
        hal_draw_rect(98 + wob, y, 44, 1, HAL_RGB565(40, 130, 220));
    }
    /* rising bubbles */
    for (int i = 0; i < 4; i++) {
        int by = 108 - (int)(((ms / 40u) + i * 53u) % (unsigned)(fh > 8 ? fh : 8));
        if (by > 52 + 4 && by < 108)
            hal_draw_rect(102 + (i * 11) % 34, by, 2, 2,
                          HAL_RGB565(120, 190, 255));
    }
    snprintf(buf, sizeof buf, "%d ML", app->water_ml);
    text_center(168, buf, HAL_COLOR_WHITE, 1);
    text_center(182, "GOAL 2000", HAL_COLOR_MUTED, 1);

    menu_btn(15, 198, 100, 32, "+250ML", 0);
    menu_btn(125, 198, 100, 32, "+500ML", 1);
}

static void render_settings(app_t *app)
{
    char buf[8];
    static const char *labels[9] = {
        "DO NOT DISTURB", "AIRPLANE", "BLUETOOTH", "TORCH",
        "WATER LOCK", "BRIGHTNESS", "CC: TORCH", "CC: WATER",
        "CC: PHONE"
    };
    draw_face_bg();
    back_chip();
    text_center(30, "SETTINGS", HAL_COLOR_MUTED, 1);
    for (int i = 0; i < 9; i++) {
        int y = 50 + i * 19;
        rr_fill(24, y, 192, 17, 6, HAL_RGB565(26, 33, 44));
        text(32, y + 5, labels[i], HAL_COLOR_MUTED, 1);
        if (i == 5) {
            snprintf(buf, sizeof buf, "%d%%", app->brightness);
            text(180, y + 5, buf, HAL_COLOR_WHITE, 1);
        } else {
            int on = i == 0 ? app->dnd_on
                   : i == 1 ? app->airplane_on
                   : i == 2 ? app->bt_on
                   : i == 3 ? app->torch_on
                   : i == 4 ? app->water_lock_on
                   : !(app->cc_hide & (1 << (i - 6)));
            const char *v = i >= 6 ? (on ? "SHOW" : "HIDE")
                                   : (on ? "ON" : "OFF");
            int vw = font_text_width(v, 1);
            text(210 - vw, y + 5, v,
                 on ? HAL_COLOR_ACCENT : HAL_COLOR_MUTED, 1);
        }
    }
}

static void render_clock(app_t *app)
{
    char buf[24];
    draw_face_bg();
    back_chip();
    text_center(30, "CLOCK", HAL_COLOR_MUTED, 1);

    action_pill(20, 48, 62, "WATCH", app->clock_tab == 0);
    action_pill(89, 48, 62, "TIMER", app->clock_tab == 1);
    action_pill(158, 48, 62, "ALARM", app->clock_tab == 2);

    if (app->clock_tab == 0) {
        /* stopwatch */
        uint32_t el = app->sw_acc +
            (app->sw_running ? hal_millis() - app->sw_t0 : 0);
        fmt_mmss(buf, el);
        text_center(88, buf, HAL_COLOR_WHITE, 3);
        menu_btn(30, 140, 84, 44,
                 app->sw_running ? "STOP" : "START", !app->sw_running);
        menu_btn(126, 140, 84, 44, "RESET", 0);
    } else if (app->clock_tab == 1) {
        /* countdown timer */
        if (app->timer_done) {
            text_center(88, "TIME'S UP", HAL_COLOR_WARN, 2);
            menu_btn(60, 140, 120, 44, "DISMISS", 1);
        } else if (app->timer_end_ms) {
            uint32_t left = app->timer_end_ms - hal_millis();
            fmt_mmss(buf, left);
            text_center(88, buf, HAL_COLOR_ACCENT, 3);
            menu_btn(60, 140, 120, 44, "CANCEL", 0);
        } else {
            text_center(80, "SET TIMER", HAL_COLOR_MUTED, 1);
            menu_btn(24, 108, 60, 34, "1M", 0);
            menu_btn(90, 108, 60, 34, "5M", 0);
            menu_btn(156, 108, 60, 34, "10M", 0);
        }
    } else {
        /* alarm */
        text_center(88, "07:00", HAL_COLOR_WHITE, 3);
        menu_btn(60, 140, 120, 44, app->alarm_on ? "ON" : "OFF",
                 app->alarm_on);
        text_center(196, "TAP TO TOGGLE", HAL_COLOR_GRAY, 1);
    }
}

/* Regions of each screen that animate between frames, for the device
   HAL's dirty-tile pass. Anything state-driven is already covered by the
   mark-all on input; these are the parts that move on their own. */
static void mark_animated(app_t *app)
{
    switch (app->screen) {
    case SCR_FACE:
        if (app->analog_face) {
            hal_mark_dirty(20, 20, 200, 200);          /* hands + pulse */
        } else {
            hal_mark_dirty(20, 55, 200, 70);           /* big time */
            hal_mark_dirty(95, 108, 50, 30);           /* seconds */
            hal_mark_dirty(50, 135, 140, 40);          /* ECG trace */
            hal_mark_dirty(70, 168, 110, 30);          /* BPM */
            hal_mark_dirty(70, 194, 110, 18);          /* steps */
        }
        {
            int s = hal_clock_second();
            for (float p = (float)s - 0.3f; p <= (float)s + 0.3f; p += 0.1f) {
                int x, y;
                app_squircle_point(p, DIAL_SQ_R - 7.5f, &x, &y);
                hal_mark_dirty(x - 5, y - 5, 11, 11);
            }
        }
        break;
    case SCR_STATS:
        hal_mark_dirty(114, 52, 110, 64);               /* live heart card */
        hal_mark_dirty(16, 52, 96, 64);                 /* live steps card */
        break;
    case SCR_STAT_DETAIL:
        hal_mark_dirty(20, 186, 200, 34);               /* live NOW value */
        break;
    case SCR_WORKOUT_RUN:
        hal_mark_dirty(30, 66, 180, 56);                /* timer */
        hal_mark_dirty(60, 136, 120, 36);               /* set pips */
        break;
    case SCR_SLEEP:
        if (app->in_bed)
            hal_mark_dirty(30, 80, 180, 60);            /* elapsed timer */
        break;
    case SCR_WATER:
        hal_mark_dirty(60, 52, 120, 124);               /* bottle surface */
        break;
    case SCR_CLOCK:
        hal_mark_dirty(20, 60, 200, 70);                /* time displays */
        break;
    default:
        break;
    }
}

void app_render(app_t *app)
{
    /* countdown expiry is global — fires regardless of the visible screen */
    if (app->timer_end_ms && hal_millis() >= app->timer_end_ms) {
        app->timer_end_ms = 0;
        app->timer_done = 1;
    }
    /* screen switch: repaint + rebuild the device's static-layer cache */
    {
        static int last_screen = -1;
        if ((int)app->screen != last_screen) {
            last_screen = app->screen;
            hal_mark_screen();
        }
    }
    mark_animated(app);
    switch (app->screen) {
    case SCR_FACE:
        if (app->analog_face)
            render_face_analog(app);
        else
            render_face_digital(app);
        break;
    case SCR_LAUNCHER: render_launcher(app); break;
    case SCR_QUICK: render_quick(app); break;
    case SCR_NOTIF: render_notif(app); break;
    case SCR_REPLY: render_reply(app); break;
    case SCR_STATS: render_stats(app); break;
    case SCR_STAT_DETAIL: render_stat_detail(app); break;
    case SCR_NUTRITION: render_nutrition(app); break;
    case SCR_SCAN: render_scan(app); break;
    case SCR_WORKOUT: render_workout(app); break;
    case SCR_WORKOUT_RUN: render_workout_run(app); break;
    case SCR_WORKOUT_DONE: render_workout_done(app); break;
    case SCR_SLEEP: render_sleep(app); break;
    case SCR_SLEEP_DONE: render_sleep_done(app); break;
    case SCR_WATER: render_water(app); break;
    case SCR_SETTINGS: render_settings(app); break;
    case SCR_CLOCK: render_clock(app); break;
    }
    /* brightness slider actually dims the panel */
    if (app->brightness < 100)
        hal_draw_rect_a(0, 0, VOIDOS_WIDTH, VOIDOS_HEIGHT,
                        HAL_COLOR_BLACK, (unsigned)(100 - app->brightness) * 2u);
    if (app->torch_on && app->screen == SCR_QUICK)
        hal_draw_rect_a(0, 0, VOIDOS_WIDTH, VOIDOS_HEIGHT,
                        HAL_COLOR_WHITE, 220);
    if (app->touch_active)
        rect(app->touch_last_x - 4, app->touch_last_y - 4, 9, 9, HAL_COLOR_WARN);
}

/* ------------------------------------------------------------------ boot */

#define BOOT_MS 8000

/* scale an RGB565 colour toward black by f in [0,1] — used for fades */
static hal_color_t dim565(hal_color_t c, float f)
{
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    unsigned r = (unsigned)(((c >> 8) & 0xF8) * f);
    unsigned g = (unsigned)(((c >> 3) & 0xFC) * f);
    unsigned b = (unsigned)(((c << 3) & 0xF8) * f);
    return HAL_RGB565(r, g, b);
}

static void ring_dot(int x, int y, hal_color_t c)
{
    rect(x - 1, y - 1, 3, 3, c);
}

/* progress traced along the perimeter of a rounded rect, like PineTime */
static void boot_progress_ring(float p, hal_color_t c)
{
    const int ins = 7, r = 22;
    int x0 = ins, y0 = ins;
    int x1 = VOIDOS_WIDTH - ins - 1, y1 = VOIDOS_HEIGHT - ins - 1;
    int seg = (x1 - x0) - 2 * r;
    float corner = (float)r * 1.5707963f;
    float total = 4.0f * (seg + (int)corner);
    float target = p * total, s = 0;
    static const float a0[4] = {-1.5707963f, 0.0f, 1.5707963f, 3.14159265f};
    for (int side = 0; side < 4 && s < target; side++) {
        for (int i = 0; i < seg && s < target; i++, s += 1.0f) {
            int x, y;
            switch (side) {
            case 0: x = x0 + r + i; y = y0; break;
            case 1: x = x1;         y = y0 + r + i; break;
            case 2: x = x1 - r - i; y = y1; break;
            default: x = x0;        y = y1 - r - i; break;
            }
            ring_dot(x, y, c);
        }
        float cx = (side < 2) ? x1 - r : x0 + r;
        float cy = (side == 0 || side == 3) ? y0 + r : y1 - r;
        for (int i = 0; i <= (int)corner && s < target; i++, s += 1.0f) {
            float a = a0[side] + (float)i / corner * 1.5707963f;
            ring_dot((int)(cx + (float)r * cosf(a)),
                     (int)(cy + (float)r * sinf(a)), c);
        }
    }
}

static void render_boot(uint32_t t)
{
    rect(0, 0, VOIDOS_WIDTH, VOIDOS_HEIGHT, HAL_COLOR_BLACK);

    /* global fade-out over the last 500 ms */
    float out = t > BOOT_MS - 500 ? (float)(BOOT_MS - t) / 500.0f : 1.0f;

    /* logo fades in over the first 600 ms; shimmer sweeps it every 2.4 s */
    float a = t < 600 ? (float)t / 600.0f : 1.0f;
    int shimmer = -1;
    if (t >= 600 && t < BOOT_MS - 500)
        shimmer = (int)(-40.0f + fmodf((float)t, 2400.0f) / 2400.0f * 340.0f);
    hal_draw_logo(56, 52, 112, 112, (unsigned)(a * out * 255.0f), shimmer);

    /* progress ring around the screen edge, sweeping 500-7000 ms */
    float p = t < 500 ? 0.0f : (float)(t - 500) / 6500.0f;
    if (p > 1.0f) p = 1.0f;
    boot_progress_ring(1.0f, dim565(HAL_COLOR_GRAY, out));
    if (p > 0.0f)
        boot_progress_ring(p, dim565(HAL_COLOR_ACCENT, out));

    hal_color_t lc = dim565(HAL_COLOR_MUTED, a * out);
    int tw = font_text_width("LOADING", 1);
    text((VOIDOS_WIDTH - tw) / 2, 192, "LOADING", lc, 1);
}

static void boot_sequence(app_t *app)
{
    uint32_t start = hal_millis();
    while (!app->quit) {
        hal_event_t ev;
        while (hal_poll_event(&ev)) {
            if (ev.type == HAL_EV_QUIT) {
                app->quit = 1;
                return;
            }
            if (ev.type == HAL_EV_TOUCH_DOWN || ev.type == HAL_EV_BUTTON)
                return;    /* tap or button skips the boot animation */
        }
        uint32_t t = hal_millis() - start;
        if (t >= BOOT_MS)
            return;
        render_boot(t);
        hal_present();
        hal_delay_ms(16);
    }
}

void app_run(app_t *app, int max_frames)
{
    uint32_t wake = 0;
    if (max_frames < 0) {
        boot_sequence(app);    /* cinematic boot only in interactive runs */
        wake = hal_millis();
    }
    int frames = 0;
    while (!app->quit && (max_frames < 0 || frames < max_frames)) {
        hal_event_t ev;
        while (hal_poll_event(&ev))
            app_handle_event(app, &ev);
        if (app->quit)
            break;
        app_render(app);
        uint32_t wt = hal_millis() - wake;
        if (wake && wt < 450) {
            /* gentle crossfade: logo dissolves as the face fades in
               from black */
            float f = (float)wt / 450.0f;
            f = f * f * (3.0f - 2.0f * f);             /* smoothstep */
            hal_draw_logo(56, 52, 112, 112,
                          (unsigned)((1.0f - f) * 255.0f), -1);
            hal_draw_rect_a(0, 0, VOIDOS_WIDTH, VOIDOS_HEIGHT,
                            HAL_COLOR_BLACK,
                            (unsigned)((1.0f - f) * 255.0f));
        }
        hal_present();
        hal_delay_ms(16);
        frames++;
    }
}
