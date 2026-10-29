/* Bonds that survive a reboot.
 *
 * NimBLE keeps bonds in ble_store_ram. Before 2.2.1 that was all: every
 * reboot or flash wiped the watch's keys while the iPhone kept its own,
 * so the iPhone tried to encrypt with a key the watch no longer had and
 * the link stayed unencrypted (no ANCS, no AMS) until the user forgot the
 * watch on the phone.
 *
 * This wraps the RAM store: every write/delete marks the store dirty, and
 * ble_bond_store_poll() writes the whole store (our keys, the peer's
 * keys, CCCDs) to the external flash a couple of seconds later, outside
 * the pairing exchange. At boot the newest copy is loaded back. Format
 * and flash slots: bondblob.h.
 */
#include <string.h>
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "board.h"
#include "bondblob.h"

int ble_store_ram_init(void);
uint32_t hal_millis(void);
#ifndef VOIDOS_GATT_SIG
#define VOIDOS_GATT_SIG 0
#endif

/* record types */
#define REC_OUR_SEC   1
#define REC_PEER_SEC  2
#define REC_CCCD      3
#define REC_GATT_SIG  4   /* CCCD handles are only valid for one GATT db */

/* layout: changes whenever the structs we copy change size */
#define LAYOUT ((uint16_t)((sizeof(struct ble_store_value_sec) << 8) ^ \
                           sizeof(struct ble_store_value_cccd)))

static ble_store_write_fn *ram_write;
static ble_store_delete_fn *ram_delete;
static uint8_t loading, dirty;
static uint32_t dirty_at, cur_seq, cur_slot;
static uint16_t gatt_sig;
/* diag */
static int8_t load_rc = -2;   /* -2 not run, -1 none/invalid, 0 loaded */
static uint8_t loaded_bonds, saves, save_errs;

/* ---- flash ops ---- */
static int f_read(uint32_t a, void *b, uint32_t n)
{ spi_flash_init(); spi_flash_read(a, b, n); return 0; }
static int f_erase(uint32_t a)
{ spi_flash_init(); spi_flash_sector_erase(a); return 0; }
static int f_write(uint32_t a, const void *b, uint32_t n)
{ spi_flash_init(); spi_flash_write(a, b, n); return 0; }
static const struct bondblob_flash flash = { f_read, f_erase, f_write };

static uint8_t blob[BONDBLOB_MAX];

/* ---- wrappers ---- */
static int w_write(int type, const union ble_store_value *v)
{
    int rc = ram_write(type, v);
    if (rc == 0 && !loading) {
        dirty = 1;
        dirty_at = hal_millis();
    }
    return rc;
}

static int w_delete(int type, const union ble_store_key *k)
{
    int rc = ram_delete(type, k);
    if (rc == 0 && !loading) {
        dirty = 1;
        dirty_at = hal_millis();
    }
    return rc;
}

/* ---- load ---- */
struct load_ctx { int sig_ok; };

static int load_rec(uint8_t type, const uint8_t *d, uint8_t n, void *vctx)
{
    struct load_ctx *c = vctx;
    union ble_store_value v;
    memset(&v, 0, sizeof v);
    switch (type) {
    case REC_GATT_SIG:
        if (n == 2) c->sig_ok = (d[0] | (d[1] << 8)) == gatt_sig;
        break;
    case REC_OUR_SEC:
    case REC_PEER_SEC:
        if (n != sizeof v.sec) break;
        memcpy(&v.sec, d, n);
        if (ram_write(type == REC_OUR_SEC ? BLE_STORE_OBJ_TYPE_OUR_SEC
                                          : BLE_STORE_OBJ_TYPE_PEER_SEC,
                      &v) == 0 && type == REC_PEER_SEC)
            loaded_bonds++;
        break;
    case REC_CCCD:
        /* a different firmware may have moved the handles: the phone
           re-subscribes on its own, so drop them rather than guess */
        if (n != sizeof v.cccd || !c->sig_ok) break;
        memcpy(&v.cccd, d, n);
        ram_write(BLE_STORE_OBJ_TYPE_CCCD, &v);
        break;
    }
    return 0;
}

void ble_bond_store_init(uint16_t sig)
{
    ble_store_ram_init();
    gatt_sig = sig;
    ram_write = ble_hs_cfg.store_write_cb;
    ram_delete = ble_hs_cfg.store_delete_cb;
    ble_hs_cfg.store_write_cb = w_write;
    ble_hs_cfg.store_delete_cb = w_delete;
    /* full store: drop the oldest bond instead of refusing a new one */
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    int len = bondblob_load(&flash, LAYOUT, blob, &cur_seq);
    if (len < 0) {
        load_rc = -1;
        cur_seq = 0;
        cur_slot = 0;
        return;
    }
    cur_slot = bondblob_slot_of(&flash, LAYOUT, cur_seq);
    struct load_ctx c = { 0 };
    loading = 1;
    bondblob_walk(blob + BONDBLOB_HDR, (uint32_t)len, load_rec, &c);
    loading = 0;
    load_rc = 0;
}

/* ---- save ---- */
static struct bondblob_builder bld;

static int add_it(int type, union ble_store_value *v, void *cookie)
{
    (void)cookie;
    if (type == BLE_STORE_OBJ_TYPE_CCCD)
        bondblob_add(&bld, REC_CCCD, &v->cccd, sizeof v->cccd);
    else
        bondblob_add(&bld, type == BLE_STORE_OBJ_TYPE_OUR_SEC ? REC_OUR_SEC
                                                              : REC_PEER_SEC,
                     &v->sec, sizeof v->sec);
    return 0;
}

static void save_now(void)
{
    bondblob_begin(&bld, blob);
    uint8_t s[2] = { gatt_sig & 0xFF, gatt_sig >> 8 };
    bondblob_add(&bld, REC_GATT_SIG, s, 2);
    ble_store_iterate(BLE_STORE_OBJ_TYPE_OUR_SEC, add_it, NULL);
    ble_store_iterate(BLE_STORE_OBJ_TYPE_PEER_SEC, add_it, NULL);
    ble_store_iterate(BLE_STORE_OBJ_TYPE_CCCD, add_it, NULL);
    uint32_t seq = bondblob_save(&flash, LAYOUT, &bld, cur_seq, cur_slot);
    if (seq) {
        cur_slot = cur_slot == BONDBLOB_SLOT_A ? BONDBLOB_SLOT_B
                                               : BONDBLOB_SLOT_A;
        cur_seq = seq;
        if (saves < 255) saves++;
    } else if (save_errs < 255) {
        save_errs++;
    }
}

/* called from ble_poll; busy = a pairing exchange is in progress */
void ble_bond_store_poll(uint32_t now, int busy)
{
    if (!dirty || busy) return;
    if ((uint32_t)(now - dirty_at) < 2000) return;   /* let it settle */
    dirty = 0;
    save_now();
}

/* forget everything (watch-side "forget phone") */
void ble_bond_store_clear(void)
{
    ble_store_clear();
    dirty = 1;
    dirty_at = hal_millis() - 2000;
}

int ble_bond_count(void)
{
    int n = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &n);
    return n;
}

/* diag word: load_rc+2 | loaded<<4 | saves<<8 | errs<<16 */
int ble_bond_dbg(void)
{
    return (load_rc + 2) | (loaded_bonds << 4) | (saves << 8) |
           (save_errs << 16) | (dirty << 24);
}
