/* Glue between NimBLE's nRF52 driver and our board: NVIC vector writes,
   HFXO clock arbitration, minimal hal_gpio. */
#include <stdint.h>
#include "nrf52.h"
#include "mcu/cmsis_nvic.h"
#include "mcu/nrf52_clock.h"
#include "hal/hal_gpio.h"
#include "board.h"

/* ---- HFXO arbitration ----------------------------------------------------- */

static int hfxo_refs;
static int hfxo_started;

int nrf52_clock_hfxo_request(void)
{
    if (hfxo_refs++ == 0 && !hfxo_started) {
        NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
        NRF_CLOCK->TASKS_HFCLKSTART = 1;
        for (unsigned spin = 0;
             !NRF_CLOCK->EVENTS_HFCLKSTARTED && spin < 1000000; spin++)
            ;
        hfxo_started = NRF_CLOCK->EVENTS_HFCLKSTARTED != 0;
    }
    return hfxo_started ? 0 : -1;
}

int nrf52_clock_hfxo_release(void)
{
    if (hfxo_refs > 0)
        hfxo_refs--;
    /* keep the crystal running once up; stopping it breaks radio timing
       anyway and the PHY requests it constantly */
    return 0;
}

/* ---- hal_gpio -------------------------------------------------------------- */

int hal_gpio_init_out(int pin, int val)
{
    if (val)
        GPIO_OUTSET = BIT(pin);
    else
        GPIO_OUTCLR = BIT(pin);
    GPIO_DIRSET = BIT(pin);
    return 0;
}

int hal_gpio_write(int pin, int val)
{
    if (val)
        GPIO_OUTSET = BIT(pin);
    else
        GPIO_OUTCLR = BIT(pin);
    return 0;
}

int hal_gpio_read(int pin)
{
    return (GPIO_IN & BIT(pin)) != 0;
}

/* ---- boot bisect marker -------------------------------------------------- */

extern void wdt_kick(void);
extern void hal_delay_ms(unsigned ms);

/* Crash breadcrumbs in POWER->GPREGRET2. The register retains only its low
   8 bits across resets, so: values 0..99 = last stage, 0xF0|pattern =
   fault (pattern: 1 hardfault, 2 stray IRQ, 3 SPI, 4 I2C). */
#define NIMBLE_GPREGRET2 (*(volatile uint32_t *)0x40000520)
#define NIMBLE_STAGE_ALIVE 99   /* written once the app is up and running */

void nimble_stage(int s)
{
    NIMBLE_GPREGRET2 = (uint32_t)(s & 0xFF);
    wdt_kick();
}

void nimble_fault(int f)
{
    NIMBLE_GPREGRET2 = 0xF0u | (uint32_t)(f & 0xF);
}

int nimble_last_stage(void)
{
    uint32_t v = NIMBLE_GPREGRET2 & 0xFF;
    return v < 0xF0 ? (int)v : -1;
}

int nimble_last_fault(void)
{
    uint32_t v = NIMBLE_GPREGRET2 & 0xFF;
    return v >= 0xF0 ? (int)(v & 0xF) : 0;
}

/* Records boot progress for the diag paths. The old version also
   pulsed the backlight `stage` times as a bring-up signal — that is
   the flashing the user sees at every boot, so it is gone for good.
   Keep the stage record: it powers boot_last_stage diagnostics. */
void nimble_mark(int stage)
{
    nimble_stage(stage);
}
