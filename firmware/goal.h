/* goal.h — daily goal celebrations (2.4.0).
 *
 * Pure C, no hardware: the firmware and tests/ble/test_store_daily share
 * it. Which goals were hit today lives in struct daily (goals_hit), so it
 * is saved with the rest of the day's counters, survives a reboot, and
 * daily_roll() clears it at midnight. Each goal is celebrated once a day.
 *
 * The goals themselves are set in the VoidHealth app and written over the
 * history service (CTRL op 4). The watch stores them in two spare bytes
 * of its settings record; 0 there (every record before 2.4.0) means the
 * default.
 */
#ifndef VOID_GOAL_H
#define VOID_GOAL_H
#include <stdint.h>
#include "daily.h"

#define GOAL_STEPS 0x01u
#define GOAL_WATER 0x02u

#define GOAL_STEPS_DEFAULT 10000
#define GOAL_STEPS_MIN     1000
#define GOAL_STEPS_MAX     50000
#define GOAL_WATER_DEFAULT 2000     /* ml */
#define GOAL_WATER_UNIT    50       /* ml per stored step */
#define GOAL_WATER_MIN_Q   5        /* 250 ml */
#define GOAL_WATER_MAX_Q   250      /* 12500 ml */

/* stored value -> goal; 0 or anything out of range reads as the default */
static inline int goal_steps_of(uint16_t v)
{
    return v >= GOAL_STEPS_MIN && v <= GOAL_STEPS_MAX ? (int)v
                                                      : GOAL_STEPS_DEFAULT;
}

static inline int goal_water_of(uint8_t q)
{
    return q >= GOAL_WATER_MIN_Q && q <= GOAL_WATER_MAX_Q
               ? (int)q * GOAL_WATER_UNIT
               : GOAL_WATER_DEFAULT;
}

/* a goal from the phone -> stored value, clamped to what fits */
static inline uint16_t goal_steps_store(uint32_t steps)
{
    if (steps < GOAL_STEPS_MIN)
        steps = GOAL_STEPS_MIN;
    if (steps > GOAL_STEPS_MAX)
        steps = GOAL_STEPS_MAX;
    return (uint16_t)steps;
}

static inline uint8_t goal_water_store(uint32_t ml)
{
    uint32_t q = (ml + GOAL_WATER_UNIT / 2) / GOAL_WATER_UNIT;
    if (q < GOAL_WATER_MIN_Q)
        q = GOAL_WATER_MIN_Q;
    if (q > GOAL_WATER_MAX_Q)
        q = GOAL_WATER_MAX_Q;
    return (uint8_t)q;
}

/* goals reached today that have not been celebrated yet. Nothing is due
   while the clock is unknown or before the counters have rolled to
   today, so a stale day can never celebrate. */
static inline unsigned goal_due(const struct daily *dl, uint32_t today,
                                uint32_t steps, int step_goal,
                                int water_goal)
{
    if (!today || dl->day != today)
        return 0;
    unsigned due = 0;
    if (step_goal > 0 && steps >= (uint32_t)step_goal)
        due |= GOAL_STEPS;
    if (water_goal > 0 && dl->water_ml >= water_goal)
        due |= GOAL_WATER;
    return due & ~(unsigned)dl->goals_hit;
}

#endif
