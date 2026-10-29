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
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return os_mbuf_append(ctxt->om, buf, cap) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
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

/* ---- weather --------------------------------------------------------- */
static struct wx wx_cur;
static int wx_have;

const struct wx *wx_get(void)
{
    if (!wx_have)
        return NULL;
    if (hal_millis() - wx_cur.rx_ms > 24u * 3600u * 1000u)
        return NULL;
    return &wx_cur;
}

const char *wx_name(int icon)
{
    static const char *n[WX_N] = {"CLEAR", "FEW CLOUDS", "CLOUDY",
                                  "OVERCAST", "SHOWERS", "RAIN",
                                  "THUNDER", "SNOW", "MIST"};
    return icon >= 0 && icon < WX_N ? n[icon] : "--";
}

static int16_t rd16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }

int phone_weather_write(const uint8_t *b, int len)
{
    if (len < 2 || b[1] > 1)
        return -1;                       /* unknown version */
    if (b[0] == 0) {                     /* current weather */
        if (len < 49)
            return -1;
        wx_cur.temp = rd16(b + 10);
        wx_cur.tmin = rd16(b + 12);
        wx_cur.tmax = rd16(b + 14);
        memcpy(wx_cur.city, b + 16, 32);
        wx_cur.city[32] = 0;
        wx_cur.icon = b[48] < WX_N ? b[48] : WX_CLOUDS;
        wx_cur.rx_ms = hal_millis();
        wx_have = 1;
        dirty = 3;
        return 0;
    }
    if (b[0] == 1) {                     /* forecast */
        if (len < 11)
            return -1;
        int n = b[10];
        if (n > WX_MAX_DAYS)
            n = WX_MAX_DAYS;
        if (len < 11 + n * 5)
            n = (len - 11) / 5;
        for (int i = 0; i < n; i++) {
            const uint8_t *p = b + 11 + i * 5;
            wx_cur.day[i].tmin = rd16(p);
            wx_cur.day[i].tmax = rd16(p + 2);
            wx_cur.day[i].icon = p[4] < WX_N ? p[4] : WX_CLOUDS;
        }
        wx_cur.ndays = n;
        dirty = 3;
        return 0;
    }
    return -1;
}

static int wx_access(uint16_t conn, uint16_t attr,
                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;
    uint8_t b[64];
    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > (int)sizeof b)
        len = sizeof b;
    os_mbuf_copydata(ctxt->om, 0, len, b);
    phone_weather_write(b, len);         /* bad packets are just ignored */
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

/* ---- GATT tables ----------------------------------------------------- */
#define IT_UUID(b13, b12, b14) BLE_UUID128_INIT(0xd0, 0x42, 0x19, 0x3a, 0x3b, 0x43, \
    0x23, 0x8e, 0xfe, 0x48, 0xfc, 0x78, b12, b13, b14, 0x00)

static const ble_uuid128_t u_wx = IT_UUID(0x00, 0x00, 0x05);
static const ble_uuid128_t u_wx_data = IT_UUID(0x00, 0x01, 0x05);
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

static const struct ble_gatt_svc_def phone_svcs[] = {
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
        .uuid = &u_wx.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &u_wx_data.u, .access_cb = wx_access,
              .flags = BLE_GATT_CHR_F_WRITE },
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
