/* VoidHealth history service: the phone app pulls the hlog outbox.
 *
 * Service  a3c10000-5f2e-4b8a-9d4e-766f69644f53
 *  INFO  a3c10001  read (encrypted), 28 bytes little-endian:
 *        u8 proto (1), u8 record size (16), u16 records per sector,
 *        u32 log_id, u32 first_seq, u32 next_seq, u32 acked_seq,
 *        u32 dropped, u32 capacity
 *  DATA  a3c10002  notify: 1..15 packed 16-byte records (hlog.h), in
 *        seq order, after the phone writes SEND; new records follow live
 *  CTRL  a3c10003  write (encrypted): [op][u32 arg]
 *        0x01 SEND from seq, 0x02 ACK through seq, 0x03 STOP
 *
 * The phone acks only what it has saved; the watch then erases those
 * sectors (hlog_poll). Full protocol: companion/PROTOCOL.md. */
#include <string.h>
#include "os/os.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gatt.h"
#include "../../hlog.h"

int ble_connected(void);
uint16_t ble_conn_handle(void);

/* a3c1000x-5f2e-4b8a-9d4e-766f69644f53, NimBLE wants it byte-reversed */
#define HIST_UUID(x) BLE_UUID128_INIT(0x53, 0x4f, 0x64, 0x69, 0x6f, 0x76, \
    0x4e, 0x9d, 0x8a, 0x4b, 0x2e, 0x5f, (x), 0x00, 0xc1, 0xa3)

static const ble_uuid128_t hist_svc_uuid = HIST_UUID(0x00);
static const ble_uuid128_t hist_info_uuid = HIST_UUID(0x01);
static const ble_uuid128_t hist_data_uuid = HIST_UUID(0x02);
static const ble_uuid128_t hist_ctrl_uuid = HIST_UUID(0x03);

static uint16_t data_handle;
static volatile uint8_t req_send, req_stop;
static volatile uint32_t req_from, req_ack;
static volatile uint8_t req_ack_set;
static uint8_t streaming;
static uint16_t stream_conn;
static uint32_t cursor;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int hist_access(uint16_t conn, uint16_t attr,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    int which = (int)(intptr_t)arg;
    if (which == 1 && ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        struct hlog_info in;
        hlog_get_info(&in);
        uint8_t b[28];
        b[0] = 1;
        b[1] = HLOG_REC;
        b[2] = (uint8_t)HLOG_PER_SECTOR;
        b[3] = (uint8_t)(HLOG_PER_SECTOR >> 8);
        put32(b + 4, in.log_id);
        put32(b + 8, in.first_seq);
        put32(b + 12, in.next_seq);
        put32(b + 16, in.acked_seq);
        put32(b + 20, in.dropped);
        put32(b + 24, HLOG_CAPACITY);
        return os_mbuf_append(ctxt->om, b, sizeof b) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    if (which == 3 && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t b[5];
        uint16_t n = 0;
        if (OS_MBUF_PKTLEN(ctxt->om) != 5 ||
            ble_hs_mbuf_to_flat(ctxt->om, b, sizeof b, &n) || n != 5)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint32_t v = (uint32_t)b[1] | (uint32_t)b[2] << 8 |
                     (uint32_t)b[3] << 16 | (uint32_t)b[4] << 24;
        /* flash work happens in hist_ble_tick, not in the stack callback */
        switch (b[0]) {
        case 1: req_from = v; stream_conn = conn; req_send = 1; break;
        case 2: req_ack = v; req_ack_set = 1; break;
        case 3: req_stop = 1; break;
        default: return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def hist_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &hist_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = &hist_info_uuid.u, .access_cb = hist_access,
             .arg = (void *)1,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
            {.uuid = &hist_data_uuid.u, .access_cb = hist_access,
             .arg = (void *)2, .val_handle = &data_handle,
             .flags = BLE_GATT_CHR_F_NOTIFY},
            {.uuid = &hist_ctrl_uuid.u, .access_cb = hist_access,
             .arg = (void *)3,
             .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC},
            {0},
        },
    },
    {0},
};

int hist_ble_init(void)
{
    int rc = ble_gatts_count_cfg(hist_svcs);
    if (rc == 0)
        rc = ble_gatts_add_svcs(hist_svcs);
    return rc;
}

void hist_ble_disconnect(void)
{
    streaming = 0;
    req_send = 0;
}

/* main loop: apply writes, stream a little each pass */
void hist_ble_tick(void)
{
    if (req_ack_set) {
        req_ack_set = 0;
        hlog_ack(req_ack);
    }
    if (req_stop) {
        req_stop = 0;
        streaming = 0;
    }
    if (req_send) {
        req_send = 0;
        cursor = req_from;
        streaming = 1;
    }
    if (!streaming)
        return;
    if (!ble_connected() || ble_conn_handle() != stream_conn) {
        streaming = 0;
        return;
    }
    int per = (ble_att_mtu(stream_conn) - 3) / (int)HLOG_REC;
    if (per > 15)
        per = 15;
    if (per < 1)
        return;
    for (int k = 0; k < 2; k++) {
        struct hrec r[15];
        uint32_t nx;
        int n = hlog_read(cursor, r, per, &nx);
        if (n == 0) {
            cursor = nx;        /* caught up (or skipped torn slots) */
            return;
        }
        uint8_t buf[15 * HLOG_REC];
        for (int i = 0; i < n; i++)
            hrec_pack(&r[i], buf + i * HLOG_REC);
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)(n * HLOG_REC));
        if (!om)
            return;
        if (ble_gatts_notify_custom(stream_conn, data_handle, om) != 0)
            return;             /* out of buffers: same records next pass */
        cursor = nx;
    }
}
