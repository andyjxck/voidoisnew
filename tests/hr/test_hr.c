/* Host test: runs firmware/sensors.c HR engine (InfiniTime port) against
   synthetic PPG signals through the real sensors_tick() scheduler.
   make build/test_hr && ./build/test_hr

   2.2 adds wrist-life scenarios: ambient-light blips, arm-swing motion
   (accelerometer + PPG artefact at the swing cadence), a fading pulse,
   a removal into light and a removal in the dark. `make hrcompare`
   builds the same scenarios against the 2.1.0 sensors.c and prints its
   numbers next to these (no checks: it is the before picture). */
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

/* firmware register macros poke MMIO; give them a scratch array */
static uint32_t fake_mmio[0x1000];
#define REG32(a) (fake_mmio[((uintptr_t)(a) >> 2) & 0xFFF])
#ifndef SENSORS_C
#define SENSORS_C "../../firmware/sensors.c"
#endif
#include SENSORS_C

#ifdef HR_OLD
/* 2.1.0 had no hold: a non-zero reading was always live */
static int sensors_bpm_stale(void) { return 0; }
#define VARIANT "2.1.0"
#else
#define VARIANT "2.2"
#endif

static uint32_t T;
static double bpm_true = 72;
static int mode;          /* 0 pulse, 1 flat noise, 2 pulse then lights on */
static uint8_t hrs_regs[0x20];

/* scenario hook: what the wrist is doing at time t (s since start) */
struct env { int pulse, als, move; };
static void (*scene)(double t, struct env *e);
static struct env cur_env(double t)
{
    struct env e = {mode != 1, 300 + (int)(20 * sin(t * 0.05)), 0};
    if (mode == 2 && t > 40)
        e.als = 4000;                     /* watch taken off */
    if (scene)
        scene(t, &e);
    return e;
}

static double frand(void) { return (rand() % 1000) / 1000.0 - 0.5; }

int i2c_read_reg(uint8_t a, uint8_t r, uint8_t *d, unsigned n)
{
    double t = T / 1000.0 - 1.0;
    struct env e = cur_env(t);
    if (a == 0x44 && r == 0x08 && n == 8) {
        double ph = fmod((t + 1.0) * bpm_true / 60.0, 1.0);
        double w = !e.pulse ? 0 : (0.5 + 0.5 * sin(6.2831853 * ph) +
                                   0.15 * sin(12.566 * ph + 1.0));
        double noise = frand() * 12;
        double art = 0;
        if (e.move)        /* arm swing at 1.8 Hz (108/min) and half that */
            art = 400 * sin(6.2831853 * 1.8 * t) +
                  200 * sin(6.2831853 * 0.9 * t + 0.7) + frand() * 120;
        int v = (int)(18000 + w * 80 + noise + art + 150 * sin((t + 1.0) * 0.3));
        if (v > 32767) v = 32767;
        int als = e.als;
        memset(d, 0, n);
        d[1] = (v >> 8) & 0xff;
        d[2] = (v >> 4) & 0x0f;
        d[7] = v & 0x0f;
        d[0] = (als >> 3) & 0xff;
        d[6] = als & 7;
        d[5] = (als >> 11) & 0x3f;
        return 1;
    }
    if (a == 0x44 && n == 1 && r < sizeof hrs_regs) {
        d[0] = r == 0 ? 0x21 : hrs_regs[r];
        return 1;
    }
    if (a == 0x18 && r == 0x12 && n == 6) {
        /* accel, 1024 counts/g, 12-bit left-justified, X/Y swapped */
        int x = 0, y = 0, z = 1000;
        if (e.move) {
            x += (int)(600 * sin(6.2831853 * 1.8 * t) + frand() * 80);
            y += (int)(300 * sin(6.2831853 * 0.9 * t) + frand() * 80);
            z += (int)(250 * sin(6.2831853 * 1.8 * t + 1.1));
        } else {
            x += (int)(frand() * 16);
            y += (int)(frand() * 16);
            z += (int)(frand() * 16);
        }
        int16_t v[3] = {(int16_t)(y * 16), (int16_t)(x * 16), (int16_t)(z * 16)};
        for (int i = 0; i < 3; i++) {
            d[2 * i] = (uint8_t)(v[i] & 0xff);
            d[2 * i + 1] = (uint8_t)((v[i] >> 8) & 0xff);
        }
        return 1;
    }
    if (n) memset(d, 0, n);
    return a == 0x44;          /* only the HRS answers; BMA set up by run_s() */
}
int i2c_write_reg(uint8_t a, uint8_t r, uint8_t v)
{
    if (a == 0x44 && r < sizeof hrs_regs) hrs_regs[r] = v;
    return a == 0x44;
}
int i2c_write_regs(uint8_t a, uint8_t r, const uint8_t *s, unsigned n)
{ (void)a; (void)r; (void)s; (void)n; return 0; }
void hal_delay_ms(uint32_t m) { T += m; }
uint32_t hal_millis_raw(void) { return T; }

static int fails;

/* per-run trace of what the face would show */
struct stats {
    int first_lock;          /* ms to the first non-zero reading */
    int dash_ms;             /* ms showing a dash after the first lock */
    int wrong_ms;            /* ms showing a value more than 7% off */
    int stale_ms;            /* ms showing a held (greyed) value */
    int stale_after_ms;      /* of which after the disturbance started */
    int relock_ms;           /* ms from the end of the disturbance to live */
    int clear_ms;            /* ms from the disturbance start to a dash */
    int min_shown, max_shown;/* values shown during the disturbance */
};
static double dist_beg = -1, dist_end = -1;

static int run_s(double true_bpm, int m, int loop_ms, int secs,
                 struct stats *st, int with_bma)
{
    T = 1000; bpm_true = true_bpm; mode = m;
    hrs_ok = 0; bpm = 0; bpm_valid = 0; hr_on = 1; hrs_powered = 0;
    memset(hrs_regs, 0, sizeof hrs_regs);
    /* BMA: skip the 600 ms engine upload, just sample raw axes */
    bma_ok = with_bma; bma_engine = 0; bma_retries = 3; base = 0;
#ifndef HR_OLD
    moved_seen = 0; moved_ms = 0; amb_run = 0; hr_gap = 0; bpm_good_ms = 0;
#endif
    hrs_init();
    memset(st, 0, sizeof *st);
    st->first_lock = st->relock_ms = st->clear_ms = -1;
    st->min_shown = 9999; st->max_shown = -1;
    for (int i = 0; i < secs * 1000 / loop_ms; i++) {
        T += loop_ms;
        sensors_tick(T);
        double t = T / 1000.0 - 1.0;
        int b = sensors_bpm(), stale = sensors_bpm_stale();
        if (b > 0 && st->first_lock < 0)
            st->first_lock = (int)(T - 1000);
        if (st->first_lock < 0)
            continue;
        if (b <= 0) st->dash_ms += loop_ms;
        else if (fabs(b - true_bpm) > true_bpm * 0.07) st->wrong_ms += loop_ms;
        if (b > 0 && stale) st->stale_ms += loop_ms;
        if (b > 0 && stale && dist_beg >= 0 && t >= dist_beg)
            st->stale_after_ms += loop_ms;
        if (dist_beg >= 0 && t >= dist_beg && (dist_end < 0 || t < dist_end)) {
            if (b > 0 && b < st->min_shown) st->min_shown = b;
            if (b > st->max_shown) st->max_shown = b;
        }
        if (dist_beg >= 0 && t >= dist_beg && b <= 0 && st->clear_ms < 0)
            st->clear_ms = (int)((t - dist_beg) * 1000);
        if (dist_end >= 0 && t >= dist_end && st->relock_ms < 0 &&
            b > 0 && !stale && fabs(b - true_bpm) <= true_bpm * 0.07)
            st->relock_ms = (int)((t - dist_end) * 1000);
    }
    return sensors_bpm();
}

static int run(double true_bpm, int m, int loop_ms, int secs, int *first_lock)
{
    struct stats st;
    int b = run_s(true_bpm, m, loop_ms, secs, &st, 0);
    *first_lock = st.first_lock;
    return b;
}

#ifdef HR_OLD
#define CHECK(c, ...) do { (void)(c); } while (0)
#else
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
#endif

/* ---- scenarios ---- */
/* light blips 30..60 s: one sample every 2 s, plus a 300 ms and a
   700 ms leak (strap shifting) */
static void sc_blips(double t, struct env *e)
{
    if (t >= 30 && t < 60) {
        if (fmod(t - 30, 2.0) < 0.1) e->als = 3000;
        if (t >= 41.0 && t < 41.3) e->als = 3000;
        if (t >= 51.0 && t < 51.7) e->als = 3000;
    }
}
/* arm swinging 30..45 s (walking to the kettle and back) */
static void sc_move(double t, struct env *e)
{
    if (t >= 30 && t < 45) e->move = 1;
}
/* off the wrist into room light at 40 s */
static void sc_off_light(double t, struct env *e)
{
    if (t >= 40) { e->als = 4000; e->pulse = 0; }
}
/* off the wrist, face down in the dark at 40 s: no light, no pulse */
static void sc_off_dark(double t, struct env *e)
{
    if (t >= 40) { e->als = 280; e->pulse = 0; }
}
/* pulse fades out 30..33 s (loose strap), comes back */
static void sc_lost(double t, struct env *e)
{
    if (t >= 30 && t < 33) e->pulse = 0;
}

static void show(const char *name, const struct stats *s, int b)
{
    printf("[%s] %-20s end=%3d dash %5d ms wrong %5d ms held %5d ms"
           " shown %d..%d relock %5d ms clear %5d ms\n",
           VARIANT, name, b, s->dash_ms, s->wrong_ms, s->stale_ms,
           s->min_shown == 9999 ? 0 : s->min_shown, s->max_shown,
           s->relock_ms, s->clear_ms);
}

int main(void)
{
    srand(1);
    int lock, b;
    struct stats st;
    (void)lock; (void)b;
#ifndef HR_OLD
    /* config must match InfiniTime's Hrs3300::Init + Enable */
    run(72, 0, 20, 1, &lock);
    CHECK(hrs_regs[0x01] == 0xD0, "ENABLE=0x%02x want 0xD0", hrs_regs[0x01]);
    CHECK(hrs_regs[0x0C] == 0x2F, "PDRIVER=0x%02x want 0x2F", hrs_regs[0x0C]);
    CHECK(hrs_regs[0x16] == 0x77, "RES=0x%02x want 0x77", hrs_regs[0x16]);
    CHECK(hrs_regs[0x17] == 0x00, "HGAIN=0x%02x want 0x00", hrs_regs[0x17]);

    double rates[] = {55, 72, 95, 130};
    int loops[] = {5, 25, 60, 90};
    for (unsigned ri = 0; ri < sizeof rates / sizeof *rates; ri++)
        for (unsigned li = 0; li < sizeof loops / sizeof *loops; li++) {
            b = run(rates[ri], 0, loops[li], 40, &lock);
            printf("true %3.0f loop %2dms: bpm=%3d first lock %5d ms snr=%.1f\n",
                   rates[ri], loops[li], b, lock, last_snr);
            CHECK(b > 0 && fabs(b - rates[ri]) <= rates[ri] * 0.07,
                  "true %.0f got %d", rates[ri], b);
            CHECK(lock >= 0 && lock < 20000, "lock took %d ms", lock);
            CHECK(!sensors_bpm_stale(), "steady %.0f reads stale", rates[ri]);
        }
    b = run(72, 1, 20, 40, &lock);
    printf("flat signal: bpm=%d\n", b);
    CHECK(b <= 0, "flat signal reported %d bpm", b);
    b = run(72, 2, 20, 45, &lock);
    printf("taken off at 40 s: bpm=%d (locked at %d ms)\n", b, lock);
    CHECK(lock >= 0 && b == 0, "ALS jump should clear the reading, got %d", b);
    /* HR switched off: LED driver off, reading cleared */
    run(72, 0, 20, 30, &lock);
    sensors_hr_set(0);
    T += 20; sensors_tick(T);
    CHECK(sensors_bpm() == 0, "off but bpm=%d", sensors_bpm());
    CHECK((hrs_regs[0x01] & 0x80) == 0 && hrs_regs[0x0C] == 0,
          "off: ENABLE=0x%02x PDRIVER=0x%02x", hrs_regs[0x01], hrs_regs[0x0C]);
#endif

    /* ---- 2.2 wrist-life scenarios, at every rate ---- */
    double srates[] = {55, 72, 95, 130};
    for (unsigned ri = 0; ri < sizeof srates / sizeof *srates; ri++) {
        double r = srates[ri];
        char nm[40];
        int e;

        /* light blips: never a dash, never a wrong value */
        scene = sc_blips; dist_beg = 30; dist_end = 60;
        e = run_s(r, 0, 20, 75, &st, 1);
        snprintf(nm, sizeof nm, "blips @%.0f", r); show(nm, &st, e);
        CHECK(st.dash_ms == 0, "%s: dash for %d ms", nm, st.dash_ms);
        CHECK(st.wrong_ms == 0, "%s: wrong value for %d ms", nm, st.wrong_ms);
        CHECK(e > 0 && fabs(e - r) <= r * 0.07, "%s: ends at %d", nm, e);
        CHECK(st.relock_ms >= 0 && st.relock_ms <= 10000,
              "%s: live again after %d ms", nm, st.relock_ms);

        /* arm swing for 15 s: hold the pre-motion value, never show the
           swing cadence, live again within 10 s of standing still (2 s
           still + a fresh 6.4 s window) */
        scene = sc_move; dist_beg = 30; dist_end = 45;
        e = run_s(r, 0, 20, 70, &st, 1);
        snprintf(nm, sizeof nm, "motion 15 s @%.0f", r); show(nm, &st, e);
        CHECK(st.dash_ms == 0, "%s: dash for %d ms", nm, st.dash_ms);
        CHECK(st.wrong_ms == 0, "%s: wrong value for %d ms (shown %d..%d)",
              nm, st.wrong_ms, st.min_shown, st.max_shown);
        CHECK(st.stale_ms >= 14000, "%s: held only %d ms", nm, st.stale_ms);
        CHECK(st.relock_ms >= 0 && st.relock_ms <= 10000,
              "%s: live again after %d ms", nm, st.relock_ms);
        CHECK(e > 0 && fabs(e - r) <= r * 0.07, "%s: ends at %d", nm, e);

        /* pulse fades for 3 s: held, then live again */
        scene = sc_lost; dist_beg = 30; dist_end = 33;
        e = run_s(r, 0, 20, 60, &st, 1);
        snprintf(nm, sizeof nm, "weak pulse 3 s @%.0f", r); show(nm, &st, e);
        CHECK(st.dash_ms == 0, "%s: dash for %d ms", nm, st.dash_ms);
        CHECK(e > 0 && fabs(e - r) <= r * 0.07, "%s: ends at %d", nm, e);

        /* taken off into light: dash after the 1.5 s debounce, no hold */
        scene = sc_off_light; dist_beg = 40; dist_end = -1;
        e = run_s(r, 0, 20, 50, &st, 1);
        snprintf(nm, sizeof nm, "off, room light @%.0f", r); show(nm, &st, e);
        CHECK(e == 0, "%s: still shows %d", nm, e);
        CHECK(st.clear_ms >= 1400 && st.clear_ms <= 2500,
              "%s: cleared after %d ms", nm, st.clear_ms);

        /* taken off in the dark: nothing to see, so the reading is held
           (greyed) for at most the minute's hold. The estimator itself
           (InfiniTime's, unchanged) can still lock onto sensor noise
           here, as 2.1 did; that shows as `wrong` and is reported, not
           checked: it is a live reading, not the hold. */
        scene = sc_off_dark; dist_beg = 40; dist_end = -1;
        e = run_s(r, 0, 20, 115, &st, 1);
        snprintf(nm, sizeof nm, "off, dark @%.0f", r); show(nm, &st, e);
        CHECK(st.stale_after_ms <= 65000, "%s: held for %d ms",
              nm, st.stale_after_ms);
        if (st.wrong_ms == 0)
            CHECK(e == 0 && st.clear_ms >= 45000 && st.clear_ms <= 65000,
                  "%s: ends %d, cleared after %d ms", nm, e, st.clear_ms);
    }
    scene = NULL; dist_beg = dist_end = -1;

#ifndef HR_OLD
    /* without a BMA the gate is simply off: a plain run still locks */
    b = run_s(72, 0, 20, 30, &st, 0);
    CHECK(b > 0 && fabs(b - 72) <= 72 * 0.07, "no BMA: got %d", b);
#endif
    printf(fails ? "%d HR checks failed\n" : "all HR checks passed\n", fails);
    return fails != 0;
}
