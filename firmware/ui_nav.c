/* VoidOS device UI: navigation and screens on LVGL.
   One root container; screens rebuild inside it. Only wired-up controls
   exist — everything a button shows is real. */
#include <math.h>
#include <string.h>
#include "lvgl.h"
#include "voidos/hal.h"
#include "board.h"
#include "ui_deco.h"
#include "phone.h"

#if defined(VOIDOS_DEVICE) && __has_include("gen/version.h")
#include "gen/version.h"         /* generated from the Makefile's VERSION */
#endif
#ifndef VOIDOS_VERSION
#define VOIDOS_VERSION "dev"
#endif

void display_backlight(int level);
void display_sleep(int on);
void touch_sleep(int on);
void ui_face_build(lv_obj_t *root);
void ui_face_breath_stop(void);
void ui_face_tick(void);
void sensors_tick(uint32_t now);
int sensors_bma_ok(void);
int sensors_bma_z(void);
int sensors_hrs_ok(void);
int sensors_steps(void);
int sensors_bpm(void);
int sensors_bpm_stale(void);   /* 1 = held last reading, show greyed */
#define BPM_STALE_RGB 0x5C6678
int hal_clock_hour(void);
int hal_clock_minute(void);
void hal_clock_set(int y, int mo, int d, int dow, int h, int mi, int s);
void hal_clock_date(int *y, int *mo, int *d, int *dow);
int hal_clock_synced(void);
uint32_t hal_clock_tod_ms(void);
extern const lv_font_t kenney9, kenney12, kenney16, kenney24, kenney56;

/* ---- state -------------------------------------------------------------- */

typedef enum {
    SCR_FACE, SCR_LAUNCHER, SCR_CC, SCR_WORKOUT, SCR_W_RUN, SCR_W_DONE,
    SCR_SLEEP, SCR_S_DONE, SCR_WATER, SCR_CLOCK, SCR_SETTINGS, SCR_STATS,
    SCR_SETTIME, SCR_NOTIF, SCR_CARDIO, SCR_DIAG, SCR_WEATHER, SCR_MUSIC
} scr_t;

static scr_t cur = SCR_FACE;
static lv_obj_t *root;

/* control centre */
static int brightness = 75;    /* 0-100 */
static int torch_on, water_lock;
static int dnd_on, airplane_on;
static unsigned cc_hide;          /* 1=torch 2=water lock 4=find phone */
static int bt_on = 1;

int ble_connected(void);
void ble_set_enabled(int on);
int ble_status(void);
void ble_diag(int *syncs, int *adv_calls, int *adv_rc, int *gap_events,
              int *addr_type);

/* radio follows BT toggle AND airplane mode, from every entry point */
static void apply_radio(void)
{
    ble_set_enabled(bt_on && !airplane_on);
}

/* workout */
static int w_active, w_paused, w_sets, w_last_sets;
static uint32_t w_start_ms, w_pause_ms, w_paused_total, w_last_ms;
static int w_routine = -1;
#define W_MAX_SETS 3

/* sleep */
static int s_track = 1, s_in_bed;
static uint32_t s_start_ms, s_last_ms;

/* water */
static int water_ml;

/* clock: 0 stopwatch, 1 timer, 2 alarm */
static int clock_tab;
static int sw_running;
static uint32_t sw_t0, sw_acc;
static uint32_t timer_end_ms;
static int timer_done;
static int resting_bpm;         /* lowest BPM seen this session */
static int alarm_on = 1;
static int alarm_h = 7, alarm_m = 0;
static int alarm_ringing;
static int alarm_fired_min = -1;    /* minute-of-day already fired */

/* watchface style: 0 = analogue, 1 = digital (concept art) */
static int face_dig = 1;
int ui_face_digital(void) { return face_dig; }

/* navigation */
static scr_t back_target = SCR_FACE;

/* the clock came from the last save, not the phone: it is behind by
   however long the watch was off or flashing. The face says so until a
   phone sync or a manual SET TIME. */
int hal_clock_restored(void);
void hal_clock_restore(int y, int mo, int d, int dow, int h, int mi, int s);
int ui_clock_stale(void) { return hal_clock_restored(); }

/* ---- persistence: settings + step history on external SPI flash ---- */
void spi_flash_init(void);
void spi_flash_read(uint32_t, uint8_t *, unsigned);
void spi_flash_write(uint32_t, const uint8_t *, unsigned);
void spi_flash_sector_erase(uint32_t);
int sensors_steps(void);
int sensors_wrist_raise(void);
int sensors_raise_wake(void);
int sensors_ppg(int i, int *out_y100);
int sensors_hr_on(void);
void sensors_hr_set(int);
int hal_battery_pct(void);
void hal_motor(int);
void ble_set_compat(int on);
int ble_get_compat(void);
int phone_cts_writes(void);

static uint32_t motor_off_at;
/* haptic tick: one short pulse, always ended by ui_tick via
   motor_off_at (capped at 60 ms). 1.6 kept these silent because the
   motor ran on; 2.0 found that cause (inverted motor pin) and fixed it,
   and notifications already buzz through the same path. */
static void buzz(int ms)
{
    if (ms > 60)
        ms = 60;
    hal_motor(1);
    motor_off_at = hal_millis() + (uint32_t)ms;
}

#define PERSIST_ADDR 0x3F0000u   /* top sector, clear of the DFU region */
struct persist {
    uint32_t magic;              /* 0x564F4944 = "VOID" */
    uint8_t ver;
    uint8_t brightness, dnd, airplane, water_lock, bt_on;
    uint8_t cc_hide, water_ml, alarm_on;
    uint32_t steps_total, chip_at_save;
    uint8_t alarm_h, alarm_m;
    uint16_t last_sleep_min, last_workout_min;
    uint8_t last_workout_sets, s_track;
    /* last-known wall clock: restores the face after a reboot until
       the phone resyncs. Stale by the off-time, but the date and the
       rough time are right — build time was always wrong */
    uint32_t clock_tod_ms;
    uint16_t clock_year;
    uint8_t clock_mon, clock_day, clock_dow, clock_ok;
    uint8_t face_dig;
    uint8_t hr_on;
    uint8_t compat;              /* 2.1: advertise as InfiniTime */
};
static struct persist P;

int ui_steps(void)
{
    int chip = sensors_steps();
    if (chip < 0)
        return -1;
    uint32_t c = (uint32_t)chip;
    uint32_t tot = P.steps_total;
    tot += c >= P.chip_at_save ? c - P.chip_at_save : c;
    return (int)tot;
}

static int persist_dirty;
static uint32_t persist_dirty_ms;

/* mark for a deferred write: the 4 KB sector erase stalls the UI and
   BLE for up to ~300 ms, so it never runs inside a tap handler */
static void persist_mark(void)
{
    persist_dirty = 1;
    persist_dirty_ms = hal_millis();
}

static void persist_save(void)
{
    persist_dirty = 0;
    P.magic = 0x564F4944u;
    P.ver = 5;
    P.brightness = (uint8_t)brightness;
    P.dnd = (uint8_t)dnd_on;
    P.airplane = (uint8_t)airplane_on;
    P.water_lock = (uint8_t)water_lock;
    P.bt_on = (uint8_t)bt_on;
    P.cc_hide = (uint8_t)cc_hide;
    P.water_ml = (uint8_t)(water_ml / 250);
    P.alarm_on = (uint8_t)alarm_on;
    P.alarm_h = (uint8_t)alarm_h;
    P.alarm_m = (uint8_t)alarm_m;
    P.last_sleep_min = (uint16_t)(s_last_ms / 60000u);
    P.last_workout_min = (uint16_t)(w_last_ms / 60000u);
    P.last_workout_sets = (uint8_t)w_last_sets;
    P.s_track = (uint8_t)s_track;
    P.face_dig = (uint8_t)face_dig;
    P.hr_on = (uint8_t)sensors_hr_on();
    P.compat = (uint8_t)ble_get_compat();
    if (sensors_steps() >= 0) {      /* a dead BMA must not poison totals */
        P.steps_total = (uint32_t)ui_steps();
        P.chip_at_save = (uint32_t)sensors_steps();
    }
    if (hal_clock_synced() || hal_clock_restored()) {
        int y, mo, d, dow;
        hal_clock_date(&y, &mo, &d, &dow);
        P.clock_tod_ms = hal_clock_tod_ms();
        P.clock_year = (uint16_t)y;
        P.clock_mon = (uint8_t)mo;
        P.clock_day = (uint8_t)d;
        P.clock_dow = (uint8_t)dow;
        P.clock_ok = 1;
    }
    spi_flash_init();
    spi_flash_sector_erase(PERSIST_ADDR);
    spi_flash_write(PERSIST_ADDR, (const uint8_t *)&P, sizeof P);
}

static void persist_load(void)
{
    spi_flash_init();
    spi_flash_read(PERSIST_ADDR, (uint8_t *)&P, sizeof P);
    /* ver 3/4 load fine too: same layout. ver 3 had no hr_on, and that
       byte was struct padding copied from a zeroed P, so it read back
       as 0 = "heart rate off". 1.6.x then re-saved it as ver 4 with
       hr_on = 0, which is why the face showed "--" for good. Only a
       ver 5 record (written by this build) is trusted for hr_on. */
    if (P.magic != 0x564F4944u || P.ver < 3 || P.ver > 5) {
        memset(&P, 0, sizeof P);
        return;
    }
    brightness = P.brightness;
    dnd_on = P.dnd;
    airplane_on = P.airplane;
    /* never boot locked: a persisted lock was a trap */
    water_lock = 0;
    bt_on = P.bt_on;
    cc_hide = P.cc_hide;
    water_ml = P.water_ml * 250;
    alarm_on = P.alarm_on;
    if (P.alarm_h < 24 && P.alarm_m < 60) {
        alarm_h = P.alarm_h;
        alarm_m = P.alarm_m;
    }
    s_last_ms = (uint32_t)P.last_sleep_min * 60000u;
    w_last_ms = (uint32_t)P.last_workout_min * 60000u;
    w_last_sets = P.last_workout_sets;
    s_track = P.s_track;
    face_dig = P.face_dig;
    ble_set_compat(P.ver >= 5 && P.compat == 1);
    if (P.ver >= 5 && P.hr_on <= 1)
        sensors_hr_set(P.hr_on);
    else
        sensors_hr_set(1);
    /* restore the saved wall clock when nothing newer has been set
       this boot; the phone will correct it on the next sync */
    if (P.clock_ok && P.clock_year >= 2024 && !hal_clock_synced()) {
        uint32_t t = P.clock_tod_ms;
        hal_clock_restore(P.clock_year, P.clock_mon, P.clock_day,
                          P.clock_dow, (int)(t / 3600000u),
                          (int)(t / 60000u % 60u), (int)(t / 1000u % 60u));
    }
}

/* ---- helpers ------------------------------------------------------------ */

#define C_BG     lv_color_hex(0x000000)
/* 2.1: one palette with the face — neon cyan + red */
#define C_PANEL  lv_color_hex(0x0B0E14)
#define C_EDGE   lv_color_hex(0x1E4E56)
#define C_FILL   lv_color_hex(0x151B26)
#define C_WHITE  lv_color_white()
#define C_MUTED  lv_color_hex(0x8890A0)
#define C_ACCENT lv_color_hex(0x46D8E0)
#define C_WARN   lv_color_hex(0xF0586E)
#define C_GOOD   lv_color_hex(0x3CC850)

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font,
                          lv_color_t c, int x, int y, const char *s)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, s);
    return l;
}

static lv_obj_t *mk_label_c(lv_obj_t *parent, const lv_font_t *font,
                            lv_color_t c, int x, int y, const char *s)
{
    lv_obj_t *l = mk_label(parent, font, c, x, y, s);
    lv_obj_update_layout(l);
    lv_obj_set_x(l, x - lv_obj_get_width(l) / 2);
    return l;
}

static uint32_t last_swipe_ms;

/* CLICKED filter: LVGL fires CLICKED on release even after a long
   drag (nothing here scrolls), so the tail of every hardware swipe
   that began on a button was also clicking it — phantom toggles and
   navigation everywhere. Stop processing when a swipe just fired. */
static void tap_filter(lv_event_t *e)
{
    if (hal_millis() - last_swipe_ms < 400)
        lv_event_stop_processing(e);
}

/* 2.1 look: every button and value pill is drawn as the face's
   chamfered neon pill instead of a grey rounded box. The widget's
   bg_color still carries its meaning (callers flip it for on/off), and
   is read here at draw time:
     C_ACCENT / C_GOOD   solid fill, dark text (an "on" state)
     C_FILL / C_PANEL / black   cyan outline, black inside
     C_EDGE              dim outline (an "off" value pill)
     anything else       outline + faint tint in that colour (red = warn)
   Pressed adds a tint so taps still show. */
static void pill_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *l = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int w = a.x2 - a.x1 + 1, h = a.y2 - a.y1 + 1;
    int cut = h / 4;
    if (cut > 9)
        cut = 9;
    if (cut < 3)
        cut = 3;
    lv_color_t c = lv_obj_get_style_bg_color(o, LV_PART_MAIN);
    uint32_t hx = lv_color_to_u32(c) & 0xFFFFFF;
    int pressed = lv_obj_has_state(o, LV_STATE_PRESSED);
    if (hx == 0x46D8E0 || hx == 0x3CC850) {
        dk_oct(l, a.x1, a.y1, w, h, cut, c, 1, LV_OPA_COVER);
        return;
    }
    lv_opa_t tint = pressed ? LV_OPA_40 : LV_OPA_TRANSP;
    lv_color_t stroke = c;
    if (hx == 0x151B26 || hx == 0x0B0E14 || hx == 0x000000)
        stroke = lv_color_hex(0x46D8E0);
    else if (hx != 0x1E4E56 && !pressed)
        tint = LV_OPA_20;
    dk_oct(l, a.x1, a.y1, w, h, cut, lv_color_black(), 1, LV_OPA_COVER);
    if (tint)
        dk_oct(l, a.x1, a.y1, w, h, cut, stroke, 1, tint);
    dk_oct(l, a.x1, a.y1, w, h, cut, stroke, 1, LV_OPA_TRANSP);
}

static void pill_style(lv_obj_t *b)
{
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_outline_width(b, 0, 0);
    lv_obj_set_style_outline_width(b, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, pill_draw, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
}

/* non-interactive neon card behind a screen's main value */
static lv_obj_t *mk_card(lv_obj_t *parent, int x, int y, int w, int h,
                         lv_color_t c)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    pill_style(o);
    return o;
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *txt,
                        int x, int y, int w, int h, lv_color_t fill,
                        lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, fill, 0);
    lv_obj_set_style_radius(b, 4, 0);   /* sharp: the 240 panel has a bezel,
                                         not the bevel-less glass the
                                         squircle look was designed for */
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, C_EDGE, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    pill_style(b);
    /* LVGL fires CLICKED on release even after a long drag (nothing
       here scrolls), so the tail of every hardware swipe that began
       on a button was also clicking it — phantom toggles/navigation
       everywhere ("dragging is buggy"). Attached BEFORE the real cb
       so stop_processing swallows the click. */
    lv_obj_add_event_cb(b, tap_filter, LV_EVENT_CLICKED, NULL);
    if (cb)
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &kenney16, 0);
    lv_obj_set_style_text_color(l, C_WHITE, 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *mk_toggle(lv_obj_t *parent, const char *txt,
                           int x, int y, int w, int on, lv_event_cb_t cb)
{
    lv_obj_t *b = mk_btn(parent, txt, x, y, w, 30, C_FILL, cb);
    lv_obj_set_style_bg_color(b, on ? C_ACCENT : C_FILL, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 0),
                                on ? C_BG : C_WHITE, 0);
    return b;
}

static void back_cb(lv_event_t *e);
static void nav_to(scr_t s);

static void mk_header(lv_obj_t *parent, const char *title, int with_back)
{
    /* concept style: small clock top-left, title top-right, hairline */
    /* back is the most-hit control in the OS: 40x30 visual (was 30x24)
       plus an invisible 8 px margin, so the whole top-left corner
       (0..52 x 0..42) takes the tap. Nothing else sits there. */
    char tb[8];
    int clock_x = with_back ? 54 : 12;
    if (with_back) {
        lv_obj_t *b = mk_btn(parent, "<", 4, 4, 40, 30, C_BG, back_cb);
        lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &kenney16, 0);
        lv_obj_set_ext_click_area(b, 8);
    }
    lv_obj_t *t = mk_label(parent, &kenney16, C_ACCENT, 0, 11, title);
    lv_obj_update_layout(t);
    lv_obj_set_x(t, 226 - lv_obj_get_width(t));
    /* small clock between back and title, only when the title leaves
       room for it (long titles at 16 px would collide) */
    lv_snprintf(tb, sizeof tb, "%d:%02d", hal_clock_hour(),
                hal_clock_minute());
    if (226 - lv_obj_get_width(t) - clock_x >=
        (int)lv_text_get_width(tb, strlen(tb), &kenney12, 0) + 8)
        mk_label(parent, &kenney12, C_MUTED, clock_x, 14, tb);
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_pos(line, 12, 37);
    lv_obj_set_size(line, 216, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x1E4E56), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

/* dynamic labels refreshed by per-screen ticks */
static lv_obj_t *lbl_time_big;

static void page_bg(lv_obj_t *parent)
{
    /* full-bleed black: bezel and background are indistinguishable */
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
}

/* ---- navigation ---------------------------------------------------------- */

static void rebuild(void);

/* real back stack: "<" and L->R return to wherever you came from
   (SETTIME -> SETTINGS, CC -> SETTINGS -> CC, launcher app -> launcher).
   Face/CC/launcher are roots: navigating to one clears the stack. */
static scr_t nav_stack[6];
static int nav_sp;
static void notif_reset_view(void);
static uint32_t notif_sel_uid;

/* Screen transitions were removed (1.6.2): full-screen software-rendered
   slides ran at ~4 fps and made every navigation feel like molasses.
   Instant switches are faster than this chip can animate. */
static int pend_dir;   /* kept for dir_nav bookkeeping; no animation */

static int set_page;          /* Settings page, see build_settings */
static int swipe_handled;     /* a swipe acted without changing screen */

static void nav_to(scr_t s)
{
    if (s == SCR_SETTINGS && cur != SCR_SETTINGS && cur != SCR_SETTIME &&
        cur != SCR_DIAG)
        set_page = 0;
    if (cur == SCR_NOTIF && s != SCR_NOTIF)
        notif_reset_view();
    if (s == SCR_FACE || s == SCR_CC || s == SCR_LAUNCHER) {
        nav_sp = 0;
    } else if (s != cur) {
        if (nav_sp < 6)
            nav_stack[nav_sp++] = cur;
        else {
            for (int i = 0; i < 5; i++)
                nav_stack[i] = nav_stack[i + 1];
            nav_stack[5] = cur;
        }
    }
    back_target = nav_sp ? nav_stack[nav_sp - 1] : SCR_FACE;
    cur = s;
    rebuild();
}

static void nav_back(void)
{
    if (nav_sp) {
        scr_t to = nav_stack[--nav_sp];
        if (cur == SCR_NOTIF)
            notif_reset_view();
        back_target = nav_sp ? nav_stack[nav_sp - 1] : SCR_FACE;
        cur = to;
        rebuild();
    } else {
        nav_to(SCR_FACE);
    }
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    nav_back();
}

/* CST816S gesture register IDs: 1=down 2=up 3=left(R->L) 4=right(L->R).
   Five-way face map, edge-aware on the down swipe:
     down on the left half  -> control centre
     down on the right half -> apps
     up                     -> stats
     L->R                   -> notifications
     R->L                   -> cardio
   Each panel exits with the opposite swipe; inner screens keep
   L->R (dir 4) as back like before. */
static void dir_nav(int cst_dir, int start_x)
{
    if (water_lock)
        return;
    pend_dir = cst_dir;        /* remembered for the slide direction */
    if (cur == SCR_FACE) {
        if (cst_dir == 1)
            nav_to(start_x < 120 ? SCR_CC : SCR_LAUNCHER);
        else if (cst_dir == 2)
            nav_to(SCR_STATS);
        else if (cst_dir == 4)
            nav_to(SCR_NOTIF);
        else if (cst_dir == 3)
            nav_to(SCR_CARDIO);
    } else if (cur == SCR_CC || cur == SCR_LAUNCHER) {
        /* up OR down exits — the swipe is hardware-detected and a
           near-vertical flick often lands on the "wrong" one; both
           directions mean "back to the face" here */
        if (cst_dir == 1 || cst_dir == 2)
            nav_to(SCR_FACE);
    } else if (cur == SCR_STATS) {
        if (cst_dir == 1 || cst_dir == 2)
            nav_to(SCR_FACE);
    } else if (cur == SCR_NOTIF) {
        /* every other screen uses dir 4 (L->R) as back; NOTIF alone
           answered dir 3 — swipes looked dead on this screen only */
        if (cst_dir == 3 || cst_dir == 4) {
            if (notif_sel_uid) {
                notif_reset_view();
                rebuild();
            } else {
                nav_back();
            }
        }
    } else if (cur == SCR_CARDIO) {
        if (cst_dir == 4)
            nav_to(SCR_FACE);
    } else if (cur == SCR_SETTINGS && (cst_dir == 1 || cst_dir == 2)) {
        /* slide up = next page, slide down = previous */
        int last = (13 - 1) / 5;
        int np = set_page + (cst_dir == 2 ? 1 : -1);
        if (np >= 0 && np <= last) {
            set_page = np;
            rebuild();
        }
        swipe_handled = 1;
    } else {
        if (cst_dir == 4)
            nav_back();
    }
}

int ui_swipe(int cst_dir, int start_x)
{
    last_swipe_ms = hal_millis();
    /* the honeycomb is dragged with the finger: horizontal hardware
       gestures are the tail of a focus drag and stay drags; vertical
       ones mean "back to the face" */
    if (cur == SCR_LAUNCHER && cst_dir != 1 && cst_dir != 2)
        return 0;
    scr_t before = cur;
    swipe_handled = 0;
    dir_nav(cst_dir, start_x);
    return cur != before || swipe_handled;
}

static uint32_t ui_start_ms;

/* ---- display sleep ------------------------------------------------------ */
static int asleep;
static uint32_t last_input_ms;
#define SLEEP_AFTER_MS 60000

int ui_asleep(void) { return asleep; }

/* any touch or gesture counts as use — without this the screen slept
   60 s after wake even mid-interaction */
void ui_activity(void) { last_input_ms = hal_millis(); }

/* the panel has three backlight steps (P0.14/22/23). 2.0.1 cycled
   25/50/75/100 % with 1 + b*2/100, so 50 and 75 were the same step.
   Now: LOW 30 -> MID 60 -> HIGH 100, any saved value maps sensibly. */
static int bright_step(void) { return brightness < 45 ? 1 : brightness < 85 ? 2 : 3; }
static void bright_cycle(void)
{
    int s = bright_step();
    brightness = s == 1 ? 60 : s == 2 ? 100 : 30;
}
static const char *bright_name(void)
{
    static const char *n[4] = {"", "LOW", "MID", "HIGH"};
    return n[bright_step()];
}
static int bl_level(void) { return torch_on ? 3 : bright_step(); }
static void torch_set(int on);
static void wlock_set(int on);

void ui_wake(void)
{
    if (asleep) {
        asleep = 0;
        display_sleep(0);
        touch_sleep(0);
        lv_obj_invalidate(lv_screen_active());  /* panel RAM is stale */
        display_backlight(bl_level());
    }
    last_input_ms = hal_millis();
}

/* taps never navigate — the five-way swipe map is the only way off the
   face, so there is no tap_cb. Root stays clickable only so the CST816S
   events still flow through the indev to child buttons. */

/* ---- baked material icons (assets/fonts PNGs -> icon_alpha[][]) ----------- */

#include "assets.h"

static lv_image_dsc_t icon_dsc[15];
static int icon_dsc_ready;

/* icon_alpha order (tools/gen_assets.py ICONS):
   0 dnd_on 1 flight 2 bt 3 torch_on 4 phone_vibrate 5 water_drop
   6 settings 7 light_mode 8 battery 9 dnd_off 10 airplane_off
   11 bt_off 12 torch_off 13 cloud 14 footprint */
static void icon_dsc_init(void)
{
    for (int i = 0; i < 15; i++) {
        icon_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        icon_dsc[i].header.w = 24;
        icon_dsc[i].header.h = 24;
        icon_dsc[i].header.cf = LV_COLOR_FORMAT_A8;
        icon_dsc[i].header.stride = 24;
        icon_dsc[i].data = icon_alpha[i];
        icon_dsc[i].data_size = sizeof icon_alpha[i];
    }
    icon_dsc_ready = 1;
}

/* vector app/control glyphs — no PNG assets needed */
enum {
    G_WORKOUT, G_CARDIO, G_STATS, G_SLEEP, G_WATER, G_CLOCK,
    G_ALERTS, G_SETTINGS, G_DIAG, G_PHONE,
    G_BATT, G_SUN, G_BT, G_PLANE, G_TORCH, G_STEPS, G_BOLT,
    G_WEATHER, G_MUSIC
};
static void hex_stroke(lv_layer_t *layer, int cx, int cy, int r,
                       lv_color_t c, int w);
static void seg_bar(lv_layer_t *layer, int x, int y, int w, int h,
                    int segs, int pct, lv_color_t on, lv_color_t off);
static void draw_txt_c(lv_layer_t *layer, const lv_font_t *f, lv_color_t c,
                       int cx, int y, const char *s);
static void draw_glyph(lv_layer_t *layer, int g, int cx, int cy, int s,
                       lv_color_t c);

/* positional icon by raw icon_alpha index — for badges and empty states */
static lv_obj_t *mk_icon_at(lv_obj_t *parent, int idx, int on, int x, int y)
{
    if (!icon_dsc_ready)
        icon_dsc_init();
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, &icon_dsc[idx]);
    lv_obj_set_style_image_recolor(img, on ? C_ACCENT : C_MUTED, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_set_pos(img, x, y);
    return img;
}

/* ---- control centre ------------------------------------------------------
   A focus wheel like the apps screen: satellites pick the control, the
   centre hex IS the control — battery shows its gauge, brightness shows
   the segmented bar, toggles show ON/OFF. cc_hide bitmask (1=torch,
   2=water lock, 4=phone) drops controls out of the ring entirely. */
enum { CCW_BATT, CCW_BRIGHT, CCW_DND, CCW_BT, CCW_AIR, CCW_TORCH,
       CCW_PHONE, CCW_LOCK, CCW_SET, CCW_DEFN };
static const struct { const char *name; int glyph; lv_color_t col; }
    ccw_def[CCW_DEFN] = {
    {"BATT",    G_BATT,     LV_COLOR_MAKE(0x46,0xD8,0xE0)},
    {"BRIGHT",  G_SUN,      LV_COLOR_MAKE(0xF0,0xD0,0x40)},
    {"DND",     G_SLEEP,    LV_COLOR_MAKE(0x70,0x40,0xC8)},
    {"BT",      G_BT,       LV_COLOR_MAKE(0x28,0x70,0xDC)},
    {"AIR",     G_PLANE,    LV_COLOR_MAKE(0xDC,0x82,0x1E)},
    {"TORCH",   G_TORCH,    LV_COLOR_MAKE(0xC8,0xA0,0x1E)},
    {"ALERTS",  G_PHONE,    LV_COLOR_MAKE(0x28,0x70,0xDC)},
    {"WTR LCK", G_WATER,    LV_COLOR_MAKE(0x1E,0x96,0xBE)},
    {"SETTING", G_SETTINGS, LV_COLOR_MAKE(0x8A,0x94,0xA8)},
};
static int ccw_list[CCW_DEFN], ccw_n, ccw_focus;
static lv_obj_t *ccw_canvas;
static lv_point_t ccw_press;
static int ccw_dx, ccw_drag;

static int ccw_on(int d)
{
    switch (d) {
    case CCW_BATT:   return 1;
    case CCW_BRIGHT: return bright_step() >= 2;
    case CCW_DND:    return dnd_on;
    case CCW_BT:     return bt_on;
    case CCW_AIR:    return airplane_on;
    case CCW_TORCH:  return torch_on;
    case CCW_PHONE:  return ble_connected();
    case CCW_LOCK:   return water_lock;
    }
    return 0;
}

static void ccw_activate(int d)
{
    switch (d) {
    case CCW_BRIGHT:
        bright_cycle();
        if (!torch_on)
            display_backlight(bl_level());
        break;
    case CCW_DND:
        dnd_on ^= 1;
        break;
    case CCW_BT:
        bt_on ^= 1;
        if (bt_on)
            airplane_on = 0;
        apply_radio();
        break;
    case CCW_AIR:
        airplane_on ^= 1;
        apply_radio();
        break;
    case CCW_TORCH:
        torch_set(!torch_on);
        break;
    case CCW_PHONE:
        nav_to(SCR_NOTIF);
        return;
    case CCW_LOCK:
        wlock_set(!water_lock);
        break;
    case CCW_SET:
        buzz(35);
        nav_to(SCR_SETTINGS);
        return;
    default:
        return;
    }
    buzz(20);                       /* toggles get a light tick */
    persist_mark();
    lv_obj_invalidate(ccw_canvas);
}

static void ccw_build_list(void)
{
    ccw_n = 0;
    for (int i = 0; i < CCW_DEFN; i++) {
        if ((i == CCW_TORCH && (cc_hide & 1)) ||
            (i == CCW_LOCK  && (cc_hide & 2)) ||
            (i == CCW_PHONE && (cc_hide & 4)))
            continue;
        ccw_list[ccw_n++] = i;
    }
    if (ccw_focus >= ccw_n)
        ccw_focus = 0;
}

/* 2.0: ring nodes are 17 px hexes (were 13) on a ring centred lower,
   below the header; each node takes taps out to CC_SAT_HIT px with the
   nearest node winning (was a 22 px radius the hub test overlapped).
   The bottom hint went: at 12 px it wrapped and hit the bottom node. */
#define CC_CY      140
#define CC_HUB     52
#define CC_HUB_HIT 54
#define CC_SAT     17
#define CC_SAT_HIT 32
static void ccw_pos(int i, int *x, int *y)
{
    float a = (-90.0f + i * (360.0f / ccw_n)) * 0.0174533f;
    *x = 120 + (int)(cosf(a) * 94);
    *y = CC_CY + (int)(sinf(a) * 74);
}

/* tap -> ring index 0..ccw_n-1, ccw_n for the hub, -1 nothing */
static int ccw_hit(int px, int py)
{
    int best = -1, bd = CC_SAT_HIT * CC_SAT_HIT;
    for (int i = 0; i < ccw_n; i++) {
        int x, y;
        ccw_pos(i, &x, &y);
        int d = (px - x) * (px - x) + (py - y) * (py - y);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    if (best >= 0)
        return best;
    int dx = px - 120, dy = py - CC_CY;
    if (dx * dx + dy * dy < CC_HUB_HIT * CC_HUB_HIT)
        return ccw_n;
    return -1;
}

static void ccw_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    int fd = ccw_list[ccw_focus];
    lv_color_t fc = ccw_def[fd].col;
    for (int i = 0; i < ccw_n; i++) {
        int x, y, d = ccw_list[i];
        int on = ccw_on(d), foc = i == ccw_focus;
        ccw_pos(i, &x, &y);
        lv_color_t sc = foc ? ccw_def[d].col
                            : (on ? lv_color_mix(ccw_def[d].col,
                                                 lv_color_black(), 120)
                                  : lv_color_hex(0x38425A));
        hex_stroke(layer, x, y, foc ? CC_SAT + 3 : CC_SAT, sc, foc ? 2 : 1);
        draw_glyph(layer, ccw_def[d].glyph, x, y, 8,
                   on || foc ? ccw_def[d].col : lv_color_hex(0x5A6478));
    }
    /* hub = the focused control itself, not just a bigger icon */
    hex_stroke(layer, 120, CC_CY, CC_HUB, fc, 2);
    hex_stroke(layer, 120, CC_CY, CC_HUB - 5,
               lv_color_mix(fc, lv_color_black(), 170), 1);
    draw_glyph(layer, ccw_def[fd].glyph, 120, CC_CY - 30, 10, fc);
    draw_txt_c(layer, &kenney12, lv_color_hex(0x8A94A8), 120, CC_CY - 16,
               ccw_def[fd].name);
    char v[12];
    if (fd == CCW_BATT)
        lv_snprintf(v, sizeof v, "%d%%", hal_battery_pct());
    else if (fd == CCW_BRIGHT)
        lv_snprintf(v, sizeof v, "%s", bright_name());
    else if (fd == CCW_SET)
        lv_snprintf(v, sizeof v, "OPEN");
    else if (fd == CCW_PHONE)
        lv_snprintf(v, sizeof v, "%s", ccw_on(fd) ? "LINKED" : "OPEN");
    else
        lv_snprintf(v, sizeof v, "%s", ccw_on(fd) ? "ON" : "OFF");
    draw_txt_c(layer, &kenney16, ccw_on(fd) ? fc : C_MUTED, 120, CC_CY - 2, v);
    if (fd == CCW_BRIGHT)
        seg_bar(layer, 88, CC_CY + 18, 64, 6, 10, bright_step() * 100 / 3, fc,
                lv_color_hex(0x232C3C));
    else if (fd == CCW_BATT)
        seg_bar(layer, 88, CC_CY + 18, 64, 6, 10, hal_battery_pct(), fc,
                lv_color_hex(0x232C3C));
}

static void ccw_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (code == LV_EVENT_PRESSED) {
        ccw_dx = ccw_drag = 0;
        if (in)
            lv_indev_get_point(in, &ccw_press);
    } else if (code == LV_EVENT_PRESSING && in) {
        lv_point_t v;
        lv_indev_get_vect(in, &v);
        ccw_dx += v.x;
        ccw_drag += (v.x < 0 ? -v.x : v.x) + (v.y < 0 ? -v.y : v.y);
        while (ccw_dx > 46) {
            ccw_dx -= 46;
            ccw_focus = (ccw_focus + 1) % ccw_n;
            lv_obj_invalidate(ccw_canvas);
        }
        while (ccw_dx < -46) {
            ccw_dx += 46;
            ccw_focus = (ccw_focus + ccw_n - 1) % ccw_n;
            lv_obj_invalidate(ccw_canvas);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        ccw_dx = 0;
        if (ccw_drag >= 10)
            return;
        int h = ccw_hit(ccw_press.x, ccw_press.y);
        if (h == ccw_n) {
            ccw_activate(ccw_list[ccw_focus]);   /* hub tap = act */
        } else if (h >= 0) {
            ccw_focus = h;              /* satellites only focus */
            lv_obj_invalidate(ccw_canvas);
        }
    }
}

/* ---- shared VoidOS draw language ---------------------------------------------
   Outlined (stroked) pointy-top hexes, chamfered cards, neon accents on
   black — the concept-art look. Everything below draws inside one canvas
   callback per screen; only transparent tap targets are real objects. */

static void hex_stroke(lv_layer_t *layer, int cx, int cy, int r,
                       lv_color_t c, int w)
{
    int hw = r * 866 / 1000;
    const int px[6] = {cx, cx + hw, cx + hw, cx, cx - hw, cx - hw};
    const int py[6] = {cy - r, cy - r / 2, cy + r / 2,
                       cy + r, cy + r / 2, cy - r / 2};
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = c;
    dsc.width = w;
    for (int i = 0; i < 6; i++) {
        dsc.p1.x = px[i];
        dsc.p1.y = py[i];
        dsc.p2.x = px[(i + 1) % 6];
        dsc.p2.y = py[(i + 1) % 6];
        lv_draw_line(layer, &dsc);
    }
}

/* horizontal segmented bar — the concept's brightness indicator */
static void seg_bar(lv_layer_t *layer, int x, int y, int w, int h,
                    int segs, int pct, lv_color_t on, lv_color_t off)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 0;
    int lit = pct * segs / 100;
    int gap = 3, sw = (w - gap * (segs - 1)) / segs;
    for (int i = 0; i < segs; i++) {
        dsc.bg_color = i < lit ? on : off;
        lv_area_t a = {x + i * (sw + gap), y, x + i * (sw + gap) + sw - 1,
                       y + h - 1};
        lv_draw_rect(layer, &dsc, &a);
    }
}

static void draw_txt(lv_layer_t *layer, const lv_font_t *f, lv_color_t c,
                     int x, int y, int w, const char *s, lv_text_align_t al)
{
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = f;
    ld.color = c;
    ld.align = al;
    ld.text = s;
#ifdef UISNAP_TEXT_HOOK
    UISNAP_TEXT_HOOK(f, s, x, y, w);    /* host tap/overflow audit */
#endif
    lv_area_t a = {x, y, x + w - 1, y + 40};
    lv_draw_label(layer, &ld, &a);
}

static void draw_txt_c(lv_layer_t *layer, const lv_font_t *f, lv_color_t c,
                       int cx, int y, const char *s)
{
    draw_txt(layer, f, c, cx - 100, y, 200, s, LV_TEXT_ALIGN_CENTER);
}

/* each glyph ~size s centred on cx,cy */
static void draw_glyph(lv_layer_t *layer, int g, int cx, int cy, int s,
                       lv_color_t c)
{
    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.color = c;
    ln.width = 2;
    ln.round_end = 1;
    lv_draw_rect_dsc_t rc;
    lv_draw_rect_dsc_init(&rc);
    rc.bg_opa = LV_OPA_COVER;
    rc.bg_color = c;
    switch (g) {
    case G_WEATHER:                        /* sun behind a cloud */
        dk_wx_icon(layer, 1, cx, cy, s, c);
        break;
    case G_MUSIC:
        dk_music(layer, DK_M_NOTE, cx, cy, s, c);
        break;
    case G_WORKOUT: {                      /* dumbbell */
        lv_area_t p1 = {cx - s, cy - s / 2, cx - s + 3, cy + s / 2};
        lv_area_t p2 = {cx - s + 5, cy - s + 2, cx - s + 7, cy + s - 2};
        lv_area_t p3 = {cx + s - 7, cy - s + 2, cx + s - 5, cy + s - 2};
        lv_area_t p4 = {cx + s - 3, cy - s / 2, cx + s, cy + s / 2};
        lv_draw_rect(layer, &rc, &p1);
        lv_draw_rect(layer, &rc, &p2);
        lv_draw_rect(layer, &rc, &p3);
        lv_draw_rect(layer, &rc, &p4);
        ln.p1.x = cx - s + 4; ln.p1.y = cy;
        ln.p2.x = cx + s - 4; ln.p2.y = cy;
        ln.width = 3;
        lv_draw_line(layer, &ln);
        break;
    }
    case G_CARDIO: {                       /* heart, 7x6 shape scaled */
        static const char sh[6][8] = {
            ".XX.XX.", "XXXXXXX", "XXXXXXX", ".XXXXX.", "..XXX..", "...X..."
        };
        int px2 = s * 2 / 7;
        if (px2 < 2)
            px2 = 2;
        for (int yy = 0; yy < 6; yy++)
            for (int xx = 0; xx < 7; xx++) {
                if (sh[yy][xx] != 'X')
                    continue;
                lv_area_t p = {cx - 7 * px2 / 2 + xx * px2,
                               cy - 3 * px2 + yy * px2,
                               cx - 7 * px2 / 2 + xx * px2 + px2 - 1,
                               cy - 3 * px2 + yy * px2 + px2 - 1};
                lv_draw_rect(layer, &rc, &p);
            }
        break;
    }
    case G_STATS: {                        /* bars */
        int bw = s * 2 / 5;
        const int ht[3] = {s, s * 3 / 2, s * 2};
        for (int i = 0; i < 3; i++) {
            lv_area_t b = {cx - s + i * (bw + 3), cy + s - ht[i],
                           cx - s + i * (bw + 3) + bw, cy + s};
            lv_draw_rect(layer, &rc, &b);
        }
        break;
    }
    case G_SLEEP: {                        /* crescent moon */
        lv_draw_arc_dsc_t ar;
        lv_draw_arc_dsc_init(&ar);
        ar.center.x = cx;
        ar.center.y = cy;
        ar.radius = s;
        ar.width = s * 2;
        ar.color = c;
        ar.start_angle = 0;
        ar.end_angle = 360;
        lv_draw_arc(layer, &ar);
        ar.center.x = cx + s / 2;
        ar.center.y = cy - s / 3;
        ar.radius = s - 1;
        ar.width = s * 2;
        ar.color = lv_color_black();
        lv_draw_arc(layer, &ar);
        break;
    }
    case G_WATER: {                        /* droplet: triangle + arc */
        lv_draw_triangle_dsc_t tr;
        lv_draw_triangle_dsc_init(&tr);
        tr.color = c;
        tr.p[0].x = cx;
        tr.p[0].y = cy - s;
        tr.p[1].x = cx - s * 3 / 4;
        tr.p[1].y = cy + s / 4;
        tr.p[2].x = cx + s * 3 / 4;
        tr.p[2].y = cy + s / 4;
        lv_draw_triangle(layer, &tr);
        lv_draw_arc_dsc_t ar;
        lv_draw_arc_dsc_init(&ar);
        ar.center.x = cx;
        ar.center.y = cy + s / 4;
        ar.radius = s * 3 / 4;
        ar.width = s * 3 / 4;
        ar.color = c;
        ar.start_angle = 0;
        ar.end_angle = 360;
        lv_draw_arc(layer, &ar);
        break;
    }
    case G_CLOCK: {                        /* clock face + hands */
        lv_draw_arc_dsc_t ar;
        lv_draw_arc_dsc_init(&ar);
        ar.center.x = cx;
        ar.center.y = cy;
        ar.radius = s;
        ar.width = 2;
        ar.color = c;
        ar.start_angle = 0;
        ar.end_angle = 360;
        lv_draw_arc(layer, &ar);
        ln.p1.x = cx; ln.p1.y = cy;
        ln.p2.x = cx; ln.p2.y = cy - s + 4;
        lv_draw_line(layer, &ln);
        ln.p2.x = cx + s - 5; ln.p2.y = cy + 3;
        lv_draw_line(layer, &ln);
        break;
    }
    case G_ALERTS: {                       /* bell */
        ln.width = 2;
        ln.p1.x = cx - s + 2; ln.p1.y = cy + s / 2;
        ln.p2.x = cx + s - 2; ln.p2.y = cy + s / 2;
        lv_draw_line(layer, &ln);
        ln.p1.x = cx - s + 2; ln.p1.y = cy + s / 2;
        ln.p2.x = cx; ln.p2.y = cy - s;
        lv_draw_line(layer, &ln);
        ln.p1.x = cx + s - 2; ln.p1.y = cy + s / 2;
        lv_draw_line(layer, &ln);
        lv_area_t d = {cx - 2, cy + s / 2 + 3, cx + 2, cy + s / 2 + 7};
        lv_draw_rect(layer, &rc, &d);
        break;
    }
    case G_SETTINGS: {                     /* gear: spokes + hub */
        ln.width = 3;
        for (int i = 0; i < 4; i++) {
            float a = i * 0.7853982f;
            int dx = (int)(cosf(a) * (s - 1));
            int dy = (int)(sinf(a) * (s - 1));
            ln.p1.x = cx - dx; ln.p1.y = cy - dy;
            ln.p2.x = cx + dx; ln.p2.y = cy + dy;
            lv_draw_line(layer, &ln);
        }
        lv_draw_arc_dsc_t ar;
        lv_draw_arc_dsc_init(&ar);
        ar.center.x = cx;
        ar.center.y = cy;
        ar.radius = s / 2;
        ar.width = 2;
        ar.color = c;
        ar.start_angle = 0;
        ar.end_angle = 360;
        lv_draw_arc(layer, &ar);
        break;
    }
    case G_DIAG: {                         /* ECG pulse line */
        ln.width = 2;
        const int vx[6] = {-s, -s / 3, -s / 5, s / 6, s / 3, s};
        const int vy[6] = {0, 0, -s, s - 2, -s / 3, -s / 3};
        for (int i = 0; i < 5; i++) {
            ln.p1.x = cx + vx[i]; ln.p1.y = cy + vy[i];
            ln.p2.x = cx + vx[i + 1]; ln.p2.y = cy + vy[i + 1];
            lv_draw_line(layer, &ln);
        }
        break;
    }
    case G_PHONE: {                        /* phone + vibration marks */
        rc.bg_color = c;
        lv_area_t ph = {cx - s / 2, cy - s, cx + s / 2, cy + s};
        lv_draw_rect(layer, &rc, &ph);
        lv_draw_rect_dsc_t bg;
        lv_draw_rect_dsc_init(&bg);
        bg.bg_opa = LV_OPA_COVER;
        bg.bg_color = lv_color_black();
        lv_area_t scr = {cx - s / 2 + 2, cy - s + 2, cx + s / 2 - 2,
                         cy + s - 3};
        lv_draw_rect(layer, &bg, &scr);
        ln.width = 2;
        ln.p1.x = cx - s / 2 - 4; ln.p1.y = cy - 4;
        ln.p2.x = cx - s / 2 - 4; ln.p2.y = cy + 4;
        lv_draw_line(layer, &ln);
        ln.p1.x = cx + s / 2 + 4;
        lv_draw_line(layer, &ln);
        break;
    }
    case G_BATT: {                       /* body + tip + two-thirds fill */
        lv_draw_rect_dsc_t bo;
        lv_draw_rect_dsc_init(&bo);
        bo.border_width = 2;
        bo.border_color = c;
        bo.border_opa = LV_OPA_COVER;
        bo.bg_opa = LV_OPA_TRANSP;
        lv_area_t bb = {cx - s, cy - s / 2, cx + s - 4, cy + s / 2};
        lv_draw_rect(layer, &bo, &bb);
        lv_area_t tp = {cx + s - 3, cy - s / 4, cx + s - 1, cy + s / 4};
        lv_draw_rect(layer, &rc, &tp);
        lv_area_t fl = {cx - s + 2, cy - s / 2 + 2,
                        cx - s + 1 + (s * 2 - 8) * 2 / 3, cy + s / 2 - 2};
        lv_draw_rect(layer, &rc, &fl);
        break;
    }
    case G_SUN: {                        /* filled disc + rays */
        lv_draw_arc_dsc_t ar;
        lv_draw_arc_dsc_init(&ar);
        ar.center.x = cx;
        ar.center.y = cy;
        ar.radius = s / 2;
        ar.width = s / 2;
        ar.color = c;
        ar.start_angle = 0;
        ar.end_angle = 360;
        lv_draw_arc(layer, &ar);
        for (int i = 0; i < 8; i++) {
            float a = i * 0.7853982f;
            ln.p1.x = cx + (int)(cosf(a) * (s * 3 / 4));
            ln.p1.y = cy + (int)(sinf(a) * (s * 3 / 4));
            ln.p2.x = cx + (int)(cosf(a) * s);
            ln.p2.y = cy + (int)(sinf(a) * s);
            lv_draw_line(layer, &ln);
        }
        break;
    }
    case G_BT: {                         /* rune: spine + two triangles */
        ln.p1.x = cx; ln.p1.y = cy - s;
        ln.p2.x = cx; ln.p2.y = cy + s;
        lv_draw_line(layer, &ln);
        int ax = cx + s * 2 / 3;
        ln.p1.x = cx; ln.p1.y = cy - s;
        ln.p2.x = ax; ln.p2.y = cy - s / 2;
        lv_draw_line(layer, &ln);
        ln.p1.x = ax; ln.p1.y = cy - s / 2;
        ln.p2.x = cx; ln.p2.y = cy;
        lv_draw_line(layer, &ln);
        ln.p1.x = cx; ln.p1.y = cy;
        ln.p2.x = ax; ln.p2.y = cy + s / 2;
        lv_draw_line(layer, &ln);
        ln.p1.x = ax; ln.p1.y = cy + s / 2;
        ln.p2.x = cx; ln.p2.y = cy + s;
        lv_draw_line(layer, &ln);
        break;
    }
    case G_PLANE: {                      /* top-down plane: triangle + spine */
        ln.p1.x = cx;     ln.p1.y = cy - s;
        ln.p2.x = cx - s; ln.p2.y = cy + s * 2 / 3;
        lv_draw_line(layer, &ln);
        ln.p2.x = cx + s;
        lv_draw_line(layer, &ln);
        ln.p1.x = cx - s; ln.p1.y = cy + s * 2 / 3;
        lv_draw_line(layer, &ln);
        ln.p1.x = cx;     ln.p1.y = cy - s;
        ln.p2.x = cx;     ln.p2.y = cy + s * 2 / 3;
        lv_draw_line(layer, &ln);
        break;
    }
    case G_TORCH: {                      /* flashlight silhouette + rays */
        lv_area_t hd = {cx - s / 2, cy - s, cx + s / 2, cy - s / 3};
        lv_area_t bd = {cx - s / 4, cy - s / 3, cx + s / 4, cy + s};
        lv_draw_rect(layer, &rc, &hd);
        lv_draw_rect(layer, &rc, &bd);
        for (int i = -1; i <= 1; i++) {
            ln.p1.x = ln.p2.x = cx + i * (s / 2);
            ln.p1.y = cy - s - 2;
            ln.p2.y = cy - s - 6;
            lv_draw_line(layer, &ln);
        }
        break;
    }
    case G_STEPS: {                      /* footprint: sole + heel */
        lv_draw_arc_dsc_t ar;
        lv_draw_arc_dsc_init(&ar);
        ar.color = c;
        ar.start_angle = 0;
        ar.end_angle = 360;
        ar.center.x = cx - s / 4;
        ar.center.y = cy - s / 3;
        ar.radius = s / 2;
        ar.width = s / 2;
        lv_draw_arc(layer, &ar);
        ar.center.x = cx + s / 5;
        ar.center.y = cy + s / 2;
        ar.radius = s * 2 / 5;
        ar.width = s * 2 / 5;
        lv_draw_arc(layer, &ar);
        break;
    }
    case G_BOLT: {                       /* lightning zigzag */
        const int bx[4] = {cx + s / 4, cx - s / 3, cx + s / 8, cx - s / 4};
        const int by[4] = {cy - s, cy + s / 8, cy + s / 8, cy + s};
        for (int i = 0; i < 3; i++) {
            ln.p1.x = bx[i]; ln.p1.y = by[i];
            ln.p2.x = bx[i + 1]; ln.p2.y = by[i + 1];
            lv_draw_line(layer, &ln);
        }
        break;
    }
    }
}

static void build_cc(lv_obj_t *r)
{
    page_bg(r);
    mk_header(r, "CONTROL", 0);
    ccw_build_list();
    ccw_canvas = lv_obj_create(r);
    lv_obj_set_pos(ccw_canvas, 0, 0);
    lv_obj_set_size(ccw_canvas, 240, 240);
    lv_obj_set_style_bg_opa(ccw_canvas, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ccw_canvas, 0, 0);
    lv_obj_set_style_pad_all(ccw_canvas, 0, 0);
    lv_obj_remove_flag(ccw_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ccw_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ccw_canvas, ccw_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(ccw_canvas, ccw_event, LV_EVENT_ALL, NULL);
}

/* ---- launcher ------------------------------------------------------------- */

/* Radial launcher (concept art): the focused app sits large inside a
   stroked centre hex — icon + name — while every app orbits as a small
   hex node joined to the hub by a thin trace. Drag sideways to move
   focus; tap the hub or any node to open that app. One canvas draws
   everything; input runs on the canvas object. */
#define LC_N 9
static const struct {
    const char *name;
    scr_t scr;
    int glyph;
    lv_color_t col;
} lc_apps[LC_N] = {
    {"WORKOUT",  SCR_WORKOUT,  G_WORKOUT,  LV_COLOR_MAKE(0xF0, 0x8C, 0x3C)},
    {"STATS",    SCR_STATS,    G_STATS,    LV_COLOR_MAKE(0x46, 0xD8, 0xE0)},
    {"CARDIO",   SCR_CARDIO,   G_CARDIO,   LV_COLOR_MAKE(0xF0, 0x58, 0x6E)},
    {"SLEEP",    SCR_SLEEP,    G_SLEEP,    LV_COLOR_MAKE(0xB0, 0x6C, 0xF0)},
    {"WATER",    SCR_WATER,    G_WATER,    LV_COLOR_MAKE(0x40, 0xB0, 0xF0)},
    {"CLOCK",    SCR_CLOCK,    G_CLOCK,    LV_COLOR_MAKE(0xF0, 0xD0, 0x40)},
    {"WEATHER",  SCR_WEATHER,  G_WEATHER,  LV_COLOR_MAKE(0x46, 0xD8, 0xE0)},
    {"ALERTS",   SCR_NOTIF,    G_ALERTS,   LV_COLOR_MAKE(0x50, 0xE0, 0x78)},
    {"SETTINGS", SCR_SETTINGS, G_SETTINGS, LV_COLOR_MAKE(0x8A, 0x94, 0xA8)},
};
static int lc_focus;
static lv_obj_t *lc_canvas;
static lv_point_t lc_press;
static int lc_dx, lc_drag_dist;

/* satellite ring positions, focused mirrored in the hub.
   2.0: nodes are 20 px hexes (were 16) on a wider ring, and each node
   takes taps out to LC_SAT_HIT px, nearest node wins, so the tap area
   per node is about 60 px across (was a 48 px circle that the hub tap
   test partly ate). The per-node names and the bottom hint went: at
   12 px they collided with the bigger nodes, and the hub already
   names the focused app. */
#define LC_CY      120
#define LC_HUB     58     /* hub hex circumradius */
#define LC_HUB_HIT 60
#define LC_SAT     20
#define LC_SAT_HIT 34
static void lc_pos(int i, int *x, int *y)
{
    float a = (-90.0f + i * (360.0f / LC_N)) * 0.0174533f;
    *x = 120 + (int)(cosf(a) * 92);
    *y = LC_CY + (int)(sinf(a) * 84);
}

/* tap -> 0..LC_N-1 node, LC_N hub, -1 nothing. Nodes first so the
   ring keeps its full area where it meets the hub. */
static int lc_hit(int px, int py)
{
    int best = -1, bd = LC_SAT_HIT * LC_SAT_HIT;
    for (int i = 0; i < LC_N; i++) {
        int x, y;
        lc_pos(i, &x, &y);
        int d = (px - x) * (px - x) + (py - y) * (py - y);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    if (best >= 0)
        return best;
    int dx = px - 120, dy = py - LC_CY;
    if (dx * dx + dy * dy < LC_HUB_HIT * LC_HUB_HIT)
        return LC_N;
    return -1;
}

static void lc_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    /* traces from hub edge to each satellite */
    for (int i = 0; i < LC_N; i++) {
        int x, y;
        lc_pos(i, &x, &y);
        float dx = x - 120, dy = y - LC_CY;
        float d = sqrtf(dx * dx + dy * dy);
        if (d < 1)
            continue;
        lv_draw_line_dsc_t ln;
        lv_draw_line_dsc_init(&ln);
        ln.color = i == lc_focus ? lv_color_mix(lc_apps[i].col,
                                                lv_color_black(), 170)
                                 : lv_color_hex(0x232C3C);
        ln.width = 1;
        ln.p1.x = 120 + (int)(dx / d * (LC_HUB + 2));
        ln.p1.y = LC_CY + (int)(dy / d * (LC_HUB + 2));
        ln.p2.x = x - (int)(dx / d * (LC_SAT + 4));
        ln.p2.y = y - (int)(dy / d * (LC_SAT + 4));
        lv_draw_line(layer, &ln);
    }
    for (int i = 0; i < LC_N; i++) {
        int x, y, on = i == lc_focus;
        lc_pos(i, &x, &y);
        lv_color_t c = on ? lc_apps[i].col : lv_color_hex(0x38425A);
        hex_stroke(layer, x, y, on ? LC_SAT + 3 : LC_SAT, c, on ? 2 : 1);
        draw_glyph(layer, lc_apps[i].glyph, x, y, 9,
                   on ? lc_apps[i].col : lv_color_hex(0x5A6478));
    }
    /* hub: the focused app, big, double-stroked */
    lv_color_t fc = lc_apps[lc_focus].col;
    hex_stroke(layer, 120, LC_CY, LC_HUB, fc, 2);
    hex_stroke(layer, 120, LC_CY, LC_HUB - 6,
               lv_color_mix(fc, lv_color_black(), 170), 1);
    draw_glyph(layer, lc_apps[lc_focus].glyph, 120, LC_CY - 14, 16, fc);
    draw_txt_c(layer, &kenney16, fc, 120, LC_CY + 12, lc_apps[lc_focus].name);
}

static void lc_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (code == LV_EVENT_PRESSED) {
        lc_dx = 0;
        lc_drag_dist = 0;
        if (in)
            lv_indev_get_point(in, &lc_press);
    } else if (code == LV_EVENT_PRESSING && in) {
        lv_point_t v;
        lv_indev_get_vect(in, &v);
        lc_dx += v.x;
        lc_drag_dist += (v.x < 0 ? -v.x : v.x) + (v.y < 0 ? -v.y : v.y);
        while (lc_dx > 46) {
            lc_dx -= 46;
            lc_focus = (lc_focus + 1) % LC_N;
            lv_obj_invalidate(lc_canvas);
        }
        while (lc_dx < -46) {
            lc_dx += 46;
            lc_focus = (lc_focus + LC_N - 1) % LC_N;
            lv_obj_invalidate(lc_canvas);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lc_dx = 0;
        if (lc_drag_dist >= 10)
            return;
        int h = lc_hit(lc_press.x, lc_press.y);
        if (h == LC_N) {
            buzz(35);
            nav_to(lc_apps[lc_focus].scr);   /* hub tap opens focused */
        } else if (h >= 0) {
            lc_focus = h;               /* nodes only take focus */
            buzz(20);                   /* haptic tick = selection */
            lv_obj_invalidate(lc_canvas);
        }
    }
}

static void lc_tick(void)
{
    /* no animation state yet — focus changes are instant */
}

static void build_launcher(lv_obj_t *r)
{
    page_bg(r);
    lc_canvas = lv_obj_create(r);
    lv_obj_set_pos(lc_canvas, 0, 0);
    lv_obj_set_size(lc_canvas, 240, 240);
    lv_obj_set_style_bg_opa(lc_canvas, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lc_canvas, 0, 0);
    lv_obj_set_style_pad_all(lc_canvas, 0, 0);
    lv_obj_remove_flag(lc_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(lc_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lc_canvas, lc_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(lc_canvas, lc_event, LV_EVENT_ALL, NULL);
}

/* ---- workout -------------------------------------------------------------- */

static void fmt_ms(uint32_t ms, char *out)
{
    unsigned s = ms / 1000;
    /* past an hour (a night in bed, a long stopwatch) go h:mm:ss, not
       "385:00" */
    if (s >= 3600)
        lv_snprintf(out, 12, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    else
        lv_snprintf(out, 12, "%02u:%02u", s / 60, s % 60);
}

static void start_workout(void)
{
    w_active = 1;
    w_paused = 0;
    w_sets = 0;
    w_paused_total = 0;
    w_start_ms = hal_millis();
    nav_to(SCR_W_RUN);
}

static void workout_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    uint32_t now = hal_millis();
    switch (code) {
    case 0: w_routine = -1; start_workout(); break;
    case 1:                     /* pause / resume */
        if (w_paused) {
            w_paused_total += now - w_pause_ms;
            w_paused = 0;
        } else {
            w_pause_ms = now;
            w_paused = 1;
        }
        rebuild();
        break;
    case 2:                     /* end */
        w_last_ms = (w_paused ? w_pause_ms : now) - w_start_ms -
                    w_paused_total;
        w_last_sets = w_sets;
        w_active = 0;
        persist_mark();
        nav_sp = 0;             /* summary returns to the face */
        nav_to(SCR_W_DONE);
        break;
    case 3:
        if (!w_paused) {
            if (++w_sets >= W_MAX_SETS) {
                /* last set ends the session */
                w_last_ms = now - w_start_ms - w_paused_total;
                w_last_sets = w_sets;
                w_active = 0;
                persist_mark();
                nav_sp = 0;
                nav_to(SCR_W_DONE);
                return;
            }
            rebuild();
        }
        break;
    }
}

static void routine_cb(lv_event_t *e)
{
    w_routine = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    start_workout();
}

static void build_w_run(lv_obj_t *r);

static void build_workout(lv_obj_t *r)
{
    if (w_active) {             /* a live session always resumes */
        cur = SCR_W_RUN;
        build_w_run(r);
        return;
    }
    page_bg(r);
    mk_header(r, "WORKOUT", 1);
    static const char *rt[3] = {"PUSH DAY", "PULL DAY", "LEG DAY"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = mk_btn(r, rt[i], 20, 44 + i * 34, 200, 32, C_FILL,
                             routine_cb);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
    }
    lv_obj_t *b = mk_btn(r, "QUICK SESSION", 20, 150, 200, 48, C_ACCENT,
                         workout_cb);
    lv_obj_set_user_data(b, (void *)0);
}

static void build_w_run(lv_obj_t *r)
{
    page_bg(r);
    static const char *rt[3] = {"PUSH DAY", "PULL DAY", "LEG DAY"};
    mk_header(r, w_routine >= 0 ? rt[w_routine] : "QUICK SESSION", 0);
    mk_card(r, 20, 68, 200, 66, C_FILL);
    char buf[16];
    fmt_ms((w_paused ? w_pause_ms : hal_millis()) - w_start_ms -
           w_paused_total, buf);
    lbl_time_big = mk_label_c(r, &kenney24, C_WHITE, 120, 80, buf);
    char sbuf[24];
    lv_snprintf(sbuf, sizeof sbuf, "SET %d OF %d", w_sets + 1, W_MAX_SETS);
    mk_label_c(r, &kenney16, C_MUTED, 120, 116, sbuf);
    lv_obj_t *b;
    b = mk_btn(r, w_sets + 1 >= W_MAX_SETS ? "FINISH SET" : "SET DONE",
               20, 138, 200, 32, C_FILL, workout_cb);
    lv_obj_set_user_data(b, (void *)(intptr_t)3);
    b = mk_btn(r, w_paused ? "RESUME" : "PAUSE", 20, 178, 97, 42, C_FILL,
               workout_cb);
    lv_obj_set_user_data(b, (void *)(intptr_t)1);
    b = mk_btn(r, "END", 123, 178, 97, 42, C_WARN, workout_cb);
    lv_obj_set_user_data(b, (void *)(intptr_t)2);
}

static void build_w_done(lv_obj_t *r)
{
    page_bg(r);
    mk_header(r, "SUMMARY", 1);
    mk_card(r, 20, 76, 200, 84, C_FILL);
    char buf[16];
    fmt_ms(w_last_ms, buf);
    mk_label_c(r, &kenney24, C_WHITE, 120, 90, buf);
    char sbuf[24];
    lv_snprintf(sbuf, sizeof sbuf, "%d SETS DONE", w_last_sets);
    mk_label_c(r, &kenney16, C_ACCENT, 120, 128, sbuf);
    mk_btn(r, "DONE", 20, 176, 200, 40, C_FILL, back_cb);
}

/* ---- sleep ----------------------------------------------------------------- */

static void sleep_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (code == 2) {
        s_track ^= 1;
        if (!s_track)
            s_in_bed = 0;
        persist_mark();
        rebuild();
        return;
    }
    if (!s_track)
        return;
    if (s_in_bed) {
        s_last_ms = hal_millis() - s_start_ms;
        s_in_bed = 0;
        persist_mark();
        nav_sp = 0;
        nav_to(SCR_S_DONE);
    } else {
        s_in_bed = 1;
        s_start_ms = hal_millis();
        rebuild();
    }
}

static void build_sleep(lv_obj_t *r)
{
    page_bg(r);
    mk_header(r, "SLEEP", 1);
    mk_card(r, 20, 38, 200, 64, s_in_bed ? C_WARN : C_FILL);
    mk_label_c(r, &kenney12, C_MUTED, 120, 46, s_in_bed ? "IN BED" : "LAST NIGHT");
    char buf[24];
    if (s_in_bed)
        fmt_ms(hal_millis() - s_start_ms, buf);
    else
        fmt_ms(s_last_ms, buf);
    lbl_time_big = mk_label_c(r, &kenney24, C_WHITE, 120, 66, buf);
    if (s_track || s_in_bed) {
        lv_obj_t *b = mk_btn(r, s_in_bed ? "WAKE UP" : "BEDTIME", 20, 108,
                             200, 52, s_in_bed ? C_WARN : C_ACCENT, sleep_cb);
        lv_obj_set_user_data(b, (void *)(intptr_t)1);
    } else {
        mk_label_c(r, &kenney12, C_MUTED, 120, 128, "TRACKING IS OFF");
    }
    lv_obj_t *t = mk_toggle(r, s_track ? "TRACKING ON" : "TRACKING OFF",
                            20, 178, 200, s_track, sleep_cb);
    lv_obj_set_user_data(t, (void *)(intptr_t)2);
}

static void build_s_done(lv_obj_t *r)
{
    page_bg(r);
    mk_header(r, "NIGHT", 1);
    mk_card(r, 20, 64, 200, 74, C_FILL);
    mk_label_c(r, &kenney12, C_MUTED, 120, 76, "TIME IN BED");
    char buf[16];
    fmt_ms(s_last_ms, buf);
    mk_label_c(r, &kenney24, C_WHITE, 120, 98, buf);
    mk_btn(r, "DONE", 20, 176, 200, 40, C_FILL, back_cb);
}

/* ---- water ----------------------------------------------------------------- */

static void water_cb(lv_event_t *e)
{
    water_ml += (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (water_ml < 0)
        water_ml = 0;
    if (water_ml > 10000)
        water_ml = 10000;
    persist_mark();
    rebuild();
}

static void build_water(lv_obj_t *r)
{
    page_bg(r);
    mk_header(r, "WATER", 1);
    mk_card(r, 20, 70, 200, 94, C_FILL);
    char buf[24];
    lv_snprintf(buf, sizeof buf, "%d ML", water_ml);
    mk_label_c(r, &kenney24, C_ACCENT, 120, 82, buf);
    mk_label_c(r, &kenney12, C_MUTED, 120, 118, "GOAL 2000 ML");
    lv_obj_t *bar = lv_bar_create(r);
    lv_obj_set_pos(bar, 30, 138);
    lv_obj_set_size(bar, 180, 16);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);   /* display only */
    lv_bar_set_range(bar, 0, 2000);
    lv_bar_set_value(bar, water_ml > 2000 ? 2000 : water_ml, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, C_EDGE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_obj_t *b;
    b = mk_btn(r, "+250", 20, 178, 97, 42, C_FILL, water_cb);
    lv_obj_set_user_data(b, (void *)(intptr_t)250);
    b = mk_btn(r, "+500", 123, 178, 97, 42, C_FILL, water_cb);
    lv_obj_set_user_data(b, (void *)(intptr_t)500);
}

/* ---- clock ----------------------------------------------------------------- */

static void clock_cb(lv_event_t *e);

static void build_clock(lv_obj_t *r)
{
    page_bg(r);
    mk_header(r, "CLOCK", 1);
    /* tabs: 40 px tall (were 70x28, and STOPWATCH at 16 px overflowed
       its tab). Widths follow the label so all three fit one row. */
    static const char *tab[3] = {"STOPWATCH", "TIMER", "ALARM"};
    static const int tab_x[3] = {4, 110, 174}, tab_w[3] = {104, 62, 62};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = mk_btn(r, tab[i], tab_x[i], 44, tab_w[i], 40,
                             clock_tab == i ? C_ACCENT : C_FILL, clock_cb);
        lv_obj_set_user_data(b, (void *)(intptr_t)(20 + i));
        lv_obj_t *l = lv_obj_get_child(b, 0);
        lv_obj_set_style_text_font(l, &kenney12, 0);
        lv_obj_set_style_text_color(l, clock_tab == i ? C_BG : C_WHITE, 0);
    }
    if (clock_tab == 0) {
        char buf[16];
        fmt_ms(sw_acc + (sw_running ? hal_millis() - sw_t0 : 0), buf);
        lbl_time_big = mk_label_c(r, &kenney24, C_WHITE, 120, 104, buf);
        lv_obj_t *b;
        b = mk_btn(r, sw_running ? "STOP" : "START", 6, 150, 111, 60,
                   C_ACCENT, clock_cb);
        lv_obj_set_user_data(b, (void *)(intptr_t)10);
        b = mk_btn(r, "RESET", 123, 150, 111, 60, C_FILL, clock_cb);
        lv_obj_set_user_data(b, (void *)(intptr_t)11);
    } else if (clock_tab == 1) {
        if (timer_done) {
            mk_label_c(r, &kenney16, C_WARN, 120, 108, "TIME'S UP");
            lv_obj_t *b = mk_btn(r, "DISMISS", 6, 150, 228, 60, C_FILL,
                                 clock_cb);
            lv_obj_set_user_data(b, (void *)(intptr_t)12);
        } else if (timer_end_ms) {
            char buf[16];
            fmt_ms(timer_end_ms - hal_millis(), buf);
            lbl_time_big = mk_label_c(r, &kenney24, C_WARN, 120, 104, buf);
            lv_obj_t *b = mk_btn(r, "CANCEL", 6, 150, 228, 60, C_FILL,
                                 clock_cb);
            lv_obj_set_user_data(b, (void *)(intptr_t)13);
        } else {
            /* presets: 72x64 (were 68x40) */
            static const char *p[3] = {"1 MIN", "5 MIN", "10 MIN"};
            for (int i = 0; i < 3; i++) {
                lv_obj_t *b = mk_btn(r, p[i], 6 + i * 78, 116, 72, 64,
                                     C_FILL, clock_cb);
                lv_obj_set_user_data(b, (void *)(intptr_t)(30 + i));
            }
        }
    } else {
        char buf[8];
        lv_snprintf(buf, sizeof buf, "%02d:%02d", alarm_h, alarm_m);
        if (alarm_ringing) {
            mk_label_c(r, &kenney24, C_WARN, 120, 100, buf);
            lv_obj_t *b = mk_btn(r, "STOP", 6, 140, 228, 72, C_WARN,
                                 clock_cb);
            lv_obj_set_user_data(b, (void *)(intptr_t)15);
            return;
        }
        /* steppers 62x48 / 62x46 (were 40x36 and 40x28); the labels
           now say what they step, so the "HOUR / 15 MIN" caption goes */
        lv_obj_t *b;
        b = mk_btn(r, "-1H", 6, 92, 62, 48, C_FILL, clock_cb);
        lv_obj_set_user_data(b, (void *)(intptr_t)40);
        mk_label_c(r, &kenney24, C_WHITE, 120, 103, buf);
        b = mk_btn(r, "+1H", 172, 92, 62, 48, C_FILL, clock_cb);
        lv_obj_set_user_data(b, (void *)(intptr_t)41);
        b = mk_btn(r, "-15M", 6, 144, 62, 46, C_FILL, clock_cb);
        lv_obj_set_user_data(b, (void *)(intptr_t)42);
        b = mk_btn(r, "+15M", 172, 144, 62, 46, C_FILL, clock_cb);
        lv_obj_set_user_data(b, (void *)(intptr_t)43);
        if (!hal_clock_synced()) {
            mk_label_c(r, &kenney12, C_MUTED, 120, 153, "SET TIME");
            mk_label_c(r, &kenney12, C_MUTED, 120, 167, "FIRST");
        }
        b = mk_toggle(r, alarm_on ? "ALARM ON" : "ALARM OFF",
                      6, 196, 228, alarm_on, clock_cb);
        lv_obj_set_height(b, 40);
        lv_obj_set_user_data(b, (void *)(intptr_t)14);
    }
}

static void clock_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (v >= 20) {
        clock_tab = v - 20;
        rebuild();
        return;
    }
    uint32_t now = hal_millis();
    switch (v) {
    case 10:
        if (sw_running) { sw_acc += now - sw_t0; sw_running = 0; }
        else { sw_t0 = now; sw_running = 1; }
        break;
    case 11: sw_running = 0; sw_acc = 0; break;
    case 12: timer_done = 0; break;
    case 13: timer_end_ms = 0; break;
    case 14: alarm_on ^= 1; persist_mark(); break;
    case 15:
        alarm_ringing = 0;
        hal_motor(0);
        motor_off_at = 0;
        break;
    case 40: alarm_h = (alarm_h + 23) % 24; persist_mark(); break;
    case 41: alarm_h = (alarm_h + 1) % 24; persist_mark(); break;
    case 42: alarm_m = (alarm_m + 45) % 60; persist_mark(); break;
    case 43: alarm_m = (alarm_m + 15) % 60; persist_mark(); break;
    case 30: timer_end_ms = now + 60000; break;
    case 31: timer_end_ms = now + 300000; break;
    case 32: timer_end_ms = now + 600000; break;
    }
    rebuild();
}

/* ---- settings --------------------------------------------------------------- */

static void settings_cb(lv_event_t *e);
static void diag_open_cb(lv_event_t *e);

/* Settings is a paged list: five full-width rows per page (216x34,
   was a 60x15 pill per row), swipe up/down to page. Nothing here
   scrolls, in keeping with the 1.6.2 no-scroll rule; paging rides the
   same hardware gesture path the rest of the OS uses. */
#define SET_ROWS     14
#define SET_PER_PAGE 5

static lv_obj_t *mk_row(lv_obj_t *r, int y, const char *name,
                        const char *val, int on, lv_event_cb_t cb)
{
    lv_obj_t *b = mk_btn(r, name, 12, y, 216, 32, C_FILL, cb);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
    if (val) {
        lv_obj_t *pill = lv_obj_create(b);
        lv_obj_set_size(pill, 64, 22);
        lv_obj_align(pill, LV_ALIGN_RIGHT_MID, -5, 0);
        lv_obj_set_style_radius(pill, 3, 0);
        lv_obj_set_style_border_width(pill, on ? 0 : 1, 0);
        lv_obj_set_style_border_color(pill, C_EDGE, 0);
        lv_obj_set_style_bg_color(pill, on ? C_ACCENT : C_EDGE, 0);
        lv_obj_set_style_pad_all(pill, 0, 0);
        pill_style(pill);
        lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *vl = lv_label_create(pill);
        lv_obj_set_style_text_font(vl, strlen(val) > 4 ? &kenney12 : &kenney16, 0);
        lv_obj_set_style_text_color(vl, on ? C_BG : C_MUTED, 0);
        lv_label_set_text(vl, val);
        lv_obj_center(vl);
    }
    return b;
}

static void build_settings(lv_obj_t *r)
{
    page_bg(r);
    {
        char tt[16];
        lv_snprintf(tt, sizeof tt, "SETTINGS %d/%d", set_page + 1,
                    (SET_ROWS + SET_PER_PAGE - 1) / SET_PER_PAGE);
        mk_header(r, tt, 1);
    }
    static const char *rows[13] = {
        "DND", "AIRPLANE", "BLUETOOTH", "TORCH", "WATER LOCK",
        "BRIGHTNESS", "CC TORCH", "CC WATER", "CC ALERTS",
        "SET TIME", "FACE", "HEART RATE", "INFINILINK"
    };
    int vals[13] = {dnd_on, airplane_on, bt_on, torch_on, water_lock,
                    brightness, !(cc_hide & 1), !(cc_hide & 2),
                    !(cc_hide & 4), 1, face_dig, sensors_hr_on(),
                    ble_get_compat()};
    int first = set_page * SET_PER_PAGE;
    for (int i = first; i < SET_ROWS && i < first + SET_PER_PAGE; i++) {
        int y = 42 + (i - first) * 36;
        if (i == 13) {
            static const char *bs[5] = {"OFF", "START", "ON", "LINK",
                                        "RETRY"};
            int st = ble_status();
            lv_obj_t *db = mk_row(r, y, "BLE / DIAG",
                                  bs[st < 0 || st > 4 ? 4 : st],
                                  st == 2 || st == 3, diag_open_cb);
            (void)db;
            continue;
        }
        char v[8];
        if (i == 5)
            lv_snprintf(v, sizeof v, "%s", bright_name());
        else if (i == 9)
            lv_snprintf(v, sizeof v, ">");
        else if (i == 10)
            lv_snprintf(v, sizeof v, face_dig ? "DIGI" : "ANLG");
        else
            lv_snprintf(v, sizeof v, "%s", vals[i] ? "ON" : "OFF");
        lv_obj_t *b = mk_row(r, y, rows[i], v,
                             i == 9 || i == 10 ? 0 : vals[i], settings_cb);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
    }
    /* page hint along the bottom */
    if (set_page < (SET_ROWS - 1) / SET_PER_PAGE)
        mk_label_c(r, &kenney12, C_MUTED, 120, 223, "SWIPE UP FOR MORE");
    else
        mk_label_c(r, &kenney12, C_MUTED, 120, 223, "VOIDOS " VOIDOS_VERSION);
}

/* manual time set, like the reference watches: +/- on hour/minute,
   DONE writes it and marks the clock synced */
static int set_h, set_m;

static void settime_cb(lv_event_t *e)
{
    int act = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (act == 0) set_h = (set_h + 23) % 24;
    if (act == 1) set_h = (set_h + 1) % 24;
    if (act == 2) set_m = (set_m + 59) % 60;
    if (act == 3) set_m = (set_m + 1) % 60;
    if (act == 4) {
        int y, mo, d, dow;
        hal_clock_date(&y, &mo, &d, &dow);
        if (!y) { y = 2025; mo = 1; d = 1; dow = 3; }
        hal_clock_set(y, mo, d, dow, set_h, set_m, 0);
        nav_to(SCR_SETTINGS);
        return;
    }
    if (act == 5) {
        nav_to(SCR_SETTINGS);
        return;
    }
    rebuild();
}

static void build_settime(lv_obj_t *r)
{
    page_bg(r);
    mk_header(r, "SET TIME", 1);
    mk_card(r, 20, 40, 90, 52, C_FILL);
    mk_card(r, 130, 40, 90, 52, C_FILL);
    if (!set_h && !set_m) {
        set_h = hal_clock_hour();
        set_m = hal_clock_minute();
    }
    /* hour column left, minute column right, + above -, each 90x40
       (1.6.2 had four 40x32 buttons, and hour + overlapped minute -) */
    char buf[8];
    mk_label_c(r, &kenney12, C_MUTED, 65, 44, "HOUR");
    mk_label_c(r, &kenney12, C_MUTED, 175, 44, "MIN");
    lv_snprintf(buf, sizeof buf, "%02d", set_h);
    mk_label_c(r, &kenney24, C_WHITE, 65, 60, buf);
    mk_label_c(r, &kenney24, C_ACCENT, 120, 60, ":");
    lv_snprintf(buf, sizeof buf, "%02d", set_m);
    mk_label_c(r, &kenney24, C_WHITE, 175, 60, buf);
    lv_obj_set_user_data(mk_btn(r, "+", 20, 96, 90, 40, C_FILL, settime_cb),
                         (void *)(intptr_t)1);
    lv_obj_set_user_data(mk_btn(r, "-", 20, 142, 90, 40, C_FILL, settime_cb),
                         (void *)(intptr_t)0);
    lv_obj_set_user_data(mk_btn(r, "+", 130, 96, 90, 40, C_FILL, settime_cb),
                         (void *)(intptr_t)3);
    lv_obj_set_user_data(mk_btn(r, "-", 130, 142, 90, 40, C_FILL, settime_cb),
                         (void *)(intptr_t)2);
    lv_obj_set_user_data(mk_btn(r, "DONE", 50, 190, 140, 42, C_ACCENT,
                                settime_cb), (void *)(intptr_t)4);
}

static void settings_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    switch (i) {
    case 0: dnd_on ^= 1; break;
    case 1: airplane_on ^= 1; apply_radio(); break;
    case 2:
        bt_on ^= 1;
        if (bt_on)
            airplane_on = 0;
        apply_radio();
        break;
    case 3:
        torch_set(!torch_on);
        break;
    case 4: wlock_set(!water_lock); break;
    case 5:
        bright_cycle();
        if (!torch_on)
            display_backlight(bl_level());
        break;
    case 6: cc_hide ^= 1; break;
    case 7: cc_hide ^= 2; break;
    case 8: cc_hide ^= 4; break;
    case 9:
        set_h = hal_clock_hour();
        set_m = hal_clock_minute();
        nav_to(SCR_SETTIME);
        return;
    case 10: face_dig ^= 1; break;
    case 11: sensors_hr_set(!sensors_hr_on()); break;
    case 12: ble_set_compat(!ble_get_compat()); break;
    }
    rebuild();
    persist_mark();
}

/* ---- stats ------------------------------------------------------------------- */

static void ppg_draw(lv_event_t *ev)
{
    lv_layer_t *layer = lv_event_get_layer(ev);
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target(ev), &area);
    int x0 = area.x1, x1 = area.x2;
    int mid = (area.y1 + area.y2) / 2;
    int half = (area.y2 - area.y1) / 2 - 2;
    int y100;
    if (!sensors_ppg(0, &y100))
        return;
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = lv_color_hex(0xF0A03C);   /* concept's orange trace */
    dsc.width = 2;
    int prev_y = mid - y100 * half / 100;
    for (int x = x0 + 1; x <= x1; x++) {
        sensors_ppg((x - x0) * 127 / (x1 - x0), &y100);
        int y = mid - y100 * half / 100;
        dsc.p1.x = x - 1;
        dsc.p1.y = prev_y;
        dsc.p2.x = x;
        dsc.p2.y = y;
        lv_draw_line(layer, &dsc);
        prev_y = y;
    }
}

/* one stat at a time in the hub, a hex strip picks which — drag or tap
   a hex to change focus, tap the hub for the deep screen */
static const struct { const char *name; int glyph; lv_color_t col;
                      int to; } st_def[] = {
    {"STEPS",  G_STEPS,  LV_COLOR_MAKE(0x46,0xD8,0xE0), -1},
    {"HEART",  G_CARDIO, LV_COLOR_MAKE(0xF0,0x58,0x6E), SCR_CARDIO},
    {"REST",   G_DIAG,   LV_COLOR_MAKE(0xF0,0x8C,0x3C), SCR_CARDIO},
    {"ENERGY", G_BOLT,   LV_COLOR_MAKE(0xF0,0xD0,0x40), -1},
    {"SLEEP",  G_SLEEP,  LV_COLOR_MAKE(0xB0,0x6C,0xF0), SCR_SLEEP},
    {"WATER",  G_WATER,  LV_COLOR_MAKE(0x40,0xB0,0xF0), SCR_WATER},
};
#define ST_N ((int)(sizeof st_def / sizeof st_def[0]))
static int st_focus;
static lv_obj_t *st_canvas;
static lv_point_t st_press;
static int st_dx, st_drag;

static void st_value(int i, char *v, char *u)
{
    int steps = ui_steps();
    int bpm = sensors_bpm();
    switch (i) {
    case 0:
        lv_snprintf(v, 16, "%d", steps > 0 ? steps : 0);
        lv_snprintf(u, 12, "TODAY");
        break;
    case 1:
        lv_snprintf(v, 16, "%s", bpm > 0 ? "" : "--");
        if (bpm > 0)
            lv_snprintf(v, 16, "%d", bpm);
        lv_snprintf(u, 12, "%s", bpm <= 0 ? "NO SIGNAL"
                                 : sensors_bpm_stale() ? "LAST BPM" : "BPM");
        break;
    case 2:
        lv_snprintf(v, 16, "%s", resting_bpm ? "" : "--");
        if (resting_bpm)
            lv_snprintf(v, 16, "%d", resting_bpm);
        lv_snprintf(u, 12, "%s", resting_bpm ? "BPM" : "LEARNING");
        break;
    case 3:
        lv_snprintf(v, 16, "%d", steps > 0 ? steps * 4 / 100 : 0);
        lv_snprintf(u, 12, "KCAL");
        break;
    case 4:
        if (s_in_bed) {
            lv_snprintf(v, 16, "IN BED");
            u[0] = 0;
        } else if (s_last_ms) {
            unsigned sm = s_last_ms / 60000;
            lv_snprintf(v, 16, "%uH%uM", sm / 60, sm % 60);
            lv_snprintf(u, 12, "LAST NIGHT");
        } else {
            lv_snprintf(v, 16, "--");
            lv_snprintf(u, 12, "NO DATA");
        }
        break;
    case 5:
        lv_snprintf(v, 16, "%d", water_ml);
        lv_snprintf(u, 12, "ML TODAY");
        break;
    }
}

/* 2.0: the node row spreads to the full width (40 px pitch, was 35)
   with 17 px hexes (were 14); each node takes taps out to ST_SAT_HIT px,
   nearest node wins, and the workout strip below takes the whole
   bottom band. */
#define ST_CY       106
#define ST_HUB_HIT  58
#define ST_SAT      17
#define ST_SAT_HIT  30
#define ST_STRIP_Y0 210
static void st_pos(int i, int *x, int *y)
{
    *x = 19 + i * 40;
    *y = 186;
}

/* tap -> -2 workout strip, ST_N hub, 0..ST_N-1 node, -1 nothing */
static int st_hit(int px, int py)
{
    if (py >= ST_STRIP_Y0 && py <= 239)
        return -2;
    int best = -1, bd = ST_SAT_HIT * ST_SAT_HIT;
    for (int i = 0; i < ST_N; i++) {
        int x, y;
        st_pos(i, &x, &y);
        int d = (px - x) * (px - x) + (py - y) * (py - y);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    if (best >= 0)
        return best;
    int dx = px - 120, dy = py - ST_CY;
    if (dx * dx + dy * dy < ST_HUB_HIT * ST_HUB_HIT)
        return ST_N;
    return -1;
}

static void st_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    for (int i = 0; i < ST_N; i++) {
        int x, y, foc = i == st_focus;
        st_pos(i, &x, &y);
        hex_stroke(layer, x, y, foc ? ST_SAT + 3 : ST_SAT,
                   foc ? st_def[i].col : lv_color_hex(0x38425A),
                   foc ? 2 : 1);
        draw_glyph(layer, st_def[i].glyph, x, y, 8,
                   foc ? st_def[i].col : lv_color_hex(0x5A6478));
    }
    lv_color_t fc = st_def[st_focus].col;
    hex_stroke(layer, 120, 106, 54, fc, 2);
    hex_stroke(layer, 120, 106, 49,
               lv_color_mix(fc, lv_color_black(), 170), 1);
    draw_glyph(layer, st_def[st_focus].glyph, 120, 82, 11, fc);
    char v[16], u[12];
    st_value(st_focus, v, u);
    draw_txt_c(layer, &kenney24, fc, 120, 96, v);
    draw_txt_c(layer, &kenney12, lv_color_hex(0x8A94A8), 120, 124,
               st_def[st_focus].name);
    draw_txt_c(layer, &kenney12, lv_color_mix(fc, lv_color_black(), 140),
               120, 136, u);
    /* workout status strip — tap opens the workout app */
    lv_draw_rect_dsc_t bo;
    lv_draw_rect_dsc_init(&bo);
    bo.border_width = 1;
    bo.border_color = lv_color_hex(0x38425A);
    bo.border_opa = LV_OPA_COVER;
    bo.bg_opa = LV_OPA_TRANSP;
    lv_area_t wb = {12, 213, 228, 235};
    lv_draw_rect(layer, &bo, &wb);
    char ws[28];
    if (w_active)
        lv_snprintf(ws, sizeof ws, "WORKOUT LIVE - %d SETS", w_sets);
    else if (w_last_ms)
        lv_snprintf(ws, sizeof ws, "LAST %uM - %d SETS",
                    (unsigned)(w_last_ms / 60000u), w_last_sets);
    else
        lv_snprintf(ws, sizeof ws, "WORKOUT - NONE YET");
    draw_txt_c(layer, &kenney12,
               w_active ? C_WARN : lv_color_hex(0x5A6478), 120, 217, ws);
}

static void st_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (code == LV_EVENT_PRESSED) {
        st_dx = st_drag = 0;
        if (in)
            lv_indev_get_point(in, &st_press);
    } else if (code == LV_EVENT_PRESSING && in) {
        lv_point_t v;
        lv_indev_get_vect(in, &v);
        st_dx += v.x;
        st_drag += (v.x < 0 ? -v.x : v.x) + (v.y < 0 ? -v.y : v.y);
        while (st_dx > 42) {
            st_dx -= 42;
            st_focus = (st_focus + 1) % ST_N;
            lv_obj_invalidate(st_canvas);
        }
        while (st_dx < -42) {
            st_dx += 42;
            st_focus = (st_focus + ST_N - 1) % ST_N;
            lv_obj_invalidate(st_canvas);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        st_dx = 0;
        if (st_drag >= 10)
            return;
        int h = st_hit(st_press.x, st_press.y);
        if (h == -2) {
            buzz(35);
            nav_to(SCR_WORKOUT);       /* footer strip */
        } else if (h == ST_N) {
            if (st_def[st_focus].to >= 0) {
                buzz(35);
                nav_to((scr_t)st_def[st_focus].to);
            }
        } else if (h >= 0) {
            st_focus = h;
            buzz(20);
            lv_obj_invalidate(st_canvas);
        }
    }
}

static void build_stats(lv_obj_t *r)
{
    page_bg(r);
    mk_header(r, "STATS", 0);
    st_canvas = lv_obj_create(r);
    lv_obj_set_pos(st_canvas, 0, 0);
    lv_obj_set_size(st_canvas, 240, 240);
    lv_obj_set_style_bg_opa(st_canvas, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(st_canvas, 0, 0);
    lv_obj_set_style_pad_all(st_canvas, 0, 0);
    lv_obj_remove_flag(st_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(st_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(st_canvas, st_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(st_canvas, st_event, LV_EVENT_ALL, NULL);
}

/* ---- notifications + cardio ------------------------------------------------ */

/* ---- notifications -------------------------------------------------------- */

int ancs_ready(void), ancs_state(void), ancs_count(void);
int ancs_take_dirty(void), ancs_take_new(void);
struct ancs_notif_view {
    uint32_t uid; uint8_t flags; uint8_t has_pos, has_neg;
    const char *app, *title, *msg; uint32_t rx_ms;
};
const struct ancs_notif_view *ancs_get(int i);
void ancs_dismiss(uint32_t uid);
void ancs_positive(uint32_t uid);

/* selection is by uid (declared near the nav stack), never by list
   index: the list reorders as notifications arrive and clear */
static int notif_reply;            /* 1 = action confirm open */

static void notif_reset_view(void)
{
    notif_sel_uid = 0;
    notif_reply = 0;
}

static const struct ancs_notif_view *notif_selected(void)
{
    for (int i = 0; i < ancs_count(); i++) {
        const struct ancs_notif_view *v = ancs_get(i);
        if (v && v->uid == notif_sel_uid)
            return v;
    }
    return NULL;
}

static void notif_item_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    const struct ancs_notif_view *v = ancs_get(i);
    if (v) {
        notif_sel_uid = v->uid;
        rebuild();
    }
}

static void notif_back_cb(lv_event_t *e)
{
    (void)e;
    notif_reset_view();
    rebuild();
}

static void notif_dismiss_cb(lv_event_t *e)
{
    (void)e;
    const struct ancs_notif_view *v = notif_selected();
    if (v)
        ancs_dismiss(v->uid);
    notif_reset_view();
    rebuild();
}

static void notif_reply_cb(lv_event_t *e)
{
    (void)e;
    notif_reply = 1;
    rebuild();
}

static void notif_send_cb(lv_event_t *e)
{
    (void)e;
    /* ANCS has one positive action per notification (Reply / Answer /
       Open); it carries no text, so the watch can only trigger it */
    const struct ancs_notif_view *v = notif_selected();
    if (v)
        ancs_positive(v->uid);
    notif_reset_view();
    rebuild();
}

static void build_notif(lv_obj_t *r)
{
    page_bg(r);

    if (notif_sel_uid) {
        /* detail: app + title + body, actions at the bottom */
        const struct ancs_notif_view *v = notif_selected();
        if (!v) {
            notif_reset_view();
            rebuild();
            return;
        }
        lv_obj_set_ext_click_area(
            mk_btn(r, "<", 12, 10, 36, 26, C_FILL, notif_back_cb), 8);
        lv_obj_t *an = mk_label(r, &kenney12, C_MUTED, 56, 16,
                                v->app[0] ? v->app : "NOTIFICATION");
        lv_obj_set_width(an, 168);       /* app names run to 19 chars */
        lv_label_set_long_mode(an, LV_LABEL_LONG_DOT);
        lv_obj_t *t = mk_label(r, &kenney16, C_WHITE, 16, 44,
                               v->title[0] ? v->title : "(NO TITLE)");
        lv_obj_set_width(t, 208);
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_t *m = mk_label(r, &kenney12, C_MUTED, 16, 78, v->msg);
        lv_obj_set_width(m, 208);
        lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);

        char ago[16];
        unsigned mins = (hal_millis() - v->rx_ms) / 60000u;
        lv_snprintf(ago, sizeof ago, mins ? "%uM AGO" : "JUST NOW", mins);
        mk_label(r, &kenney12, C_MUTED, 160, 16, ago);
        if (notif_reply) {
            mk_label_c(r, &kenney12, C_MUTED, 120, 158,
                       "OPEN ON PHONE AND REPLY?");
            mk_btn(r, "YES", 16, 182, 100, 42, C_ACCENT, notif_send_cb);
            mk_btn(r, "NO", 124, 182, 100, 42, C_FILL, notif_back_cb);
        } else {
            if (v->has_pos)
                mk_btn(r, "REPLY", 16, 182, 100, 42, C_FILL,
                       notif_reply_cb);
            mk_btn(r, "DISMISS", v->has_pos ? 124 : 16,
                   182, v->has_pos ? 100 : 208, 42, C_FILL,
                   notif_dismiss_cb);
        }
        return;
    }

    {
        char tt[24];
        int n = ancs_count();
        lv_snprintf(tt, sizeof tt, n ? "ALERTS (%d)" : "ALERTS", n);
        mk_header(r, tt, 0);
    }

    if (!ble_connected()) {
        mk_label_c(r, &kenney12, C_WARN, 120, 110, "PHONE NOT CONNECTED");
        mk_label_c(r, &kenney12, C_MUTED, 120, 130,
                   "PAIR FOR NOTIFICATIONS");
        return;
    }
    if (ancs_state() < 6 /* ST_LIVE */) {
        mk_label_c(r, &kenney12, C_MUTED, 120, 110, "SYNCING...");
        mk_label_c(r, &kenney12, C_MUTED, 120, 130,
                   "ANCS HANDSHAKE");
        return;
    }
    if (!ancs_count()) {
        mk_icon_at(r, 13, 0, 108, 80);   /* cloud */
        mk_label_c(r, &kenney16, C_WHITE, 120, 118, "INBOX ZERO");
        mk_label_c(r, &kenney12, C_MUTED, 120, 140, "ALL CAUGHT UP");
        return;
    }

    for (int i = 0; i < ancs_count() && i < 4; i++) {
        const struct ancs_notif_view *v = ancs_get(i);
        if (!v)
            continue;
        int y = 40 + i * 48;
        lv_obj_t *row = lv_obj_create(r);
        lv_obj_set_pos(row, 12, y);
        lv_obj_set_size(row, 216, 44);
        /* chamfered-outline card, like the concept list items */
        /* neon pill: cyan for ones that alerted, dim for silent */
        lv_obj_set_style_bg_color(row, (v->flags & 0x01) ? C_EDGE
                                       : lv_color_hex(0x0B0E14), 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        pill_style(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, notif_item_cb, LV_EVENT_CLICKED, NULL);
        /* accent dot for notifications that alerted (not silent) */
        if (v->flags & 0x01)
            goto labels;
        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_set_pos(dot, 8, 18);
        lv_obj_set_size(dot, 7, 7);
        lv_obj_set_style_bg_color(dot, C_WARN, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, 0, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_remove_flag(dot,
                           LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
labels:
        mk_label(row, &kenney12, C_MUTED, 24, 6,
                 v->app[0] ? v->app : "APP");
        lv_obj_t *tl = mk_label(row, &kenney16, C_WHITE, 24, 20,
                                v->title[0] ? v->title : v->msg);
        lv_obj_set_width(tl, 184);
        lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
    }
}

static lv_obj_t *cardio_trace;
static lv_obj_t *cardio_bpm;
static lv_obj_t *cardio_art;

/* HR zone from an assumed HRmax of 190 — concept cardio satellites */
static void cardio_zone(int bpm, char *z, const char **name)
{
    static const char *zn[6] = {"--", "RECOVERY", "FAT BURN", "AEROBIC",
                                "THRESHOLD", "MAX"};
    int zi = bpm <= 0 ? 0 : bpm < 95 ? 1 : bpm < 114 ? 2 : bpm < 133 ? 3
                                     : bpm < 152 ? 4 : 5;
    lv_snprintf(z, 4, zi ? "Z%d" : "--", zi);
    *name = zn[zi];
}

static void cardio_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    int bpm = sensors_bpm();
    int locked = bpm > 0;
    int stale = locked && sensors_bpm_stale();
    lv_color_t hc = stale ? lv_color_hex(BPM_STALE_RGB)
                  : locked ? lv_color_hex(0xF0586E)
                           : lv_color_hex(0x38425A);
    /* hub hex with the heart glyph; the BPM object prints inside */
    hex_stroke(layer, 120, 92, 44, hc, 2);
    hex_stroke(layer, 120, 92, 39, lv_color_mix(hc, lv_color_black(), 170), 1);
    if (!locked) {
        draw_glyph(layer, G_CARDIO, 120, 92, 14, hc);
    } else {
        /* held reading: grey hub + number, LAST instead of BPM */
        draw_txt_c(layer, &kenney12, lv_color_hex(0x8A94A8), 120, 118,
                   stale ? "LAST" : "BPM");
    }
    /* satellites: zone left, resting right */
    char z[4];
    const char *zn;
    cardio_zone(bpm, z, &zn);
    hex_stroke(layer, 52, 78, 21, C_ACCENT, 1);
    draw_txt_c(layer, &kenney16, C_ACCENT, 52, 68, z);
    draw_txt_c(layer, &kenney12, lv_color_hex(0x5A6478), 52, 86, zn);
    hex_stroke(layer, 188, 78, 21, lv_color_hex(0xB06CF0), 1);
    char rb[8];
    lv_snprintf(rb, sizeof rb, resting_bpm ? "%d" : "--", resting_bpm);
    draw_txt_c(layer, &kenney16, lv_color_hex(0xB06CF0), 188, 68, rb);
    draw_txt_c(layer, &kenney12, lv_color_hex(0x5A6478), 188, 86, "REST");
    /* grid behind the trace, the concept's ECG panel */
    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.color = lv_color_hex(0x141C28);
    ln.width = 1;
    for (int y = 156; y < 232; y += 10) {
        ln.p1.x = 10; ln.p1.y = y; ln.p2.x = 230; ln.p2.y = y;
        lv_draw_line(layer, &ln);
    }
    for (int x = 10; x < 230; x += 20) {
        ln.p1.x = x; ln.p1.y = 152; ln.p2.x = x; ln.p2.y = 230;
        lv_draw_line(layer, &ln);
    }
}

static void build_cardio(lv_obj_t *r)
{
    page_bg(r);
    mk_header(r, "CARDIO", 0);

    /* one canvas for the hexes + trace grid */
    cardio_art = lv_obj_create(r);
    lv_obj_set_pos(cardio_art, 0, 0);
    lv_obj_set_size(cardio_art, 240, 240);
    lv_obj_set_style_bg_opa(cardio_art, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cardio_art, 0, 0);
    lv_obj_set_style_pad_all(cardio_art, 0, 0);
    lv_obj_remove_flag(cardio_art,
                       LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(cardio_art, cardio_draw, LV_EVENT_DRAW_MAIN, NULL);

    /* big live BPM on top of the hub hex */
    int bpm = sensors_bpm();
    char buf[16];
    if (bpm > 0)
        lv_snprintf(buf, sizeof buf, "%d", bpm);
    else
        buf[0] = 0;
    cardio_bpm = mk_label_c(r, &kenney56,
                            lv_color_hex(bpm > 0 && sensors_bpm_stale()
                                         ? BPM_STALE_RGB : 0xF0586E),
                            120, 60, buf);

    /* full-width live PPG — the real optical signal, orange per concept */
    lv_obj_t *t = lv_obj_create(r);
    lv_obj_set_pos(t, 10, 152);
    lv_obj_set_size(t, 220, 80);
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_pad_all(t, 0, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(t, ppg_draw, LV_EVENT_DRAW_MAIN, NULL);
    cardio_trace = t;
}


/* ---- on-watch diagnostics ------------------------------------------------- */

int sensors_hrs_raw(void), sensors_hrs_ac(void), sensors_hrs_fs10(void);
int sensors_hrs_snr10(void), sensors_hrs_windows(void), sensors_retries(void);
int sensors_bma_engine(void), sensors_bma_dbg(void), sensors_hrs_pk(void);
int hal_battery_raw(void);
int hal_clock_synced(void), hal_boot_confirmed(void);
int ancs_cts_state(void), ancs_sub_fail(void), sensors_hrs_als(void);
void ble_link_diag(int *conns, int *sec_rc, int *enc_status,
                   int *disc_reason, int *encrypted);
void ble_bond_diag(int *bonds, int *repairs);
int ams_state(void), ancs_seq_phase(void);

static lv_obj_t *diag_lbl;
static unsigned diag_loops, diag_lps;
static uint32_t diag_t0;

static void diag_open_cb(lv_event_t *e)
{
    (void)e;
    nav_to(SCR_DIAG);
}

static void diag_fill(void)
{
    char b[400];
    int conns, sec, enc, disc, encd;
    int bonds, repairs;
    ble_link_diag(&conns, &sec, &enc, &disc, &encd);
    ble_bond_diag(&bonds, &repairs);
    lv_snprintf(b, sizeof b,
        "HRS ok%d raw%d als%d ac%d\n"
        " snr%d.%d fs%d.%d pk%d bpm%d\n"
        "BMA ok%d eng%d stp%d id%X\n"
        " st%X f%d rty%d\n"
        "BLE st%d conn%d enc%d\n"
        " sec%d encst%d disc%d\n"
        " ancs%d sub%d cts%d time%d\n"
        " ams%d ph%d bond%d rp%d\n"
        "LOOP %d/s bat%d%% r%d fw%d\n"
        "VOIDOS " VOIDOS_VERSION,
        sensors_hrs_ok(), sensors_hrs_raw(), sensors_hrs_als(), sensors_hrs_ac(),
        sensors_hrs_snr10() / 10, sensors_hrs_snr10() % 10,
        sensors_hrs_fs10() / 10, sensors_hrs_fs10() % 10,
        sensors_hrs_pk(), sensors_bpm(),
        sensors_bma_ok(), sensors_bma_engine(), sensors_steps(),
        (sensors_bma_dbg() >> 16) & 0xFF,
        (sensors_bma_dbg() >> 8) & 0xFF, sensors_bma_dbg() & 0xFF,
        sensors_retries(),
        ble_status(), conns, encd, sec, enc, disc,
        ancs_state(), ancs_sub_fail(), ancs_cts_state(), hal_clock_synced(),
        ams_state(), ancs_seq_phase(), bonds, repairs,
        (int)diag_lps, hal_battery_pct(), hal_battery_raw(),
        hal_boot_confirmed());
    if (diag_lbl)
        lv_label_set_text(diag_lbl, b);
}

static void build_diag(lv_obj_t *r)
{
    page_bg(r);
    mk_header(r, "DIAG", 1);
    diag_lbl = mk_label(r, &kenney9, C_WHITE, 14, 42, "");
    lv_obj_set_width(diag_lbl, 216);
    diag_fill();
}

/* ---- dispatch ------------------------------------------------------------------ */

#include "ui_phone.inc"

static void rebuild(void)
{
    ui_face_breath_stop();   /* anim must die before its label does */
    lv_obj_clean(root);
    lbl_time_big = NULL;
    cardio_trace = NULL;
    cardio_bpm = NULL;
    cardio_art = NULL;
    diag_lbl = NULL;
    lc_canvas = NULL;
    mus_art = mus_pos_lbl = NULL;
    switch (cur) {
    case SCR_FACE:      ui_face_build(root); break;
    case SCR_LAUNCHER:  build_launcher(root); break;
    case SCR_CC:        build_cc(root); break;
    case SCR_WORKOUT:   build_workout(root); break;
    case SCR_W_RUN:     build_w_run(root); break;
    case SCR_W_DONE:    build_w_done(root); break;
    case SCR_SLEEP:     build_sleep(root); break;
    case SCR_S_DONE:    build_s_done(root); break;
    case SCR_WATER:     build_water(root); break;
    case SCR_CLOCK:     build_clock(root); break;
    case SCR_SETTINGS:  build_settings(root); break;
    case SCR_STATS:     build_stats(root); break;
    case SCR_SETTIME:   build_settime(root); break;
    case SCR_NOTIF:     build_notif(root); break;
    case SCR_CARDIO:    build_cardio(root); break;
    case SCR_DIAG:      build_diag(root); break;
    case SCR_WEATHER:   build_weather(root); break;
    case SCR_MUSIC:     build_music(root); break;
    }
}

void ui_init(void)
{
    root = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, 240, 240);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    /* no LV_EVENT_GESTURE or CLICKED handler: the CST816S detects
       gestures in hardware (ui_swipe) and taps never navigate */
    persist_load();
    apply_radio();
    display_backlight(3);
    last_input_ms = hal_millis();
    nav_to(SCR_FACE);
}

uint32_t button_held_ms(void);

void ui_tick(void)
{
    uint32_t now = hal_millis();
    diag_loops++;
    if (now - diag_t0 >= 1000) {
        diag_lps = diag_loops * 1000 / (now - diag_t0);
        diag_loops = 0;
        diag_t0 = now;
    }
    if (!ui_start_ms)
        ui_start_ms = now;   /* first loop iter, after ble_init's boot marks */
    extern int ble_dfu_busy(void);
    /* idle -> screen off (never mid-DFU) */
    if (!asleep && last_input_ms && now - last_input_ms > SLEEP_AFTER_MS &&
        !ble_dfu_busy()) {
        asleep = 1;
        display_backlight(0);
        display_sleep(1);
        touch_sleep(1);
        if (persist_dirty)
            persist_save();
    }
    /* raise-to-wake: the reference watch detects the gesture in
       software over the accel history (mean/variance + roll angle);
       the chip's wrist-wear flag stays as a bonus trigger */
    if (asleep && (sensors_wrist_raise() || sensors_raise_wake()))
        ui_wake();
    /* side button as nav fallback while touch is unreliable: a release
       before the 3 s reset threshold steps through screens */
    static int was_held;
    uint32_t held = button_held_ms();
    if (held)
        last_input_ms = now;
    if (was_held && !held) {
        if (asleep)
            ui_wake();
        else if (water_lock) {
            /* the button is the way out of water lock */
            wlock_set(0);
            hal_motor(1);
            motor_off_at = now + 120;
            rebuild();
        } else if (cur == SCR_FACE)
            nav_to(SCR_LAUNCHER);
        else if (cur == SCR_LAUNCHER)
            nav_to(SCR_CC);
        else if (cur == SCR_CC)
            nav_to(SCR_FACE);
        else
            nav_back();
    }
    was_held = held != 0;
    sensors_tick(now);
    overlay_tick(now);
    if (timer_end_ms && now >= timer_end_ms) {
        timer_end_ms = 0;
        timer_done = 1;
        hal_motor(1);
        motor_off_at = now + 400;
        if (asleep)
            ui_wake();
        if (cur == SCR_CLOCK)
            rebuild();
    }
    if (motor_off_at && now >= motor_off_at) {
        motor_off_at = 0;
        hal_motor(0);
    }
    /* incoming notification: buzz + wake unless DND is on */
    if (ancs_take_new() && !dnd_on) {
        hal_motor(1);
        motor_off_at = now + 250;
        if (asleep)
            ui_wake();
    }
    if (cur == SCR_NOTIF && ancs_take_dirty())
        rebuild();
    /* resting HR = lowest reading this session outside workouts */
    {
        int b = sensors_bpm();
        if (b > 0 && !sensors_bpm_stale() && !w_active &&
            (resting_bpm == 0 || b < resting_bpm))
            resting_bpm = b;
    }
    /* alarm: once per matching minute, buzz in 500 ms pulses until STOP */
    /* a restored clock rings too: a late alarm beats a missed one */
    if (alarm_on && (hal_clock_synced() || hal_clock_restored())) {
        int mod = hal_clock_hour() * 60 + hal_clock_minute();
        if (mod == alarm_h * 60 + alarm_m && alarm_fired_min != mod) {
            alarm_fired_min = mod;
            alarm_ringing = 1;
            clock_tab = 2;
            if (asleep)
                ui_wake();
            nav_sp = 0;
            nav_to(SCR_CLOCK);
        }
        if (mod != alarm_h * 60 + alarm_m)
            alarm_fired_min = -1;
    }
    if (alarm_ringing) {
        hal_motor((now / 500) & 1);
        last_input_ms = now;     /* never sleep through it */
    }
    /* deferred flash write: 2 s after the last change, or on sleep */
    if (persist_dirty && now - persist_dirty_ms > 2000)
        persist_save();
    switch (cur) {
    case SCR_FACE:
        ui_face_tick();
        break;
    case SCR_MUSIC:
        music_tick();
        break;
    case SCR_WEATHER:
        if (phone_take_dirty())
            rebuild();
        break;
    case SCR_LAUNCHER:
        lc_tick();
        break;
    case SCR_CC:
    case SCR_STATS: {
        /* wheel content is live: batt %, bt state, bpm, steps. A
           full-canvas invalidate repaints every hex + glyph — at
           500 ms it was eating half the frame budget for nothing */
        static uint32_t last_wh;
        if (now - last_wh >= 1000) {
            last_wh = now;
            lv_obj_t *cv = cur == SCR_CC ? ccw_canvas : st_canvas;
            if (cv)
                lv_obj_invalidate(cv);
        }
        break;
    }
    case SCR_DIAG: {
        static uint32_t last_d;
        if (now - last_d >= 500) {
            last_d = now;
            diag_fill();
        }
        break;
    }
    case SCR_CARDIO:
        if (cardio_trace) {
            static uint32_t last_tr;
            if (now - last_tr >= 100) {
                last_tr = now;
                lv_obj_invalidate(cardio_trace);
            }
        }
        {
            static int last_key = -4;
            int bpm = sensors_bpm();
            int stale = bpm > 0 && sensors_bpm_stale();
            int key = bpm * 2 + stale;       /* held flips the colour */
            if (key != last_key) {
                int was_locked = last_key > 0;
                last_key = key;
                if (cardio_art)
                    lv_obj_invalidate(cardio_art);  /* zone + hub tint */
                if ((bpm > 0) != was_locked) {
                    rebuild();       /* hub swaps glyph for the number */
                } else if (bpm > 0 && cardio_bpm) {
                    char buf[16];
                    lv_snprintf(buf, sizeof buf, "%d", bpm);
                    lv_label_set_text(cardio_bpm, buf);
                    lv_obj_set_style_text_color(cardio_bpm,
                        lv_color_hex(stale ? BPM_STALE_RGB : 0xF0586E), 0);
                }
            }
        }
        break;
    case SCR_W_RUN: {
        static uint32_t last;
        if (!w_paused && now - last >= 500) {
            last = now;
            if (lbl_time_big) {
                char buf[16];
                fmt_ms(now - w_start_ms - w_paused_total, buf);
                lv_label_set_text(lbl_time_big, buf);
            }
        }
        break;
    }
    case SCR_SLEEP:
    case SCR_CLOCK: {
        static uint32_t last;
        if (now - last >= 500) {
            last = now;
            if (lbl_time_big) {
                char buf[16];
                if (cur == SCR_SLEEP && s_in_bed)
                    fmt_ms(now - s_start_ms, buf);
                else if (clock_tab == 0)
                    fmt_ms(sw_acc + (sw_running ? now - sw_t0 : 0), buf);
                else if (timer_end_ms)
                    fmt_ms(timer_end_ms - now, buf);
                else
                    break;
                lv_label_set_text(lbl_time_big, buf);
            }
        }
        break;
    }
    default:
        break;
    }
}
