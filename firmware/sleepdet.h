/* sleepdet.h — automatic sleep detection (VoidOS 2.4.2).
 *
 * Pure C, no hardware: firmware/ui_sleep.inc and tests/test_sleep.c
 * share it.
 *
 * Works on the 5-minute history slots the watch already records
 * (ui_hist.inc): steps, seconds with movement, the average live heart
 * rate of the slot (with the screen off the HR sensor takes one
 * background reading every 5 minutes) and whether the watch sat on the
 * charger. One call per slot, so it costs nothing on battery.
 *
 * Falling asleep: a run of still slots (no more than ~10 s of movement,
 * no steps), 30 minutes long (45 during the day, 10:00-19:00), with at
 * most one restless slot, a pulse in at least half of them, and the
 * average pulse low (resting + 15, or under 90 while resting is not
 * known yet). Sleep starts at the first still slot of the run.
 *
 * Off the wrist: the charger ends a night at once. No pulse and no
 * movement at all for 20 minutes means the watch came off (a worn,
 * still wrist always gives the background reading a pulse); the night
 * ends where that began. A watch with no pulse never starts a night, so
 * one lying on a table or a charger never counts. With heart rate
 * switched off, a watch that never moved in the run does not start a
 * night, and an hour with no movement at all ends one.
 *
 * Waking: 15 minutes awake in a row (30 s or more of movement per slot,
 * the app's own restless mark, or steps), or 10 minutes walking. The
 * night ends where that began. Shorter wakes count as awake time inside
 * the night. After a wake the night is held for 30 minutes: falling
 * asleep again within them (a trip to the bathroom) continues the same
 * night. 500 steps or the charger end the wait early.
 *
 * Nights and naps: anything under an hour is dropped. A session of 4 h
 * or more, or one whose middle falls between 20:00 and 10:00, is a
 * night; the rest are naps.
 *
 * Record (hlog HREC_SLEEP, unchanged meaning): t = sleep start,
 * b = seconds from start to wake. a was always 0; it now carries
 * bit 0 auto-detected, bit 1 nap, bits 4-15 minutes awake inside the
 * session. Manual BEDTIME / WAKE UP nights keep a = 0, so older app
 * builds read every record as before.
 *
 * Reboots: the state is journalled to flash every slot while a night is
 * open (sd_j_* below), so a restart continues the same night. After a
 * restart the clock is restored from an older save and runs behind; the
 * detector assumes the restart was short and carries on from where the
 * journal stopped until the phone sets the clock. If the phone's time
 * then shows the watch was off for over 30 minutes, the night ends where
 * the journal stopped. A clock the phone corrects within one boot (a
 * resync, a time zone change) moves the open night with it.
 */
#ifndef VOID_SLEEPDET_H
#define VOID_SLEEPDET_H
#include <stdint.h>
#include <string.h>

#define SD_EPOCH        300u
#define SD_STILL_MOVE   5     /* 2-s units: up to 10 s moving in 5 min */
#define SD_STILL_STEPS  15
#define SD_AWAKE_MOVE   15    /* 30 s moving: the app's restless mark */
#define SD_AWAKE_STEPS  20
#define SD_UP_MOVE      60    /* 2 min moving */
#define SD_UP_STEPS     40
#define SD_ONSET_NIGHT  6     /* slots: 30 min */
#define SD_ONSET_DAY    9     /* 45 min while 10:00-19:00 */
#define SD_WAKE_RUN     3     /* 15 min awake */
#define SD_UP_RUN       2     /* 10 min walking */
#define SD_OFF_RUN      4     /* 20 min with no pulse and no movement */
#define SD_OFF_ONSET    2     /* the same, enough to stop a start */
#define SD_ZERO_RUN     12    /* HR switched off: 60 min not moving */
#define SD_MIN_SECS     3600u
#define SD_NIGHT_SECS   (4u * 3600u)
#define SD_MERGE_SECS   1800u
#define SD_PEND_STEPS   500
#define SD_GAP_SECS     1800u
#define SD_JUMP_SECS    600
#define SD_MAX_SECS     (18u * 3600u)

enum { SD_IDLE = 0, SD_ASLEEP = 1, SD_MANUAL = 2, SD_PENDING = 3 };
#define SD_F_GUESS      0x01  /* a slot of the night had a restored clock */

/* HREC_SLEEP a field */
#define SD_A_AUTO       0x0001u
#define SD_A_NAP        0x0002u
#define SD_A_AWAKE_SH   4

struct sd_epoch {
    uint32_t t;        /* slot start, seconds since 2020-01-01 local */
    uint32_t ms;       /* uptime when the slot closed (hal_millis) */
    uint16_t steps;
    uint8_t move;      /* 2-s units with movement, 0..150 */
    uint8_t bpm;       /* average live bpm of the slot, 0 = none */
    uint8_t charging;  /* on the charger at any time in the slot */
    uint8_t hr_on;     /* background HR readings are expected */
    uint8_t guess;     /* clock restored from flash, not set by the phone */
    uint8_t rest;      /* resting bpm reference, 0 = not known */
};

struct sd_night {
    uint32_t t, secs;
    uint16_t a;
    uint8_t guess;
};

struct sd_state {
    /* journalled */
    uint8_t state, flags, wake_run, up_run, off_run, zero_run;
    uint16_t awake;        /* awake slots inside the open night */
    uint16_t pend_steps;   /* steps since the wake, while held */
    uint32_t start_t, end_t, last_t;
    /* RAM only */
    uint32_t cand_t;       /* first still slot of a possible start, 0 = none */
    uint8_t cand_n, cand_still, cand_hr_n, cand_moved, cand_guess;
    uint16_t cand_hr_sum;
    uint8_t ms_known;      /* last_ms is from this boot */
    uint32_t last_ms;
    uint32_t adj;          /* added to a restored clock after a restart */
    uint32_t boot_t;       /* adjusted time the watch came back, 0 = none */
};

static inline void sd_reset(struct sd_state *s) { memset(s, 0, sizeof *s); }

static inline int sd_still(const struct sd_epoch *e)
{
    return !e->charging && e->move <= SD_STILL_MOVE &&
           e->steps <= SD_STILL_STEPS;
}
static inline int sd_awake_ep(const struct sd_epoch *e)
{
    return e->move >= SD_AWAKE_MOVE || e->steps >= SD_AWAKE_STEPS;
}
static inline int sd_up(const struct sd_epoch *e)
{
    return e->move >= SD_UP_MOVE || e->steps >= SD_UP_STEPS;
}

static inline int sd_is_nap(uint32_t start, uint32_t secs)
{
    if (secs >= SD_NIGHT_SECS)
        return 0;
    uint32_t tod = (start + secs / 2u) % 86400u;
    return tod >= 10u * 3600u && tod < 20u * 3600u;
}

static inline void sd_cand_reset(struct sd_state *s)
{
    s->cand_t = 0;
    s->cand_n = s->cand_still = s->cand_hr_n = s->cand_moved = 0;
    s->cand_guess = 0;
    s->cand_hr_sum = 0;
}

/* close the night start_t..end_t. Writes it to *out when it counts
   (min_secs or longer) and returns 1; the state goes back to idle. The
   start candidate is left alone. */
static inline int sd_finish(struct sd_state *s, struct sd_night *out,
                            uint32_t min_secs)
{
    int n = 0;
    int manual = s->state == SD_MANUAL;
    uint32_t secs = s->end_t > s->start_t ? s->end_t - s->start_t : 0;
    if (s->start_t && secs && secs >= min_secs && secs <= SD_MAX_SECS) {
        uint32_t aw = (uint32_t)s->awake * (SD_EPOCH / 60u);
        if (aw > secs / 60u)
            aw = secs / 60u;
        if (aw > 4095u)
            aw = 4095u;
        out->t = s->start_t;
        out->secs = secs;
        out->a = manual ? 0 : (uint16_t)(SD_A_AUTO |
                 (sd_is_nap(s->start_t, secs) ? SD_A_NAP : 0) |
                 aw << SD_A_AWAKE_SH);
        out->guess = s->flags & SD_F_GUESS;
        n = 1;
    }
    s->state = SD_IDLE;
    s->flags = 0;
    s->awake = 0;
    s->wake_run = s->up_run = 0;
    s->pend_steps = 0;
    s->start_t = s->end_t = 0;
    return n;
}

/* the watch was off or came off at `at`: an open auto night ends there,
   a held one is closed. A manual night stays open. */
static inline int sd_cut(struct sd_state *s, uint32_t at, struct sd_night *out)
{
    if (s->state == SD_ASLEEP) {
        s->end_t = at;
        return sd_finish(s, out, SD_MIN_SECS);
    }
    if (s->state == SD_PENDING)
        return sd_finish(s, out, SD_MIN_SECS);
    return 0;
}

/* the clock was moved by `jump` seconds while the watch ran: move the
   times from `from` on with it (0 = all of them) */
static inline void sd_shift(struct sd_state *s, int32_t jump, uint32_t from)
{
    if (s->start_t && s->start_t >= from)
        s->start_t += (uint32_t)jump;
    if (s->end_t && s->end_t >= from)
        s->end_t += (uint32_t)jump;
    if (s->cand_t && s->cand_t >= from)
        s->cand_t += (uint32_t)jump;
    s->last_t += (uint32_t)jump;
}

/* feed one slot to the start candidate; 1 = sleep started at cand_t */
static inline int sd_cand(struct sd_state *s, const struct sd_epoch *e,
                          int off)
{
    if (off || e->charging || sd_awake_ep(e)) {
        sd_cand_reset(s);
        return 0;
    }
    int still = sd_still(e);
    if (!s->cand_t) {
        if (!still)
            return 0;
        s->cand_t = e->t;
    }
    s->cand_n++;
    s->cand_still += (uint8_t)still;
    if (e->move)
        s->cand_moved = 1;
    if (e->guess)
        s->cand_guess = 1;
    if (e->bpm) {
        s->cand_hr_n++;
        s->cand_hr_sum += e->bpm;
    }
    if (s->cand_n - s->cand_still > 1) {     /* a second restless slot */
        sd_cand_reset(s);
        return 0;
    }
    uint32_t tod = s->cand_t % 86400u;
    int day = tod >= 10u * 3600u && tod < 19u * 3600u;
    int need = day ? SD_ONSET_DAY : SD_ONSET_NIGHT;
    if (s->cand_n < need)
        return 0;
    int ok = 1;
    if (e->hr_on) {
        if (s->cand_hr_n * 2 < s->cand_n)
            ok = 0;
        else {
            int avg = (s->cand_hr_sum + s->cand_hr_n / 2) / s->cand_hr_n;
            int lim = e->rest ? e->rest + (day ? 10 : 15) : (day ? 85 : 90);
            if (avg > lim)
                ok = 0;
        }
    } else if (!s->cand_moved) {
        ok = 0;              /* never moved at all: on a table */
    }
    if (!ok && s->cand_n >= 2 * need)  /* start over with fresh slots */
        sd_cand_reset(s);
    return ok;
}

static inline void sd_begin(struct sd_state *s, uint32_t start, uint8_t guess)
{
    s->state = SD_ASLEEP;
    s->start_t = start;
    s->end_t = 0;
    s->awake = 0;
    s->wake_run = s->up_run = 0;
    s->pend_steps = 0;
    s->flags = guess ? SD_F_GUESS : 0;
}

/* the time to use for `t` from a clock that may have been restored */
static inline uint32_t sd_now(const struct sd_state *s, uint32_t t, int guess)
{
    if (!t || !guess)
        return t;
    if (!s->ms_known && s->state != SD_IDLE && t < s->last_t + SD_EPOCH)
        return s->last_t + SD_EPOCH;    /* restarted, no slot yet */
    return t + s->adj;
}

/* one 5-minute slot. Returns how many finished sessions were written to
   out[] (0 or 1; out must hold 2). */
static inline int sd_epoch(struct sd_state *s, const struct sd_epoch *ein,
                           struct sd_night *out)
{
    struct sd_epoch e = *ein;
    int n = 0;
    if (!e.t)
        return 0;

    /* ---- which time this slot really is ---- */
    if (!s->ms_known) {
        /* first slot since boot */
        s->adj = 0;
        s->boot_t = 0;
        if (s->state != SD_IDLE && s->last_t) {
            if (e.guess && e.t < s->last_t + SD_EPOCH) {
                /* restored clock, behind the journal: assume a short
                   restart and carry on from where the journal stopped */
                s->adj = s->last_t + SD_EPOCH - e.t;
                s->boot_t = s->last_t + SD_EPOCH;
            } else if (e.t > s->last_t + SD_GAP_SECS) {
                n += sd_cut(s, s->last_t + SD_EPOCH, out + n);  /* was off */
            } else if (e.guess) {
                s->boot_t = e.t;
            } else if (e.t + SD_EPOCH < s->last_t) {
                /* the phone's time is behind the journal's: the saved
                   times were ahead; move the night back */
                sd_shift(s, (int32_t)(e.t - (s->last_t + SD_EPOCH)), 0);
            }
        }
        if (e.guess)
            e.t += s->adj;
    } else {
        uint32_t el = (e.ms - s->last_ms) / 1000u;   /* really passed */
        if (e.guess)
            e.t += s->adj;
        int32_t jump = (int32_t)(e.t - s->last_t - el);
        if (!e.guess && s->boot_t) {
            /* the phone set the clock after a restart on a restored
               one. Off for longer than assumed: a night from before the
               restart ended when the journal stopped; what came after
               the restart moves to the phone's time. */
            uint32_t b = s->boot_t;
            if (jump > (int32_t)SD_GAP_SECS) {
                if (s->state == SD_ASLEEP && s->start_t < b) {
                    n += sd_cut(s, b, out + n);
                } else if (s->state == SD_PENDING && s->start_t < b) {
                    if (s->end_t > b)
                        s->end_t = b;
                    n += sd_cut(s, 0, out + n);
                }
            }
            sd_shift(s, jump, b);
            s->adj = 0;
            s->boot_t = 0;
        } else if (jump > SD_JUMP_SECS || jump < -SD_JUMP_SECS) {
            sd_shift(s, jump, 0);
        }
    }
    s->ms_known = 1;
    s->last_ms = e.ms;
    if (s->state != SD_IDLE && s->last_t && e.t + SD_EPOCH <= s->last_t) {
        return n;                    /* a slot already seen */
    }
    s->last_t = e.t;

    /* ---- off the wrist ---- */
    int zero = e.move == 0 && e.steps == 0;
    if (e.charging || (e.hr_on && !e.bpm && zero))
        s->off_run = s->off_run < 255 ? s->off_run + 1 : 255;
    else
        s->off_run = 0;
    if (e.charging || zero)
        s->zero_run = s->zero_run < 255 ? s->zero_run + 1 : 255;
    else
        s->zero_run = 0;
    int off_night = e.charging || (e.hr_on ? s->off_run >= SD_OFF_RUN
                                           : s->zero_run >= SD_ZERO_RUN);
    int off_start = e.charging || (e.hr_on ? s->off_run >= SD_OFF_ONSET
                                           : s->zero_run >= SD_ZERO_RUN);
    uint8_t run = e.hr_on ? s->off_run : s->zero_run;
    uint32_t off_at = e.t - (uint32_t)(run ? run - 1 : 0) * SD_EPOCH;

    /* the night's times are on the phone's clock again once a slot is:
       everything before was moved onto it above */
    if (s->state != SD_IDLE) {
        if (e.guess)
            s->flags |= SD_F_GUESS;
        else
            s->flags &= (uint8_t)~SD_F_GUESS;
    }

    switch (s->state) {
    case SD_MANUAL:
        break;

    case SD_ASLEEP:
        if (off_night) {
            s->end_t = off_at > s->start_t ? off_at : e.t;
            n += sd_finish(s, out + n, SD_MIN_SECS);
            sd_cand_reset(s);
            break;
        }
        if (sd_awake_ep(&e)) {
            s->awake++;
            s->wake_run++;
            s->up_run = sd_up(&e) ? s->up_run + 1 : 0;
        } else {
            s->wake_run = s->up_run = 0;
        }
        if (s->wake_run >= SD_WAKE_RUN || s->up_run >= SD_UP_RUN) {
            s->end_t = e.t - (uint32_t)(s->wake_run - 1) * SD_EPOCH;
            s->awake = s->awake >= s->wake_run ? s->awake - s->wake_run : 0;
            s->pend_steps = e.steps;
            s->wake_run = s->up_run = 0;
            s->state = SD_PENDING;
            sd_cand_reset(s);
        } else if (e.t + SD_EPOCH - s->start_t > SD_MAX_SECS) {
            s->end_t = e.t + SD_EPOCH;
            n += sd_finish(s, out + n, SD_MIN_SECS);
            sd_cand_reset(s);
        }
        break;

    case SD_PENDING: {
        uint32_t ps = (uint32_t)s->pend_steps + e.steps;
        s->pend_steps = (uint16_t)(ps > 0xFFFF ? 0xFFFF : ps);
        if (e.charging || off_night || s->pend_steps >= SD_PEND_STEPS) {
            n += sd_finish(s, out + n, SD_MIN_SECS);
            sd_cand_reset(s);
            break;
        }
        int started = sd_cand(s, &e, off_start);
        if (started && s->cand_t <= s->end_t + SD_MERGE_SECS) {
            /* back to sleep within the wait: the same night */
            uint32_t gap = s->cand_t > s->end_t ? s->cand_t - s->end_t : 0;
            s->awake += (uint16_t)(gap / SD_EPOCH);
            s->state = SD_ASLEEP;
            s->end_t = 0;
            if (s->cand_guess)
                s->flags |= SD_F_GUESS;
            sd_cand_reset(s);
            break;
        }
        if (started || (e.t >= s->end_t + SD_MERGE_SECS &&
                        (!s->cand_t || s->cand_t > s->end_t + SD_MERGE_SECS))) {
            n += sd_finish(s, out + n, SD_MIN_SECS);
            if (started) {
                sd_begin(s, s->cand_t, s->cand_guess);
                sd_cand_reset(s);
            }
        }
        break;
    }

    case SD_IDLE:
    default:
        if (sd_cand(s, &e, off_start)) {
            sd_begin(s, s->cand_t, s->cand_guess);
            sd_cand_reset(s);
        }
        break;
    }
    return n;
}

/* BEDTIME: a manual night starts at t (0 when the clock is not known).
   A night still held after a wake is written first. */
static inline int sd_bed(struct sd_state *s, uint32_t t, struct sd_night *out)
{
    int n = 0;
    if (s->state == SD_PENDING)
        n = sd_finish(s, out, SD_MIN_SECS);
    sd_cand_reset(s);
    s->state = SD_MANUAL;
    s->start_t = t;
    s->end_t = 0;
    s->flags = 0;
    s->awake = 0;
    return n;
}

/* WAKE UP at t. A manual night lasts t - start, or `secs` (uptime
   since the tap) when its start time is not known; an auto night ends
   now whatever its length. Returns 1 with the night in *out. */
static inline int sd_wake(struct sd_state *s, uint32_t t, uint32_t secs,
                          struct sd_night *out)
{
    int n = 0;
    if (s->state == SD_MANUAL) {
        if (s->start_t && t > s->start_t && t - s->start_t <= SD_MAX_SECS)
            secs = t - s->start_t;
        if (t && secs) {
            s->start_t = t > secs ? t - secs : t;
            s->end_t = s->start_t + secs;
            n = sd_finish(s, out, 1);
        } else {
            sd_finish(s, out, 0xFFFFFFFFu);
        }
    } else if (s->state == SD_ASLEEP) {
        s->end_t = t;
        n = sd_finish(s, out, 1);
    } else if (s->state == SD_PENDING) {
        n = sd_finish(s, out, SD_MIN_SECS);
    }
    sd_cand_reset(s);
    return n;
}

/* the TRACKING toggle went off at t: an auto night ends now, a held
   one is written, a manual one is dropped (as before 2.4.2) */
static inline int sd_off(struct sd_state *s, uint32_t t, struct sd_night *out)
{
    int n = 0;
    if (s->state == SD_ASLEEP) {
        s->end_t = t;
        n = sd_finish(s, out, SD_MIN_SECS);
    } else if (s->state == SD_PENDING) {
        n = sd_finish(s, out, SD_MIN_SECS);
    } else if (s->state == SD_MANUAL) {
        sd_finish(s, out, 0xFFFFFFFFu);
    }
    sd_cand_reset(s);
    return n;
}

/* ---- journal: the state survives a restart ---------------------------
 * Two 4 KB sectors of the external flash (0x3F8000, 0x3F9000, unused
 * until now) hold 32-byte entries written one after another; the valid
 * entry with the highest sequence number wins. When one sector is full
 * the other is erased (in the background) and writing moves there, so
 * the previous entry is kept until the new one is down. One entry per
 * 5-minute slot while a night is open: a sector a night, each sector
 * erased every other day or so. */
#define SD_J_BASE    0x3F8000u
#define SD_J_SECTOR  4096u
#define SD_J_REC     32u
#define SD_J_PER     (SD_J_SECTOR / SD_J_REC)
#define SD_J_MAGIC   0x314C5356u          /* "VSL1" */

struct sd_flash {
    void (*read)(uint32_t addr, uint8_t *buf, unsigned len);
    void (*write)(uint32_t addr, const uint8_t *buf, unsigned len);
    void (*erase_start)(uint32_t addr);
    int (*busy)(void);
};

struct sd_jrec {
    uint32_t magic, seq;
    uint32_t start_t, end_t, last_t;
    uint16_t awake, pend_steps;
    uint8_t state, flags, wake_run, up_run, off_run, zero_run;
    uint16_t crc;
};
_Static_assert(sizeof(struct sd_jrec) == SD_J_REC, "journal entry size");

struct sd_journal {
    const struct sd_flash *fl;
    uint32_t seq;
    unsigned sec, slot;     /* where the next entry goes */
    int pend;               /* sector erasing, entry waits in rec */
    struct sd_jrec rec;
};

static inline uint16_t sd_crc16(const uint8_t *p, unsigned n)
{
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= (uint16_t)(*p++ << 8);
        for (int k = 0; k < 8; k++)
            c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1);
    }
    return c;
}

static inline uint32_t sd_j_addr(unsigned sec, unsigned slot)
{
    return SD_J_BASE + sec * SD_J_SECTOR + slot * SD_J_REC;
}

/* read the newest entry into *s (idle when there is none) */
static inline void sd_j_load(struct sd_journal *j, const struct sd_flash *fl,
                             struct sd_state *s)
{
    struct sd_jrec r, best;
    int have = 0;
    unsigned bsec = 0, last_used[2] = {0, 0};
    memset(j, 0, sizeof *j);
    memset(&best, 0, sizeof best);
    j->fl = fl;
    for (unsigned sec = 0; sec < 2; sec++) {
        for (unsigned i = 0; i < SD_J_PER; i++) {
            fl->read(sd_j_addr(sec, i), (uint8_t *)&r, sizeof r);
            if (r.magic == 0xFFFFFFFFu && r.seq == 0xFFFFFFFFu)
                continue;
            last_used[sec] = i + 1;
            if (r.magic != SD_J_MAGIC ||
                r.crc != sd_crc16((const uint8_t *)&r, sizeof r - 2))
                continue;
            if (!have || (int32_t)(r.seq - best.seq) > 0) {
                best = r;
                bsec = sec;
                have = 1;
            }
        }
    }
    sd_reset(s);
    if (have && best.state <= SD_PENDING) {
        s->state = best.state;
        s->flags = best.flags;
        s->wake_run = best.wake_run;
        s->up_run = best.up_run;
        s->off_run = best.off_run;
        s->zero_run = best.zero_run;
        s->awake = best.awake;
        s->pend_steps = best.pend_steps;
        s->start_t = best.start_t;
        s->end_t = best.end_t;
        s->last_t = best.last_t;
    }
    if (have) {
        j->seq = best.seq;
        j->sec = bsec;
        j->slot = last_used[bsec];
    } else {
        /* nothing valid: start in sector 0, erased first when used */
        j->sec = 0;
        j->slot = last_used[0] ? SD_J_PER : 0;
        if (last_used[0])
            j->sec = 1;     /* full of junk: the switch erases sector 0 */
    }
}

/* write the state; never waits on an erase */
static inline void sd_j_save(struct sd_journal *j, const struct sd_state *s)
{
    struct sd_jrec r;
    memset(&r, 0, sizeof r);
    r.magic = SD_J_MAGIC;
    r.seq = ++j->seq;
    r.start_t = s->start_t;
    r.end_t = s->end_t;
    r.last_t = s->last_t;
    r.awake = s->awake;
    r.pend_steps = s->pend_steps;
    r.state = s->state;
    r.flags = s->flags;
    r.wake_run = s->wake_run;
    r.up_run = s->up_run;
    r.off_run = s->off_run;
    r.zero_run = s->zero_run;
    r.crc = sd_crc16((const uint8_t *)&r, sizeof r - 2);
    if (j->pend) {            /* erase still running: newest one waits */
        j->rec = r;
        return;
    }
    if (j->slot >= SD_J_PER) {
        j->sec ^= 1u;
        j->slot = 0;
        j->rec = r;
        j->pend = 1;
        j->fl->erase_start(sd_j_addr(j->sec, 0));
        return;
    }
    j->fl->write(sd_j_addr(j->sec, j->slot), (const uint8_t *)&r, sizeof r);
    j->slot++;
}

/* finish a save that waited for its erase; call often */
static inline void sd_j_poll(struct sd_journal *j)
{
    if (!j->pend || j->fl->busy())
        return;
    j->pend = 0;
    j->fl->write(sd_j_addr(j->sec, 0), (const uint8_t *)&j->rec,
                 sizeof j->rec);
    j->slot = 1;
}

#endif
