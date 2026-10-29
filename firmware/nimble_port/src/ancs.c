/* VoidOS ANCS client: pulls real notifications from the iPhone over
   the Apple Notification Center Service. The phone stays the GAP
   central; we act as a GATT client on the same link, exactly like
   every other watch that speaks ANCS.

   Flow: on connect, discover the ANCS service + its three
   characteristics + their CCCDs, subscribe to Notification Source and
   Data Source, then fetch attributes for every incoming notification.
   The subscribe usually fails once with insufficient authentication —
   iOS then pairs us, and we retry after ENC setup. */
#include <string.h>
#include <stdint.h>
#include "os/os.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "host/ble_uuid.h"

uint32_t hal_millis(void);

/* ANCS UUIDs, wire order (little-endian, reversed from canonical) */
static const ble_uuid128_t uuid_ancs = BLE_UUID128_INIT(
    0xd0, 0x00, 0x2d, 0x12, 0x1e, 0x4b, 0x0f, 0xa4,
    0x99, 0x4e, 0xce, 0xb5, 0x31, 0xf4, 0x05, 0x79);
static const ble_uuid128_t uuid_ns = BLE_UUID128_INIT(
    0xbd, 0x1d, 0xa2, 0x99, 0xe6, 0x25, 0x58, 0x8c,
    0xd9, 0x42, 0x01, 0x63, 0x0d, 0x12, 0xbf, 0x9f);
static const ble_uuid128_t uuid_cp = BLE_UUID128_INIT(
    0xd9, 0xd9, 0xaa, 0xfd, 0xbd, 0x9b, 0x21, 0x98,
    0xa8, 0x49, 0xe1, 0x45, 0xf3, 0xd8, 0xd1, 0x69);
static const ble_uuid128_t uuid_ds = BLE_UUID128_INIT(
    0xfb, 0x7b, 0x7c, 0xce, 0x6a, 0xb3, 0x44, 0xbe,
    0xb5, 0x4b, 0xd6, 0x24, 0xe9, 0xc6, 0xea, 0x22);

/* notification store shown on the NOTIF screen */
#define ANCS_MAX_NOTIFS 4
struct ancs_notif {
    uint32_t uid;
    uint8_t flags;          /* ANCS event flags (silent/important/…) */
    uint8_t has_pos, has_neg;
    uint32_t rx_ms;
    char app[20];
    char title[48];
    char msg[96];
};
static struct ancs_notif notifs[ANCS_MAX_NOTIFS];
static int notif_count;
static int new_notif;       /* set on arrival, UI consumes for the buzz */
static int notif_dirty;     /* set whenever the list changed */

enum { ST_IDLE, ST_SVC, ST_CHR, ST_DSC, ST_SUB_NS, ST_SUB_DS, ST_LIVE };
static int state;
static uint16_t a_conn;
static uint16_t svc_start, svc_end;
static uint16_t h_ns_val, h_ns_cccd, h_ds_val, h_ds_cccd, h_cp_val;
static uint32_t disc_retry_at;
static uint32_t ancs_start_at;      /* begin discovery once CTS/AMS allow */
static uint8_t ancs_tries;          /* discovery attempts this link */
static uint8_t link_enc;            /* this link is encrypted */
static int16_t ancs_last_err;       /* diag: last GATT error, any step */
static int cts_state;               /* 0 idle 1 disc 2 read 3 synced */
static uint32_t cts_pending_at;
static int sub_inflight;            /* a CCCD write awaits its callback */
static uint32_t cts_ok_at;          /* when the clock last synced */

/* Data Source responses can fragment across notifications; accumulate */
static uint8_t ds_buf[320];
static int ds_len;
static uint32_t pending_uid;

int ancs_ready(void)     { return state == ST_LIVE; }
int ancs_state(void)     { return state; }
int ancs_count(void)     { return notif_count; }
int ancs_take_dirty(void){ int d = notif_dirty; notif_dirty = 0; return d; }
int ancs_take_new(void)  { int d = new_notif; new_notif = 0; return d; }

struct ancs_notif_view {
    uint32_t uid; uint8_t flags; uint8_t has_pos, has_neg;
    const char *app, *title, *msg; uint32_t rx_ms;
};
static struct ancs_notif_view ancs_view;
const struct ancs_notif_view *ancs_get(int i)
{
    if (i < 0 || i >= notif_count)
        return NULL;
    struct ancs_notif *n = &notifs[i];
    ancs_view.uid = n->uid;
    ancs_view.flags = n->flags;
    ancs_view.has_pos = n->has_pos;
    ancs_view.has_neg = n->has_neg;
    ancs_view.app = n->app;
    ancs_view.title = n->title;
    ancs_view.msg = n->msg;
    ancs_view.rx_ms = n->rx_ms;
    return &ancs_view;
}

static struct ancs_notif *slot_for(uint32_t uid)
{
    for (int i = 0; i < notif_count; i++)
        if (notifs[i].uid == uid)
            return &notifs[i];
    if (notif_count < ANCS_MAX_NOTIFS)
        return &notifs[notif_count++];
    /* drop the oldest */
    memmove(notifs, notifs + 1, sizeof notifs[0] * (ANCS_MAX_NOTIFS - 1));
    return &notifs[ANCS_MAX_NOTIFS - 1];
}

static void slot_drop(uint32_t uid)
{
    for (int i = 0; i < notif_count; i++) {
        if (notifs[i].uid == uid) {
            memmove(&notifs[i], &notifs[i + 1],
                    sizeof notifs[0] * (notif_count - i - 1));
            notif_count--;
            notif_dirty = 1;
            return;
        }
    }
}

static void cp_write(const uint8_t *cmd, int len)
{
    if (h_cp_val)
        ble_gattc_write_flat(a_conn, h_cp_val, cmd, len, NULL, NULL);
}

/* ask the phone for a notification's attributes */
static void fetch_attrs(uint32_t uid)
{
    uint8_t req[5 + 1 + 3 + 1 + 3 + 1 + 1 + 1 + 1];
    int n = 0;
    req[n++] = 0x00;                 /* GetNotificationAttributes */
    req[n++] = uid & 0xFF;
    req[n++] = (uid >> 8) & 0xFF;
    req[n++] = (uid >> 16) & 0xFF;
    req[n++] = (uid >> 24) & 0xFF;
    req[n++] = 0x00;                 /* AppIdentifier, full len */
    req[n++] = 0x01;                 /* Title */
    req[n++] = 48; req[n++] = 0x00;
    req[n++] = 0x02;                 /* Subtitle */
    req[n++] = 0x03;                 /* Message */
    req[n++] = 95; req[n++] = 0x00;
    req[n++] = 0x05;                 /* Date */
    req[n++] = 0x06;                 /* PositiveActionLabel */
    req[n++] = 0x07;                 /* NegativeActionLabel */
    pending_uid = uid;
    ds_len = 0;
    cp_write(req, n);
}

void ancs_dismiss(uint32_t uid)
{
    uint8_t req[6] = {0x02, uid & 0xFF, (uid >> 8) & 0xFF,
                      (uid >> 16) & 0xFF, (uid >> 24) & 0xFF, 0x01};
    cp_write(req, sizeof req);
    slot_drop(uid);
}

void ancs_positive(uint32_t uid)
{
    uint8_t req[6] = {0x02, uid & 0xFF, (uid >> 8) & 0xFF,
                      (uid >> 16) & 0xFF, (uid >> 24) & 0xFF, 0x00};
    cp_write(req, sizeof req);
}

/* ---- incoming data ------------------------------------------------------- */

static void on_ds(const uint8_t *d, int len)
{
    if (ds_len + len > (int)sizeof ds_buf) {
        ds_len = 0;
        return;                      /* runaway; drop and resync */
    }
    memcpy(ds_buf + ds_len, d, len);
    ds_len += len;
    if (ds_len < 5 || ds_buf[0] != 0x00)
        return;
    uint32_t uid = ds_buf[1] | (ds_buf[2] << 8) |
                   ((uint32_t)ds_buf[3] << 16) | ((uint32_t)ds_buf[4] << 24);
    struct ancs_notif *n = slot_for(uid);
    int p = 5, complete = 1;
    while (p < ds_len) {
        if (p + 3 > ds_len) { complete = 0; break; }
        int attr = ds_buf[p];
        int alen = ds_buf[p + 1] | (ds_buf[p + 2] << 8);
        if (p + 3 + alen > ds_len) { complete = 0; break; }
        const uint8_t *v = ds_buf + p + 3;
        int cl = alen;
        switch (attr) {
        case 0: {                    /* AppIdentifier -> friendly name */
            const char *s = (const char *)v;
            const char *dot = strrchr(s, '.');
            s = dot ? dot + 1 : s;
            int i = 0;
            for (; s[i] && i < (int)sizeof n->app - 1; i++)
                n->app[i] = (s[i] >= 'a' && s[i] <= 'z')
                            ? s[i] - 32 : s[i];
            n->app[i] = 0;
            break;
        }
        case 1:
            if (cl >= (int)sizeof n->title) cl = sizeof n->title - 1;
            memcpy(n->title, v, cl); n->title[cl] = 0;
            break;
        case 3:
            if (cl >= (int)sizeof n->msg) cl = sizeof n->msg - 1;
            memcpy(n->msg, v, cl); n->msg[cl] = 0;
            break;
        case 6: n->has_pos = cl > 0; break;
        case 7: n->has_neg = cl > 0; break;
        }
        p += 3 + alen;
    }
    if (complete) {
        ds_len = 0;
        notif_dirty = 1;
    }
}

static void on_ns(const uint8_t *d, int len)
{
    if (len < 8)
        return;
    uint8_t ev = d[0], flags = d[1];
    uint32_t uid = d[4] | (d[5] << 8) |
                   ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);
    if (ev == 2) {                   /* removed on the phone */
        slot_drop(uid);
        return;
    }
    if (ev > 1)
        return;                      /* reserved event ids */
    struct ancs_notif *n = slot_for(uid);
    if (n->rx_ms == 0) {             /* genuinely new */
        n->flags = flags;
        n->rx_ms = hal_millis();
        n->app[0] = n->title[0] = n->msg[0] = 0;
        n->has_pos = n->has_neg = 0;
        new_notif = 1;
    }
    fetch_attrs(uid);
}

void ancs_on_notify(uint16_t attr_handle, struct os_mbuf *om)
{
    /* DS PDUs run to MTU-3 (~250 B) — never clip them to a small stack
       buffer or the attribute framing falls apart */
    uint8_t d[256];
    int len = OS_MBUF_PKTLEN(om);
    if (len > (int)sizeof d)
        len = sizeof d;
    os_mbuf_copydata(om, 0, len, d);
    if (attr_handle == h_ns_val)
        on_ns(d, len);
    else if (attr_handle == h_ds_val)
        on_ds(d, len);
}

/* ---- discovery state machine --------------------------------------------- */

static int sub_fail;                /* diag: last CCCD write error */
static int sub_done(uint16_t conn, const struct ble_gatt_error *err,
                    struct ble_gatt_attr *attr, void *arg);

static int ds_sub_done(uint16_t conn, const struct ble_gatt_error *err,
                       struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    sub_inflight = 0;
    if (err->status == 0) {
        state = ST_LIVE;
        notif_dirty = 1;
    } else {
        sub_fail = err->status;
        state = ST_SUB_NS;           /* retried from ancs_tick / enc_ok */
        disc_retry_at = hal_millis() + 1500;
    }
    return 0;
}

static void subscribe_ns(void)
{
    uint8_t on[2] = {1, 0};
    state = ST_SUB_NS;
    sub_inflight = ble_gattc_write_flat(a_conn, h_ns_cccd, on, 2,
                                        sub_done, NULL) == 0;
}

static int sub_done(uint16_t conn, const struct ble_gatt_error *err,
                    struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    sub_inflight = 0;
    if (err->status != 0) {
        /* 0x05/0x0F = not yet paired/encrypted: iOS pairs us now and
           ENC_CHANGE re-arms the subscribe; anything else -> retry */
        sub_fail = err->status;
        disc_retry_at = hal_millis() + 1500;
        return 0;
    }
    state = ST_SUB_DS;
    uint8_t on[2] = {1, 0};
    if (h_ds_cccd)
        sub_inflight = ble_gattc_write_flat(a_conn, h_ds_cccd, on, 2,
                                            ds_sub_done, NULL) == 0;
    else
        state = ST_LIVE;
    return 0;
}
int ancs_sub_fail(void) { return sub_fail; }

static void disc_failed(int status);

/* NimBLE's "discover all descriptors" over a service range reports every
   attribute after the start handle, and its chr_val_handle argument is
   just the start handle we passed (the service declaration) for every
   one of them. 2.2.0 compared that against the NS/DS value handles,
   which can never match, so neither CCCD was ever found and ANCS fell
   back to idle without subscribing. A CCCD belongs to the closest
   characteristic value handle below it. */
static int dsc_cb(uint16_t conn, const struct ble_gatt_error *error,
                  uint16_t chr, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)conn; (void)arg; (void)chr;
    if (error->status == BLE_HS_EDONE) {
        if (h_ns_cccd) {
            state = ST_SUB_NS;
            if (h_ds_cccd)
                subscribe_ds();             /* DS first, then NS */
            else
                subscribe_ns();
        } else {
            disc_failed(-2);                /* no NS CCCD in the service */
        }
        return 0;
    }
    if (error->status != 0) {
        disc_failed(error->status);
        return 0;
    }
    if (!dsc || ble_uuid_u16(&dsc->uuid.u) != 0x2902)
        return 0;
    uint16_t owner = gatt_owner_of(dsc->handle, h_ns_val, h_ds_val, h_cp_val);
    if (owner && owner == h_ns_val)
        h_ns_cccd = dsc->handle;
    else if (owner && owner == h_ds_val)
        h_ds_cccd = dsc->handle;
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn; (void)arg;
    if (error->status == BLE_HS_EDONE) {
        if (h_ns_val && h_cp_val) {
            state = ST_DSC;
            int rc = ble_gattc_disc_all_dscs(a_conn, svc_start, svc_end,
                                             dsc_cb, NULL);
            if (rc) disc_failed(rc);
        } else {
            disc_failed(-3);                /* NS or CP missing */
        }
        return 0;
    }
    if (error->status != 0) {
        disc_failed(error->status);
        return 0;
    }
    if (!chr)
        return 0;
    if (!ble_uuid_cmp(&chr->uuid.u, &uuid_ns.u))
        h_ns_val = chr->val_handle;
    else if (!ble_uuid_cmp(&chr->uuid.u, &uuid_cp.u))
        h_cp_val = chr->val_handle;
    else if (!ble_uuid_cmp(&chr->uuid.u, &uuid_ds.u))
        h_ds_val = chr->val_handle;
    return 0;
}

static int svc_cb(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn; (void)arg;
    if (error->status == BLE_HS_EDONE) {
        /* data call already saved the bounds; launch char discovery */
        if (svc_start) {
            state = ST_CHR;
            ble_gattc_disc_all_chrs(a_conn, svc_start, svc_end,
                                    chr_cb, NULL);
        } else {
            state = ST_IDLE;
        }
        return 0;
    }
    if (error->status != 0 || !svc)
        return 0;
    svc_start = svc->start_handle;
    svc_end = svc->end_handle;
    return 0;
}

static void ancs_begin(void)
{
    svc_start = 0;
    h_ns_val = h_ns_cccd = h_ds_val = h_ds_cccd = h_cp_val = 0;
    state = ST_SVC;
    ble_gattc_disc_svc_by_uuid(a_conn, &uuid_ancs.u, svc_cb, NULL);
    disc_retry_at = hal_millis() + 12000;
}

/* discovery only starts once the link is encrypted (ancs_on_enc_ok):
   iOS refuses ANCS and CTS access on an unencrypted link */
void ancs_on_connect(uint16_t conn)
{
    a_conn = conn;
    state = ST_IDLE;
    sub_inflight = 0;
    cts_state = 0;
    /* ask for the time on every connect, whatever ANCS does. On an
       unencrypted link iOS refuses the read; the 10 s retry and
       ancs_on_enc_ok then try again once pairing is done */
    cts_pending_at = hal_millis() + 2500;
}

void ancs_on_disconnect(void)
{
    a_conn = 0;
    state = ST_IDLE;
    sub_inflight = 0;
    cts_state = 0;
    cts_pending_at = 0;
    notif_count = 0;
    ds_len = 0;
    notif_dirty = 1;
}

/* the CCCD write gets bounced until iOS finishes pairing — retry */
void ancs_on_enc_ok(void)
{
    if (state == ST_IDLE)
        ancs_begin();
    else if (state == ST_SUB_NS && h_ns_cccd)
        subscribe_ns();
    if (cts_state != 3)
        cts_pending_at = hal_millis() + 1500;
}


/* ---- Current Time Service client ----------------------------------------
   iOS exposes the phone's clock as a CTS *server*; it never writes to
   the watch. So the watch reads 0x2A2B once the link is encrypted. */
static const ble_uuid16_t uuid_cts = BLE_UUID16_INIT(0x1805);
static const ble_uuid16_t uuid_ct = BLE_UUID16_INIT(0x2A2B);
static uint16_t cts_start_h, cts_end_h, cts_val_h;
int ancs_cts_state(void) { return cts_state; }
void hal_clock_set(int y, int mo, int d, int dow, int h, int mi, int s);
int phone_apply_cts(const uint8_t *t, int len);
int ams_busy(void);

/* no ANCS procedure in flight: idle, live, or parked between the
   12 s resubscribe retries while iOS withholds notification access
   (2.0.1 only synced the clock from IDLE/LIVE, so a phone that never
   granted ANCS never set the time -> the face kept its placeholder
   date, "FRI, OCT 24") */
static int client_quiet(void)
{
    if (ams_busy() || sub_inflight)
        return 0;
    if (state == ST_LIVE || state == ST_IDLE)
        return 1;
    /* parked between resubscribe attempts: nothing on the wire */
    return state == ST_SUB_NS;
}
int ancs_client_idle(void)
{
    return client_quiet() && cts_state != 1 && cts_state != 2 &&
           !cts_pending_at;
}

static int cts_read_cb(uint16_t conn, const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)arg;
    if (error->status != 0 || !attr || !attr->om) {
        cts_state = 0;               /* retried after the next ENC change */
        return 0;
    }
    uint8_t t[10];
    if (OS_MBUF_PKTLEN(attr->om) < 7 ||
        os_mbuf_copydata(attr->om, 0, 7, t) != 0) {
        cts_state = 0;
        return 0;
    }
    /* shared validation + weekday computed from the date (ble_svcs.c) */
    cts_state = phone_apply_cts(t, 7) == 0 ? 3 : 0;
    if (cts_state == 3)
        cts_ok_at = hal_millis();
    return 0;
}

static int cts_chr_cb(uint16_t conn, const struct ble_gatt_error *error,
                      const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn; (void)arg;
    if (error->status == BLE_HS_EDONE) {
        if (cts_val_h) {
            cts_state = 2;
            ble_gattc_read(a_conn, cts_val_h, cts_read_cb, NULL);
        } else {
            cts_state = 0;
        }
        return 0;
    }
    if (error->status == 0 && chr &&
        !ble_uuid_cmp(&chr->uuid.u, &uuid_ct.u))
        cts_val_h = chr->val_handle;
    return 0;
}

static int cts_svc_cb(uint16_t conn, const struct ble_gatt_error *error,
                      const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn; (void)arg;
    if (error->status == BLE_HS_EDONE) {
        if (cts_start_h) {
            ble_gattc_disc_all_chrs(a_conn, cts_start_h, cts_end_h,
                                    cts_chr_cb, NULL);
        } else {
            cts_state = 0;
        }
        return 0;
    }
    if (error->status == 0 && svc) {
        cts_start_h = svc->start_handle;
        cts_end_h = svc->end_handle;
    }
    return 0;
}

static void cts_begin(void)
{
    cts_start_h = cts_end_h = cts_val_h = 0;
    cts_state = 1;
    if (ble_gattc_disc_svc_by_uuid(a_conn, &uuid_cts.u, cts_svc_cb, NULL))
        cts_state = 0;
}

/* watchdog: if discovery stalls mid-connection, restart it once */
void ancs_tick(void)
{
    /* CTS goes after the ANCS handshake settles — one GATT client
       procedure at a time on the single link */
    if (cts_pending_at && hal_millis() >= cts_pending_at &&
        client_quiet()) {
        cts_pending_at = 0;
        if (cts_state == 0)
            cts_begin();
    }
    if (state > ST_IDLE && state < ST_LIVE && disc_retry_at &&
        hal_millis() > disc_retry_at) {
        if (ams_busy() || cts_state == 1 || cts_state == 2 ||
            sub_inflight) {
            disc_retry_at = hal_millis() + 2000;   /* let it finish */
            return;
        }
        /* discovery stalled with the clock still unset: the link is
           free, so give the time read its turn before starting over */
        if (state != ST_SUB_NS && cts_state == 0) {
            disc_retry_at = hal_millis() + 4000;
            cts_begin();
            return;
        }
        disc_retry_at = hal_millis() + 12000;
        if (state == ST_SUB_NS && h_ns_cccd)
            subscribe_ns();          /* resubscribe after an auth bounce */
        else
            ancs_begin();            /* discovery stalled: start over */
    }
    /* re-read the phone clock every 6 h on a long-lived link (drift) */
    if (cts_state == 3 && a_conn && hal_millis() - cts_ok_at > 21600000u)
        cts_state = 0;
    /* CTS: retry a failed read every 10 s while connected + idle */
    if (cts_state == 0 && !cts_pending_at && a_conn && client_quiet()) {
        static uint32_t cts_last_try;
        if (hal_millis() - cts_last_try > 10000) {
            cts_last_try = hal_millis();
            cts_pending_at = hal_millis() + 200;
        }
    }
}
