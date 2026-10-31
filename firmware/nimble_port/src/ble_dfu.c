/* InfiniTime-compatible BLE DFU service (0x1530/1531/1532/1534). Receives a
   signed MCUBoot image over BLE and writes it to the external SPI flash
   secondary slot at 0x40000. The image is left UNCONFIRMED: the bootloader
   test-boots it once, and a manual restart always reverts to the previous
   firmware unless the app explicitly confirms itself. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "os/os_mbuf.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "board.h"

#define DFU_WRITE_OFFSET 0x40000u
#define DFU_MAX_SIZE     475136u
#define DFU_MAGIC_ADDR   (DFU_WRITE_OFFSET + DFU_MAX_SIZE - 16)

enum { ST_IDLE, ST_START, ST_INIT, ST_DATA, ST_VALIDATE, ST_VALIDATED };
enum { OP_START = 1, OP_INIT = 2, OP_RECV_IMG = 3, OP_VALIDATE = 4,
       OP_ACTIVATE = 5, OP_SYS_RESET = 6, OP_REPORT = 7,
       OP_PRN_REQ = 8, OP_RESP = 0x10, OP_PRN = 0x11 };

static const ble_uuid128_t dfu_svc_uuid = BLE_UUID128_INIT(
    0x23, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15,
    0xDE, 0xEF, 0x12, 0x12, 0x30, 0x15, 0x00, 0x00);
static const ble_uuid128_t dfu_pkt_uuid = BLE_UUID128_INIT(
    0x23, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15,
    0xDE, 0xEF, 0x12, 0x12, 0x32, 0x15, 0x00, 0x00);
static const ble_uuid128_t dfu_ctrl_uuid = BLE_UUID128_INIT(
    0x23, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15,
    0xDE, 0xEF, 0x12, 0x12, 0x31, 0x15, 0x00, 0x00);
static const ble_uuid128_t dfu_rev_uuid = BLE_UUID128_INIT(
    0x23, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15,
    0xDE, 0xEF, 0x12, 0x12, 0x34, 0x15, 0x00, 0x00);
/* vendor diag chars (read-only): dump bring-up state as short text */
static const ble_uuid128_t diag_uuid = BLE_UUID128_INIT(
    0x77, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB, 0x00, 0x00, 0x00, 0x00);
static const ble_uuid128_t diag2_uuid = BLE_UUID128_INIT(
    0x77, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB, 0x00, 0x00, 0x00, 0x01);
/* 2.2.2: the Current Time Service lives in ble_svcs.c only. This file
   used to register a second 0x1805, so a phone saw two CTS services and
   InfiniLink kept whichever 0x2A2B it discovered last. */

static int dfu_state;
static int flash_ready;
static uint16_t h_pkt, h_ctrl, h_rev, h_diag, h_diag2;
static uint8_t dfu_buf[200];
static unsigned buf_len, total_len, app_size;
static uint32_t erased_to; /* pay-as-you-go erase frontier */
static uint16_t expect_crc;
static uint8_t prn_every;
static uint32_t pkt_count, bytes_rx;
static int d_ctrl_writes, d_pkt_writes, d_notify_rc = -1;
static int d_access_calls;        /* every gatt access cb invocation */
static int d_last_op0 = -1, d_last_op1 = -1, d_last_pkt0 = -1;

/* 2.3.1: a session that the phone abandoned (disconnect, app killed,
   error) used to leave dfu_state non-idle until the next reboot. That
   kept the 15-30 ms "DFU" connection interval forever (battery) and made
   every later START silently ignored, so the next attempt timed out. */
#define DFU_IDLE_ABORT_MS 30000u
static volatile int dfu_touched;
static uint32_t dfu_last_act;
static uint16_t dfu_conn = 0xFFFF;

static void dfu_abort(void)
{
    dfu_state = 0; /* ST_IDLE */
    buf_len = total_len = app_size = 0;
    pkt_count = bytes_rx = 0;
    prn_every = 0;
    dfu_conn = 0xFFFF;
}

/* erase 4 KB sectors only just before they're written — keeps the BLE
   callback short so the connection survives the transfer */
static void dfu_erase_to(uint32_t end)
{
    if (!flash_ready) {
        spi_flash_init();
        flash_ready = 1;
    }
    while (erased_to < end) {
        spi_flash_sector_erase(DFU_WRITE_OFFSET + erased_to);
        erased_to += 0x1000;
        wdt_kick();
    }
}

static void dfu_notify(uint16_t conn, const uint8_t *d, unsigned n)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(d, n);
    d_notify_rc = om ? ble_gatts_notify_custom(conn, h_ctrl, om) : -2;
}

static uint16_t crc16(const uint8_t *p, uint32_t n, uint16_t crc)
{
    for (uint32_t i = 0; i < n; i++) {
        crc = (uint8_t)(crc >> 8) | (crc << 8);
        crc ^= p[i];
        crc ^= (uint8_t)(crc & 0xFF) >> 4;
        crc ^= (crc << 8) << 4;
        crc ^= ((crc & 0xFF) << 4) << 1;
    }
    return crc;
}

static void dfu_flush(void)
{
    dfu_erase_to(total_len + buf_len);
    spi_flash_write(DFU_WRITE_OFFSET + total_len, dfu_buf, buf_len);
    total_len += buf_len;
    buf_len = 0;
}

/* MCUBoot trailer (swap status + flags + magic) lives in the last sectors
   of the slot. Only those need to be blank; the old code erased every
   sector from the end of the image to the end of the slot inside the BLE
   callback (up to ~110 erases, several seconds), which outlasted the 4 s
   supervision timeout and dropped the link right at 100%. */
#define DFU_TRAILER_ERASE 0x2000u
static void dfu_write_magic(void)
{
    const uint32_t magic[4] = {0xf395c277, 0x7fefd260, 0x0f505235, 0x8079b62c};
    uint32_t t = DFU_MAX_SIZE - DFU_TRAILER_ERASE;
    if (erased_to < t)
        erased_to = t; /* the gap after the image is never read */
    dfu_erase_to(DFU_MAX_SIZE);
    spi_flash_write(DFU_MAGIC_ADDR, (const uint8_t *)magic, 16);
}

static int dfu_packet(uint16_t conn, struct os_mbuf *om)
{
    d_pkt_writes++;
    d_last_pkt0 = OS_MBUF_PKTLEN(om) ? om->om_data[0] : -1;
    switch (dfu_state) {
    case ST_START: { /* 12-byte sizes: softdevice, bootloader, application */
        uint8_t sz[12];
        if (os_mbuf_copydata(om, 0, sizeof sz, sz) != 0)
            break;
        app_size = (uint32_t)sz[8] | ((uint32_t)sz[9] << 8) |
                   ((uint32_t)sz[10] << 16) | ((uint32_t)sz[11] << 24);
        if (app_size == 0 || app_size > DFU_MAX_SIZE) {
            const uint8_t d[3] = {OP_RESP, OP_START, 4};
            dfu_notify(conn, d, 3);
            dfu_state = ST_IDLE;
            break;
        }
        erased_to = 0;
        { const uint8_t d[3] = {OP_RESP, OP_START, 1};
          dfu_notify(conn, d, 3); }
        dfu_state = ST_INIT;
        break;
    }
    case ST_INIT: { /* dat file: deviceType/rev/appVer/sd array/crc16 */
        uint8_t h[2];
        uint16_t sd_len;
        /* sd array length is a uint16 at offset 8; the CRC16 follows the
           array at offset 10 + sd_len * 2 */
        if (os_mbuf_copydata(om, 8, 2, h) != 0)
            break;
        sd_len = h[0] | (h[1] << 8);
        if (OS_MBUF_PKTLEN(om) < 12u + (uint32_t)sd_len * 2u ||
            os_mbuf_copydata(om, 10 + sd_len * 2, 2, h) != 0)
            break;
        expect_crc = h[0] | (h[1] << 8);
        break;
    }
    case ST_DATA:
        pkt_count++;
        while (om) {
            /* clamp to the remaining image bytes; anything extra a buggy
               client sends must not write past the slot */
            unsigned want = om->om_len;
            unsigned room = app_size - (total_len + buf_len);
            unsigned rd = 0;
            if (want > room)
                want = room;
            while (want) {
                unsigned n = sizeof(dfu_buf) - buf_len;
                if (n > want)
                    n = want;
                memcpy(dfu_buf + buf_len, om->om_data + rd, n);
                buf_len += n;
                rd += n;
                want -= n;
                bytes_rx += n;
                if (buf_len == sizeof(dfu_buf))
                    dfu_flush();
            }
            om = SLIST_NEXT(om, om_next);
        }
        if (prn_every && (pkt_count % prn_every) == 0 &&
            bytes_rx != app_size) {
            const uint8_t d[5] = {OP_PRN, bytes_rx, bytes_rx >> 8,
                                  bytes_rx >> 16, bytes_rx >> 24};
            dfu_notify(conn, d, 5);
        }
        /* total_len+buf_len hits app_size exactly, even when the final
           dfu_flush just emptied buf_len (app_size % 200 == 0) — without
           this the RECV_IMG response was never sent and the client
           stalled waiting for it */
        if (total_len + buf_len == app_size) {
            dfu_flush();
            if (app_size < DFU_MAX_SIZE)
                dfu_write_magic();
            const uint8_t d[3] = {OP_RESP, OP_RECV_IMG, 1};
            dfu_notify(conn, d, 3);
            dfu_state = ST_VALIDATE;
        }
        break;
    default:
        break;
    }
    return 0;
}

static int dfu_control(uint16_t conn, struct os_mbuf *om)
{
    uint8_t op0 = 0, op1 = 0;
    d_ctrl_writes++;
    if (os_mbuf_copydata(om, 0, 1, &op0) != 0)
        return 0;
    os_mbuf_copydata(om, 1, 1, &op1);
    d_last_op0 = op0;
    d_last_op1 = op1;
    switch (op0) {
    case OP_START:
        /* a new START always begins a fresh session, whatever state an
           earlier, abandoned attempt left behind */
        if (op1 == 0x04) {
            dfu_abort();
            dfu_conn = conn;
            dfu_state = ST_START;
        }
        break;
    case OP_INIT:
        /* only init-complete (param != 0) gets a response; answering the
           init-start write can let the client send RECEIVE_IMAGE before
           the dat file lands */
        if (dfu_state == ST_INIT && op1 != 0) {
            const uint8_t d[3] = {OP_RESP, OP_INIT, 1};
            dfu_notify(conn, d, 3);
        }
        break;
    case OP_PRN_REQ:
        prn_every = op1;
        break;
    case OP_SYS_RESET: {
        /* 2.3.1: abort the session, never reboot. The running OS is the
           DFU target itself, so there is no "DFU mode" to reset into.
           DFU apps send this opcode to abort after an error or a cancel;
           rebooting here killed the link mid-session, so the phone lost
           the DFU characteristic and the update only ever completed from
           the bootloader. */
        const uint8_t d[3] = {OP_RESP, OP_SYS_RESET, 1};
        dfu_abort();
        dfu_notify(conn, d, 3);
        break;
    }
    case OP_REPORT: {
        const uint8_t d[7] = {OP_RESP, OP_REPORT, 1,
                              (uint8_t)total_len, (uint8_t)(total_len >> 8),
                              (uint8_t)(total_len >> 16),
                              (uint8_t)(total_len >> 24)};
        dfu_notify(conn, d, 7);
        break;
    }
    case OP_RECV_IMG:
        if (dfu_state == ST_INIT) {
            buf_len = total_len = bytes_rx = pkt_count = 0;
            dfu_state = ST_DATA;
        }
        break;
    case OP_VALIDATE: {
        if (dfu_state != ST_VALIDATE)
            break;
        uint8_t rd[200];
        uint16_t crc = 0xFFFF;
        int first = 1;
        for (uint32_t off = 0; off < app_size; off += 200) {
            uint32_t n = app_size - off > 200 ? 200 : app_size - off;
            spi_flash_read(DFU_WRITE_OFFSET + off, rd, n);
            crc = crc16(rd, n, first ? 0xFFFF : crc);
            first = 0;
            wdt_kick();
        }
        const uint8_t d[3] = {OP_RESP, OP_VALIDATE,
                              crc == expect_crc ? 1 : 5};
        dfu_notify(conn, d, 3);
        if (crc == expect_crc)
            dfu_state = ST_VALIDATED;
        else
            dfu_state = ST_IDLE;
        break;
    }
    case OP_ACTIVATE:
        if (dfu_state == ST_VALIDATED)
            SCB_AIRCR = 0x05FA0004; /* reset; bootloader swaps slot */
        break;
    default:
        break;
    }
    return 0;
}

/* Write s into the response mbuf; if append fails, report the buffer's
   real trailing space + databuf size instead so we can see why. */
static int diag_put(struct os_mbuf *om, const char *s, int n)
{
    struct os_mbuf *last = om;
    int sp;

    while (SLIST_NEXT(last, om_next) != NULL)
        last = SLIST_NEXT(last, om_next);
    sp = OS_MBUF_TRAILINGSPACE(last);
    if (os_mbuf_append(om, s, n) == 0)
        return 0;
    {
        char d[48];
        int dn = snprintf(d, sizeof d, "sp:%d n:%d dl:%u",
                          sp, n,
                          last->om_omp
                              ? (unsigned)last->om_omp->omp_databuf_len : 0u);
        if (os_mbuf_append(om, d, dn) == 0)
            return 0;
    }
    return os_mbuf_append(om, "?", 1) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

/* touch state: "CID|F|raw|reads|errors"  (~24 bytes) */

static int diag_read(struct os_mbuf *om)
{
    char s[56];
    extern unsigned touch_twim_enable(void);
    extern int d_subscribed, d_mtu_events;
    int n = snprintf(s, sizeof s,
        "C%XF%dR%uE%uT%uL%XEN%uB%Xst%uLt%uV%du%dm%do%d.%d",
        touch_diag.chip_id, touch_diag.last_fingers,
        touch_diag.reads, touch_diag.twi_err, touch_diag.twi_timeout,
        touch_diag.last_err, touch_twim_enable(), touch_diag.bus_lvl,
        touch_diag.stall, touch_diag.latches,
        d_access_calls, d_subscribed, d_mtu_events,
        d_last_op0, d_last_op1);
    return diag_put(om, s, n);
}

/* sensors+dfu: "scan|HR|ST|dfu"  (~30 bytes) */
static int diag_read2(struct os_mbuf *om)
{
    char s[64];
    extern int sensors_bpm(void), sensors_steps(void);
    extern int sensors_hrs_ok(void), sensors_bma_ok(void);
    extern int sensors_hrs_raw(void), sensors_hrs_ac(void);
    extern int sensors_hrs_beats(void), sensors_hrs_ibi(void);
    extern int sensors_bma_engine(void);
    extern int hal_clock_synced(void);
    int n = snprintf(s, sizeof s,
        "I%02X%02X%02X M%d H%d.%d r%d a%d b%ui%u S%d.%d e%d t%d D%dC%dP%dN%d",
        touch_diag.scan[0], touch_diag.scan[1], touch_diag.scan[2],
        os_msys_num_free(),
        sensors_hrs_ok(), sensors_bpm(),
        sensors_hrs_raw(), sensors_hrs_ac(), sensors_hrs_beats(),
        sensors_hrs_ibi(),
        sensors_bma_ok(), sensors_steps(), sensors_bma_engine(),
        hal_clock_synced(),
        dfu_state, d_ctrl_writes, d_pkt_writes, d_notify_rc);
    return diag_put(om, s, n);
}

int ble_dfu_busy(void) { return dfu_state != 0; }

/* Main loop: drop a session nobody is driving any more. */
void ble_dfu_poll(uint32_t now_ms)
{
    if (dfu_touched) {
        dfu_touched = 0;
        dfu_last_act = now_ms;
        return;
    }
    if (dfu_state != 0 && (uint32_t)(now_ms - dfu_last_act) > DFU_IDLE_ABORT_MS)
        dfu_abort();
}

/* The link that ran the session went away: the session is dead. */
void ble_dfu_on_disconnect(uint16_t conn)
{
    if (dfu_state != 0 && (dfu_conn == 0xFFFF || dfu_conn == conn))
        dfu_abort();
}

static int dfu_access(uint16_t conn, uint16_t attr,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;
    if (attr != h_diag && attr != h_diag2)   /* don't count our own probes */
        d_access_calls++;
    if (attr == h_ctrl || attr == h_pkt)
        dfu_touched = 1;
    if (attr == h_ctrl)
        return dfu_control(conn, ctxt->om);
    if (attr == h_pkt)
        return dfu_packet(conn, ctxt->om);
    if (attr == h_rev && ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const uint16_t rev = 8;
        /* os_mbuf_append() returns 0 on success. This test was inverted
           from the first VoidOS release, so every read of the DFU
           Revision answered "Insufficient Resources" and the phone's DFU
           library gave up before it ever used the control point. */
        return os_mbuf_append(ctxt->om, &rev, 2)
                   ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    if (attr == h_diag && ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return diag_read(ctxt->om);
    if (attr == h_diag2 && ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return diag_read2(ctxt->om);
    return 0;
}

static const struct ble_gatt_chr_def dfu_chrs[] = {
    {.uuid = &dfu_pkt_uuid.u, .access_cb = dfu_access,
     .flags = BLE_GATT_CHR_F_WRITE_NO_RSP, .val_handle = &h_pkt},
    {.uuid = &dfu_ctrl_uuid.u, .access_cb = dfu_access,
     .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
              BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &h_ctrl},
    {.uuid = &dfu_rev_uuid.u, .access_cb = dfu_access,
     .flags = BLE_GATT_CHR_F_READ, .val_handle = &h_rev},
    {.uuid = &diag_uuid.u, .access_cb = dfu_access,
     .flags = BLE_GATT_CHR_F_READ, .val_handle = &h_diag},
    {.uuid = &diag2_uuid.u, .access_cb = dfu_access,
     .flags = BLE_GATT_CHR_F_READ, .val_handle = &h_diag2},
    {0},
};

static const struct ble_gatt_svc_def dfu_svcs[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &dfu_svc_uuid.u,
     .characteristics = dfu_chrs},
    {0},
};

void ble_dfu_init(void)
{
    ble_gatts_count_cfg(dfu_svcs);
    ble_gatts_add_svcs(dfu_svcs);
}

void ble_dfu_diag(int *state, int *ctrl, int *pkt, int *rc, unsigned *bytes)
{
    *state = dfu_state;
    *ctrl = d_ctrl_writes;
    *pkt = d_pkt_writes;
    *rc = d_notify_rc;
    *bytes = bytes_rx;
}
