/* 2.2.3 battery percentage: noisy simulated traces through batt.h, the
   same code the watch runs. */
#include <stdio.h>
#include <stdlib.h>
#include "../firmware/batt.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

static unsigned rng = 12345;
static int rnd(int lo, int hi)
{
    rng = rng * 1103515245u + 12345u;
    return lo + (int)((rng >> 8) % (unsigned)(hi - lo + 1));
}
/* noise of a single 12-bit read (1 LSB = 1.17 mV) is ~+-15 mV; after
   64x averaging the residual is ~+-2-4 mV; keep it nasty: +-8 mV, plus
   load spikes and load-model error on top */
static int noise(void) { return rnd(-8, 8); }

/* inverse curve for building traces: percent -> resting mV */
static int pct_to_mv(double p)
{
    for (int i = 1; i < BATT_NPTS; i++)
        if (p <= batt_curve[i][1])
            return batt_curve[i - 1][0] + (int)((p - batt_curve[i - 1][1]) *
                (batt_curve[i][0] - batt_curve[i - 1][0]) /
                (double)(batt_curve[i][1] - batt_curve[i - 1][1]));
    return 4180;
}

static struct batt_load idle = {0, 0, 0};

static void test_curve(void)
{
    int prev = -1;
    for (int mv = 3400; mv <= 4250; mv += 5) {
        int p = batt_curve10(mv);
        CHECK(p >= prev, "curve not monotonic at %d", mv);
        prev = p;
    }
    CHECK(batt_curve10(3400) == 0, "empty");
    CHECK(batt_curve10(4250) == 1000, "full");
    CHECK(batt_curve10(3776) == 480, "anchor 3776 -> 48%% (%d)", batt_curve10(3776));
    /* nonlinear: the 10..90 % band is not one straight line */
    int mid = batt_curve10((3700 + 4080) / 2);
    CHECK(mid > 550, "curve shape (mid %d)", mid);
}

/* the user's report: 26 > 30 > 25 > 29 > 30 at a steady charge */
static void test_steady_noise(void)
{
    batt_t b;
    batt_reset(&b);
    int mv = pct_to_mv(28);
    int lo = 1000, hi = -1, prev = -1, moves = 0, ups = 0;
    for (int i = 0; i < 600; i++) {      /* 10 hours of minute readings */
        struct batt_load ld = {(uint8_t)rnd(0, 3), (uint8_t)rnd(0, 1), 0};
        int load = batt_load_comp_mv(&ld) + rnd(-3, 3);   /* model error */
        int spike = rnd(0, 9) == 0 ? -rnd(30, 80) : 0;   /* radio burst */
        int p = batt_feed(&b, mv - load + noise() + spike, &ld, 0, 0);
        if (p < lo) lo = p;
        if (p > hi) hi = p;
        if (prev >= 0 && p != prev) moves++;
        if (prev >= 0 && i > 3 && p > prev) ups++;
        prev = p;
    }
    printf("  steady 28%%: shown %d..%d, %d changes (%d up) in 600 readings\n",
           lo, hi, moves, ups);
    CHECK(ups == 0, "went back up at a steady charge (%d times)", ups);
    CHECK(hi - lo <= 4, "steady charge wanders %d..%d", lo, hi);
    CHECK(lo >= 24 && hi <= 31, "steady value off (%d..%d)", lo, hi);
}

/* full discharge with load spikes: never up, 1 % steps, tracks truth */
static void test_discharge(void)
{
    batt_t b;
    batt_reset(&b);
    int prev = -1, maxerr = 0, ups = 0, bigsteps = 0;
    const int N = 2400;                  /* 40 hours */
    for (int i = 0; i < N; i++) {
        double truth = 98.0 - 96.0 * i / N;
        int mv = pct_to_mv(truth);
        struct batt_load ld = {(uint8_t)(i % 7 == 0 ? 3 : 0), (uint8_t)(i % 5 == 0), 0};
        int load = batt_load_comp_mv(&ld) + rnd(-3, 3);
        int spike = rnd(0, 7) == 0 ? -rnd(20, 90) : 0;
        int p = batt_feed(&b, mv - load + noise() + spike, &ld, 0, 0);
        if (prev >= 0) {
            if (p > prev) ups++;
            if (prev - p > 1) bigsteps++;
        }
        prev = p;
        if (i > 20) {
            int e = abs(p - (int)(truth + 0.5));
            if (e > maxerr) maxerr = e;
        }
    }
    printf("  discharge 98->2%%: ups %d, jumps >1%% %d, worst error %d%%, end %d%%\n",
           ups, bigsteps, maxerr, prev);
    CHECK(ups == 0, "rose while discharging (%d times)", ups);
    CHECK(bigsteps == 0, "moved more than 1%% at once (%d)", bigsteps);
    CHECK(maxerr <= 5, "tracking error %d%%", maxerr);
    CHECK(prev <= 5, "end value %d", prev);
}

/* charging: never down, ends at 100 when the charger says full */
static void test_charge(void)
{
    batt_t b;
    batt_reset(&b);
    for (int i = 0; i < 5; i++)
        batt_feed(&b, pct_to_mv(20) + noise(), &idle, 0, 0);
    int prev = b.disp, downs = 0, big = 0;
    const int N = 280;                    /* 2 h 20 at 30 s readings */
    for (int i = 0; i < N; i++) {
        double truth = i < 240 ? 20 + 80.0 * i / 240 : 100;
        int full = i > 220;
        int mv = pct_to_mv(truth) + (full ? 0 : BATT_CHG_OFFSET_MV) + noise();
        int p = batt_feed(&b, mv, &idle, 1, full);
        if (p < prev) downs++;
        if (p - prev > 1 && i > BATT_SETTLE_N) big++;
        prev = p;
    }
    printf("  charge 20->full: downs %d, jumps %d, end %d%%\n", downs, big, prev);
    CHECK(downs == 0, "fell while charging (%d)", downs);
    CHECK(big == 0, "jumped while charging (%d)", big);
    CHECK(prev >= 98, "charge end %d", prev);
}

/* plug in and unplug: one correction each, then monotonic again */
static void test_plug_unplug(void)
{
    batt_t b;
    batt_reset(&b);
    for (int i = 0; i < 30; i++)
        batt_feed(&b, pct_to_mv(60) + noise(), &idle, 0, 0);
    int before = b.disp;
    /* plug in: voltage jumps ~ +90 mV; shown value must not leap up
       with it, and after settling one correction is allowed */
    int jumps = 0, prev = before;
    for (int i = 0; i < 12; i++) {
        int p = batt_feed(&b, pct_to_mv(60) + BATT_CHG_OFFSET_MV + noise(), &idle, 1, 0);
        if (abs(p - prev) > 1) jumps++;
        CHECK(p >= prev, "fell after plugging in");
        prev = p;
    }
    printf("  plug at %d%%: shown %d%% after settling, %d corrections\n", before, prev, jumps);
    CHECK(abs(prev - before) <= 3, "plug-in moved %d -> %d", before, prev);
    CHECK(jumps <= 1, "more than one correction on plug-in");
    /* unplug: the surface charge relaxes downward over a few minutes */
    int up = 0;
    prev = b.disp;
    for (int i = 0; i < 20; i++) {
        int relax = i < 5 ? 40 - 8 * i : 0;
        int p = batt_feed(&b, pct_to_mv(61) + relax + noise(), &idle, 0, 0);
        if (i >= BATT_SETTLE_N && p > prev) up++;
        prev = p;
    }
    printf("  unplug: shown %d%%\n", prev);
    CHECK(up == 0, "rose after the unplug correction");
    CHECK(abs(prev - 61) <= 3, "unplug value %d", prev);
}

int main(void)
{
    test_curve();
    test_steady_noise();
    test_discharge();
    test_charge();
    test_plug_unplug();
    printf("test_batt: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
