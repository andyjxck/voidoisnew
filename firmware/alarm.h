/* alarm.h — when the alarm may ring (2.4.1).
 *
 * Pure C, no hardware: firmware/ui_nav.c and tests/test_alarm.c share it.
 *
 * Before 2.4.1 the alarm also rang on a *restored* clock. After a
 * reboot the watch restores the wall clock from its last settings save,
 * and saves are rare (a settings change, a water tap, midnight), so the
 * restored time is usually the moment the alarm was last set or changed.
 * Every restart wound the clock back to that moment, the alarm minute
 * came round again a minute or so later and the alarm rang, whatever
 * the real time was.
 *
 * Now the alarm only rings on a clock it can trust:
 *   CLK_SYNCED  set by the phone (Current Time) or SET TIME this boot
 *   CLK_FRESH   restored from a save made just before a deliberate
 *               restart (power menu, long hold): seconds behind at most
 * and never on CLK_NONE (never set) or CLK_STALE (restored from an older
 * save). And it rings once per day: the day it last rang is saved, so a
 * restart inside the alarm minute does not ring it a second time.
 */
#ifndef VOID_ALARM_H
#define VOID_ALARM_H
#include <stdint.h>

enum { CLK_NONE = 0, CLK_SYNCED = 1, CLK_STALE = 2, CLK_FRESH = 3 };

static inline int alarm_clock_trusted(int clk)
{
    return clk == CLK_SYNCED || clk == CLK_FRESH;
}

/* days since 2024-01-01 for a yyyymmdd key, 1-based so 0 means "never".
   Fits a uint16 until the 2190s. */
static inline uint16_t alarm_day_num(uint32_t ymd)
{
    if (ymd < 20240101u)
        return 0;
    int y = (int)(ymd / 10000u), m = (int)(ymd / 100u % 100u),
        d = (int)(ymd % 100u);
    if (m < 1 || m > 12 || d < 1 || d > 31)
        return 0;
    /* days from civil (Howard Hinnant), relative to 2024-01-01 */
    y -= m <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + doe - 719468;   /* since 1970-01-01 */
    long n = days - 19723 + 1;                       /* 2024-01-01 -> 1 */
    return n > 0 && n < 65536 ? (uint16_t)n : 0;
}

/* Should the main alarm start ringing now?
 *   clk       clock state (CLK_*)
 *   mod       current minute of the day
 *   alarm_mod the alarm's minute of the day
 *   today     alarm_day_num() of today
 *   rang_day  alarm_day_num() of the day it last rang (saved)
 * 1 = ring now (the caller then saves rang_day = today). */
static inline int alarm_due(int on, int clk, int mod, int alarm_mod,
                            uint16_t today, uint16_t rang_day)
{
    if (!on || !alarm_clock_trusted(clk) || mod != alarm_mod)
        return 0;
    if (today && today == rang_day)
        return 0;                 /* already rang today, before a restart */
    return 1;
}

#endif
