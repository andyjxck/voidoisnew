/* ui_goal.inc — daily goal celebrations (2.4.0), included by ui_nav.c.

   When today's steps or water reach the goal, the watch buzzes a short
   "da-da-da-daa" pattern, wakes the screen and plays a 2 s animation: a
   ring sweeps closed around the goal's icon in a hex badge, a shockwave
   rings out and confetti bursts from the ring. The goal and "GOAL HIT"
   stay up until 4 s. A tap, a swipe, the side button or sleeping closes
   it.

   Once per goal per day: the hit goals are bits in DL.goals_hit, saved
   with the day's counters and cleared by the midnight roll (goal.h).
   With Do Not Disturb on there is no buzz and no wake; the animation
   plays the next time the screen is on. Alarms, the timer, sleep
   tracking and water lock hold it back until they are over.

   Goals come from the VoidHealth app (history CTRL op 4); until the app
   sets them they are 10000 steps and 2000 ml.

   Cost: one top-layer object drawn by a callback, no images beyond the
   existing icons, ~40 bytes of static state. The 2 s animation redraws
   the screen every 40 ms; after that the screen is static. */

#define GOAL_SHOW_MS   4000u
#define GOAL_ANIM_MS   2200u
#define GOAL_FRAME_MS  40u
#define GOAL_SWEEP_MS  500u
#define GOAL_CX        120
#define GOAL_CY        92
#define GOAL_R         58
#define GOAL_NCONF     20

int hist_ble_take_goals(uint32_t *v);      /* ble_hist.c */

static lv_obj_t *goal_box;
static unsigned goal_kind;                 /* GOAL_STEPS / GOAL_WATER */
static unsigned goal_pend;                 /* hit, not shown yet */
static uint32_t goal_pend_day;
static uint32_t goal_t0, goal_el, goal_frame_at, goal_chk;
static int goal_bz = -1;                   /* buzz pattern step, -1 = idle */
static uint32_t goal_bz_at;

/* on, off, on, off, on, off, on (ms) */
static const uint8_t goal_bz_pat[] = {50, 80, 50, 80, 50, 150, 220};

static int goal_steps_n(void) { return goal_steps_of(goal_steps_v); }
static int goal_water_n(void) { return goal_water_of(goal_water_qv); }

static int goal_visible(void)
{
    return goal_box && !lv_obj_has_flag(goal_box, LV_OBJ_FLAG_HIDDEN);
}

static void goal_poke(void)
{
    goal_chk = 0;                          /* check on the next tick */
}

/* ---- drawing ---------------------------------------------------------------- */

static void goal_line(lv_layer_t *l, int x1, int y1, int x2, int y2,
                      lv_color_t c, int w, lv_opa_t o)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = w;
    d.opa = o;
    d.round_start = d.round_end = 1;
    d.p1.x = x1; d.p1.y = y1; d.p2.x = x2; d.p2.y = y2;
    lv_draw_line(l, &d);
}

static void goal_ring(lv_layer_t *l, int r, int w, int a0, int a1,
                      lv_color_t c, lv_opa_t o)
{
    lv_draw_arc_dsc_t a;
    lv_draw_arc_dsc_init(&a);
    a.center.x = GOAL_CX;
    a.center.y = GOAL_CY;
    a.radius = r;
    a.width = w;
    a.color = c;
    a.opa = o;
    a.start_angle = a0;
    a.end_angle = a1;
    a.rounded = a1 - a0 < 360;
    lv_draw_arc(l, &a);
}

/* a small hex outline for confetti: not a layout shape, so it is not
   reported to the snapshot audit (dk_hex is) */
static void goal_hexbit(lv_layer_t *l, int cx, int cy, int r, lv_color_t c,
                        lv_opa_t o)
{
    int hw = r * 866 / 1000;
    const int px[6] = {cx, cx + hw, cx + hw, cx, cx - hw, cx - hw};
    const int py[6] = {cy - r, cy - r / 2, cy + r / 2, cy + r, cy + r / 2,
                       cy - r / 2};
    for (int i = 0; i < 6; i++)
        goal_line(l, px[i], py[i], px[(i + 1) % 6], py[(i + 1) % 6], c, 2, o);
}

static void goal_confetti(lv_layer_t *l, uint32_t el)
{
    /* per-piece angle jitter and speed: fixed, so every burst is the same
       hand-tuned shape */
    static const int8_t jit[GOAL_NCONF] = {3, -6, 5, -2, 7, -4, 1, -7, 4, -3,
                                           6, -5, 2, -1, 5, -6, 3, -2, 7, -4};
    static const uint8_t spd[GOAL_NCONF] = {92, 70, 110, 80, 64, 100, 76, 118,
                                            86, 68, 104, 74, 96, 116, 72, 88,
                                            108, 66, 98, 82};
    const lv_color_t pal[4] = {DK_CYAN, DK_RED, DK_AMBER, C_GOOD};
    if (el < GOAL_SWEEP_MS - 50 || el >= GOAL_ANIM_MS)
        return;
    int tau = (int)(el - (GOAL_SWEEP_MS - 50));     /* ms since the burst */
    int fade = tau < 1100 ? 255 : 255 - (tau - 1100) * 255 / 650;
    if (fade <= 0)
        return;
    for (int i = 0; i < GOAL_NCONF; i++) {
        int ang = i * (360 / GOAL_NCONF) + jit[i];
        /* distance with a little drag, then gravity on y */
        int d = GOAL_R + 4 + spd[i] * tau / 1000 -
                spd[i] * tau / 1000 * tau / 3200;
        int gy = 36 * tau / 1000 * tau / 1000;
        int x = GOAL_CX + lv_trigo_cos((int16_t)ang) * d / LV_TRIGO_SIN_MAX;
        int y = GOAL_CY + lv_trigo_sin((int16_t)ang) * d / LV_TRIGO_SIN_MAX + gy;
        lv_color_t c = pal[i & 3];
        lv_opa_t o = (lv_opa_t)fade;
        int spin = ang + (i & 1 ? 1 : -1) * tau * 2 / 5;   /* deg */
        switch (i % 3) {
        case 0: {                                     /* tumbling streak */
            int dx = lv_trigo_cos((int16_t)spin) * 4 / LV_TRIGO_SIN_MAX;
            int dy = lv_trigo_sin((int16_t)spin) * 4 / LV_TRIGO_SIN_MAX;
            goal_line(l, x - dx, y - dy, x + dx, y + dy, c, 3, o);
            break;
        }
        case 1:                                       /* square bit */
            dk_rect_raw(l, x - 2, y - 2, x + 2, y + 2, c, o);
            break;
        default:                                      /* hex, the motif */
            goal_hexbit(l, x, y, 4, c, o);
            break;
        }
    }
}

static void goal_draw(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    uint32_t el = goal_el;
    int steps = goal_kind == GOAL_STEPS;
    lv_color_t col = steps ? DK_AMBER : DK_CYAN;

    /* the ring: a dim track, then the goal colour sweeping closed from
       12 o'clock (ease-out) */
    goal_ring(l, GOAL_R, 6, 0, 360, lv_color_mix(col, lv_color_black(), 60),
              LV_OPA_COVER);
    int p = el >= GOAL_SWEEP_MS ? 1000 : (int)(el * 1000 / GOAL_SWEEP_MS);
    int deg = 360 - 360 * (1000 - p) / 1000 * (1000 - p) / 1000;
    if (deg >= 360)
        goal_ring(l, GOAL_R, 6, 0, 360, col, LV_OPA_COVER);
    else if (deg > 0)
        goal_ring(l, GOAL_R, 6, 270, 270 + deg, col, LV_OPA_COVER);

    /* shockwave when it closes */
    if (el >= GOAL_SWEEP_MS && el < GOAL_SWEEP_MS + 600) {
        int q = (int)(el - GOAL_SWEEP_MS);
        goal_ring(l, GOAL_R + 6 + q * 46 / 600, 2, 0, 360, col,
                  (lv_opa_t)(220 - q * 220 / 600));
    }

    goal_confetti(l, el);

    /* hex badge pops in with a little overshoot, icon inside */
    int hr;
    if (el < 420)
        hr = 14 + 20 * (int)el / 420;
    else if (el < 640)
        hr = 34 + 6 * lv_trigo_sin((int16_t)(((int)el - 420) * 180 / 220)) /
                  LV_TRIGO_SIN_MAX;
    else
        hr = 34;
    dk_hex(l, GOAL_CX, GOAL_CY, hr, col, 2);
    mi_draw(l, steps ? MI_FOOTPRINT : MI_WATER_DROP, GOAL_CX, GOAL_CY, 36,
            lv_color_white());

    char v[16];
    if (steps)
        lv_snprintf(v, sizeof v, "%d", goal_steps_n());
    else
        lv_snprintf(v, sizeof v, "%d ML", goal_water_n());
    draw_txt_c(l, &kenney24, C_WHITE, 120, GOAL_CY + GOAL_R + 18, v);
    draw_txt_c(l, &kenney12, col, 120, GOAL_CY + GOAL_R + 50,
               steps ? "STEP GOAL HIT" : "WATER GOAL HIT");
}

/* ---- show / hide ---------------------------------------------------------- */

static void goal_hide(void)
{
    if (goal_visible())
        lv_obj_add_flag(goal_box, LV_OBJ_FLAG_HIDDEN);
}

static void goal_tap(lv_event_t *e)
{
    (void)e;
    goal_hide();
}

static void goal_show(uint32_t now, unsigned kind)
{
    if (!goal_box) {
        goal_box = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(goal_box);
        lv_obj_set_size(goal_box, 240, 240);
        lv_obj_set_pos(goal_box, 0, 0);
        lv_obj_set_style_bg_color(goal_box, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(goal_box, LV_OPA_COVER, 0);
        lv_obj_remove_flag(goal_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(goal_box, LV_OBJ_FLAG_CLICKABLE);  /* eats taps */
        lv_obj_add_event_cb(goal_box, goal_draw, LV_EVENT_DRAW_MAIN, NULL);
        lv_obj_add_event_cb(goal_box, goal_tap, LV_EVENT_CLICKED, NULL);
    }
    goal_kind = kind;
    goal_t0 = goal_frame_at = now;
    goal_el = 0;
    lv_obj_remove_flag(goal_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(goal_box);
    lv_obj_move_foreground(goal_box);
}

/* ---- haptics ---------------------------------------------------------------- */

static void goal_buzz_start(uint32_t now)
{
    goal_bz = 0;
    goal_bz_at = now;
}

static void goal_buzz_tick(uint32_t now)
{
    if (goal_bz < 0)
        return;
    if (alarm_ringing || timer_done) {     /* they own the motor */
        goal_bz = -1;
        return;
    }
    if ((int32_t)(now - goal_bz_at) < 0)
        return;
    if (goal_bz >= (int)sizeof goal_bz_pat) {
        hal_motor(0);
        goal_bz = -1;
        return;
    }
    hal_motor(!(goal_bz & 1));
    goal_bz_at = now + goal_bz_pat[goal_bz];
    goal_bz++;
}

/* ---- every ui_tick, asleep or not -------------------------------------------- */

static void goal_tick(uint32_t now)
{
    uint32_t gv;
    if (hist_ble_take_goals(&gv)) {
        /* bits 0-15 steps, bits 16-31 water in ml */
        uint16_t s = goal_steps_store(gv & 0xFFFFu);
        uint8_t w = goal_water_store(gv >> 16);
        if (s != goal_steps_v || w != goal_water_qv) {
            goal_steps_v = s;
            goal_water_qv = w;
            persist_mark();
            if (cur == SCR_WATER && !asleep)
                rebuild();
            goal_chk = 0;
        }
    }
    goal_buzz_tick(now);

    int held = alarm_ringing || timer_done || s_in_bed || water_lock;
    if (!goal_chk || now - goal_chk >= 1000) {
        goal_chk = now ? now : 1;
        uint32_t today = today_key();
        int st = ui_steps_today();
        unsigned due = goal_due(&DL, today, st < 0 ? 0 : (uint32_t)st,
                                goal_steps_n(), goal_water_n());
        if (due && !held) {
            DL.goals_hit |= (uint8_t)due;
            persist_mark();                /* once a day, across reboots */
            if (goal_pend_day != DL.day)
                goal_pend = 0;
            goal_pend |= due;
            goal_pend_day = DL.day;
            if (!dnd_on) {
                if (asleep)
                    ui_wake();
                else
                    last_input_ms = now;
                goal_buzz_start(now);
            }
        }
    }

    if (goal_visible()) {
        goal_el = now - goal_t0;
        if (asleep || held || chg_visible() || goal_el >= GOAL_SHOW_MS) {
            goal_hide();
            return;
        }
        last_input_ms = now;               /* never sleep under it */
        if (goal_el < GOAL_ANIM_MS + 2 * GOAL_FRAME_MS &&
            now - goal_frame_at >= GOAL_FRAME_MS) {
            goal_frame_at = now;
            lv_obj_invalidate(goal_box);
        }
        return;
    }
    if (goal_pend && goal_pend_day != DL.day)
        goal_pend = 0;                     /* yesterday's, never seen */
    if (goal_pend && !asleep && !held && !chg_visible()) {
        unsigned k = goal_pend & GOAL_STEPS ? GOAL_STEPS : GOAL_WATER;
        goal_pend &= ~k;
        goal_show(now, k);
    }
}
