/* Phone-fed data: music, shared by the BLE services and the UI.
 * Wire formats follow InfiniTime so InfiniLink / Gadgetbridge can feed
 * VoidOS unchanged; music can also come from iOS AMS directly. */
#ifndef VOIDOS_PHONE_H
#define VOIDOS_PHONE_H
#include <stdint.h>

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
/* set whenever music changed; the UI consumes it */
int phone_take_dirty(void);

#endif
