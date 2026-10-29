/* Battery percentage estimator (2.2.3). Header-only so tests/test_batt.c
 * runs the exact code the watch runs.
 *
 * Why 2.2.2 jumped (26 > 30 > 25 > 29): every caller took one raw
 * 12-bit SAADC sample and mapped it straight to a percent. One sample
 * carries ~10-20 mV of noise, and the screen, HR LED, radio and motor
 * pull the cell down by tens of mV while they run. In the middle of the
 * PineTime curve 1 mV is about 0.5 %, so the number bounced.
 *
 * Now, per reading (taken by hal_sys.c about once a minute):
 *   1. 64 oversampled conversions averaged by the SAADC (hal_sys.c)
 *   2. load compensation for what is drawing current right now
 *   3. median of the last three readings (kills one-off spikes)
 *   4. IIR low-pass (1/16: ~16 minute time constant)
 *   5. nonlinear LiPo curve -> tenths of a percent
 *   6. the shown value moves at most 1 % per reading, only after two
 *      three readings in a row agree, never up while discharging,
 *      never down while charging; one correction after plugging in or
 *      unplugging once the voltage has settled, and only if the
 *      estimate is 3 % or more away (so a plug-in doesn't wobble it).
 */
#ifndef VOIDOS_BATT_H
#define VOIDOS_BATT_H
#include <stdint.h>

/* PineTime discharge curve. The anchor points are InfiniTime's measured
   ones (BatteryController.cpp: 3500/0, 3616/3, 3723/22, 3776/48,
   3979/79, 4180/100); the points between them follow a LiPo's shape
   instead of straight lines between six points. */
static const int16_t batt_curve[][2] = {
    {3500, 0}, {3570, 1}, {3616, 3}, {3660, 8}, {3700, 15}, {3723, 22},
    {3750, 35}, {3776, 48}, {3820, 57}, {3870, 64}, {3920, 71},
    {3979, 79}, {4030, 85}, {4080, 90}, {4130, 95}, {4180, 100},
};
#define BATT_NPTS ((int)(sizeof batt_curve / sizeof batt_curve[0]))

/* charging lifts the terminal voltage above the resting voltage */
#define BATT_CHG_OFFSET_MV  90
/* readings to wait after a plug/unplug before the one correction */
#define BATT_SETTLE_N        5

/* mV -> tenths of a percent, 0..1000 */
static inline int batt_curve10(int mv)
{
    if (mv <= batt_curve[0][0])
        return 0;
    for (int i = 1; i < BATT_NPTS; i++) {
        if (mv <= batt_curve[i][0]) {
            int v0 = batt_curve[i - 1][0], v1 = batt_curve[i][0];
            int p0 = batt_curve[i - 1][1] * 10, p1 = batt_curve[i][1] * 10;
            return p0 + (mv - v0) * (p1 - p0) / (v1 - v0);
        }
    }
    return 1000;
}

/* what is loading the cell during the conversion, for compensation */
struct batt_load {
    uint8_t backlight;   /* 0..3 */
    uint8_t hr_led;      /* HRS3300 powered */
    uint8_t motor;       /* vibration motor running (caller defers) */
};

/* mV drop per load, from typical currents x ~0.3 ohm cell + path
   resistance: backlight ~5/10/20 mA, HR LED ~6 mA average */
static inline int batt_load_comp_mv(const struct batt_load *l)
{
    static const uint8_t bl[4] = {0, 2, 4, 7};
    return bl[l->backlight & 3] + (l->hr_led ? 2 : 0);
}

typedef struct {
    int16_t hist[3];     /* last three compensated readings, mV */
    uint8_t nhist;
    int32_t filt16;      /* IIR state, mV x 16 */
    int16_t disp;        /* shown percent, -1 until the first reading */
    int8_t  pend;        /* readings in a row asking to move (+/-) */
    uint8_t init;
    int8_t  chg;         /* last power state: 1 charging/powered, 0 not */
    uint8_t settle;      /* readings left before the correction */
    uint8_t corr;        /* a correction is owed */
    int16_t raw10;       /* last estimate, tenths of a percent (diag) */
} batt_t;

static inline void batt_reset(batt_t *b)
{
    b->nhist = 0;
    b->filt16 = 0;
    b->disp = -1;
    b->pend = 0;
    b->init = 0;
    b->chg = -1;
    b->settle = 0;
    b->corr = 0;
    b->raw10 = 0;
}

static inline int batt_med3(int a, int b, int c)
{
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) { int t = b; b = c; c = t; }
    if (a > b) { int t = a; a = b; b = t; }
    return b;
}

/* Feed one reading. mv: averaged cell voltage; powered: USB power is
   present; full: powered and the charger says it has finished.
   Returns the percent to show. */
static inline int batt_feed(batt_t *b, int mv, const struct batt_load *ld,
                            int powered, int full)
{
    int c = mv + batt_load_comp_mv(ld);
    if (powered && !full)
        c -= BATT_CHG_OFFSET_MV;

    /* plug / unplug: restart the filter on the new regime, and owe one
       correction once BATT_SETTLE_N readings have come in */
    int chg = powered ? 1 : 0;
    if (b->init && chg != b->chg) {
        b->nhist = 0;
        b->settle = BATT_SETTLE_N;
        b->corr = 1;
        b->pend = 0;
    }
    b->chg = (int8_t)chg;

    if (b->nhist < 3) {
        b->hist[b->nhist++] = (int16_t)c;
    } else {
        b->hist[0] = b->hist[1];
        b->hist[1] = b->hist[2];
        b->hist[2] = (int16_t)c;
    }
    int m = b->nhist == 3 ? batt_med3(b->hist[0], b->hist[1], b->hist[2])
                          : c;
    if (b->nhist == 1)
        b->filt16 = m * 16;           /* new regime: start from here */
    else
        b->filt16 += (m * 16 - b->filt16) / 16;
    int raw10 = batt_curve10((int)((b->filt16 + 8) / 16));
    if (full)
        raw10 = 1000;
    b->raw10 = (int16_t)raw10;

    if (!b->init) {
        /* boot: show the plain estimate for the first readings (taken
           2 s apart), then seed the filter from their median */
        b->disp = (int16_t)((raw10 + 5) / 10);
        if (b->nhist == 3) {
            b->init = 1;
            b->filt16 = m * 16;
            b->disp = (int16_t)((batt_curve10(m) + 5) / 10);
            if (full)
                b->disp = 100;
        }
        return b->disp;
    }

    if (b->corr) {
        /* settling: follow the median directly so the correction uses
           the settled voltage, not a slow average of the transient */
        b->filt16 = m * 16;
        raw10 = full ? 1000 : batt_curve10(m);
        b->raw10 = (int16_t)raw10;
        if (b->settle)
            b->settle--;
        if (!b->settle) {
            b->corr = 0;
            int d = raw10 - b->disp * 10;
            if (d >= 30 || d <= -30)
                b->disp = (int16_t)((raw10 + 5) / 10);
            b->pend = 0;
            return b->disp;
        }
        return b->disp;               /* hold while it settles */
    }

    int d10 = b->disp * 10;
    if (!chg) {                       /* discharging: down only */
        if (raw10 < d10 - 15) {
            if (b->pend < 0) b->pend--; else b->pend = -1;
        } else {
            b->pend = 0;
        }
        if (b->pend <= -3 && b->disp > 0) {
            b->disp--;
            b->pend = 0;
        }
    } else {                          /* charging / full: up only */
        if (raw10 > d10 + 15) {
            if (b->pend > 0) b->pend++; else b->pend = 1;
        } else {
            b->pend = 0;
        }
        if (b->pend >= 3 && b->disp < 100) {
            b->disp++;
            b->pend = 0;
        }
    }
    return b->disp;
}

#endif
