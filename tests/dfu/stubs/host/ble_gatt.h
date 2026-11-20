/* Minimal ble_gatt for the host DFU test. */
#ifndef VOIDOS_TEST_BLE_GATT_H
#define VOIDOS_TEST_BLE_GATT_H

#include <stdint.h>
#include "os/os_mbuf.h"
#include "host/ble_uuid.h"

#define BLE_GATT_SVC_TYPE_PRIMARY 0x00
#define BLE_GATT_CHR_F_READ        0x0002
#define BLE_GATT_CHR_F_WRITE       0x0008
#define BLE_GATT_CHR_F_WRITE_NO_RSP 0x0004
#define BLE_GATT_CHR_F_NOTIFY      0x0200

#define BLE_GATT_ACCESS_OP_READ_CHR  0
#define BLE_GATT_ACCESS_OP_WRITE_CHR 1

#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 0x0d
#define BLE_ATT_ERR_INSUFFICIENT_RES       0x11
#define BLE_ATT_ERR_UNLIKELY               0x0e

struct ble_gatt_access_ctxt;

struct ble_gatt_chr_def {
    const ble_uuid_t *uuid;
    int (*access_cb)(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg);
    void *arg;
    uint16_t flags;
    uint16_t *val_handle;
};

struct ble_gatt_svc_def {
    uint8_t type;
    const ble_uuid_t *uuid;
    const struct ble_gatt_chr_def *characteristics;
};

struct ble_gatt_access_ctxt {
    uint8_t op;
    struct os_mbuf *om;
};

#endif
