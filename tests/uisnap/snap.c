/* Host renderer for the device UI: builds every screen of firmware/ui_nav.c
   with the real LVGL config and fonts and writes each one to a PPM, so
   layout changes can be checked without flashing. Hardware calls are
   stubbed with plausible values.
   make -C tests/uisnap && ./tests/uisnap/snap out_dir

   It also audits every screen and exits non-zero on a failure:
   - tap targets: each clickable object's click area (ext click area
     included) must be at least TAP_MIN px on its short side, or a
     full-width row (>= TAP_ROW_W wide) at least TAP_ROW_H tall; no two
     click areas may overlap.
   - canvas screens (launcher, control centre, stats) are hit-tested
     pixel by pixel through their real hit functions; every target's
     region must hold a TAP_MIN x TAP_MIN square.
   - text: labels must sit inside their button and on screen; canvas
     text must fit the box draw_txt() gives it (it wraps otherwise).
   The findings go to stdout and <out_dir>/taps.txt. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define TAP_MIN   40
#define TAP_ROW_W 150
#define TAP_ROW_H 30
struct _lv_font_t;
static void snap_text(const struct _lv_font_t *f, const char *s, int x, int y,
                      int w);
#define UISNAP_TEXT_HOOK(f, s, x, y, w) snap_text(f, s, x, y, w)
static uint32_t fake_mmio[0x4000];
#define REG32(a) (fake_mmio[((uintptr_t)(a) >> 2) & 0x3FFF])
#include "../../firmware/ui_nav.c"

/* ---- hardware stubs ---- */
#include "stubs.inc"

/* ---- framebuffer ---- */
static uint16_t fb[240 * 240];
static uint16_t buf[240 * 40];

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const uint16_t *p = (const uint16_t *)px;
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++) {
            uint16_t v = *p++;
            fb[y * 240 + x] = (uint16_t)((v >> 8) | (v << 8));
        }
    lv_display_flush_ready(d);
}

static void save(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n240 240\n255\n");
    for (int i = 0; i < 240 * 240; i++) {
        uint16_t v = fb[i];
        unsigned char rgb[3] = {(unsigned char)((v >> 11) << 3),
                                (unsigned char)(((v >> 5) & 63) << 2),
                                (unsigned char)((v & 31) << 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void settle(void)
{
    for (int i = 0; i < 20; i++) {
        now_ms += 50;
        lv_timer_handler();
    }
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
}

static FILE *audit, *taps;
static const char *cur_name = "";
static int fails, checks;

static void finding(int bad, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void finding(int bad, const char *fmt, ...)
{
    char m[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    checks++;
    if (bad)
        fails++;
    if (bad || getenv("SNAP_VERBOSE")) {
        printf("%-18s %s %s\n", cur_name, bad ? "FAIL" : "ok  ", m);
        if (taps)
            fprintf(taps, "%-18s %s %s\n", cur_name, bad ? "FAIL" : "ok  ", m);
    }
}

static void snap_text(const struct _lv_font_t *f, const char *s, int x, int y,
                      int w)
{
    int tw = (int)lv_text_get_width(s, strlen(s), (const lv_font_t *)f, 0);
    int cx = x + w / 2;
    finding(tw > w || cx - tw / 2 < 0 || cx + tw / 2 > 240,
            "canvas text \"%s\" %d px in a %d px box at %d,%d", s, tw, w,
            cx - tw / 2, y);
}

static lv_area_t tgt[64];
static int ntgt;
static void walk(lv_obj_t *o, int depth)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int w = a.x2 - a.x1 + 1, h = a.y2 - a.y1 + 1;
    int hidden = lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
    if (hidden) return;
    int click = lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE);
    if (click && depth > 1 && !(w >= 200 && h >= 200)) {
        lv_area_t c;
        lv_obj_get_click_area(o, &c);
        int cw = c.x2 - c.x1 + 1, ch = c.y2 - c.y1 + 1;
        int sh = cw < ch ? cw : ch, lg = cw < ch ? ch : cw;
        const char *txt = lv_obj_get_child_count(o) &&
            lv_obj_check_type(lv_obj_get_child(o, 0), &lv_label_class)
            ? lv_label_get_text(lv_obj_get_child(o, 0)) : "";
        finding(!(sh >= TAP_MIN || (lg >= TAP_ROW_W && sh >= TAP_ROW_H)),
                "tap \"%s\" %dx%d at %d,%d", txt, cw, ch, c.x1, c.y1);
        for (int i = 0; i < ntgt; i++) {
            lv_area_t *t = &tgt[i];
            if (c.x1 <= t->x2 && t->x1 <= c.x2 && c.y1 <= t->y2 && t->y1 <= c.y2)
                finding(1, "tap \"%s\" at %d,%d overlaps another at %d,%d",
                        txt, c.x1, c.y1, t->x1, t->y1);
        }
        if (ntgt < 64)
            tgt[ntgt++] = c;
    }
    if (lv_obj_check_type(o, &lv_label_class)) {
        lv_obj_t *par = lv_obj_get_parent(o);
        lv_area_t pa;
        lv_obj_get_coords(par, &pa);
        int inside_btn = lv_obj_check_type(par, &lv_button_class);
        int off = a.x1 < 0 || a.x2 > 239 || a.y1 < 0 || a.y2 > 239;
        int spill = inside_btn && (a.x1 < pa.x1 || a.x2 > pa.x2 ||
                                   a.y1 < pa.y1 || a.y2 > pa.y2);
        if (off || spill)
            finding(1, "label \"%s\" %s", lv_label_get_text(o),
                    off ? "runs off screen" : "spills out of its button");
    }
    const char *kind = lv_obj_check_type(o, &lv_label_class) ? "label" :
                       lv_obj_check_type(o, &lv_button_class) ? "button" : "obj";
    if (!strcmp(kind, "label")) {
        const lv_font_t *f = lv_obj_get_style_text_font(o, LV_PART_MAIN);
        fprintf(audit, "%*s%s %d,%d %dx%d font_h=%d%s \"%s\"\n", depth * 2, "", kind,
                a.x1, a.y1, w, h, f ? f->line_height : 0, click ? " CLICK" : "",
                lv_label_get_text(o));
    } else {
        fprintf(audit, "%*s%s %d,%d %dx%d%s\n", depth * 2, "", kind, a.x1, a.y1, w, h,
                click ? " CLICK" : "");
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
        walk(lv_obj_get_child(o, i), depth + 1);
}

/* largest axis-aligned square inside each region id of a 240x240 map */
static int big_square(const signed char *map, int id)
{
    static short dp[240 * 240];
    int best = 0;
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 240; x++) {
            int i = y * 240 + x, v = 0;
            if (map[i] == id) {
                v = 1;
                if (x && y) {
                    int a = dp[i - 1], b = dp[i - 240], c = dp[i - 241];
                    int m = a < b ? a : b;
                    v = 1 + (m < c ? m : c);
                }
            }
            dp[i] = (short)v;
            if (v > best)
                best = v;
        }
    return best;
}

static void canvas_audit(int (*hit)(int, int), int n, int hub,
                         const char *const *names)
{
    static signed char map[240 * 240];
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 240; x++)
            map[y * 240 + x] = (signed char)hit(x, y);
    for (int id = -2; id <= n; id++) {
        if (id == -1 || (id == -2 && hub != -2 && !names))
            continue;
        int sq = big_square(map, id);
        if (id == -2 && sq == 0)
            continue;
        finding(sq < (id == -2 ? TAP_ROW_H : TAP_MIN),
                "canvas %s holds a %dx%d square",
                id == n ? "hub" : id == -2 ? "strip" : names[id], sq, sq);
    }
}

static void shot(const char *dir, scr_t s, const char *name)
{
    cur_name = name;
    ntgt = 0;
    cur = s;
    rebuild();
    settle();
    save(dir, name);
    char path[512];
    snprintf(path, sizeof path, "%s/%s.txt", dir, name);
    audit = fopen(path, "w");
    walk(lv_screen_active(), 0);
    {
        static const char *lcn[LC_N], *ccn[CCW_DEFN], *stn[16];
        for (int i = 0; i < LC_N; i++) lcn[i] = lc_apps[i].name;
        for (int i = 0; i < ccw_n; i++) ccn[i] = ccw_def[ccw_list[i]].name;
        for (int i = 0; i < ST_N; i++) stn[i] = st_def[i].name;
        if (s == SCR_LAUNCHER) canvas_audit(lc_hit, LC_N, LC_N, lcn);
        if (s == SCR_CC)       canvas_audit(ccw_hit, ccw_n, ccw_n, ccn);
        if (s == SCR_STATS)    canvas_audit(st_hit, ST_N, -2, stn);
    }
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    fprintf(audit, "LVGL heap: used %u%% max_used %u free_biggest %u\n",
            (unsigned)m.used_pct, (unsigned)m.max_used, (unsigned)m.free_biggest_size);
    fclose(audit);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    lv_init();
    if (argc > 2 && !strcmp(argv[1], "-m")) {
        const lv_font_t *fs[] = {&kenney9, &kenney12, &kenney16, &kenney24};
        for (int i = 2; i < argc; i++) {
            printf("%-16s", argv[i]);
            for (int k = 0; k < 4; k++)
                printf(" %4d", (int)lv_text_get_width(argv[i], strlen(argv[i]), fs[k], 0));
            printf("\n");
        }
        return 0;
    }
    lv_tick_set_cb(hal_millis_raw);
    lv_display_t *d = lv_display_create(240, 240);
    lv_display_set_flush_cb(d, flush);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(d, buf, NULL, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    ui_init();
    char tp[512];
    snprintf(tp, sizeof tp, "%s/taps.txt", dir);
    taps = fopen(tp, "w");
    shot(dir, SCR_FACE, "00_face");
    face_dig = 0; shot(dir, SCR_FACE, "01_face_analog"); face_dig = 1;
    snap_bpm = 0; shot(dir, SCR_FACE, "02_face_measuring");
    snap_hr_on = 0; shot(dir, SCR_FACE, "03_face_hr_off");
    snap_hr_on = 1; snap_bpm = 72;
    shot(dir, SCR_LAUNCHER, "10_launcher");
    shot(dir, SCR_CC, "11_cc");
    cc_hide = 7; shot(dir, SCR_CC, "11_cc_min"); cc_hide = 0;
    for (int f = 0; f < CCW_DEFN; f++) {     /* hub contents per control */
        ccw_focus = f; char nm[24]; snprintf(nm, sizeof nm, "11_cc_f%d", f);
        cur_name = nm; ntgt = 0; cur = SCR_CC; rebuild(); settle();
    }
    ccw_focus = 0;
    for (int f = 0; f < LC_N; f++) {
        lc_focus = f; char nm[24]; snprintf(nm, sizeof nm, "10_launcher_f%d", f);
        cur_name = nm; cur = SCR_LAUNCHER; rebuild(); settle();
    }
    lc_focus = 0;
    shot(dir, SCR_WORKOUT, "12_workout");
    w_active = 1; w_start_ms = now_ms - 754000; shot(dir, SCR_W_RUN, "13_w_run");
    w_last_ms = 1754000; w_last_sets = 2; shot(dir, SCR_W_DONE, "14_w_done");
    w_active = 0;
    shot(dir, SCR_SLEEP, "15_sleep");
    shot(dir, SCR_S_DONE, "16_s_done");
    water_ml = 750; shot(dir, SCR_WATER, "17_water");
    clock_tab = 0; shot(dir, SCR_CLOCK, "18_clock_sw");
    clock_tab = 1; shot(dir, SCR_CLOCK, "19_clock_timer");
    clock_tab = 2; shot(dir, SCR_CLOCK, "20_clock_alarm");
    alarm_ringing = 1; shot(dir, SCR_CLOCK, "20_clock_ringing"); alarm_ringing = 0;
    clock_tab = 1; timer_end_ms = now_ms + 300000;
    shot(dir, SCR_CLOCK, "19_clock_timer_run"); timer_end_ms = 0;
    timer_done = 1; shot(dir, SCR_CLOCK, "19_clock_timer_done"); timer_done = 0;
    for (set_page = 0; set_page < 3; set_page++) {
        char nm[24]; snprintf(nm, sizeof nm, "21_settings_p%d", set_page + 1);
        shot(dir, SCR_SETTINGS, nm);
    }
    set_page = 0;
    shot(dir, SCR_STATS, "22_stats");
    shot(dir, SCR_SETTIME, "23_settime");
    shot(dir, SCR_NOTIF, "24_notif");
    notif_sel_uid = 100; shot(dir, SCR_NOTIF, "25_notif_open");
    notif_sel_uid = 0;
    shot(dir, SCR_CARDIO, "26_cardio");
    shot(dir, SCR_DIAG, "27_diag");
    cur_name = "";
    printf("wrote screens to %s; tap/text audit: %d checks, %d failed\n",
           dir, checks, fails);
    if (taps) {
        fprintf(taps, "%d checks, %d failed\n", checks, fails);
        fclose(taps);
    }
    return fails ? 1 : 0;
}
