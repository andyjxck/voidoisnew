/* Host test: runs firmware/sensors.c HR engine (InfiniTime port) against
   synthetic PPG signals through the real sensors_tick() scheduler.
   make build/test_hr && ./build/test_hr */
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

/* firmware register macros poke MMIO; give them a scratch array */
static uint32_t fake_mmio[0x1000];
#define REG32(a) (fake_mmio[((uintptr_t)(a) >> 2) & 0xFFF])
#include "../../firmware/sensors.c"

static uint32_t T;
static double bpm_true = 72;
static int mode;          /* 0 pulse, 1 flat noise, 2 pulse then lights on */
static uint8_t hrs_regs[0x20];

int i2c_read_reg(uint8_t a, uint8_t r, uint8_t *d, unsigned n)
{
    if (a == 0x44 && r == 0x08 && n == 8) {
        double t = T / 1000.0, ph = fmod(t * bpm_true / 60.0, 1.0);
        double w = (mode == 1) ? 0 : (0.5 + 0.5 * sin(6.2831853 * ph) +
                                      0.15 * sin(12.566 * ph + 1.0));
        double noise = ((rand() % 1000) / 1000.0 - 0.5) * 12;
        int als = 300 + (int)(20 * sin(t * 0.05));
        if (mode == 2 && t > 40)
            als = 4000;                     /* watch taken off */
        int v = (int)(18000 + w * 80 + noise + 150 * sin(t * 0.3));
        if (v > 32767) v = 32767;
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
    if (n) memset(d, 0, n);
    return a == 0x44;          /* only the HRS answers; BMA absent */
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

static int run(double true_bpm, int m, int loop_ms, int secs, int *first_lock)
{
    T = 1000; bpm_true = true_bpm; mode = m;
    hrs_ok = 0; bpm = 0; bpm_valid = 0; hr_on = 1;
    memset(hrs_regs, 0, sizeof hrs_regs);
    hrs_init();
    *first_lock = -1;
    for (int i = 0; i < secs * 1000 / loop_ms; i++) {
        T += loop_ms;
        sensors_tick(T);
        if (sensors_bpm() > 0 && *first_lock < 0)
            *first_lock = (int)(T - 1000);
    }
    return sensors_bpm();
}

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    srand(1);
    int lock;
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
            int b = run(rates[ri], 0, loops[li], 40, &lock);
            printf("true %3.0f loop %2dms: bpm=%3d first lock %5d ms snr=%.1f\n",
                   rates[ri], loops[li], b, lock, last_snr);
            CHECK(b > 0 && fabs(b - rates[ri]) <= rates[ri] * 0.07,
                  "true %.0f got %d", rates[ri], b);
            CHECK(lock >= 0 && lock < 20000, "lock took %d ms", lock);
        }
    int b = run(72, 1, 20, 40, &lock);
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
    printf(fails ? "%d HR checks failed\n" : "all HR checks passed\n", fails);
    return fails != 0;
}
