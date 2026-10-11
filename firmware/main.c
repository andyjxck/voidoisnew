/* VoidOS on PineTime, LVGL build. */
#include <stdint.h>
#include "lvgl.h"
#include "voidos/hal.h"
#include "board.h"

void lvgl_port_init(void);
void ui_init(void);
void hal_frame_begin(void);
void ui_tick(void);
uint32_t button_held_ms(void);
int ble_init(void);
void ble_poll(void);
extern int nimble_last_stage(void);
extern int nimble_last_fault(void);
extern void nimble_stage(int);
extern void nimble_fault(int);

int boot_last_stage, boot_last_fault;
extern void hal_boot_confirm(void);

int main(void)
{
    wdt_kick();
    boot_last_stage = nimble_last_stage();  /* furthest stage of last run */
    boot_last_fault = nimble_last_fault();
    nimble_stage(0);                        /* 0 = reached app entry */
    nimble_fault(0);                        /* clear stale fault record */
    hal_init();
    lvgl_port_init();
    ui_init();
    ble_init();
    nimble_stage(99);                       /* 99 = init survived */

    for (;;) {
        hal_frame_begin();    /* one time instant for the whole frame */
        ui_tick();
        extern int ui_asleep(void);
        if (!ui_asleep())
            lv_timer_handler();   /* render before ble_poll: if the BLE
                                     path hangs, the face + banner stay */
        else
            hal_delay_ms(20);     /* dark: nothing to draw, idle slower */
        wdt_kick();
        ble_poll();
        /* 3 s of survived main-loop = sane boot: permanently confirm the
           DFU'd image or the next reset reverts to the old firmware */
        static int img_confirmed;
        if (!img_confirmed && hal_millis() >= 3000) {
            hal_boot_confirm();
            img_confirmed = 1;
        }
        if (button_held_ms() >= 3000)
            SCB_AIRCR = 0x05FA0004;      /* reset -> revert to InfiniTime */
        hal_delay_ms(5);
    }
}
