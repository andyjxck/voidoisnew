/* ---- phone history (included by ui_nav.c) ----------------------------------
 * Records what the watch measures into the hlog outbox (hlog.h) for the
 * VoidHealth phone app: a sample every 5 minutes (steps, heart rate,
 * movement, battery), each water tap, each workout and night, and the
 * day totals at midnight. Nothing is recorded while the clock is unknown:
 * a record the phone cannot place in time is no use to it. */
#include "hlog.h"

int sensors_take_move_secs(void);

static void hl_read(uint32_t a, uint8_t *b, unsigned n) { spi_flash_read(a, b, n); }
static void hl_write(uint32_t a, const uint8_t *b, unsigned n) { spi_flash_write(a, b, n); }
static void hl_erase(uint32_t a) { spi_flash_sector_erase_start(a); }
static int hl_busy(void) { return spi_flash_busy(); }
static const struct hlog_flash hl_flash = {hl_read, hl_write, hl_erase, hl_busy};

static uint32_t hs_slot;              /* open 5-minute slot (t / 300), 0 = none */
static int hs_steps0;                 /* ui_steps() when it opened */
static uint32_t hs_bpm_sum;
static uint16_t hs_bpm_n;
static uint8_t hs_bpm_max, hs_flag;
static uint32_t hw_sum;               /* heart rate during the workout */
static uint16_t hw_n;
static uint8_t hw_max, hw_on;

/* now as seconds since 2020-01-01 local; 0 while the clock is unknown */
static uint32_t hist_now(uint8_t *flag)
{
    uint32_t key = today_key();
    if (!key)
        return 0;
    int32_t days = daily_ordinal(key) - daily_ordinal(HLOG_EPOCH_KEY);
    if (days < 0)
        return 0;
    if (flag)
        *flag = hal_clock_synced() ? 0 : HREC_F_CLOCK_GUESS;
    return (uint32_t)days * 86400u + (uint32_t)hal_clock_hour() * 3600u +
           (uint32_t)hal_clock_minute() * 60u + (uint32_t)hal_clock_second();
}

static void hist_init(void)
{
    uint32_t seed = hal_millis() * 2654435761u ^ P.steps_total * 40503u ^
                    today_key() ^ (uint32_t)hal_clock_second() << 24 ^
                    (uint32_t)hal_battery_pct() << 8;
    hlog_init(&hl_flash, seed);
}

static void hist_open_slot(uint32_t slot, uint8_t flag)
{
    hs_slot = slot;
    hs_flag = flag;
    hs_steps0 = ui_steps();
    hs_bpm_sum = 0;
    hs_bpm_n = 0;
    hs_bpm_max = 0;
    (void)sensors_take_move_secs();
}

static void hist_close_slot(void)
{
    int st = ui_steps();
    int d = hs_steps0 >= 0 && st >= hs_steps0 ? st - hs_steps0 : 0;
    if (d > 0xFFFF)
        d = 0xFFFF;
    uint32_t avg = hs_bpm_n ? (hs_bpm_sum + hs_bpm_n / 2u) / hs_bpm_n : 0;
    int batt = hal_battery_pct();
    batt = batt < 0 ? 0 : batt > 100 ? 100 : batt;
    int mv = (sensors_take_move_secs() + 1) / 2;
    if (mv > 150)
        mv = 150;
    uint32_t b = (avg > 255 ? 255 : avg) | (uint32_t)hs_bpm_max << 8 |
                 (uint32_t)(batt | (hal_on_charger() ? 0x80 : 0)) << 16 |
                 (uint32_t)mv << 24;
    hlog_append(hs_slot * 300u, (uint8_t)(HREC_SAMPLE | hs_flag), (uint16_t)d, b);
}

/* every ui_tick */
static void hist_tick(uint32_t now)
{
    static uint32_t last;
    hlog_poll();
    if (now - last < 1000 && last)
        return;
    last = now ? now : 1;
    int bpm = sensors_bpm();
    if (bpm > 0 && bpm < 255 && !sensors_bpm_stale()) {
        hs_bpm_sum += (uint32_t)bpm;
        hs_bpm_n++;
        if (bpm > hs_bpm_max)
            hs_bpm_max = (uint8_t)bpm;
        if (w_active && !w_paused) {
            hw_sum += (uint32_t)bpm;
            hw_n++;
            if (bpm > hw_max)
                hw_max = (uint8_t)bpm;
        }
    }
    if (w_active && !hw_on) {
        hw_on = 1;
        hw_sum = 0;
        hw_n = 0;
        hw_max = 0;
    }
    uint8_t flag = 0;
    uint32_t t = hist_now(&flag);
    if (!t) {
        hs_slot = 0;
        return;
    }
    uint32_t slot = t / 300u;
    if (!hs_slot) {
        hist_open_slot(slot, flag);
        return;
    }
    if (slot != hs_slot) {
        hist_close_slot();
        hist_open_slot(slot, flag);
    }
}

/* the day that just ended (daily_roll moved it to y_*) */
static void hist_day(void)
{
    if (!DL.y_day)
        return;
    int32_t days = daily_ordinal(DL.y_day) - daily_ordinal(HLOG_EPOCH_KEY);
    if (days < 0)
        return;
    uint8_t flag = hal_clock_synced() ? 0 : HREC_F_CLOCK_GUESS;
    uint32_t st = DL.y_steps > 0xFFFFFFu ? 0xFFFFFFu : DL.y_steps;
    hlog_append((uint32_t)days * 86400u, (uint8_t)(HREC_DAY | flag), DL.y_water,
                st | (uint32_t)DL.y_rest << 24);
}

static void hist_water(int delta)
{
    uint8_t flag;
    uint32_t t = hist_now(&flag);
    if (t && delta)
        hlog_append(t, (uint8_t)(HREC_WATER | flag), (uint16_t)(int16_t)delta,
                    DL.water_ml);
}

/* call after w_last_ms / w_last_sets are set */
static void hist_workout_end(void)
{
    uint8_t flag;
    uint32_t t = hist_now(&flag);
    uint32_t avg = hw_n ? (hw_sum + hw_n / 2u) / hw_n : 0;
    uint32_t max = hw_max;
    hw_on = 0;
    if (!t)
        return;
    uint32_t since = (hal_millis() - w_start_ms) / 1000u;
    uint32_t start = t > since ? t - since : t;
    uint32_t act = w_last_ms / 1000u;
    uint32_t sets = w_last_sets > 255 ? 255 : (uint32_t)w_last_sets;
    uint32_t routine = w_routine < 0 ? 0xFF : (uint32_t)w_routine;
    hlog_append(start, (uint8_t)(HREC_WORKOUT | flag),
                (uint16_t)(act > 0xFFFF ? 0xFFFF : act),
                sets | routine << 8 | avg << 16 | max << 24);
}

/* call after s_last_ms is set */
static void hist_sleep_end(void)
{
    uint8_t flag;
    uint32_t t = hist_now(&flag);
    uint32_t secs = s_last_ms / 1000u;
    if (t && secs)
        hlog_append(t > secs ? t - secs : t, (uint8_t)(HREC_SLEEP | flag), 0,
                    secs);
}
