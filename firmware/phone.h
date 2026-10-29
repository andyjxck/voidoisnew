/* Phone-fed data: weather + music, shared by the BLE services and the UI.
 * Wire formats follow InfiniTime so InfiniLink / Gadgetbridge can feed
 * VoidOS unchanged; music can also come from iOS AMS directly. */
#ifndef VOIDOS_PHONE_H
#define VOIDOS_PHONE_H
#include <stdint.h>

/* InfiniTime SimpleWeatherService icon ids */
enum { WX_SUN, WX_FEWCLOUDS, WX_CLOUDS, WX_HEAVYCLOUDS, WX_SHOWERS,
       WX_RAIN, WX_THUNDER, WX_SNOW, WX_MIST, WX_N };

#define WX_MAX_DAYS 5
struct wx_day { int16_t tmin, tmax; uint8_t icon; };
struct wx {
    int16_t temp, tmin, tmax;          /* centi-degrees C, as sent */
    uint8_t icon;
    char city[33];
    uint32_t rx_ms;                    /* hal_millis() at arrival */
    uint8_t ndays;                     /* forecast days, 0 = none */
    struct wx_day day[WX_MAX_DAYS];
};
/* NULL when there is no weather or it is over 24 h old */
const struct wx *wx_get(void);
const char *wx_name(int icon);

enum { MS_SRC_NONE, MS_SRC_INFINI, MS_SRC_AMS };
struct music {
    uint8_t playing;
    uint8_t src;                       /* who last told us what plays */
    char artist[32], track[40], album[32];
    uint32_t pos_s, len_s;             /* position at pos_ms, length */
    uint32_t pos_ms;
};
const struct music *music_get(void);
/* music controls show only while this is true: playing + phone linked */
int music_active(void);
enum { MC_PLAY, MC_PAUSE, MC_TOGGLE, MC_NEXT, MC_PREV, MC_VOLUP, MC_VOLDN };
void music_cmd(int c);
int music_vol_ok(void);                /* source accepts volume steps */
/* set whenever weather or music changed; the UI consumes it */
int phone_take_dirty(void);

#endif
