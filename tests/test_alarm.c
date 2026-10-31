/* 2.4.1: the alarm only rings at its time, on a clock it can trust, once
   a day (firmware/alarm.h). */
#include <stdio.h>
#include "alarm.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, \
                      __LINE__, #c); fails++; } } while (0)

int main(void)
{
    uint16_t today = alarm_day_num(20261031u);
    int a = 7 * 60;                                  /* 07:00 */

    /* day numbers */
    CHECK(alarm_day_num(20240101u) == 1);
    CHECK(alarm_day_num(20240102u) == 2);
    CHECK(alarm_day_num(20250101u) == 367);          /* 2024 is a leap year */
    CHECK(alarm_day_num(20261101u) == today + 1);
    CHECK(alarm_day_num(20270101u) - alarm_day_num(20261231u) == 1);
    CHECK(alarm_day_num(0) == 0);
    CHECK(alarm_day_num(20231231u) == 0);

    /* the bug: after a restart the clock is restored from the last save,
       e.g. 06:59 when the alarm was set. It must not ring on it. */
    CHECK(!alarm_due(1, CLK_STALE, a, a, today, 0));
    /* clock never set: never rings */
    CHECK(!alarm_due(1, CLK_NONE, a, a, today, 0));
    CHECK(!alarm_due(1, CLK_NONE, a, a, 0, 0));

    /* rings at its time on a synced clock, or one saved at a restart */
    CHECK(alarm_due(1, CLK_SYNCED, a, a, today, 0));
    CHECK(alarm_due(1, CLK_FRESH, a, a, today, 0));
    /* not at any other minute, not when off */
    CHECK(!alarm_due(1, CLK_SYNCED, a + 1, a, today, 0));
    CHECK(!alarm_due(1, CLK_SYNCED, a - 1, a, today, 0));
    CHECK(!alarm_due(0, CLK_SYNCED, a, a, today, 0));

    /* rang today, watch restarted inside the alarm minute: no second ring */
    CHECK(!alarm_due(1, CLK_FRESH, a, a, today, today));
    CHECK(!alarm_due(1, CLK_SYNCED, a, a, today, today));
    /* rang yesterday: rings again today */
    CHECK(alarm_due(1, CLK_SYNCED, a, a, today, (uint16_t)(today - 1)));
    /* a synced clock whose date is unknown still rings */
    CHECK(alarm_due(1, CLK_SYNCED, a, a, 0, 0));

    if (fails) {
        printf("test_alarm: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_alarm: all checks passed\n");
    return 0;
}
