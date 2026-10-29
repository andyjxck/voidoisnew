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
#include "gatt_owner.h"

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

/* notification store shown on the NOTIF screen (ancs_store.h) */
#include "ancs_store.h"
static struct an_store ns;

enum { ST_IDLE, ST_SVC, ST_CHR, ST_DSC, ST_SUB_NS, ST_SUB_DS, ST_LIVE };
static int state;
static uint16_t a_conn;
static uint16_t svc_start, svc_end;
static uint16_t h_ns_val, h_ns_cccd, h_ds_val, h_ds_cccd, h_cp_val;
static struct gatt_spans spans;     /* every char found in the service */
static uint32_t ancs_start_at;      /* retry discovery at (0 = no retry) */
static uint8_t ancs_tries;          /* discovery attempts this link */
static uint8_t link_enc;            /* this link is encrypted */
static int16_t ancs_last_err;       /* diag: last GATT error, any step */
static int cts_state;               /* 0 idle 1 disc 2 read 3 synced */
static uint32_t cts_ok_at;          /* when the clock last synced */
static uint32_t cts_retry_at;
static uint8_t seq_phase, seq_started;  /* see link sequencer below */

/* Data Source responses can fragment across notifications; accumulate */
static uint8_t ds_buf[400];
static int ds_len;

int ancs_ready(void)     { return state == ST_LIVE; }
int ancs_state(void)     { return state; }
int ancs_count(void)     { return an_count(&ns); }
int ancs_take_dirty(void){ int d = ns.dirty; ns.dirty = 0; return d; }
int ancs_take_new(void)  { int d = ns.buzz; ns.buzz = 0; return d; }
uint32_t ancs_take_popup(void) { uint32_t u = ns.popup; ns.popup = 0; return u; }

const struct ancs_notif_view *ancs_get(int i)
{
    static struct ancs_notif_view v;
    const struct an_slot *n = an_at(&ns, i);
    if (!n)
        return NULL;
    v.uid = n->uid;
    v.flags = n->flags;
    v.has_pos = n->has_pos;
    v.has_neg = n->has_neg;
    v.cat = n->cat;
    v.app = n->app[0] ? n->app : "iPhone";
    v.title = n->title;
    v.sub = n->sub;
    v.msg = n->msg;
    v.rx_ms = n->rx_ms;
    v.date = n->date;
    v.mod = n->mod;
    return &v;
}

/* ---- one GATT client procedure at a time -------------------------------
   NimBLE does not queue ATT requests: a second request sent while one
   is outstanding is a protocol violation, the phone answers only one,
   and the other procedure times out after 30 s, at which point NimBLE
   drops the link itself (DIAG "disc534"). CTS, AMS, ANCS and the
   Control Point all take this lock for the whole chain they run and
   give it back from their final callback. */
enum { GC_FREE, GC_CTS, GC_AMS, GC_ANCS, GC_CP, GC_AMSCMD };
static uint8_t gc_owner;
static uint32_t gc_since;
int gc_take(int who)
{
    if (gc_owner != GC_FREE || !a_conn)
        return 0;
    gc_owner = (uint8_t)who;
    gc_since = hal_millis();
    return 1;
}
void gc_give(int who)
{
    if (gc_owner == who)
        gc_owner = GC_FREE;
}
int gc_held(void) { return gc_owner; }

/* Control Point writes: queued, sent one at a time with a callback */
#define CPQ_N 4
static struct { uint8_t len; uint8_t b[20]; } cpq[CPQ_N];
static uint8_t cpq_n;
static void cp_pump(void);
static int cp_done(uint16_t conn, const struct ble_gatt_error *err,
                   struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (err->status)
        ancs_last_err = err->status;
    gc_give(GC_CP);
    return 0;
}
static void cp_write(const uint8_t *cmd, int len)
{
    if (!h_cp_val || len > 20 || cpq_n >= CPQ_N)
        return;
    cpq[cpq_n].len = (uint8_t)len;
    memcpy(cpq[cpq_n].b, cmd, len);
    cpq_n++;
    cp_pump();
}
static void cp_pump(void)
{
    if (!cpq_n || !h_cp_val || !gc_take(GC_CP))
        return;
    if (ble_gattc_write_flat(a_conn, h_cp_val, cpq[0].b, cpq[0].len,
                             cp_done, NULL))
        gc_give(GC_CP);
    memmove(cpq, cpq + 1, sizeof cpq[0] * (CPQ_N - 1));
    cpq_n--;
}

/* attribute fetches: one outstanding (the Data Source reply is
   reassembled into one buffer). Which notification to fetch next, and
   the timeout/retry, live in ancs_store.h. */
static void fetch_pump(void)
{
    if (state != ST_LIVE)
        return;
    uint32_t uid = an_next_fetch(&ns, hal_millis());
    if (!uid)
        return;
    uint8_t req[20];
    int n = an_request(req, uid);
    ds_len = 0;
    cp_write(req, n);
}

void ancs_dismiss(uint32_t uid)
{
    uint8_t req[6] = {0x02, uid & 0xFF, (uid >> 8) & 0xFF,
                      (uid >> 16) & 0xFF, (uid >> 24) & 0xFF, 0x01};
    cp_write(req, sizeof req);
    an_drop(&ns, uid);
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
    int r = an_on_ds(&ns, ds_buf, ds_len);
    if (r == 0)
        return;                      /* more fragments to come */
    ds_len = 0;
    if (r > 0)
        fetch_pump();                /* next one without waiting */
}

static void on_ns(const uint8_t *d, int len)
{
    if (len < 8)
        return;
    uint8_t ev = d[0], flags = d[1], cat = d[2];
    uint32_t uid = d[4] | (d[5] << 8) |
                   ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);
    if (ev > 2)
        return;                      /* reserved event ids */
    an_on_ns(&ns, ev, flags, cat, uid, hal_millis());
    fetch_pump();
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

/* ---- discovery state machine ---------------------------------------------
   svc -> chars -> descriptors -> subscribe DS -> subscribe NS -> live,
   one chain holding the GATT lock. DS goes first so the burst of
   existing notifications iOS sends right after the NS subscribe can
   already be fetched. */

static int sub_fail;                /* diag: last CCCD write error */
int ancs_sub_fail(void) { return sub_fail; }

static void disc_failed(int status)
{
    ancs_last_err = (int16_t)status;
    state = ST_IDLE;
    gc_give(GC_ANCS);
    /* iOS refuses the CCCD writes until notification access is allowed
       on the phone: keep retrying, quickly at first */
    ancs_tries++;
    /* 2.2.3: after ~20 min of refusals, every 5 min (each retry is a full
       GATT discovery; once a minute forever cost radio time all day) */
    ancs_start_at = hal_millis() + (ancs_tries < 8 ? 12000 :
                                    ancs_tries < 28 ? 60000 : 300000);
}

static int ns_sub_done(uint16_t conn, const struct ble_gatt_error *err,
                       struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (err->status != 0) {
        sub_fail = err->status;
        disc_failed(err->status);
        return 0;
    }
    state = ST_LIVE;
    ancs_tries = 0;
    ns.dirty = 1;
    gc_give(GC_ANCS);
    return 0;
}

static void subscribe_ns(void)
{
    static const uint8_t on[2] = {1, 0};
    state = ST_SUB_NS;
    if (ble_gattc_write_flat(a_conn, h_ns_cccd, on, 2, ns_sub_done, NULL))
        disc_failed(-4);
}

static int ds_sub_done(uint16_t conn, const struct ble_gatt_error *err,
                       struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (err->status != 0) {
        sub_fail = err->status;
        disc_failed(err->status);
        return 0;
    }
    subscribe_ns();
    return 0;
}

static void subscribe_ds(void)
{
    static const uint8_t on[2] = {1, 0};
    state = ST_SUB_DS;
    if (ble_gattc_write_flat(a_conn, h_ds_cccd, on, 2, ds_sub_done, NULL))
        disc_failed(-4);
}

/* NimBLE passes the start handle we gave (the service start) as chr for
   every descriptor, never the characteristic's own value handle; see
   gatt_owner.h for how the owner is worked out instead. */
static int dsc_cb(uint16_t conn, const struct ble_gatt_error *error,
                  uint16_t chr, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)conn; (void)arg; (void)chr;
    if (error->status == BLE_HS_EDONE) {
        if (h_ns_cccd && h_ds_cccd)
            subscribe_ds();
        else
            disc_failed(-2);                /* a CCCD is missing */
        return 0;
    }
    if (error->status != 0) {
        disc_failed(error->status);
        return 0;
    }
    if (!dsc || ble_uuid_u16(&dsc->uuid.u) != 0x2902)
        return 0;
    uint16_t owner = gatt_owner_of(&spans, dsc->handle);
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
        if (h_ns_val && h_cp_val && h_ds_val) {
            state = ST_DSC;
            int rc = ble_gattc_disc_all_dscs(a_conn, svc_start, svc_end,
                                             dsc_cb, NULL);
            if (rc) disc_failed(rc);
        } else {
            disc_failed(-3);                /* NS, CP or DS missing */
        }
        return 0;
    }
    if (error->status != 0) {
        disc_failed(error->status);
        return 0;
    }
    if (!chr)
        return 0;
    gatt_spans_add(&spans, chr->def_handle, chr->val_handle);
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
        if (svc_start) {
            state = ST_CHR;
            if (ble_gattc_disc_all_chrs(a_conn, svc_start, svc_end,
                                        chr_cb, NULL))
                disc_failed(-5);
        } else {
            disc_failed(-1);                /* no ANCS on this link yet */
        }
        return 0;
    }
    if (error->status != 0) {
        disc_failed(error->status);
        return 0;
    }
    if (svc) {
        svc_start = svc->start_handle;
        svc_end = svc->end_handle;
    }
    return 0;
}

static void ancs_begin(void)
{
    if (!gc_take(GC_ANCS))
        return;
    ancs_start_at = 0;
    svc_start = svc_end = 0;
    h_ns_val = h_ns_cccd = h_ds_val = h_ds_cccd = h_cp_val = 0;
    gatt_spans_reset(&spans);
    state = ST_SVC;
    if (ble_gattc_disc_svc_by_uuid(a_conn, &uuid_ancs.u, svc_cb, NULL))
        disc_failed(-6);
}

/* ---- Current Time Service client ----------------------------------------
   iOS exposes the phone's clock as a CTS *server*; it never writes to
   the watch. So the watch reads 0x2A2B once the link is encrypted. */
static const ble_uuid16_t uuid_cts = BLE_UUID16_INIT(0x1805);
static const ble_uuid16_t uuid_ct = BLE_UUID16_INIT(0x2A2B);
static uint16_t cts_start_h, cts_end_h, cts_val_h;
int ancs_cts_state(void) { return cts_state; }
int phone_apply_cts(const uint8_t *t, int len);

static void cts_end(int ok)
{
    cts_state = ok ? 3 : 0;
    if (ok)
        cts_ok_at = hal_millis();
    else
        cts_retry_at = hal_millis() + 10000;
    gc_give(GC_CTS);
}

static int cts_read_cb(uint16_t conn, const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)arg;
    uint8_t t[10];
    if (error->status != 0 || !attr || !attr->om ||
        OS_MBUF_PKTLEN(attr->om) < 7 ||
        os_mbuf_copydata(attr->om, 0, 7, t) != 0) {
        cts_end(0);
        return 0;
    }
    /* shared validation + weekday computed from the date (ble_svcs.c) */
    cts_end(phone_apply_cts(t, 7) == 0);
    return 0;
}

static int cts_chr_cb(uint16_t conn, const struct ble_gatt_error *error,
                      const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn; (void)arg;
    if (error->status == BLE_HS_EDONE) {
        cts_state = 2;
        if (!cts_val_h ||
            ble_gattc_read(a_conn, cts_val_h, cts_read_cb, NULL))
            cts_end(0);
        return 0;
    }
    if (error->status != 0) {
        cts_end(0);
        return 0;
    }
    if (chr && !ble_uuid_cmp(&chr->uuid.u, &uuid_ct.u))
        cts_val_h = chr->val_handle;
    return 0;
}

static int cts_svc_cb(uint16_t conn, const struct ble_gatt_error *error,
                      const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn; (void)arg;
    if (error->status == BLE_HS_EDONE) {
        if (!cts_start_h ||
            ble_gattc_disc_all_chrs(a_conn, cts_start_h, cts_end_h,
                                    cts_chr_cb, NULL))
            cts_end(0);
        return 0;
    }
    if (error->status != 0) {
        cts_end(0);
        return 0;
    }
    if (svc) {
        cts_start_h = svc->start_handle;
        cts_end_h = svc->end_handle;
    }
    return 0;
}

static void cts_begin(void)
{
    if (!gc_take(GC_CTS))
        return;
    cts_start_h = cts_end_h = cts_val_h = 0;
    cts_state = 1;
    if (ble_gattc_disc_svc_by_uuid(a_conn, &uuid_cts.u, cts_svc_cb, NULL))
        cts_end(0);
}

/* ---- link sequencer ------------------------------------------------------
   Once the link is encrypted: the phone's time first, then AMS (music),
   then ANCS (notifications). Each phase runs to its end, holding the
   GATT lock, before the next one starts. */
int ams_busy(void);
int ams_start(void);
void ams_abort(void);

void ancs_on_connect(uint16_t conn)
{
    a_conn = conn;
    state = ST_IDLE;
    link_enc = 0;
    seq_phase = seq_started = 0;
    cts_state = 0;
    ancs_tries = 0;
    ancs_start_at = 0;
    gc_owner = GC_FREE;
    cpq_n = 0;
}

void ancs_on_disconnect(void)
{
    a_conn = 0;
    state = ST_IDLE;
    link_enc = 0;
    seq_phase = seq_started = 0;
    cts_state = 0;
    ancs_start_at = 0;
    gc_owner = GC_FREE;
    cpq_n = 0;
    an_clear(&ns);       /* iOS sends the current set again on resubscribe */
    ds_len = 0;
}

/* ENC_CHANGE ok: start (or, after a re-encryption, restart) the chain */
void ancs_on_enc_ok(void)
{
    link_enc = 1;
    if (seq_phase == 0) {
        seq_phase = 1;
        seq_started = 0;
    }
}

int ancs_link_enc(void) { return link_enc; }
int ancs_seq_phase(void) { return seq_phase; }

/* no ANCS-side client procedure in flight (AMS asks before retrying) */
int ancs_client_idle(void)
{
    return gc_owner == GC_FREE && seq_phase >= 4;
}

void ancs_tick(void)
{
    uint32_t now = hal_millis();
    if (!a_conn)
        return;
    /* safety net: NimBLE times a procedure out at 30 s and calls back,
       so the lock should never be held this long */
    if (gc_owner != GC_FREE && now - gc_since > 40000) {
        if (gc_owner == GC_ANCS)
            disc_failed(-7);
        else if (gc_owner == GC_CTS)
            cts_end(0);
        else if (gc_owner == GC_AMS)
            ams_abort();
        gc_owner = GC_FREE;
    }
    if (!link_enc)
        return;
    switch (seq_phase) {
    case 1:                              /* time */
        if (!seq_started) {
            if (gc_owner == GC_FREE) {
                seq_started = 1;
                if (cts_state != 3)
                    cts_begin();
            }
        } else if (gc_owner == GC_FREE) {
            seq_phase = 2;
            seq_started = 0;
        }
        return;
    case 2:                              /* music */
        if (!seq_started) {
            if (gc_owner == GC_FREE) {
                seq_started = 1;
                ams_start();
            }
        } else if (gc_owner == GC_FREE && !ams_busy()) {
            seq_phase = 3;
            seq_started = 0;
        }
        return;
    case 3:                              /* notifications */
        if (!seq_started) {
            if (gc_owner == GC_FREE) {
                seq_started = 1;
                if (state != ST_LIVE)
                    ancs_begin();
            }
        } else if (gc_owner == GC_FREE) {
            seq_phase = 4;
        }
        return;
    default:
        break;
    }
    /* steady state: retries and queued writes, still one at a time */
    if (state == ST_IDLE && ancs_start_at &&
        (int32_t)(now - ancs_start_at) >= 0 && gc_owner == GC_FREE)
        ancs_begin();
    /* re-read the phone clock every 6 h on a long-lived link (drift) */
    if (cts_state == 3 && now - cts_ok_at > 21600000u)
        cts_state = 0;
    if (cts_state == 0 && (int32_t)(now - cts_retry_at) >= 0 &&
        gc_owner == GC_FREE)
        cts_begin();
    fetch_pump();
    cp_pump();
}
