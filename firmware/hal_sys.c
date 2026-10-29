/* PineTime system HAL: RTC1 timebase, wall clock, side button, CST816S
   touch over TWIM1, watchdog feed, diagnostics. */
#include <string.h>
#include "voidos/hal.h"
#include "board.h"

void display_init(void);
void display_backlight(int level);
void display_fill(int x0, int y0, int x1, int y1, uint16_t c);
void sensors_init(void);

/* ---- watchdog / diagnostics ---------------------------------------- */

void wdt_kick(void)
{
    WDT_RR0 = WDT_KICK;
}

static void spin_ms(unsigned ms)
{
    while (ms--) {
        wdt_kick();
        volatile unsigned n = 4800;
        while (n--)
            __asm volatile("nop");
    }
}

/* blink the backlight `pattern` times per burst for ~10 s, then reset.
   1 = HardFault, 2 = stray IRQ, 3 = SPI timeout, 4 = I2C timeout */
void fault_blink(int pattern)
{
    extern int nimble_last_stage(void);
    extern void nimble_fault(int);
    int st = nimble_last_stage();
    __asm volatile("cpsid i");
    nimble_fault(pattern);   /* survives reset in GPREGRET2 */
    GPIO_DIRSET = BIT(PIN_BL_H);
    for (int rep = 0; rep < 8; rep++) {
        for (int i = 0; i < pattern; i++) {
            GPIO_OUTCLR = BIT(PIN_BL_H);
            spin_ms(150);
            GPIO_OUTSET = BIT(PIN_BL_H);
            spin_ms(150);
        }
        spin_ms(800);
        /* then the stage as digits: tens blinks, gap, ones blinks */
        if (st >= 0 && st <= 99) {
            for (int i = 0; i < st / 10; i++) {
                GPIO_OUTCLR = BIT(PIN_BL_H);
                spin_ms(150);
                GPIO_OUTSET = BIT(PIN_BL_H);
                spin_ms(150);
            }
            spin_ms(500);
            for (int i = 0; i < st % 10; i++) {
                GPIO_OUTCLR = BIT(PIN_BL_H);
                spin_ms(150);
                GPIO_OUTSET = BIT(PIN_BL_H);
                spin_ms(150);
            }
        }
        spin_ms(800);
    }
    SCB_AIRCR = 0x05FA0004;
    for (;;)
        ;
}

/* ---- time ------------------------------------------------------------ */

static uint32_t rtc_last;      /* last raw 24-bit counter seen */
static uint64_t rtc_ticks;     /* monotonic 32768 Hz tick count */

static uint64_t ticks_now(void)
{
    uint32_t c = RTC1_COUNTER & 0xFFFFFF;
    rtc_ticks += (c - rtc_last) & 0xFFFFFF;
    rtc_last = c;
    return rtc_ticks;
}

static uint32_t millis_raw(void)
{
    return (uint32_t)(ticks_now() * 1000u / 32768u);
}

uint32_t hal_millis_raw(void) { return millis_raw(); }

/* The frame is rendered in several bands; every band must see the same
   instant or moving parts (second hand) tear between bands. main latches
   the time once per frame via hal_frame_begin(). */
static uint32_t frame_ms;
static int frame_latched;

void hal_frame_begin(void)
{
    frame_ms = millis_raw();
    frame_latched = 1;
}

uint32_t hal_millis(void)
{
    return frame_latched ? frame_ms : millis_raw();
}

/* 2.2.3 battery: hal_delay_ms used to spin on the RTC counter, so the
   CPU never slept — the main loop's 20 + 5 ms waits kept it running
   at 64 MHz around the clock (several mA, the watch's biggest drain).
   Now it sleeps in WFE until an RTC1 COMPARE0 event. The RTC1 interrupt
   is enabled in the peripheral but NOT in the NVIC; SEVONPEND turns its
   pending bit into a wake-up event, so no handler is needed and nothing
   else changes. Radio/timer interrupts still wake it (and run their
   ISRs) as before; the loop just re-arms until the time is up. */
#define RTC1_EVENTS_COMPARE0 REG32(0x40011140)
#define RTC1_INTENSET        REG32(0x40011304)
#define RTC1_CC0             REG32(0x40011540)
#define NVIC_ICPR0           REG32(0xE000E280)
#define SCB_SCR              REG32(0xE000ED10)
#define RTC1_IRQ_BIT         (1u << 17)
static int idle_ready;
static uint32_t idle_sleeps;
uint32_t hal_idle_sleeps(void) { return idle_sleeps; }

static void idle_init(void)
{
    RTC1_EVENTS_COMPARE0 = 0;
    RTC1_INTENSET = 1u << 16;          /* COMPARE0 -> pending (no ISR) */
    NVIC_ICPR0 = RTC1_IRQ_BIT;
    SCB_SCR |= 1u << 4;                /* SEVONPEND */
    idle_ready = 1;
}

void hal_delay_ms(unsigned ms)
{
    uint32_t t0 = millis_raw();
    if (!idle_ready || ms < 2) {
        while (millis_raw() - t0 < ms)
            wdt_kick();
        return;
    }
    for (;;) {
        uint32_t el = millis_raw() - t0;
        if (el >= ms)
            break;
        uint32_t left = ms - el;
        if (left > 60000u)
            left = 60000u;
        uint32_t ticks = left * 32768u / 1000u;
        if (ticks < 3)
            ticks = 3;                 /* CC must be >= COUNTER + 2 */
        RTC1_EVENTS_COMPARE0 = 0;
        (void)RTC1_EVENTS_COMPARE0;
        NVIC_ICPR0 = RTC1_IRQ_BIT;
        RTC1_CC0 = (RTC1_COUNTER + ticks) & 0xFFFFFF;
        wdt_kick();
        idle_sleeps++;
        /* a stale event flag just makes this WFE return at once; the
           loop re-arms and the next one sleeps */
        __asm volatile ("dsb\n\twfe" ::: "memory");
    }
    RTC1_EVENTS_COMPARE0 = 0;
    NVIC_ICPR0 = RTC1_IRQ_BIT;
}

/* on the charger: P0.19 POWER_PRESENT pulls low (InfiniTime PinMap) */
int hal_on_charger(void)
{
    return (GPIO_IN & BIT(19)) == 0;
}

/* wall clock: seeded at boot, overwritten when the phone writes the
   BLE Current Time characteristic (hal_clock_set). */
#define BUILD_TOD_MS ((((__TIME__[0] - '0') * 10u + (__TIME__[1] - '0')) * 3600u + \
                      ((__TIME__[3] - '0') * 10u + (__TIME__[4] - '0')) * 60u) * 1000u)
static uint32_t clock_seed_ms = BUILD_TOD_MS;   /* unsynced fallback: build time */
static uint32_t clock_set_at, clock_set_tod;
static int clock_valid;
static uint16_t clock_year;
static uint8_t clock_mon, clock_day, clock_dow;

static uint32_t wall_ms(void)
{
    return (clock_seed_ms + hal_millis()) % 86400000u;
}

int hal_clock_hour(void)   { return (int)(wall_ms() / 3600000u); }
int hal_clock_minute(void) { return (int)(wall_ms() / 60000u % 60u); }
int hal_clock_second(void) { return (int)(wall_ms() / 1000u % 60u); }
int hal_clock_millis(void) { return (int)(wall_ms() % 1000u); }
/* clock_valid: 0 never set, 1 set by the phone or SET TIME this boot,
   2 restored from the last save. A restored clock is behind by however
   long the watch was off or being flashed, so it is NOT "synced": the
   face shows it as pending and alarms wait for a real sync. */
int hal_clock_synced(void) { return clock_valid == 1; }
int hal_clock_restored(void) { return clock_valid == 2; }
uint32_t hal_clock_tod_ms(void) { return wall_ms(); }

void hal_clock_set(int y, int mo, int d, int dow, int h, int mi, int s)
{
    uint32_t tod = ((uint32_t)h * 3600u + (uint32_t)mi * 60u +
                    (uint32_t)s) * 1000u;
    clock_seed_ms = (tod + 86400000u - hal_millis() % 86400000u) %
                    86400000u;
    clock_set_at = hal_millis();
    clock_set_tod = tod;
    clock_year = (uint16_t)y;
    clock_mon = (uint8_t)mo;
    clock_day = (uint8_t)d;
    clock_dow = (uint8_t)dow;
    clock_valid = 1;
}

/* last saved wall clock, after a reboot or a flash: shown as a rough
   time, never trusted (see hal_clock_synced) */
void hal_clock_restore(int y, int mo, int d, int dow, int h, int mi, int s)
{
    if (clock_valid == 1)
        return;
    hal_clock_set(y, mo, d, dow, h, mi, s);
    clock_valid = 2;
}

/* Sakamoto's algorithm: weekday from Y/M/D, returns 1=Mon .. 7=Sun.
   Only needed because older persisted/synced records never stored
   dow — the calendar knows it anyway. */
static int ymd_to_dow(int y, int m, int d)
{
    static const uint8_t t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3)
        y -= 1;
    int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7; /* 0=Sun */
    return ((w + 6) % 7) + 1;                                  /* 1=Mon..7=Sun */
}

void hal_clock_date(int *y, int *mo, int *d, int *dow)
{
    /* roll the synced date forward by however many midnights have
       passed since it was set, with month/year/leap handling */
    int yy = clock_year, mm = clock_mon, dd = clock_day, ww = clock_dow;
    if (clock_valid && yy) {
        uint32_t days = (clock_set_tod + (hal_millis() - clock_set_at)) /
                        86400000u;
        while (days--) {
            static const uint8_t dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
            int leap = (yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0;
            int lim = dim[(mm - 1) % 12] + (mm == 2 && leap);
            if (++dd > lim) {
                dd = 1;
                if (++mm > 12) {
                    mm = 1;
                    yy++;
                }
            }
            ww = ww % 7 + 1;           /* 1=Mon .. 7=Sun */
        }
    }
    /* dow was only ever set by BLE time sync — a persisted date has
       dow=0 forever. Derive it from the calendar instead. */
    if (ww < 1 || ww > 7) {
        if (yy >= 2000 && mm >= 1 && mm <= 12 && dd >= 1 && dd <= 31)
            ww = ymd_to_dow(yy, mm, dd);
    }
    if (y) *y = yy;
    if (mo) *mo = mm;
    if (d) *d = dd;
    if (dow) *dow = ww;
}

/* vibration motor, GPIO P0.16. ACTIVE LOW: the pin drives a P-channel
   switch, so pin high = motor off, pin low = motor on. InfiniTime
   (MotorController) sets the pin high at init and clears it to run.
   1.6.x had this inverted: the first hal_motor() call (a notification,
   timer or water-lock exit) drove the pin low and every "off" after
   that kept it low, which is the runaway vibration 1.6.1 chased. */
static void motor_init(void)
{
    GPIO_OUTSET = 1u << PIN_VIBRA;        /* off before it becomes an output */
    GPIO_DIRSET = 1u << PIN_VIBRA;
}

void hal_motor(int on)
{
    if (on)
        GPIO_OUTCLR = 1u << PIN_VIBRA;
    else
        GPIO_OUTSET = 1u << PIN_VIBRA;
}

/* battery voltage on P0.31 (AIN7) through the 2:1 divider */
#include "hal_saadc.h"   /* register offsets, checked against the nRF MDK */
static int battery_raw;
static int battery_fail;
int hal_battery_raw(void) { return battery_raw; }
int hal_battery_fail(void) { return battery_fail; }

int hal_battery_pct(void)
{
    /* InfiniTime's discharge curve (BatteryController.cpp) */
    static const uint16_t curve[][2] = {
        {3500, 0}, {3616, 3}, {3723, 22}, {3776, 48}, {3979, 79}, {4180, 100}
    };
    /* volatile: EasyDMA writes this behind the compiler's back. In 2.2.0
       it was a plain array, so GCC folded "result[0] = 0 ... raw =
       result[0]" into the constant 0 and the reading was always 0. */
    static volatile int16_t result[2];
    SAADC_ENABLE = 1;
    SAADC_CH0_PSELP = 8;            /* PSEL is AIN#+1: 8 = AIN7 = P0.31 */
    SAADC_CH0_PSELN = 0;
    /* InfiniTime's channel config: gain 1/4, internal 0.6 V ref (2.4 V
       full scale), 40 us acquisition (the 2:1 divider is high impedance) */
    SAADC_CH0_CONF = (2u << 8) | (5u << 16);
    SAADC_RESOL = 2;                /* 12 bit */
    SAADC_OVERSAMPLE = 0;
    SAADC_SAMPLERATE = 0;           /* one sample per SAMPLE task */
    SAADC_RES_PTR = (uint32_t)result;
    SAADC_RES_MAX = 1;
    SAADC_EVENTS_STARTED = 0;
    SAADC_EVENTS_END = 0;
    SAADC_EVENTS_STOPPED = 0;
    result[0] = -32768;             /* sentinel: never written by the ADC */
    SAADC_START = 1;
    /* datasheet order: START -> STARTED -> SAMPLE -> END (the result is
       in RAM) -> STOP -> STOPPED. 2.2.0 waited on DONE (0x108, the
       conversion only) and then on CALIBRATEDONE (0x110), which never
       fires, so it stopped the SAADC before the result reached RAM. */
    for (unsigned spin = 0; !SAADC_EVENTS_STARTED && spin < 200000; spin++)
        ;
    SAADC_SAMPLE = 1;
    for (unsigned spin = 0; !SAADC_EVENTS_END && spin < 200000; spin++)
        ;
    SAADC_STOP = 1;
    for (unsigned spin = 0; !SAADC_EVENTS_STOPPED && spin < 200000; spin++)
        ;
    SAADC_ENABLE = 0;
    __asm volatile ("" ::: "memory");
    int raw = result[0];
    if (raw == -32768) {            /* no conversion landed: keep the last */
        battery_fail++;
        raw = battery_raw > 0 ? battery_raw : 0;
    }
    battery_raw = raw;
    if (raw < 0) raw = 0;
    /* raw/4096 * 2.4 V input range * 2 (the 2:1 battery divider) */
    uint32_t mv = (uint32_t)raw * 4800u / 4096u;
    if (mv <= curve[0][0])
        return 0;
    for (unsigned i = 1; i < sizeof curve / sizeof curve[0]; i++) {
        if (mv <= curve[i][0]) {
            uint32_t dv = curve[i][0] - curve[i - 1][0];
            uint32_t dp = curve[i][1] - curve[i - 1][1];
            return (int)(curve[i - 1][1] +
                         (mv - curve[i - 1][0]) * dp / dv);
        }
    }
    return 100;
}

static void time_init(void)
{
    CLOCK_LFCLKSRC = 1;  /* external 32.768 kHz crystal */
    CLOCK_EVENTS_LFCLKSTARTED = 0;
    CLOCK_LFCLKSTART = 1;
    for (unsigned spin = 0; !CLOCK_EVENTS_LFCLKSTARTED && spin < 2000000; spin++)
        wdt_kick();
    RTC1_PRESCALER = 0;
    RTC1_CLEAR = 1;
    RTC1_START = 1;
    rtc_last = 0;
    rtc_ticks = 0;
    /* power-present / charging pins as plain inputs (InfiniTime) */
    GPIO_PIN_CNF(19) = 0;
    GPIO_PIN_CNF(12) = 0;
    idle_init();
}

/* ---- I2C (TWIM1) ------------------------------------------------------ */

static uint8_t twi_buf[80] __attribute__((aligned(4)));

/* If a reset caught a slave mid-transfer it holds SDA low forever — every
   TWIM transaction then times out waiting for bus idle. Clock SCL until
   the slave releases SDA, then issue a STOP by hand. */
static void twi_unstick(void)
{
    /* both lines as pull-up inputs: read the real bus level */
    GPIO_PIN_CNF(PIN_SDA) = (3u << 2);
    GPIO_PIN_CNF(PIN_SCL) = (3u << 2);
    hal_delay_ms(1);
    touch_diag.bus_lvl = ((GPIO_IN >> PIN_SDA) & 3);   /* SDA|1 SCL|2 */
    if (GPIO_IN & BIT(PIN_SDA))
        return;                                       /* bus free */

    /* SCL as open-drain output; pulse up to 9 clocks */
    GPIO_PIN_CNF(PIN_SCL) = (1u << 0) | (3u << 2) | (6u << 8);
    GPIO_DIRSET = BIT(PIN_SCL);
    for (int i = 0; i < 9 && !(GPIO_IN & BIT(PIN_SDA)); i++) {
        GPIO_OUTCLR = BIT(PIN_SCL);
        hal_delay_ms(1);
        GPIO_OUTSET = BIT(PIN_SCL);
        hal_delay_ms(1);
    }
    /* STOP condition: SDA low, raise SCL, release SDA high */
    GPIO_PIN_CNF(PIN_SDA) = (1u << 0) | (3u << 2) | (6u << 8);
    GPIO_DIRSET = BIT(PIN_SDA);
    GPIO_OUTCLR = BIT(PIN_SDA);
    hal_delay_ms(1);
    GPIO_OUTSET = BIT(PIN_SCL);
    hal_delay_ms(1);
    GPIO_OUTSET = BIT(PIN_SDA);
    hal_delay_ms(1);
    /* re-read lines; report recovery result in high nibble */
    GPIO_PIN_CNF(PIN_SDA) = (3u << 2);
    GPIO_PIN_CNF(PIN_SCL) = (3u << 2);
    hal_delay_ms(1);
    touch_diag.bus_lvl |= (((GPIO_IN >> PIN_SDA) & 3) << 4);
}

static void twi_init(void)
{
    TWIM1_ENABLE = 0;
    twi_unstick();
    /* SDA/SCL: input, S0D1 drive, pull-up */
    GPIO_PIN_CNF(PIN_SDA) = (3u << 2) | (6u << 8);
    GPIO_PIN_CNF(PIN_SCL) = (3u << 2) | (6u << 8);
    TWIM1_PSEL_SCL = PIN_SCL;
    TWIM1_PSEL_SDA = PIN_SDA;
    /* 390 kHz, not 400: nRF52832 errata 219 (TWIM tLOW too short at the
       400 kHz setting). Same value InfiniTime uses
       (MaxTwiFrequencyWithoutHardwareBug = 0x06200000). */
    TWIM1_FREQ = 0x06200000;
    TWIM1_ENABLE = 6;
}

/* diagnostics, shown on-screen while bringing touch up */
touch_diag_t touch_diag;
unsigned touch_twim_enable(void) { return TWIM1_ENABLE; }

static void twi_recover(void);

static void twi_abort(void)
{
    TWIM1_EVENTS_STOPPED = 0;
    TWIM1_STOP = 1;
    int stopped = 0;
    for (unsigned spin = 0; spin < 100000 && !stopped; spin++)
        stopped = TWIM1_EVENTS_STOPPED != 0;
    TWIM1_EVENTS_STOPPED = 0;
    if (!stopped)
        twi_recover();
}

/* a jammed peripheral ignores STOP — every later transfer then stalls
   waiting for TX. Full reset: disable (aborts in-flight work), unstick
   the lines, re-init. One bad transaction can no longer poison the bus
   for the rest of the session. */
static void twi_recover(void)
{
    TWIM1_ENABLE = 0;
    hal_delay_ms(1);
    twi_unstick();
    GPIO_PIN_CNF(PIN_SDA) = (3u << 2) | (6u << 8);
    GPIO_PIN_CNF(PIN_SCL) = (3u << 2) | (6u << 8);
    TWIM1_PSEL_SCL = PIN_SCL;
    TWIM1_PSEL_SDA = PIN_SDA;
    TWIM1_FREQ = 0x06200000;   /* errata 219, see twi_init */
    TWIM1_ENABLE = 6;
}

/* boot progress stripes: a colored bar per init step, top of screen.
   The LCD holds the last frame even through a crash, so the bars that
   appear (or don't) show exactly which step died. */
void display_send(int x0, int y0, int x1, int y1, const uint16_t *px);
/* 1 = show the colored boot stripes (boot debug only — the user sees
   these as random lines/flashes at every boot, so production = 0) */
static int boot_marks;
static void imark(int row, uint16_t c)
{
    static uint16_t line[240];
    if (!boot_marks)
        return;
    for (int i = 0; i < 240; i++)
        line[i] = c;
    display_send(0, row * 12, 239, row * 12 + 9, line);
}

extern void nimble_stage(int);
extern void nimble_mark(int);

static int twi_wait(volatile uint32_t *ev)
{
    for (unsigned spin = 0; spin < 200000; spin++) {
        if (*ev) {
            *ev = 0;
            touch_diag.twi_ok++;
            return 1;
        }
        if (TWIM1_EVENTS_ERROR) {
            TWIM1_EVENTS_ERROR = 0;
            touch_diag.last_err = TWIM1_ERRORSRC;
            TWIM1_ERRORSRC = 7;
            touch_diag.twi_err++;
            twi_abort();
            return 0;
        }
    }
    touch_diag.twi_timeout++;
    twi_recover();
    return 0;
}

/* write `reg`, then read `n` bytes; returns 1 on success */
int i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *dst, unsigned n)
{
    TWIM1_ADDRESS = addr;
    twi_buf[0] = reg;
    TWIM1_TXD_PTR = (uint32_t)twi_buf;
    TWIM1_TXD_MAXCNT = 1;
    TWIM1_RXD_PTR = (uint32_t)twi_buf;
    TWIM1_RXD_MAXCNT = n;
    TWIM1_SHORTS = 0;   /* step the transaction by hand; diag which stalls */
    TWIM1_EVENTS_STOPPED = 0;
    TWIM1_EVENTS_ERROR = 0;
    TWIM1_EVENTS_LASTTX = 0;
    TWIM1_EVENTS_LASTRX = 0;
    TWIM1_STARTTX = 1;
    if (!twi_wait(&TWIM1_EVENTS_LASTTX)) { touch_diag.stall = 1; return 0; }
    TWIM1_STARTRX = 1;
    if (!twi_wait(&TWIM1_EVENTS_LASTRX)) { touch_diag.stall = 2; return 0; }
    TWIM1_STOP = 1;
    if (!twi_wait(&TWIM1_EVENTS_STOPPED)) { touch_diag.stall = 3; return 0; }
    for (unsigned i = 0; i < n; i++)
        dst[i] = twi_buf[i];
    return 1;
}

int i2c_write_regs(uint8_t addr, uint8_t reg, const uint8_t *src, unsigned n);

int i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
    return i2c_write_regs(addr, reg, &val, 1);
}

/* write `reg` followed by `n` data bytes in one transaction */
int i2c_write_regs(uint8_t addr, uint8_t reg, const uint8_t *src, unsigned n)
{
    if (n + 1 > sizeof(twi_buf))
        return 0;
    TWIM1_ADDRESS = addr;
    twi_buf[0] = reg;
    for (unsigned i = 0; i < n; i++)
        twi_buf[i + 1] = src[i];
    TWIM1_TXD_PTR = (uint32_t)twi_buf;
    TWIM1_TXD_MAXCNT = n + 1;
    TWIM1_SHORTS = 0;
    TWIM1_EVENTS_STOPPED = 0;
    TWIM1_EVENTS_ERROR = 0;
    TWIM1_EVENTS_LASTTX = 0;
    TWIM1_STARTTX = 1;
    if (!twi_wait(&TWIM1_EVENTS_LASTTX)) { touch_diag.stall = 4; return 0; }
    TWIM1_STOP = 1;
    if (!twi_wait(&TWIM1_EVENTS_STOPPED)) { touch_diag.stall = 5; return 0; }
    return 1;
}

/* ---- touch (CST816S) --------------------------------------------------- */

#define CST816_ADDR 0x15

static int touch_ok;
static int touch_was_down;
static int last_gesture;
static int last_gesture_x;

static void touch_init(void)
{
    imark(0, 0xFFFF);          /* white:  entered touch_init */
    GPIO_DIRSET = BIT(PIN_TP_RESET);
    GPIO_OUTCLR = BIT(PIN_TP_RESET);
    hal_delay_ms(20);
    GPIO_OUTSET = BIT(PIN_TP_RESET);
    hal_delay_ms(200);
    imark(1, 0xF800);          /* red:    reset released */

    /* wake it (first transaction after reset is often NAKed) */
    uint8_t dummy;
    i2c_read_reg(CST816_ADDR, 0x15, &dummy, 1);
    hal_delay_ms(5);
    imark(2, 0xFD20);          /* orange: wake read done */
    /* explicit exit-sleep in case a previous firmware left it asleep */
    i2c_write_reg(CST816_ADDR, 0xA5, 0x00);
    hal_delay_ms(5);
    imark(3, 0xFFE0);          /* yellow: sleep-exit write done */
    /* bus scan: which I2C devices actually ACK (diagnostic). One green
       stripe per 8 probed addresses — the count of bars shows where a
       probe kills the bus, and the stage register holds the address. */
    for (int a = 0x08; a < 0x78; a++) {
        if ((a & 7) == 0)
            imark(4 + (a >> 3), 0x07E0);
        nimble_stage(a);
        uint8_t b;
        if (i2c_read_reg(a, 0x00, &b, 1) && touch_diag.scan_count < 8)
            touch_diag.scan[touch_diag.scan_count++] = (uint8_t)a;
    }
    nimble_stage(79);
    imark(4, 0x07E0);          /* green:  bus scan done */
    /* motion mask: continuous left/right + double-click (as InfiniTime) */
    i2c_write_reg(CST816_ADDR, 0xEC, 0x05);
    /* IRQ control: pulse INT on touch, on touch-state change, on gesture.
       Without this the controller stays silent for plain taps/drags. */
    i2c_write_reg(CST816_ADDR, 0xFA, 0x70);
    /* disable the 5s no-gesture auto-reset (it kills mid-swipe touches) */
    i2c_write_reg(CST816_ADDR, 0xFB, 0x00);
    imark(5, 0x07FF);          /* cyan:   config writes done */
    /* input, pull-up, SENSE low; with DETECTMODE=LDETECT the LATCH bit
       captures the controller's short INT pulse until we clear it */
    GPIO_PIN_CNF(PIN_TP_INT) = (3u << 2) | (3u << 16);
    GPIO_DETECTMODE = 1;
    GPIO_LATCH = BIT(PIN_TP_INT);
    imark(6, 0x001F);          /* blue:   int gpio configured */
    /* even if the ID read fails, keep polling: the bus may just be slow
       to wake and reads fail gracefully */
    uint8_t id[3] = {0, 0, 0};
    if (i2c_read_reg(CST816_ADDR, 0xA7, id, 3))
        touch_diag.chip_id = id[0] | (id[1] << 8) | (id[2] << 16);
    /* drain whatever touch state the controller latched during power-on */
    i2c_read_reg(CST816_ADDR, 0x00, id, 3);
    (void)hal_millis_raw;
    touch_ok = 1;
    imark(7, 0xF81F);          /* magenta: touch_init complete */
}

/* ---- button ------------------------------------------------------------ */

static int btn_was_down;
static uint32_t btn_down_ms;

static void button_init(void)
{
    GPIO_DIRSET = BIT(PIN_BTN_OUT);
    GPIO_OUTSET = BIT(PIN_BTN_OUT);
    GPIO_PIN_CNF(PIN_BTN_IN) = (1u << 2); /* input, pull-down */
}

/* ---- touch sampling ------------------------------------------------------ */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* One raw sample for LVGL's pointer indev: 1 if a finger is on the glass,
   x/y holding the position. LVGL derives taps/swipes itself. */
int touch_sample(int *x, int *y)
{
    if (!touch_ok)
        return 0;
    /* Read the chip only when it raised INT or a finger is down, plus a
       100 ms safety poll. Reading it every loop iteration (~200/s) was
       hogging TWIM1 away from the sensors and exposing the bus wedge. */
    static uint32_t last_forced;
    static int bad_reads;
    uint32_t now = millis_raw();
    if (!touch_was_down && !(GPIO_LATCH & BIT(PIN_TP_INT)) &&
        now - last_forced < 100) {
        *x = touch_diag.last_x;
        *y = touch_diag.last_y;
        return 0;
    }
    last_forced = now;
    uint8_t d[7];
    if (!i2c_read_reg(CST816_ADDR, 0x00, d, 7)) {
        *x = touch_diag.last_x;
        *y = touch_diag.last_y;
        if (++bad_reads > 3)              /* don't hold a phantom press */
            touch_was_down = 0;
        return touch_was_down;
    }
    bad_reads = 0;
    /* Gesture IDs persist in d[1] forever; the INT latch marks fresh
       pulses. Use both: a changed nonzero ID = new gesture even if the
       latch missed the pulse. Re-arm on finger-up so repeats re-fire. */
    int fresh = (GPIO_LATCH & BIT(PIN_TP_INT)) != 0;
    GPIO_LATCH = BIT(PIN_TP_INT);
    if (fresh)
        touch_diag.latches++;
    int fingers = d[2] & 0x0F;
    int event = d[3] >> 6;
    *x = clampi(((d[3] & 0x0F) << 8) | d[4], 0, VOIDOS_WIDTH - 1);
    *y = clampi(((d[5] & 0x0F) << 8) | d[6], 0, VOIDOS_HEIGHT - 1);
    touch_diag.reads++;
    touch_diag.last_x = *x;
    touch_diag.last_y = *y;
    touch_diag.last_fingers = fingers;
    touch_diag.last_event = d[1];
    memcpy(touch_diag.raw, d, 6);
    static uint8_t prev_gest;
    /* the gesture register holds its last value even after release —
       re-arm only when it actually reads 0 (new touch clears it).
       Re-arming on finger-up re-latched the same swipe and bounced
       face->apps instantly after CC->face. */
    if (d[1] == 0)
        prev_gest = 0;
    if (d[1] && d[1] != prev_gest) {
        last_gesture = d[1];                 /* CST816S gesture ID */
        last_gesture_x = *x;                 /* which half it landed on */
        prev_gest = d[1];
    }
    touch_was_down = (fingers != 0 && event != 1);
    return touch_was_down;
}

/* CST816S gesture register (0x01): 1=up 2=down 3=left 4=right
   5=single-tap 0x0B=double-tap 0x0C=long-press. Returns and clears the latest ID. */
/* CST816S low-power mode while the display is off; INT still fires on
   a touch so the wake path keeps working */
void touch_sleep(int on)
{
    if (!touch_ok)
        return;
    if (on)
        i2c_write_reg(CST816_ADDR, 0xA5, 0x03);
    else {
        /* only a reset pulse wakes it: toggle the TP reset line */
        GPIO_OUTCLR = BIT(PIN_TP_RESET);
        hal_delay_ms(5);
        GPIO_OUTSET = BIT(PIN_TP_RESET);
        hal_delay_ms(50);
        i2c_write_reg(CST816_ADDR, 0xA5, 0x00);
    }
}

int touch_gesture(void)
{
    int g = last_gesture;
    last_gesture = 0;
    return g;
}

/* X coordinate captured when the current gesture latched — lets the
   nav layer tell a left-half down-swipe from a right-half one */
int touch_gesture_x(void)
{
    return last_gesture_x;
}

/* ---- HAL entry points --------------------------------------------------- */

int hal_init(void)
{
    /* instruction cache is off out of reset — flash code runs ~2x slower */
    *(volatile uint32_t *)0x4001E540 = 1;   /* NVMC->ICACHECNF */
    /* backlight stays OFF through init: nothing is allowed on-screen
       until the first face frame renders (no garbage, no stripes) */
    GPIO_DIRSET = BIT(PIN_BL_L) | BIT(PIN_BL_M) | BIT(PIN_BL_H);
    GPIO_OUTSET = BIT(PIN_BL_L) | BIT(PIN_BL_M) | BIT(PIN_BL_H);
    time_init();
    button_init();
    motor_init();
    nimble_stage(80);
    display_init();
    twi_init();
    touch_init();
    imark(9, 0x8410);          /* grey:  hal i2c init complete */
    sensors_init();
    nimble_stage(84);
    return 1;
}

void hal_shutdown(void) {}

void hal_present(void) {}

/* button state: 0 released, else ms held; >=3000 triggers the revert reset */
uint32_t button_held_ms(void)
{
    static uint32_t last_now;
    uint32_t now = millis_raw();
    int btn = (GPIO_IN & BIT(PIN_BTN_IN)) != 0;
    if (btn && !btn_was_down) {
        btn_was_down = 1;
        btn_down_ms = now;
    } else if (!btn) {
        btn_was_down = 0;
    }
    last_now = now;
    (void)last_now;
    return btn ? now - btn_down_ms : 0;
}

/* MCUBoot image_ok flag: the DFU'd image is test-booted once and reverts
   on the next reset unless the app marks itself confirmed. PineTime app
   slot ends at 0x7C000; the 16-byte boot magic is last, image_ok is the
   byte 8 before it (0x7BFE8 — the address InfiniTime's validator uses). */
#define IMAGE_OK_WORD (*(volatile uint32_t *)0x0007BFE8u)

int hal_boot_confirmed(void) { return IMAGE_OK_WORD == 1u; }

void hal_boot_confirm(void)
{
    volatile uint32_t *nvmc_ready  = (volatile uint32_t *)0x4001E400u;
    volatile uint32_t *nvmc_config = (volatile uint32_t *)0x4001E504u;
    if (IMAGE_OK_WORD == 1u)
        return;
    *nvmc_config = 1u;                 /* NVMC write-enable */
    IMAGE_OK_WORD = 1u;
    while (!(*nvmc_ready & 1u)) { }
    *nvmc_config = 0u;
}

int hal_poll_event(hal_event_t *ev) { (void)ev; return 0; }
