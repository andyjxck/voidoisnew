#ifndef VOIDOS_HAL_H
#define VOIDOS_HAL_H

#include <stdint.h>

#define VOIDOS_WIDTH 240
#define VOIDOS_HEIGHT 240

typedef uint16_t hal_color_t;

#define HAL_RGB565(r, g, b) \
    ((hal_color_t)((((uint16_t)(r) & 0xF8) << 8) | \
                   (((uint16_t)(g) & 0xFC) << 3) | \
                   (((uint16_t)(b) & 0xF8) >> 3)))

#define HAL_COLOR_BLACK   HAL_RGB565(0, 0, 0)
#define HAL_COLOR_WHITE   HAL_RGB565(255, 255, 255)
#define HAL_COLOR_GRAY    HAL_RGB565(90, 90, 90)
#define HAL_COLOR_MUTED   HAL_RGB565(150, 150, 150)
#define HAL_COLOR_DARK    HAL_RGB565(20, 24, 28)
#define HAL_COLOR_ACCENT  HAL_RGB565(0, 200, 220)
#define HAL_COLOR_WARN    HAL_RGB565(255, 120, 40)
#define HAL_COLOR_GREEN   HAL_RGB565(60, 220, 120)
#define HAL_COLOR_RED     HAL_RGB565(230, 60, 60)

typedef enum {
    HAL_EV_NONE = 0,
    HAL_EV_QUIT,
    HAL_EV_TOUCH_DOWN,
    HAL_EV_TOUCH_MOVE,
    HAL_EV_TOUCH_UP,
    HAL_EV_BUTTON
} hal_event_type_t;

typedef struct {
    hal_event_type_t type;
    int x;
    int y;
} hal_event_t;

int hal_init(void);
void hal_shutdown(void);
int hal_poll_event(hal_event_t *ev);
void hal_draw_rect(int x, int y, int w, int h, hal_color_t color);
void hal_draw_rect_a(int x, int y, int w, int h, hal_color_t color,
                     unsigned alpha);
void hal_present(void);
void hal_delay_ms(unsigned ms);   /* sleeps in WFE (2.2.3) */
int hal_on_charger(void);          /* 1 on USB power (P0.19 low) */
uint32_t hal_idle_sleeps(void);    /* WFE sleeps taken, for the diag screen */
uint32_t hal_millis(void);
int hal_clock_hour(void);
int hal_clock_minute(void);
int hal_clock_second(void);
int hal_clock_millis(void);   /* ms within the current wall-clock second */
void hal_face_text_size(const char *text, int px, int *w, int *h);
void hal_face_draw_text(int x, int y, const char *text,
                        hal_color_t color, int px);
void hal_draw_logo(int x, int y, int w, int h, unsigned alpha,
                   int shimmer_col);

/* material-symbol icon set (assets/fonts, 24 px sources) */
typedef enum {
    HAL_ICON_DND = 0,       /* do_not_disturb_on   */
    HAL_ICON_AIRPLANE,      /* flight              */
    HAL_ICON_BT,            /* bluetooth           */
    HAL_ICON_TORCH,         /* flashlight_on       */
    HAL_ICON_PHONE,         /* mobile_vibrate      */
    HAL_ICON_WATER,         /* water_drop          */
    HAL_ICON_SETTINGS,      /* settings            */
    HAL_ICON_SUN,           /* light_mode          */
    HAL_ICON_BATTERY,       /* battery_full        */
    HAL_ICON_DND_OFF,       /* do_not_disturb_off  */
    HAL_ICON_AIRPLANE_OFF,  /* airplanemode_inactive */
    HAL_ICON_BT_OFF,        /* bluetooth_disabled  */
    HAL_ICON_TORCH_OFF,     /* flashlight_off      */
    HAL_ICON_CLOUD,         /* cloud               */
    HAL_ICON_COUNT
} hal_icon_t;

void hal_draw_icon(hal_icon_t icon, int x, int y, int size, unsigned alpha);

/* device partial-update hints; no-ops in the simulator */
void hal_mark_dirty(int x, int y, int w, int h);
void hal_mark_all_dirty(void);
void hal_mark_screen(void);   /* screen changed: rebuild the static layer */
int hal_render_static(void);  /* 1 while caching static content (sim: always 1) */

#endif
