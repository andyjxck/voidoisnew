/* 2.1 phone-fed screens and overlays, #included by ui_nav.c (so the host
   renderer in tests/uisnap builds them exactly like the firmware):
   - weather (InfiniTime SimpleWeatherService data from InfiniLink or
     Gadgetbridge), with a how-to placeholder when there is none
   - music (InfiniTime music service or iOS AMS), entered only while
     music plays and the phone is linked
   - torch: a full-white layer over everything, tap to turn off
   - water lock: a layer that swallows taps and says how to unlock */

/* ---- shared bits ---------------------------------------------------------- */

/* invisible tap target with the same swipe-tail filter as mk_btn */
static lv_obj_t *mk_hit(lv_obj_t *parent, int x, int y, int w, int h,
                        lv_event_cb_t cb, int ud)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o, tap_filter, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_user_data(o, (void *)(intptr_t)ud);
    return o;
}

/* full-screen canvas layer under the labels */
static lv_obj_t *mk_art(lv_obj_t *parent, lv_event_cb_t draw)
{
    lv_obj_t *a = lv_obj_create(parent);
    lv_obj_set_pos(a, 0, 0);
    lv_obj_set_size(a, 240, 240);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(a, 0, 0);
    lv_obj_set_style_pad_all(a, 0, 0);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(a, draw, LV_EVENT_DRAW_MAIN, NULL);
    return a;
}

/* the kenney fonts are ASCII only: fold UTF-8 Latin accents to their
   base letter, drop everything else (emoji, CJK) to '?' once */
void ui_music_ascii(char *dst, const char *src, int cap)
{
    static const char fold[64 + 1] =
        "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs"   /* U+00C0..U+00DF */
        "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";  /* U+00E0..U+00FF */
    const unsigned char *s = (const unsigned char *)src;
    int n = 0, lastq = 0;
    while (*s && n < cap - 1) {
        unsigned c = *s;
        if (c < 0x80) {
            dst[n++] = c >= 32 ? (char)c : ' ';
            s++;
            lastq = 0;
            continue;
        }
        int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        unsigned cp = 0;
        if (len == 2 && (s[1] & 0xC0) == 0x80)
            cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        char out = 0;
        if (cp >= 0xC0 && cp <= 0xFF)
            out = fold[cp - 0xC0];
        else if (cp == 0x2019 || cp == 0x2018)
            out = '\'';
        if (len == 3 && s[1] == 0x80 && s[2] >= 0x98 && s[2] <= 0x99 &&
            c == 0xE2)
            out = '\'';                      /* curly apostrophes */
        if (len == 3 && c == 0xE2 && s[1] == 0x80 && s[2] >= 0x93 &&
            s[2] <= 0x94)
            out = '-';                       /* en/em dash */
        if (out) {
            dst[n++] = out;
            lastq = 0;
        } else if (!lastq) {
            dst[n++] = '?';
            lastq = 1;
        }
        for (int i = 0; i < len && *s; i++)
            s++;
    }
    dst[n] = 0;
}

/* ---- weather -------------------------------------------------------------- */

static int wx_c(int centi) /* centi-degrees -> rounded whole degrees */
{
    return centi >= 0 ? (centi + 50) / 100 : -((-centi + 50) / 100);
}

static void wx_draw(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    const struct wx *w = wx_get();
    if (!w) {
        dk_hex(l, 120, 82, 30, DK_DIM, 2);
        dk_wx_icon(l, 2, 120, 84, 16, DK_GREY);
        dk_oct(l, 12, 132, 216, 96, 8, DK_DIM, 1, LV_OPA_TRANSP);
        return;
    }
    /* hero: icon in a cyan hex, temperature card beside it */
    dk_hex(l, 58, 84, 38, DK_CYAN, 2);
    dk_hex(l, 58, 84, 32, DK_DIM, 1);
    dk_wx_icon(l, w->icon, 58, 82, 18, lv_color_white());
    dk_oct(l, 104, 46, 124, 76, 8, DK_CYAN, 2, LV_OPA_TRANSP);
    /* forecast strip: one chamfered pill per day */
    int n = w->ndays;
    if (n > 5)
        n = 5;
    if (n) {
        int cw = 216 / n;
        for (int i = 0; i < n; i++) {
            int x = 12 + i * cw;
            dk_oct(l, x + 2, 140, cw - 4, 92, 6,
                   i == 0 ? DK_CYAN : DK_DIM, 1, LV_OPA_TRANSP);
            dk_wx_icon(l, w->day[i].icon, x + cw / 2, 178, 9,
                       lv_color_white());
        }
    } else {
        dk_oct(l, 12, 140, 216, 92, 8, DK_DIM, 1, LV_OPA_TRANSP);
    }
}

static void build_weather(lv_obj_t *r)
{
    page_bg(r);
    mk_art(r, wx_draw);
    mk_header(r, "WEATHER", 1);
    const struct wx *w = wx_get();
    char b[40];
    if (!w) {
        mk_label_c(r, &kenney16, C_WHITE, 120, 140, "NO WEATHER YET");
        mk_label_c(r, &kenney12, C_MUTED, 120, 164, "SETTINGS > INFINILINK ON");
        mk_label_c(r, &kenney12, C_MUTED, 120, 182, "THEN OPEN INFINILINK");
        mk_label_c(r, &kenney12, C_MUTED, 120, 200, "OR GADGETBRIDGE");
        return;
    }
    lv_snprintf(b, sizeof b, "%d", wx_c(w->temp));
    lv_obj_t *t = mk_label_c(r, &kenney24, C_WHITE, 160, 52, b);
    lv_snprintf(b, sizeof b, "%s C", lv_label_get_text(t));
    lv_label_set_text(t, b);
    lv_obj_update_layout(t);
    lv_obj_set_x(t, 166 - lv_obj_get_width(t) / 2);
    lv_snprintf(b, sizeof b, "L %d  H %d", wx_c(w->tmin), wx_c(w->tmax));
    mk_label_c(r, &kenney12, C_ACCENT, 166, 82, b);
    mk_label_c(r, &kenney12, C_MUTED, 166, 101, wx_name(w->icon));
    if (w->city[0]) {
        char c[34];
        ui_music_ascii(c, w->city, sizeof c);
        lv_obj_t *cl = mk_label(r, &kenney12, C_MUTED, 12, 124, c);
        lv_label_set_long_mode(cl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(cl, 120);
    }
    uint32_t age = (hal_millis() - w->rx_ms) / 60000u;
    if (age < 1)
        lv_snprintf(b, sizeof b, "NOW");
    else if (age < 120)
        lv_snprintf(b, sizeof b, "%u MIN AGO", (unsigned)age);
    else
        lv_snprintf(b, sizeof b, "%u H AGO", (unsigned)(age / 60));
    lv_obj_t *al = mk_label(r, &kenney12, C_MUTED, 0, 124, b);
    lv_obj_update_layout(al);
    lv_obj_set_x(al, 228 - lv_obj_get_width(al));
    int n = w->ndays > 5 ? 5 : w->ndays;
    if (!n) {
        mk_label_c(r, &kenney12, C_MUTED, 120, 178, "NO FORECAST SENT");
        return;
    }
    int cw = 216 / n;
    int y, mo, d, dow = 0;
    if (hal_clock_synced() || hal_clock_restored())
        hal_clock_date(&y, &mo, &d, &dow);
    static const char *dn[8] = {"", "MON", "TUE", "WED", "THU", "FRI",
                                "SAT", "SUN"};
    for (int i = 0; i < n; i++) {
        int cx = 12 + i * cw + cw / 2;
        if (i == 0)
            lv_snprintf(b, sizeof b, "TODAY");
        else if (dow >= 1 && dow <= 7)
            lv_snprintf(b, sizeof b, "%s", dn[(dow - 1 + i) % 7 + 1]);
        else
            lv_snprintf(b, sizeof b, "+%d", i);
        mk_label_c(r, &kenney12, i == 0 ? C_ACCENT : C_MUTED, cx, 148, b);
        lv_snprintf(b, sizeof b, "%d", wx_c(w->day[i].tmax));
        mk_label_c(r, &kenney16, C_WHITE, cx, 194, b);
        lv_snprintf(b, sizeof b, "%d", wx_c(w->day[i].tmin));
        mk_label_c(r, &kenney12, C_MUTED, cx, 213, b);
    }
}

/* ---- music ---------------------------------------------------------------- */

static lv_obj_t *mus_art, *mus_pos_lbl;
static int mus_shown_playing = -1;

enum { MU_PREV, MU_TOGGLE, MU_NEXT, MU_VOLDN, MU_VOLUP };

static void mus_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    static const int cmd[5] = {MC_PREV, MC_TOGGLE, MC_NEXT, MC_VOLDN,
                               MC_VOLUP};
    buzz(20);
    music_cmd(cmd[k]);
    if (mus_art)
        lv_obj_invalidate(mus_art);
}

static uint32_t mus_pos_now(const struct music *m)
{
    uint32_t p = m->pos_s;
    if (m->playing)
        p += (hal_millis() - m->pos_ms) / 1000u;
    if (m->len_s && p > m->len_s)
        p = m->len_s;
    return p;
}

static void mus_draw(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    const struct music *m = music_get();
    /* track card */
    dk_oct(l, 12, 44, 216, 64, 8, DK_CYAN, 2, LV_OPA_TRANSP);
    dk_music(l, DK_M_NOTE, 32, 70, 9, DK_RED);
    /* progress: thin dim rule, cyan fill */
    if (m->len_s) {
        int w = (int)(196u * mus_pos_now(m) / m->len_s);
        dk_rect(l, 22, 118, 218, 119, DK_DIM, LV_OPA_COVER);
        if (w > 0)
            dk_rect(l, 22, 117, 22 + w, 120, DK_CYAN, LV_OPA_COVER);
    }
    /* transport: prev / play-pause (big red hex) / next */
    dk_hex(l, 46, 160, 22, DK_DIM, 2);
    dk_music(l, DK_M_PREV, 46, 160, 9, lv_color_white());
    dk_hex(l, 120, 160, 30, DK_RED, 2);
    dk_hex(l, 120, 160, 25, DK_DIMR, 1);
    dk_music(l, m->playing ? DK_M_PAUSE : DK_M_PLAY, 122, 160, 11,
             lv_color_white());
    dk_hex(l, 194, 160, 22, DK_DIM, 2);
    dk_music(l, DK_M_NEXT, 194, 160, 9, lv_color_white());
    if (music_vol_ok()) {
        dk_oct(l, 16, 200, 98, 34, 6, DK_DIM, 1, LV_OPA_TRANSP);
        dk_music(l, DK_M_VOLDN, 65, 217, 8, DK_CYAN);
        dk_oct(l, 126, 200, 98, 34, 6, DK_DIM, 1, LV_OPA_TRANSP);
        dk_music(l, DK_M_VOLUP, 175, 217, 8, DK_CYAN);
    }
}

static void mus_pos_text(char *b, int cap, const struct music *m)
{
    uint32_t p = mus_pos_now(m);
    if (m->len_s)
        lv_snprintf(b, cap, "%u:%02u / %u:%02u", (unsigned)(p / 60),
                    (unsigned)(p % 60), (unsigned)(m->len_s / 60),
                    (unsigned)(m->len_s % 60));
    else
        lv_snprintf(b, cap, "%s", m->playing ? "PLAYING" : "PAUSED");
}

static void build_music(lv_obj_t *r)
{
    page_bg(r);
    mus_art = mk_art(r, mus_draw);
    mk_header(r, "MUSIC", 1);
    const struct music *m = music_get();
    char t[44];
    ui_music_ascii(t, m->track[0] ? m->track : "UNKNOWN TRACK", sizeof t);
    lv_obj_t *tl = mk_label(r, &kenney16, C_WHITE, 50, 54, t);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(tl, 168);
    ui_music_ascii(t, m->artist[0] ? m->artist : "", sizeof t);
    lv_obj_t *al = mk_label(r, &kenney12, C_MUTED, 50, 78, t);
    lv_label_set_long_mode(al, LV_LABEL_LONG_DOT);
    lv_obj_set_width(al, 168);
    mus_pos_text(t, sizeof t, m);
    mus_pos_lbl = mk_label_c(r, &kenney12, C_MUTED, 120, 124, t);
    mk_hit(r, 12, 134, 66, 54, mus_cb, MU_PREV);
    mk_hit(r, 84, 128, 72, 66, mus_cb, MU_TOGGLE);
    mk_hit(r, 162, 134, 66, 54, mus_cb, MU_NEXT);
    if (music_vol_ok()) {
        mk_hit(r, 14, 198, 102, 40, mus_cb, MU_VOLDN);
        mk_hit(r, 124, 198, 102, 40, mus_cb, MU_VOLUP);
    }
    mus_shown_playing = m->playing;
}

static void music_tick(void)
{
    /* the phone went away: nothing left to control */
    if (!ble_connected()) {
        nav_back();
        return;
    }
    const struct music *m = music_get();
    if (phone_take_dirty() || m->playing != mus_shown_playing) {
        rebuild();
        return;
    }
    static uint32_t last;
    uint32_t now = hal_millis();
    if (now - last >= 1000 && mus_pos_lbl) {
        last = now;
        char t[24];
        mus_pos_text(t, sizeof t, m);
        lv_label_set_text(mus_pos_lbl, t);
        lv_obj_update_layout(mus_pos_lbl);
        lv_obj_set_x(mus_pos_lbl, 120 - lv_obj_get_width(mus_pos_lbl) / 2);
        lv_obj_invalidate(mus_art);
    }
}

/* face chips: 1 weather, 2 music */
void ui_nav_open(int which)
{
    if (water_lock)
        return;
    if (which == 1)
        nav_to(SCR_WEATHER);
    else if (which == 2 && music_active())
        nav_to(SCR_MUSIC);
}

/* ---- torch + water lock overlays (lv_layer_top, above every screen) ------- */

static lv_obj_t *torch_ov, *wl_ov, *wl_note;
static uint32_t torch_on_ms, wl_note_until;
#define TORCH_MAX_MS 300000u      /* auto-off after 5 min */

static lv_obj_t *mk_overlay(void)
{
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_size(o, 240, 240);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void torch_tap(lv_event_t *e)
{
    (void)e;
    if (hal_millis() - last_swipe_ms < 400)
        return;
    torch_set(0);
    persist_mark();
}

static void torch_set(int on)
{
    on = !!on;
    if (on && water_lock)
        return;                    /* lock first, light later */
    torch_on = on;
    if (on) {
        torch_on_ms = hal_millis();
        if (!torch_ov) {
            torch_ov = mk_overlay();
            lv_obj_set_style_bg_color(torch_ov, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(torch_ov, LV_OPA_COVER, 0);
            lv_obj_add_event_cb(torch_ov, torch_tap, LV_EVENT_CLICKED, NULL);
            lv_obj_t *h = mk_label_c(torch_ov, &kenney12,
                                     lv_color_hex(0xB8BEC8), 120, 214,
                                     "TAP TO TURN OFF");
            (void)h;
        }
    } else if (torch_ov) {
        lv_obj_delete(torch_ov);
        torch_ov = NULL;
    }
    if (!asleep)
        display_backlight(bl_level());
}

static void wl_show_note(void)
{
    if (!wl_note)
        return;
    lv_obj_remove_flag(wl_note, LV_OBJ_FLAG_HIDDEN);
    wl_note_until = hal_millis() + 2500;
}

static void wl_tap(lv_event_t *e)
{
    (void)e;
    wl_show_note();                /* the tap goes nowhere else */
}

static void wl_note_draw(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    dk_oct(l, a.x1, a.y1, a.x2 - a.x1 + 1, a.y2 - a.y1 + 1, 10,
           lv_color_black(), 2, LV_OPA_COVER);
    dk_oct(l, a.x1, a.y1, a.x2 - a.x1 + 1, a.y2 - a.y1 + 1, 10, DK_CYAN, 2, LV_OPA_TRANSP);
    /* water drop */
    int cx = a.x1 + 60, cy = a.y1 + 28;
    dk_tri(l, cx, cy - 12, cx - 7, cy, cx + 7, cy, DK_CYAN, LV_OPA_COVER);
    dk_arc(l, cx, cy + 2, 7, 7, 0, 360, DK_CYAN);
}

static void wlock_set(int on)
{
    on = !!on;
    if (on && torch_on)
        torch_set(0);
    water_lock = on;
    if (on) {
        if (!wl_ov) {
            wl_ov = mk_overlay();
            lv_obj_set_style_bg_opa(wl_ov, LV_OPA_TRANSP, 0);
            lv_obj_add_event_cb(wl_ov, wl_tap, LV_EVENT_CLICKED, NULL);
            wl_note = lv_obj_create(wl_ov);
            lv_obj_set_pos(wl_note, 24, 70);
            lv_obj_set_size(wl_note, 192, 100);
            lv_obj_set_style_bg_opa(wl_note, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(wl_note, 0, 0);
            lv_obj_set_style_pad_all(wl_note, 0, 0);
            lv_obj_remove_flag(wl_note, LV_OBJ_FLAG_CLICKABLE |
                                        LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_event_cb(wl_note, wl_note_draw, LV_EVENT_DRAW_MAIN,
                                NULL);
            mk_label(wl_note, &kenney16, C_WHITE, 78, 20, "LOCKED");
            mk_label_c(wl_note, &kenney12, C_MUTED, 96, 50, "PRESS THE BUTTON");
            mk_label_c(wl_note, &kenney12, C_MUTED, 96, 68, "TO UNLOCK");
        }
        wl_show_note();
    } else if (wl_ov) {
        lv_obj_delete(wl_ov);
        wl_ov = wl_note = NULL;
    }
}

/* from ui_tick: torch keeps the screen up (capped), notice times out */
static void overlay_tick(uint32_t now)
{
    if (torch_on) {
        if (now - torch_on_ms > TORCH_MAX_MS) {
            torch_set(0);
            persist_mark();
        } else {
            last_input_ms = now;
        }
    }
    if (wl_note && wl_note_until && (int32_t)(now - wl_note_until) >= 0) {
        wl_note_until = 0;
        lv_obj_add_flag(wl_note, LV_OBJ_FLAG_HIDDEN);
    }
}
