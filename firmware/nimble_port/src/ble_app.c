/* VoidOS BLE: NimBLE host + controller on the bare-metal port.
   One cooperative pump (ble_poll) drains both event queues; the main
   loop calls it. Currently: GAP peripheral named "VoidOS", re-advertising
   on disconnect. DFU/data services layer on top of this. */
#include <string.h>
#include "os/os.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "nimble/nimble_port.h"
#include "controller/ble_ll.h"
#include "controller/ble_phy.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "host/ble_store.h"
#include "host/util/util.h"
/* 2.2.5: the connection record, to put a resolved identity on it */
#include "vendor/nimble/nimble/host/src/ble_hs_priv.h"
#include "tinycrypt/aes.h"
#include "tinycrypt/constants.h"
#include "rpa_match.h"
#if __has_include("gen/version.h")
#include "gen/version.h"
#endif
#ifndef VOIDOS_VERSION
#define VOIDOS_VERSION "dev"
#endif

extern void nrf52_clock_hfxo_request(void);
extern void npl_callout_tick(void);
extern void nimble_mark(int stage);

static int ble_up;
static int ble_adv_on;
static int ble_want = 1;        /* desired radio state, valid before init */
static int ble_synced;
static int d_disc_reason = -1, d_enc_status = -1, d_sec_rc = -99, d_conns;
static uint32_t adv_check_ms;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
/* diagnostics visible on the stats screen while BLE is being brought up */
static int d_syncs, d_adv_calls, d_adv_rc = -1, d_gap_events;
int d_subscribed, d_mtu_events;   /* exposed to the DFU diag char */
static int d_ll_evs, d_hs_evs;
int d_ll_cmds, d_hs_evt_tx;   /* patched into ble_ll_hci.c */
int d_st1, d_st2, d_st2rc = -9, d_startgo, d_resetrc = -9; /* ble_hs startup */
/* pairing request is deferred ~1.5 s after connect: firing
   ble_gap_security_initiate() inside the CONNECT event hits phones
   before their link is ready (the request is silently dropped on
   several stacks, leaving ANCS/CTS unreachable forever) */
static uint32_t sec_pending_at;
static int sec_attempts;

static void ble_advertise(void);
int ble_connected(void);

void ancs_on_connect(uint16_t conn);
void ancs_on_disconnect(void);
void ancs_on_enc_ok(void);
void ancs_on_notify(uint16_t attr_handle, struct os_mbuf *om);
void ancs_tick(void);
void ams_on_connect(uint16_t conn);
void ams_on_disconnect(void);
void ams_on_enc_ok(void);
int ams_on_notify(uint16_t attr_handle, struct os_mbuf *om);
void ams_tick(void);
int phone_svcs_init(void);
void phone_music_reset(void);
static int ble_compat;          /* advertise as "InfiniTime" for InfiniLink */
uint16_t ble_conn_handle(void) { return conn_handle; }
void ble_bond_store_init(uint16_t sig);
void ble_bond_store_poll(uint32_t now, int busy);
int ble_bond_count(void);
static int d_repeat_pairs;      /* diag: re-pairs accepted */
static int d_rpa_res = -1;      /* diag: -1 not an RPA, 0 no IRK matched,
                                   1 resolved to a bonded phone */

/* ---- 2.2.5: resolve the phone's private address ---------------------------
   See rpa_match.h. On connect, a resolvable private address is checked
   against the IRK of every bond; on a match the connection is given that
   bond's identity address (as an LL-privacy controller would have
   reported it), so the key lookup, the saved subscriptions and repeat-
   pairing all find the bond the phone still holds. */
static int aes_be(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    struct tc_aes_key_sched_struct ks;
    if (tc_aes128_set_encrypt_key(&ks, key) != TC_CRYPTO_SUCCESS)
        return -1;
    return tc_aes_encrypt(out, in, &ks) == TC_CRYPTO_SUCCESS ? 0 : -1;
}

struct rpa_find {
    const uint8_t *rpa;
    ble_addr_t id;
    int found;
};

static int rpa_find_cb(int obj_type, union ble_store_value *val, void *arg)
{
    struct rpa_find *f = arg;
    (void)obj_type;
    if (val->sec.irk_present && rpa_matches(val->sec.irk, f->rpa, aes_be)) {
        f->id = val->sec.peer_addr;
        f->found = 1;
        return 1;                       /* stop iterating */
    }
    return 0;
}

static void peer_resolve(uint16_t h)
{
    struct ble_gap_conn_desc cd;
    d_rpa_res = -1;
    if (ble_gap_conn_find(h, &cd) != 0 ||
        cd.peer_id_addr.type != BLE_ADDR_RANDOM ||
        !rpa_is_resolvable(cd.peer_id_addr.val))
        return;
    struct rpa_find f = {cd.peer_id_addr.val, {0}, 0};
    d_rpa_res = 0;
    ble_store_iterate(BLE_STORE_OBJ_TYPE_PEER_SEC, rpa_find_cb, &f);
    if (!f.found)
        return;
    ble_hs_lock();
    struct ble_hs_conn *c = ble_hs_conn_find(h);
    if (c && c->bhc_peer_addr.type == BLE_ADDR_RANDOM) {
        c->bhc_peer_rpa_addr = c->bhc_peer_addr;
        c->bhc_peer_addr.type = f.id.type == BLE_ADDR_PUBLIC
                                ? BLE_ADDR_PUBLIC_ID : BLE_ADDR_RANDOM_ID;
        memcpy(c->bhc_peer_addr.val, f.id.val, 6);
        d_rpa_res = 1;
    }
    ble_hs_unlock();
}
extern uint32_t hal_millis(void);

/* 2.2.3 battery: advertising used NimBLE's default 30-60 ms interval
   forever (and 100-150 ms for the compat advert while connected), about
   20 radio bursts a second whenever the phone was away. Now: Apple's
   recommended pattern, as InfiniTime does it — 20-29 ms for the first
   30 s after boot, a disconnect or a mode change, then 1022.5 ms. */
#define ADV_FAST_MS 30000u
static uint32_t adv_fast_until;
static void adv_fast_window(void) { adv_fast_until = hal_millis() + ADV_FAST_MS; }

/* 2.2.3 battery: iOS opens the link at 30 ms with no peripheral latency
   (33 radio wake-ups a second, all day). Ten seconds after connecting
   (discovery and the ANCS burst done) we ask for 45-60 ms with latency 3:
   the watch can skip up to three idle events, so it wakes ~4-6x a second
   and never waits more than 240 ms. All values meet Apple's accessory
   rules (min multiple of 15 ms, max >= min + 15, max x (lat+1) <= 2 s,
   timeout > 3 x that). During a DFU it asks for 15-30 ms, no latency.
   Asked once per connection per mode; a refusal is never re-asked. */
static uint32_t cp_at;
static int cp_state;            /* 0 none, 1 slow asked, 2 fast asked */
static int d_cp_rc = -1, d_cp_status = -1, d_cp_itvl, d_cp_lat;
int ble_diag_conn_itvl(void) { return d_cp_itvl; }
int ble_diag_conn_lat(void) { return d_cp_lat; }
static void conn_params_poll(uint32_t now)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE)
        return;
    extern int ble_dfu_busy(void);
    int fast = ble_dfu_busy();
    struct ble_gap_upd_params up;
    memset(&up, 0, sizeof up);
    if (fast && cp_state != 2) {
        up.itvl_min = 12;               /* 15 ms */
        up.itvl_max = 24;               /* 30 ms */
        up.latency = 0;
        up.supervision_timeout = 400;   /* 4 s */
        d_cp_rc = ble_gap_update_params(conn_handle, &up);
        cp_state = 2;
    } else if (!fast && cp_state != 1 && (int32_t)(now - cp_at) >= 0) {
        up.itvl_min = 36;               /* 45 ms */
        up.itvl_max = 48;               /* 60 ms */
        up.latency = 3;
        up.supervision_timeout = 400;   /* 4 s > 3 x 240 ms */
        d_cp_rc = ble_gap_update_params(conn_handle, &up);
        if (d_cp_rc == 0 || d_cp_rc == BLE_HS_EALREADY)
            cp_state = 1;
        else
            cp_at = now + 30000;        /* busy: try again later */
    }
}

static int gap_cb(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    d_gap_events++;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            conn_handle = ev->connect.conn_handle;
            /* before anything asks for a key: name the bonded phone */
            peer_resolve(conn_handle);
            /* ANCS discovery kicks off as soon as the link settles */
            ancs_on_connect(conn_handle);
            ams_on_connect(conn_handle);
            /* ask the phone to pair/encrypt: ANCS + CTS need it, and
               iOS only prompts when the accessory requests security.
               Deferred a beat — see sec_pending_at. */
            d_conns++;
            d_sec_rc = -1;
            sec_attempts = 0;
            sec_pending_at = hal_millis() + 1500;
            cp_state = 0;
            cp_at = hal_millis() + 10000;
            {
                struct ble_gap_conn_desc cd;
                if (ble_gap_conn_find(conn_handle, &cd) == 0) {
                    d_cp_itvl = cd.conn_itvl;
                    d_cp_lat = cd.conn_latency;
                }
            }
            if (ble_compat)
                ble_advertise();   /* keep InfiniLink able to see us */
        } else {
            adv_fast_window();
            ble_advertise();
        }
        break;
    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc cd;
        d_cp_status = ev->conn_update.status;
        if (ble_gap_conn_find(ev->conn_update.conn_handle, &cd) == 0) {
            d_cp_itvl = cd.conn_itvl;
            d_cp_lat = cd.conn_latency;
        }
        break;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        /* the fast window ran out: carry on at the slow interval */
        if (ev->adv_complete.reason == BLE_HS_ETIMEOUT &&
            (!ble_connected() || ble_compat))
            ble_advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        d_disc_reason = ev->disconnect.reason;
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        sec_pending_at = 0;
        ancs_on_disconnect();
        ams_on_disconnect();
        phone_music_reset();
        cp_state = 0;
        adv_fast_window();
        ble_advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        d_subscribed++;
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        /* incoming GATT notifications on client handles (ANCS NS/DS) */
        if (!ams_on_notify(ev->notify_rx.attr_handle, ev->notify_rx.om))
            ancs_on_notify(ev->notify_rx.attr_handle, ev->notify_rx.om);
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* The iPhone wants to pair again while we still hold a key for
           it (it was forgotten on the phone, or its key no longer
           matches). NimBLE's default answer is to ignore the request,
           which left the link unencrypted: drop the old bond and let
           the new pairing go ahead. */
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0)
            ble_store_util_delete_peer(&d.peer_id_addr);
        d_repeat_pairs++;
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_ENC_CHANGE:
        d_enc_status = ev->enc_change.status;
        if (ev->enc_change.status == 0) {
            ancs_on_enc_ok();
            ams_on_enc_ok();
        }
        break;
    case BLE_GAP_EVENT_MTU:
        d_mtu_events++;
        break;
    default:
        break;
    }
    return 0;
}

static void ble_advertise(void)
{
    if (!ble_adv_on)
        return;
    /* a non-connectable compat advert can still be up when the link
       drops; restart it as connectable or nobody can reconnect */
    if (ble_gap_adv_active())
        ble_gap_adv_stop();
    /* raw adv data: flags, name, and the ANCS service-solicitation UUID
       (AD type 0x15) — the Apple convention that makes iOS list and
       pair the accessory and grant it notification access */
    static const uint8_t adv[] = {
        0x02, 0x01, 0x06,
        0x07, 0x09, 'V', 'o', 'i', 'd', 'O', 'S',
        0x11, 0x15,
        0xd0, 0x00, 0x2d, 0x12, 0x1e, 0x4b, 0x0f, 0xa4,
        0x99, 0x4e, 0xce, 0xb5, 0x31, 0xf4, 0x05, 0x79
    };
    /* InfiniLink-compat, 2.2.2: laid out like InfiniTime's own advert.
       InfiniLink's scan keeps a peripheral only when it is named
       "InfiniTime" AND its advertised service list (AD 0x06/0x07, not
       the 0x15 solicitation 2.1 sent) holds the DFU service
       00001530-1212-EFDE-1523-785FEABCD123. Advert: flags, 16-bit
       Heart Rate, 128-bit DFU, TX power (28 B). Scan response: the
       name and the ANCS solicitation (30 B). */
    static const uint8_t adv_c[] = {
        0x02, 0x01, 0x06,
        0x03, 0x03, 0x0D, 0x18,
        0x11, 0x07,
        0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15,
        0xde, 0xef, 0x12, 0x12, 0x30, 0x15, 0x00, 0x00,
        0x02, 0x0A, 0x00
    };
    static const uint8_t rsp_c[] = {
        0x0B, 0x09, 'I', 'n', 'f', 'i', 'n', 'i', 'T', 'i', 'm', 'e',
        0x11, 0x15,
        0xd0, 0x00, 0x2d, 0x12, 0x1e, 0x4b, 0x0f, 0xa4,
        0x99, 0x4e, 0xce, 0xb5, 0x31, 0xf4, 0x05, 0x79
    };
    _Static_assert(sizeof adv_c <= 31 && sizeof rsp_c <= 31, "adv > 31 B");
    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof p);
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int32_t fast_left = (int32_t)(adv_fast_until - hal_millis());
    int32_t dur = BLE_HS_FOREVER;
    if (!ble_connected() && fast_left > 100) {
        p.itvl_min = 32;                /* 20 ms */
        p.itvl_max = 47;                /* 29.4 ms */
        dur = fast_left;                /* then ADV_COMPLETE -> slow */
    } else {
        p.itvl_min = 1636;              /* 1022.5 ms */
        p.itvl_max = 1651;
    }
    if (ble_compat) {
        ble_gap_adv_set_data(adv_c, sizeof adv_c);
        ble_gap_adv_rsp_set_data(rsp_c, sizeof rsp_c);
        /* iOS holds the link for ANCS, so while connected keep a
           scannable, non-connectable advert up: InfiniLink's scan can
           then find the watch and CoreBluetooth reuses the link */
        p.conn_mode = ble_connected() ? BLE_GAP_CONN_MODE_NON
                                      : BLE_GAP_CONN_MODE_UND;
    } else {
        if (ble_connected())
            return;
        ble_gap_adv_set_data(adv, sizeof adv);
        p.conn_mode = BLE_GAP_CONN_MODE_UND;
    }
    d_adv_calls++;
    d_adv_rc = ble_gap_adv_start(own_addr_type, NULL, dur,
                                 &p, gap_cb, NULL);
}

/* settings toggle; applied live */
void ble_set_compat(int on)
{
    on = !!on;
    if (on == ble_compat)
        return;
    ble_compat = on;
    if (!ble_up)
        return;
    ble_svc_gap_device_name_set(on ? "InfiniTime" : "VoidOS");
    if (ble_synced && ble_gap_adv_active())
        ble_gap_adv_stop();
    adv_fast_window();             /* a new name: be quick to find */
    if (ble_synced)
        ble_advertise();
}
int ble_get_compat(void) { return ble_compat; }

static void on_sync(void)
{
    /* public addr if burnt in, else the FICR static random */
    d_syncs++;
    ble_synced = 1;
    ble_hs_id_infer_auto(0, &own_addr_type);
    ble_adv_on = ble_want;
    adv_fast_window();
    ble_advertise();
}

static void on_reset(int reason)
{
    (void)reason;
}

int ble_init(void)
{
    if (ble_up)
        return 1;
    nrf52_clock_hfxo_request();
    nimble_port_init();
    nimble_mark(10);
    ble_svc_gap_init();
    ble_svc_gatt_init();
    extern void ble_dfu_init(void);
    ble_dfu_init();
    phone_svcs_init();          /* CTS, music, DIS, battery, HR, steps */
    nimble_mark(11);
    ble_svc_gap_device_name_set(ble_compat ? "InfiniTime" : "VoidOS");
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    /* ANCS requires an encrypted link; a screenless watch can only do
       Just Works, which iOS accepts */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_HS_KEY_DIST_ENC_KEY |
                                 BLE_HS_KEY_DIST_ID_KEY;
    ble_hs_cfg.sm_their_key_dist = BLE_HS_KEY_DIST_ENC_KEY |
                                   BLE_HS_KEY_DIST_ID_KEY;
    /* bonds in RAM, mirrored to external flash (0x3F3000-0x3F4FFF) so
       they survive a reboot or a flash. The signature ties saved CCCDs
       to this firmware's GATT table. */
    {
        uint16_t sig = 0x5644;
        for (const char *v = VOIDOS_VERSION; *v; v++)
            sig = (uint16_t)(sig * 31u + (uint8_t)*v);
        ble_bond_store_init(sig);
    }
    ble_up = 1;
    return 1;
}

/* Pop and run one LL-queue event. Used by ble_poll and by ble_npl_sem_pend
   (blocking HCI calls only complete when the LL queue is serviced). */
void ble_port_pump_llq(void)
{
    struct ble_npl_event *ev = ble_npl_eventq_get(&g_ble_ll_data.ll_evq, 0);
    if (ev) {
        d_ll_evs++;
        ble_npl_event_run(ev);
    }
}

/* drain the LL + host event queues; called from the main loop.
   Bounded so a busy queue can never starve the watchdog. */
void ble_poll(void)
{
    if (!ble_up)
        return;
    /* Cooperative replacement for the first half of ble_ll_task(): the
       blocking task loop doesn't exist on bare metal, so run its one-time
       PHY/tx-power preamble here before draining the LL queue. */
    static int ll_started;
    if (!ll_started) {
        extern int ble_ll_tx_power_round(int);
        extern void ble_ll_tx_power_set(int);
        extern int8_t g_ble_ll_tx_power;
        extern void nimble_stage(int);
        ll_started = 1;
        nimble_stage(12);
        ble_phy_init();
        nimble_stage(13);
        g_ble_ll_tx_power = ble_ll_tx_power_round(
            MYNEWT_VAL(BLE_LL_TX_PWR_DBM) < MYNEWT_VAL(BLE_LL_TX_PWR_MAX_DBM)
                ? MYNEWT_VAL(BLE_LL_TX_PWR_DBM)
                : MYNEWT_VAL(BLE_LL_TX_PWR_MAX_DBM));
        ble_ll_tx_power_set(g_ble_ll_tx_power);
        nimble_stage(14);
    }
    npl_callout_tick();
    /* deferred pairing request + bounded retries. The first request
       races the phone's own post-connect setup on some stacks and is
       silently dropped — with no retry the link stayed unencrypted
       and ANCS/CTS never came up ("SYNCING... ANCS HANDSHAKE"). */
    if (ble_connected()) {
        struct ble_gap_conn_desc sd;
        int enc = ble_gap_conn_find(conn_handle, &sd) == 0 &&
                  sd.sec_state.encrypted;
        if (!enc && sec_pending_at && hal_millis() >= sec_pending_at) {
            d_sec_rc = ble_gap_security_initiate(conn_handle);
            sec_attempts++;
            sec_pending_at = sec_attempts < 4
                                 ? hal_millis() + 6000
                                 : 0;
        } else if (enc) {
            sec_pending_at = 0;
        }
    }
    ancs_tick();
    ams_tick();
    {
        extern void phone_svcs_tick(uint32_t now);
        extern uint32_t hal_millis(void);
        phone_svcs_tick(hal_millis());
    }
    /* write changed bonds to flash, never mid-pairing */
    {
        struct ble_gap_conn_desc bd;
        int pairing = ble_connected() &&
                      ble_gap_conn_find(conn_handle, &bd) == 0 &&
                      !bd.sec_state.encrypted && sec_attempts > 0 &&
                      sec_pending_at;
        ble_bond_store_poll(hal_millis(), pairing);
    }
    /* advertising watchdog: any failed/interrupted adv start (disconnect
       races, failed pairing) would otherwise leave the watch invisible */
    {
        extern uint32_t hal_millis(void);
        uint32_t now = hal_millis();
        conn_params_poll(now);
        if (ble_synced && ble_adv_on && now - adv_check_ms > 2000) {
            adv_check_ms = now;
            if (!ble_gap_adv_active() && (!ble_connected() || ble_compat))
                ble_advertise();
        }
    }
    for (int i = 0; i < 32; i++) {
        extern void nimble_stage(int);
        struct ble_npl_event *ev =
            ble_npl_eventq_get(&g_ble_ll_data.ll_evq, 0);
        if (!ev)
            break;
        d_ll_evs++;
        if (d_ll_evs < 10)
            nimble_stage(30 + d_ll_evs);   /* 31..39 = about to run LL ev N */
        ble_npl_event_run(ev);
        if (d_ll_evs < 10)
            nimble_stage(40 + d_ll_evs);   /* 41..49 = LL ev N completed */
    }
    for (int i = 0; i < 32; i++) {
        struct ble_npl_event *ev =
            ble_npl_eventq_get(nimble_port_get_dflt_eventq(), 0);
        if (!ev)
            break;
        d_hs_evs++;
        ble_npl_event_run(ev);
    }
}

int ble_connected(void)
{
    struct ble_gap_conn_desc d;
    return ble_up && ble_gap_conn_find(conn_handle, &d) == 0;
}

void ble_diag(int *syncs, int *adv_calls, int *adv_rc, int *gap_events,
              int *addr_type)
{
    extern uint8_t ble_hs_sync_state;
    /* diag packing: syncs=state, adv_calls=st1*10+st2, adv_rc=hs_start
       rc (-9 = never ran), gap_events=reset cmd rc (-9 = never ran) */
    extern uint8_t ble_hs_sync_state;
    if (syncs)      *syncs = ble_hs_sync_state;   /* 0 bad 1 bringup 2 good */
    if (adv_calls)  *adv_calls = d_st1 * 10 + d_st2;
    if (adv_rc)     *adv_rc = d_st2rc;
    if (gap_events) *gap_events = d_gap_events;
    if (addr_type)  *addr_type = own_addr_type;
}

void ble_set_enabled(int on)
{
    ble_want = on != 0;
    if (!ble_up || !ble_synced)
        return;                      /* on_sync applies ble_want */
    ble_adv_on = ble_want;
    if (on) {
        adv_fast_window();
        ble_advertise();
    } else {
        ble_gap_adv_stop();
        if (ble_connected())
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

/* 0 off, 1 waiting for sync, 2 advertising, 3 connected, 4 idle/retrying */
int ble_status(void)
{
    if (!ble_want)
        return 0;
    if (!ble_synced)
        return 1;
    if (ble_connected())
        return 3;
    return ble_gap_adv_active() ? 2 : 4;
}

/* link diagnostics for the on-watch DIAG screen */
/* DIAG: bonds held, re-pairs accepted */
void ble_bond_diag(int *bonds, int *repairs)
{
    if (bonds)   *bonds = ble_up ? ble_bond_count() : 0;
    if (repairs) *repairs = d_repeat_pairs;
}

void ble_link_diag(int *conns, int *sec_rc, int *enc_status,
                   int *disc_reason, int *encrypted)
{
    struct ble_gap_conn_desc d;
    if (conns)       *conns = d_conns;
    if (sec_rc)      *sec_rc = d_sec_rc;
    if (enc_status)  *enc_status = d_enc_status;
    if (disc_reason) *disc_reason = d_disc_reason;
    if (encrypted)
        *encrypted = ble_up && ble_gap_conn_find(conn_handle, &d) == 0 &&
                     d.sec_state.encrypted;
}
