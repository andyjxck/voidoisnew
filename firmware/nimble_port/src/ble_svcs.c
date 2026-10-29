/* InfiniTime-compatible phone services, so the companion apps that feed
 * InfiniTime (InfiniLink on iPhone, Gadgetbridge on Android) feed VoidOS:
 *
 *  - Current Time Service (0x1805) server: the app WRITES the local wall
 *    time to 0x2A2B on connect, exactly like InfiniTime's
 *    CurrentTimeService. (The ANCS module separately READS the iPhone's
 *    own CTS — this covers phones/apps that push instead.)
 *  - SimpleWeatherService 00050000-78fc-48fe-8e23-433b3a1942d0, data char
 *    00050001: current weather (type 0) and forecast (type 1), v0/v1.
 *  - MusicService 00000000-78fc-48fe-8e23-433b3a1942d0: the app writes
 *    status/track/artist/album/position/length; the watch notifies
 *    button presses on the event char 00000001.
 *
 * On top, an Apple Media Service client (ams.c) can fill the same music
 * state straight from iOS for any player app; music_cmd() routes the
 * buttons to whichever source last reported. */
#include <string.h>
#include "os/os.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gatt.h"
#include "../../phone.h"

extern uint32_t hal_millis(void);
void hal_clock_set(int y, int mo, int d, int dow, int h, int mi, int s);
int ble_connected(void);
uint16_t ble_conn_handle(void);
int ams_cmd(int c);                     /* ams.c; 0 when sent */
int ams_live(void);

static int dirty;
int phone_take_dirty(void) { int d = dirty & 1; dirty &= ~1; return d; }
int phone_take_face_dirty(void) { int d = dirty & 2; dirty &= ~2; return d; }

/* ---- day of week from the date (InfiniLink sends a locale weekday) --- */
int phone_dow(int y, int m, int d)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    int w = (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
    return w == 0 ? 7 : w;              /* 1 = Monday .. 7 = Sunday */
}

/* ---- Current Time Service (server) ---------------------------------- */
static uint8_t cts_now[10];
static uint8_t cts_lti[2];
static int cts_writes;
int phone_cts_writes(void) { return cts_writes; }

/* shared by the CTS server and client: validate + apply 7+ bytes */
int phone_apply_cts(const uint8_t *t, int len)
{
    if (len < 7)
        return -1;
    int y = t[0] | (t[1] << 8);
    int mo = t[2], d = t[3], h = t[4], mi = t[5], s = t[6];
    if (y < 2020 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
        h > 23 || mi > 59 || s > 59)
        return -1;
    hal_clock_set(y, mo, d, phone_dow(y, mo, d), h, mi, s);
    dirty = 3;
    return 0;
}

static int cts_access(uint16_t conn, uint16_t attr,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr;
    int lti = arg != NULL;
    uint8_t *buf = lti ? cts_lti : cts_now;
    int cap = lti ? (int)sizeof cts_lti : (int)sizeof cts_now;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (!lti) {          /* 2.2.2: a read gets the live clock */
            extern int hal_clock_hour(void), hal_clock_minute(void),
                       hal_clock_second(void);
            extern void hal_clock_date(int *, int *, int *, int *);
            int y, mo, d, dow;
            hal_clock_date(&y, &mo, &d, &dow);
            buf[0] = (uint8_t)y; buf[1] = (uint8_t)(y >> 8);
            buf[2] = (uint8_t)mo; buf[3] = (uint8_t)d;
            buf[4] = (uint8_t)hal_clock_hour();
            buf[5] = (uint8_t)hal_clock_minute();
            buf[6] = (uint8_t)hal_clock_second();
            buf[7] = (uint8_t)dow; buf[8] = 0; buf[9] = 0;
        }
        return os_mbuf_append(ctxt->om, buf, cap) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;
    int len = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t tmp[16];
    if (len > (int)sizeof tmp)
        len = sizeof tmp;   /* InfiniLink pads the fraction field */
    os_mbuf_copydata(ctxt->om, 0, len, tmp);
    memcpy(buf, tmp, len < cap ? len : cap);
    if (!lti) {
        if (phone_apply_cts(tmp, len))
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        cts_writes++;
    }
    return 0;
}


/* ---- music ----------------------------------------------------------- */
static struct music ms;
static uint16_t ms_event_h;

const struct music *music_get(void) { return &ms; }
int music_active(void) { return ms.playing && ble_connected(); }

void phone_music_reset(void)            /* link dropped */
{
    if (ms.playing)
        dirty = 3;
    ms.playing = 0;
}

static void put_str(char *dst, int cap, const uint8_t *s, int len)
{
    if (len > cap - 1) {
        len = cap - 1;
        /* keep UTF-8 whole: back off continuation bytes */
        while (len > 0 && (s[len] & 0xC0) == 0x80)
            len--;
    }
    memcpy(dst, s, len);
    dst[len] = 0;
}

static uint32_t rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | (p[2] << 8) | p[3];
}

/* char ids: the x of InfiniTime's CharUuid(x, 0) */
int phone_music_write(int id, const uint8_t *b, int len)
{
    if (len < 1)
        return 0;
    switch (id) {
    case 0x02: {                          /* status */
        int p = b[0] != 0;
        if (!p && ms.playing && ms.len_s)  /* freeze the position */
            ms.pos_s += (hal_millis() - ms.pos_ms) / 1000;
        ms.pos_ms = hal_millis();
        ms.playing = p;
        break;
    }
    case 0x03: put_str(ms.artist, sizeof ms.artist, b, len); break;
    case 0x04: put_str(ms.track, sizeof ms.track, b, len); break;
    case 0x05: put_str(ms.album, sizeof ms.album, b, len); break;
    case 0x06: if (len >= 4) { ms.pos_s = rd32be(b); ms.pos_ms = hal_millis(); } break;
    case 0x07: if (len >= 4) ms.len_s = rd32be(b); break;
    default: return 0;                    /* number/total/speed/repeat/shuffle */
    }
    ms.src = MS_SRC_INFINI;
    dirty = 3;
    return 0;
}

/* AMS feeds the same state */
void phone_music_ams(int playing, const char *artist, const char *track,
                     const char *album, int elapsed_s, int dur_s)
{
    if (playing >= 0) {
        ms.playing = playing;
        if (elapsed_s >= 0) {
            ms.pos_s = elapsed_s;
            ms.pos_ms = hal_millis();
        }
    }
    if (artist) put_str(ms.artist, sizeof ms.artist, (const uint8_t *)artist, strlen(artist));
    if (track)  put_str(ms.track, sizeof ms.track, (const uint8_t *)track, strlen(track));
    if (album)  put_str(ms.album, sizeof ms.album, (const uint8_t *)album, strlen(album));
    if (dur_s >= 0) ms.len_s = dur_s;
    ms.src = MS_SRC_AMS;
    dirty = 3;
}

static int ms_access(uint16_t conn, uint16_t attr,
                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr;
    int id = (int)(intptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (id == 0x02)
            return os_mbuf_append(ctxt->om, &ms.playing, 1) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        return 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return 0;
    uint8_t b[64];
    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > (int)sizeof b)
        len = sizeof b;
    os_mbuf_copydata(ctxt->om, 0, len, b);
    return phone_music_write(id, b, len);
}

static int ms_event_sent;
int phone_music_events(void) { return ms_event_sent; }

void music_cmd(int c)
{
    if (ms.src == MS_SRC_AMS && ams_live()) {
        if (ams_cmd(c) == 0) {
            ms_event_sent++;
            return;
        }
    }
    /* InfiniTime event codes: play 0, pause 1, next 3, prev 4, vol 5/6 */
    uint8_t ev;
    switch (c) {
    case MC_PLAY:   ev = 0x00; break;
    case MC_PAUSE:  ev = 0x01; break;
    case MC_TOGGLE: ev = ms.playing ? 0x01 : 0x00; break;
    case MC_NEXT:   ev = 0x03; break;
    case MC_PREV:   ev = 0x04; break;
    case MC_VOLUP:  ev = 0x05; break;
    case MC_VOLDN:  ev = 0x06; break;
    default: return;
    }
    if (!ble_connected() || !ms_event_h)
        return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&ev, 1);
    if (om && ble_gatts_notify_custom(ble_conn_handle(), ms_event_h, om) == 0)
        ms_event_sent++;
    /* optimistic: show the new state at once, the app confirms */
    if (ev == 0x00 || ev == 0x01) {
        phone_music_write(0x02, (const uint8_t *)(ev == 0x00 ? "\1" : "\0"), 1);
    }
}

int music_vol_ok(void) { return 1; }

/* ---- InfiniTime-shaped info services (2.2.2) -------------------------
   What InfiniLink reads after it connects, laid out as InfiniTime has
   them: Device Information (0x180A), Battery (0x180F), Heart Rate
   (0x180D) and the InfiniTime motion service's step count. */
#if __has_include("gen/version.h")
#include "gen/version.h"
#endif
#ifndef VOIDOS_VERSION
#define VOIDOS_VERSION "dev"
#endif
static const char *const dis_str[] = {
    "PINE64", "PineTime", "0", VOIDOS_VERSION, "1.0.0", "VoidOS"
};
static int dis_access(uint16_t conn, uint16_t attr,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr;
    const char *v = dis_str[(int)(intptr_t)arg];
    return os_mbuf_append(ctxt->om, v, strlen(v)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

extern int hal_battery_pct(void);
extern int sensors_bpm(void);
extern int sensors_bpm_stale(void);
extern int ui_steps_today(void);
static uint16_t bas_h, hrs_h, steps_h;
static int info_value(int which, uint8_t *b)
{
    if (which == 0) {                         /* battery % */
        int p = hal_battery_pct();
        b[0] = (uint8_t)(p < 0 ? 0 : p > 100 ? 100 : p);
        return 1;
    }
    if (which == 1) {                         /* HR measurement: flags, bpm */
        int v = sensors_bpm();
        b[0] = 0;
        b[1] = (uint8_t)(v > 0 && !sensors_bpm_stale() && v < 256 ? v : 0);
        return 2;
    }
    int st = ui_steps_today();                /* steps today, le32 */
    uint32_t u = st > 0 ? (uint32_t)st : 0;
    b[0] = (uint8_t)u; b[1] = (uint8_t)(u >> 8);
    b[2] = (uint8_t)(u >> 16); b[3] = (uint8_t)(u >> 24);
    return 4;
}
static int info_access(uint16_t conn, uint16_t attr,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr;
    uint8_t b[4];
    int n = info_value((int)(intptr_t)arg, b);
    return os_mbuf_append(ctxt->om, b, n) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

/* push battery / heart rate / steps to a subscribed app when they
   change, checked every 5 s from the BLE poll */
void phone_svcs_tick(uint32_t now)
{
    static uint32_t last;
    static uint8_t prev[3][4];
    if (!ble_connected() || now - last < 5000)
        return;
    last = now;
    const uint16_t h[3] = {bas_h, hrs_h, steps_h};
    for (int i = 0; i < 3; i++) {
        uint8_t b[4] = {0};
        info_value(i, b);
        if (memcmp(b, prev[i], 4)) {
            memcpy(prev[i], b, 4);
            if (h[i])
                ble_gatts_chr_updated(h[i]);
        }
    }
}

/* ---- GATT tables ----------------------------------------------------- */
#define IT_UUID(b13, b12, b14) BLE_UUID128_INIT(0xd0, 0x42, 0x19, 0x3a, 0x3b, 0x43, \
    0x23, 0x8e, 0xfe, 0x48, 0xfc, 0x78, b12, b13, b14, 0x00)

static const ble_uuid128_t u_ms = IT_UUID(0x00, 0x00, 0x00);
static const ble_uuid128_t u_ms_c[12] = {
    IT_UUID(0x00, 0x01, 0), IT_UUID(0x00, 0x02, 0), IT_UUID(0x00, 0x03, 0),
    IT_UUID(0x00, 0x04, 0), IT_UUID(0x00, 0x05, 0), IT_UUID(0x00, 0x06, 0),
    IT_UUID(0x00, 0x07, 0), IT_UUID(0x00, 0x08, 0), IT_UUID(0x00, 0x09, 0),
    IT_UUID(0x00, 0x0a, 0), IT_UUID(0x00, 0x0b, 0), IT_UUID(0x00, 0x0c, 0),
};
#define RW (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_READ)
#define MSC(i) { .uuid = &u_ms_c[(i) - 1].u, .access_cb = ms_access, \
                 .arg = (void *)(intptr_t)(i), .flags = RW }

#define DIS(uuid16, i) { .uuid = BLE_UUID16_DECLARE(uuid16), \
    .access_cb = dis_access, .arg = (void *)(i), .flags = BLE_GATT_CHR_F_READ }
static const ble_uuid128_t u_motion = IT_UUID(0x00, 0x00, 0x03);
static const ble_uuid128_t u_steps = IT_UUID(0x00, 0x01, 0x03);

static const struct ble_gatt_svc_def phone_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]){
            DIS(0x2A29, 0), DIS(0x2A24, 1), DIS(0x2A25, 2),
            DIS(0x2A26, 3), DIS(0x2A27, 4), DIS(0x2A28, 5),
            { 0 } },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = info_access,
              .arg = (void *)0, .val_handle = &bas_h,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { 0 } },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180D),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = BLE_UUID16_DECLARE(0x2A37), .access_cb = info_access,
              .arg = (void *)1, .val_handle = &hrs_h,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { 0 } },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &u_motion.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &u_steps.u, .access_cb = info_access,
              .arg = (void *)2, .val_handle = &steps_h,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { 0 } },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1805),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = BLE_UUID16_DECLARE(0x2A2B), .access_cb = cts_access,
              .flags = RW },
            { .uuid = BLE_UUID16_DECLARE(0x2A0F), .access_cb = cts_access,
              .arg = (void *)1, .flags = RW },
            { 0 } },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &u_ms.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &u_ms_c[0].u, .access_cb = ms_access,
              .arg = (void *)1, .val_handle = &ms_event_h,
              .flags = BLE_GATT_CHR_F_NOTIFY },
            MSC(2), MSC(3), MSC(4), MSC(5), MSC(6), MSC(7),
            MSC(8), MSC(9), MSC(10), MSC(11), MSC(12),
            { 0 } },
    },
    { 0 },
};

int phone_svcs_init(void)
{
    int rc = ble_gatts_count_cfg(phone_svcs);
    if (rc == 0)
        rc = ble_gatts_add_svcs(phone_svcs);
    return rc;
}
