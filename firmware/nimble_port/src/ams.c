/* Apple Media Service client: iOS publishes what any player app is
 * playing (Spotify, Apple Music, podcasts...) to a paired accessory, and
 * accepts play/pause/skip/volume commands back — the music twin of ANCS,
 * no companion app needed. Fills the same state as the InfiniTime music
 * service (ble_svcs.c).
 *
 * GATT client procedures are strictly one at a time: we only start when
 * the ANCS/CTS machine reports the client idle, and every step chains
 * from the previous step's callback. */
#include <string.h>
#include <stdlib.h>
#include "os/os.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gatt.h"
#include "../../phone.h"

extern uint32_t hal_millis(void);
int ancs_client_idle(void);
void phone_music_ams(int playing, const char *artist, const char *track,
                     const char *album, int elapsed_s, int dur_s);

/* 89D3502B-0F36-433A-8EF4-C502AD55F8DC */
static const ble_uuid128_t u_ams = BLE_UUID128_INIT(
    0xdc, 0xf8, 0x55, 0xad, 0x02, 0xc5, 0xf4, 0x8e,
    0x3a, 0x43, 0x36, 0x0f, 0x2b, 0x50, 0xd3, 0x89);
/* Remote Command 9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2 */
static const ble_uuid128_t u_rc = BLE_UUID128_INIT(
    0xc2, 0x51, 0xca, 0xf7, 0x56, 0x0e, 0xdf, 0xb8,
    0x8a, 0x4a, 0xb1, 0x57, 0xd8, 0x81, 0x3c, 0x9b);
/* Entity Update 2F7CABCE-808D-411F-9A0C-BB92BA96C102 */
static const ble_uuid128_t u_eu = BLE_UUID128_INIT(
    0x02, 0xc1, 0x96, 0xba, 0x92, 0xbb, 0x0c, 0x9a,
    0x1f, 0x41, 0x8d, 0x80, 0xce, 0xab, 0x7c, 0x2f);

enum { A_IDLE, A_SVC, A_CHR, A_DSC, A_SUB, A_REG_P, A_REG_T, A_LIVE, A_NONE };
static int st;
static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_start, s_end, h_rc, h_eu, h_eu_cccd;
static uint32_t start_at;
static int tries;

int ams_live(void) { return st == A_LIVE; }
int ams_state(void) { return st; }
int ams_busy(void) { return st > A_IDLE && st < A_LIVE; }

void ams_on_connect(uint16_t c) { conn = c; st = A_IDLE; start_at = 0; tries = 0; }
void ams_on_disconnect(void) { conn = BLE_HS_CONN_HANDLE_NONE; st = A_IDLE; start_at = 0; }
void ams_on_enc_ok(void)
{
    if (st == A_IDLE || st == A_LIVE)
        start_at = hal_millis() + 5000;  /* after CTS + ANCS kick off */
    if (st == A_LIVE)
        st = A_IDLE;                     /* re-encrypt: resubscribe */
}

static void fail(void)
{
    st = A_IDLE;
    start_at = ++tries < 4 ? hal_millis() + 10000 : 0;
}

static int reg_cb(uint16_t c, const struct ble_gatt_error *e,
                  struct ble_gatt_attr *a, void *arg)
{
    (void)c; (void)a; (void)arg;
    if (e->status) { fail(); return 0; }
    if (st == A_REG_P) {
        /* Track: Artist 0, Album 1, Title 2, Duration 3 */
        static const uint8_t t[] = {2, 0, 1, 2, 3};
        st = A_REG_T;
        if (ble_gattc_write_flat(conn, h_eu, t, sizeof t, reg_cb, NULL))
            fail();
    } else {
        st = A_LIVE;
        tries = 0;
    }
    return 0;
}

static int sub_cb(uint16_t c, const struct ble_gatt_error *e,
                  struct ble_gatt_attr *a, void *arg)
{
    (void)c; (void)a; (void)arg;
    if (e->status) { fail(); return 0; }
    static const uint8_t p[] = {0, 1};   /* Player: PlaybackInfo */
    st = A_REG_P;
    if (ble_gattc_write_flat(conn, h_eu, p, sizeof p, reg_cb, NULL))
        fail();
    return 0;
}

static int dsc_cb(uint16_t c, const struct ble_gatt_error *e, uint16_t chr,
                  const struct ble_gatt_dsc *d, void *arg)
{
    (void)c; (void)arg;
    if (e->status == BLE_HS_EDONE) {
        if (!h_eu_cccd) { fail(); return 0; }
        static const uint8_t on[2] = {1, 0};
        st = A_SUB;
        if (ble_gattc_write_flat(conn, h_eu_cccd, on, 2, sub_cb, NULL))
            fail();
        return 0;
    }
    if (e->status || !d)
        return 0;
    if (chr == h_eu && d->uuid.u.type == BLE_UUID_TYPE_16 &&
        d->uuid.u16.value == 0x2902)
        h_eu_cccd = d->handle;
    return 0;
}

static int chr_cb(uint16_t c, const struct ble_gatt_error *e,
                  const struct ble_gatt_chr *ch, void *arg)
{
    (void)c; (void)arg;
    if (e->status == BLE_HS_EDONE) {
        if (!h_eu || !h_rc) { fail(); return 0; }
        st = A_DSC;
        if (ble_gattc_disc_all_dscs(conn, s_start, s_end, dsc_cb, NULL))
            fail();
        return 0;
    }
    if (e->status || !ch)
        return 0;
    if (!ble_uuid_cmp(&ch->uuid.u, &u_rc.u)) h_rc = ch->val_handle;
    else if (!ble_uuid_cmp(&ch->uuid.u, &u_eu.u)) h_eu = ch->val_handle;
    return 0;
}

static int svc_cb(uint16_t c, const struct ble_gatt_error *e,
                  const struct ble_gatt_svc *s, void *arg)
{
    (void)c; (void)arg;
    if (e->status == BLE_HS_EDONE) {
        if (!s_start) {                  /* not an iPhone: stop asking */
            st = A_NONE;
            return 0;
        }
        st = A_CHR;
        if (ble_gattc_disc_all_chrs(conn, s_start, s_end, chr_cb, NULL))
            fail();
        return 0;
    }
    if (e->status) {
        if (e->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_ATTR_NOT_FOUND))
            st = A_NONE;
        else
            fail();
        return 0;
    }
    if (s) { s_start = s->start_handle; s_end = s->end_handle; }
    return 0;
}

void ams_tick(void)
{
    if (conn == BLE_HS_CONN_HANDLE_NONE || st != A_IDLE || !start_at)
        return;
    if ((int32_t)(hal_millis() - start_at) < 0 || !ancs_client_idle())
        return;
    start_at = 0;
    s_start = s_end = h_rc = h_eu = h_eu_cccd = 0;
    st = A_SVC;
    if (ble_gattc_disc_svc_by_uuid(conn, &u_ams.u, svc_cb, NULL))
        fail();
}

/* Entity Update: [entity, attribute, flags, utf-8 value...] */
int ams_on_notify(uint16_t h, struct os_mbuf *om)
{
    if (st != A_LIVE && st != A_REG_T && st != A_REG_P)
        return 0;
    if (h != h_eu)
        return 0;
    char b[72];
    int len = OS_MBUF_PKTLEN(om);
    if (len > (int)sizeof b - 1)
        len = sizeof b - 1;
    os_mbuf_copydata(om, 0, len, b);
    b[len] = 0;
    if (len < 3)
        return 1;
    const char *v = b + 3;
    int ent = b[0], att = b[1];
    if (ent == 0 && att == 1) {          /* "state,rate,elapsed" */
        int state = atoi(v);
        const char *e = strchr(v, ',');
        if (e) e = strchr(e + 1, ',');
        int el = e ? atoi(e + 1) : -1;
        phone_music_ams(state == 1 || state == 3, NULL, NULL, NULL, el, -1);
    } else if (ent == 2) {
        if (att == 0) phone_music_ams(-1, v, NULL, NULL, -1, -1);
        else if (att == 1) phone_music_ams(-1, NULL, NULL, v, -1, -1);
        else if (att == 2) phone_music_ams(-1, NULL, v, NULL, -1, -1);
        else if (att == 3) phone_music_ams(-1, NULL, NULL, NULL, -1, atoi(v));
    }
    return 1;
}

/* AMS RemoteCommandID: play 0, pause 1, toggle 2, next 3, prev 4,
   volume up 5, volume down 6 — same order as MC_* */
int ams_cmd(int c)
{
    if (st != A_LIVE || !h_rc || c < 0 || c > 6)
        return -1;
    uint8_t id = (uint8_t)c;
    return ble_gattc_write_flat(conn, h_rc, &id, 1, NULL, NULL) ? -1 : 0;
}
