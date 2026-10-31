/* ---- automatic sleep tracking, 2.4.2 (included by ui_nav.c) ---------------
 * sleepdet.h does the detecting; this file feeds it the 5-minute history
 * slots (hist_close_slot), keeps its state in the flash journal so a
 * restart carries on the same night, writes finished nights to the phone
 * outbox, and drives the SLEEP screen: s_in_bed is set while a night is
 * open, detected or started with BEDTIME. TRACKING switches detection
 * on and off; BEDTIME / WAKE UP still work as an override. */
#include "sleepdet.h"

static struct sd_state SD;
static struct sd_journal SJ;
static uint8_t sd_dirty, sd_ready;

static void sj_read(uint32_t a, uint8_t *b, unsigned n) { spi_flash_read(a, b, n); }
static void sj_write(uint32_t a, const uint8_t *b, unsigned n) { spi_flash_write(a, b, n); }
static void sj_erase(uint32_t a) { spi_flash_sector_erase_start(a); }
static int sj_busy(void) { return spi_flash_busy(); }
static const struct sd_flash sj_flash = {sj_read, sj_write, sj_erase, sj_busy};

/* now on the night's clock (a restored clock carries on from the
   journal after a restart); 0 while the clock is unknown */
static uint32_t sleep_now(uint8_t *flag)
{
    uint8_t f = 0;
    uint32_t t = hist_now(&f);
    if (flag)
        *flag = f;
    return sd_now(&SD, t, f != 0);
}

/* s_in_bed and the screen timer follow the detector */
static void sleep_sync_ui(void)
{
    int open = SD.state == SD_ASLEEP || SD.state == SD_MANUAL;
    if (open && SD.start_t) {
        uint32_t now = sleep_now(0);
        if (now) {
            uint32_t el = now > SD.start_t ? now - SD.start_t : 0;
            if (el > SD_MAX_SECS)
                el = SD_MAX_SECS;
            s_start_ms = hal_millis() - el * 1000u;
        }
    } else if (open && !s_in_bed) {
        s_start_ms = hal_millis();
    }
    s_in_bed = open;
}

static void sleep_log(const struct sd_night *n)
{
    hlog_append(n->t, (uint8_t)(HREC_SLEEP | (n->guess ? HREC_F_CLOCK_GUESS : 0)),
                n->a, n->secs);
    if (!(n->a & SD_A_NAP)) {
        uint32_t aw = (uint32_t)(n->a >> SD_A_AWAKE_SH) * 60u;
        uint32_t secs = n->secs > aw ? n->secs - aw : n->secs;
        s_last_ms = secs * 1000u;
        sleep_day = today_key();
        persist_mark();
    }
}

static void sleep_init(void)
{
    sd_j_load(&SJ, &sj_flash, &SD);
    sd_ready = 1;
    sleep_sync_ui();
}

/* one closed 5-minute slot from hist_close_slot */
static void sleep_slot(uint32_t t, int steps, int mv, uint32_t bpm, int chg,
                       uint8_t flag)
{
    if (!sd_ready || (!s_track && SD.state == SD_IDLE))
        return;
    struct sd_epoch e;
    struct sd_night out[2];
    uint8_t was = SD.state;
    int rest = DL.rest_bpm > DL.y_rest ? DL.rest_bpm : DL.y_rest;
    e.t = t;
    e.ms = hal_millis();
    e.steps = (uint16_t)(steps > 0xFFFF ? 0xFFFF : steps < 0 ? 0 : steps);
    e.move = (uint8_t)(mv < 0 ? 0 : mv > 150 ? 150 : mv);
    e.bpm = (uint8_t)(bpm > 255 ? 255 : bpm);
    e.charging = (uint8_t)(chg != 0);
    e.hr_on = (uint8_t)(sensors_hr_on() && sensors_bpm() >= 0);
    e.guess = (uint8_t)(flag != 0);
    e.rest = (uint8_t)rest;
    int n = s_track ? sd_epoch(&SD, &e, out) : 0;
    for (int i = 0; i < n; i++)
        sleep_log(&out[i]);
    if (n || was != SD_IDLE || SD.state != SD_IDLE)
        sd_dirty = 1;
    int was_open = s_in_bed;
    sleep_sync_ui();
    if (was_open != s_in_bed && cur == SCR_SLEEP && !asleep)
        rebuild();
}

/* BEDTIME */
static void sleep_bed_tap(void)
{
    struct sd_night o;
    if (sd_bed(&SD, sleep_now(0), &o))
        sleep_log(&o);
    s_in_bed = 0;                 /* start the screen timer now */
    sleep_sync_ui();
    s_start_ms = hal_millis();
    sd_dirty = 1;
}

/* WAKE UP: ends a manual or a detected night now */
static void sleep_wake_tap(void)
{
    struct sd_night o;
    uint8_t flag = 0;
    uint32_t now = sleep_now(&flag);
    uint32_t secs = (hal_millis() - s_start_ms) / 1000u;
    if (sd_wake(&SD, now, secs, &o)) {
        if (flag)
            o.guess = 1;
        sleep_log(&o);
        uint32_t aw = (uint32_t)(o.a >> SD_A_AWAKE_SH) * 60u;
        s_last_ms = (o.secs > aw ? o.secs - aw : o.secs) * 1000u;
    } else {
        s_last_ms = secs * 1000u;  /* the screen still shows the time */
    }
    sleep_sync_ui();
    sd_dirty = 1;
}

/* TRACKING switched off */
static void sleep_track_off(void)
{
    struct sd_night o;
    if (sd_off(&SD, sleep_now(0), &o))
        sleep_log(&o);
    sleep_sync_ui();
    sd_dirty = 1;
}

/* every ui_tick: journal writes wait for the flash, never in a tap */
static void sleep_poll(uint32_t now)
{
    static uint32_t last;
    if (!sd_ready)
        return;
    sd_j_poll(&SJ);
    if (sd_dirty && !SJ.pend && !spi_flash_busy()) {
        sd_j_save(&SJ, &SD);
        sd_dirty = 0;
    }
    /* the clock may have been set since: keep the timer right */
    if (s_in_bed && now - last >= 60000u) {
        last = now;
        sleep_sync_ui();
    }
}

/* the open night was detected, not started with BEDTIME */
static int sleep_auto_open(void) { return SD.state == SD_ASLEEP; }
