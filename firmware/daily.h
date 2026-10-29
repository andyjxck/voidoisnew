/* daily.h — the day boundary for every daily stat (2.2.2).
 *
 * Pure C, no hardware: the firmware and tests/daily share it.
 *
 * One record holds the counters that belong to one local calendar day
 * (yyyymmdd key) and the totals of the last finished day. daily_roll()
 * is called with today's local date whenever the clock is usable:
 *   - the first time (key 0, e.g. straight after the 2.2.1 -> 2.2.2
 *     update) it only adopts today's key: nothing is wiped, and the
 *     lifetime step count becomes today's starting point;
 *   - when the date has moved FORWARD it files the old day's totals as
 *     "last day" and zeroes the counters, once. That covers a watch that
 *     slept or was switched off over midnight: the first check after
 *     wake, or after the phone resyncs the clock, does the roll;
 *   - the same date again (a reboot, a resync) changes nothing;
 *   - a date BEFORE the stored one is ignored, so a clock or timezone
 *     step back over midnight can never wipe the day twice. The one
 *     exception is a phone-synced clock that is two or more days behind
 *     the stored key: the stored key was wrong (a bad clock), so the
 *     phone's date is adopted, without wiping.
 * Local time is whatever the phone wrote (CTS carries local wall time),
 * so midnight is the phone's local midnight, DST and travel included.
 */
#ifndef VOID_DAILY_H
#define VOID_DAILY_H
#include <stdint.h>

#define DAILY_BASE_PENDING 0xFFFFFFFFu   /* step chip unreadable at roll */

struct daily {
    uint32_t day;          /* yyyymmdd the counters belong to, 0 = unset */
    uint32_t steps_base;   /* lifetime steps when `day` began */
    uint16_t water_ml;
    uint8_t rest_bpm;      /* lowest resting reading today, 0 = none */
    uint8_t sleep_day_ok;  /* reserved */
    uint32_t y_day;        /* the last finished day, 0 = none */
    uint32_t y_steps;
    uint16_t y_water;
    uint8_t y_rest, y_pad;
};

static inline uint32_t daily_key(int y, int m, int d)
{
    if (y < 2020 || y > 2099 || m < 1 || m > 12 || d < 1 || d > 31)
        return 0;
    return (uint32_t)y * 10000u + (uint32_t)m * 100u + (uint32_t)d;
}

/* days since 1970-01-01 for a yyyymmdd key (Howard Hinnant's
   days_from_civil), for day differences across months and years */
static inline int32_t daily_ordinal(uint32_t key)
{
    int y = (int)(key / 10000u), m = (int)(key / 100u % 100u),
        d = (int)(key % 100u);
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static inline uint32_t daily_steps(const struct daily *dl, int lifetime)
{
    if (lifetime < 0 || dl->steps_base == DAILY_BASE_PENDING ||
        (uint32_t)lifetime < dl->steps_base)
        return 0;
    return (uint32_t)lifetime - dl->steps_base;
}

/* returns 1 when the counters were rolled over to `today` */
static inline int daily_roll(struct daily *dl, uint32_t today, int synced,
                             int lifetime)
{
    /* a pending base is filled as soon as the step chip answers */
    if (dl->steps_base == DAILY_BASE_PENDING && lifetime >= 0)
        dl->steps_base = (uint32_t)lifetime;
    /* a lifetime count below the base (chip or record reset) restarts
       today's count instead of showing a huge wrapped number */
    if (lifetime >= 0 && dl->steps_base != DAILY_BASE_PENDING &&
        (uint32_t)lifetime < dl->steps_base)
        dl->steps_base = (uint32_t)lifetime;
    if (!today)
        return 0;                         /* no usable clock yet */
    if (!dl->day) {
        dl->day = today;
        if (lifetime >= 0)
            dl->steps_base = (uint32_t)lifetime;
        else
            dl->steps_base = DAILY_BASE_PENDING;
        return 0;
    }
    if (today == dl->day)
        return 0;
    if (today < dl->day) {
        if (synced && daily_ordinal(dl->day) - daily_ordinal(today) >= 2)
            dl->day = today;              /* the stored key was bogus */
        return 0;
    }
    dl->y_day = dl->day;
    dl->y_steps = daily_steps(dl, lifetime);
    dl->y_water = dl->water_ml;
    dl->y_rest = dl->rest_bpm;
    dl->day = today;
    dl->steps_base = lifetime >= 0 ? (uint32_t)lifetime : DAILY_BASE_PENDING;
    dl->water_ml = 0;
    dl->rest_bpm = 0;
    return 1;
}

/* the last finished day's totals are only shown when it really was
   yesterday */
static inline int daily_have_yesterday(const struct daily *dl)
{
    return dl->y_day && dl->day &&
           daily_ordinal(dl->day) - daily_ordinal(dl->y_day) == 1;
}

#endif
