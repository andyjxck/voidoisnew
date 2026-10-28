/* Host-side test for the BLE DFU state machine in
   firmware/nimble_port/src/ble_dfu.c. Compiled with stub NimBLE headers
   (tests/dfu/stubs) and a simulated SPI flash, then drives the real
   protocol: START -> sizes -> INIT -> dat -> RECV -> image bytes ->
   VALIDATE, using the checked-in voidos-dfu-* package contents.

   Build/run via the top-level Makefile: make build/test_ble_dfu
   Fixture dir can be overridden: -DDFU_FIXTURE_DIR=\"...\" */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os/os_mbuf.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "board.h"

/* ---- minimal os_mbuf implementation (single-buffer segments, chained) -- */

static struct os_mbuf_pool g_pool;

static struct os_mbuf *mbuf_new(uint16_t cap)
{
    struct os_mbuf *m = calloc(1, sizeof *m);
    m->om_alloc = malloc(cap);
    m->om_data = m->om_alloc;
    m->om_cap = cap;
    m->om_len = 0;
    m->om_pkthdr.omp_len = 0;
    m->om_omp = &g_pool;
    return m;
}

struct os_mbuf *os_mbuf_get_pkthdr(uint8_t *buf, uint16_t cap)
{
    struct os_mbuf *m = mbuf_new(cap);
    memcpy(m->om_alloc, buf, cap);
    m->om_len = cap;
    m->om_pkthdr.omp_len = cap;
    return m;
}

static struct os_mbuf *mbuf_from(const void *d, uint16_t n)
{
    struct os_mbuf *m = mbuf_new(n > 0 ? n : 1);
    if (n)
        memcpy(m->om_alloc, d, n);
    m->om_len = n;
    m->om_pkthdr.omp_len = n;
    return m;
}

/* split a write into two chained segments at `cut` — exercises the
   cross-mbuf paths that broke the original om_data[] indexing */
static struct os_mbuf *mbuf_chain(const void *d, uint16_t n, uint16_t cut)
{
    struct os_mbuf *a = mbuf_from(d, cut);
    struct os_mbuf *b = mbuf_from((const uint8_t *)d + cut, n - cut);
    a->om_next.sle_next = b;
    a->om_pkthdr.omp_len = n;   /* pktlen on the head covers the chain */
    return a;
}

void os_mbuf_free_chain(struct os_mbuf *m)
{
    while (m) {
        struct os_mbuf *n = SLIST_NEXT(m, om_next);
        free(m->om_alloc);
        free(m);
        m = n;
    }
}

int os_mbuf_copydata(const struct os_mbuf *m, int off, int len, void *dst)
{
    uint8_t *d = dst;
    while (m && len > 0) {
        if (off >= m->om_len) {
            off -= m->om_len;
            m = SLIST_NEXT(m, om_next);
            continue;
        }
        int n = m->om_len - off;
        if (n > len)
            n = len;
        memcpy(d, m->om_data + off, n);
        d += n;
        len -= n;
        off = 0;
        m = SLIST_NEXT(m, om_next);
    }
    return len ? -1 : 0;
}

int os_mbuf_append(struct os_mbuf *m, const void *data, uint16_t len)
{
    struct os_mbuf *last = m;
    while (SLIST_NEXT(last, om_next))
        last = SLIST_NEXT(last, om_next);
    if (OS_MBUF_TRAILINGSPACE(last) < len)
        return -1;
    memcpy(last->om_data + last->om_len, data, len);
    last->om_len += len;
    /* chain pktlen lives on the head mbuf */
    m->om_pkthdr.omp_len += len;
    return 0;
}

int os_msys_num_free(void) { return 4; }

/* ---- BLE stack stubs -------------------------------------------------- */

struct os_mbuf *ble_hs_mbuf_from_flat(const void *buf, uint16_t len)
{
    return mbuf_from(buf, len);
}

static uint8_t g_notif[64];
static int g_notif_len;
static int g_notif_count;

int ble_gatts_notify_custom(uint16_t conn, uint16_t h, struct os_mbuf *om)
{
    (void)conn; (void)h;
    g_notif_len = OS_MBUF_PKTLEN(om);
    assert(g_notif_len <= (int)sizeof g_notif);
    os_mbuf_copydata(om, 0, g_notif_len, g_notif);
    g_notif_count++;
    os_mbuf_free_chain(om);
    return 0;
}

int ble_gatts_count_cfg(const void *svcs) { (void)svcs; return 0; }

int ble_gatts_add_svcs(const void *svcs)
{
    /* nimble assigns real handles at registration time; do the same so the
       dfu_access dispatch is exercised end-to-end */
    const struct ble_gatt_svc_def *s = svcs;
    uint16_t h = 0x20;
    for (; s->characteristics; s++)
        for (const struct ble_gatt_chr_def *c = s->characteristics;
             c->uuid; c++)
            if (c->val_handle)
                *c->val_handle = h++;
    return 0;
}

/* ---- hardware stubs ---------------------------------------------------- */

void wdt_kick(void) {}

#define FLASH_TOP (0x40000u + 475136u)
static uint8_t g_flash[FLASH_TOP];

void spi_flash_init(void) {}

void spi_flash_read(uint32_t addr, uint8_t *buf, unsigned len)
{
    memcpy(buf, g_flash + addr, len);
}

void spi_flash_write(uint32_t addr, const uint8_t *buf, unsigned len)
{
    for (unsigned i = 0; i < len; i++)
        g_flash[addr + i] &= buf[i];   /* NOR: writes can only clear bits */
}

void spi_flash_sector_erase(uint32_t addr)
{
    memset(g_flash + addr, 0xFF, 0x1000);
}

/* BLE app / sensor externs used by the diag path */
int d_subscribed, d_mtu_events;
unsigned touch_twim_enable(void) { return 6; }
int sensors_bpm(void) { return 0; }
int sensors_steps(void) { return 0; }
int sensors_hrs_ok(void) { return 0; }
int sensors_bma_ok(void) { return 0; }
int sensors_hrs_raw(void) { return 0; }
int sensors_hrs_ac(void) { return 0; }
int sensors_hrs_beats(void) { return 0; }
int sensors_hrs_ibi(void) { return 0; }
int sensors_bma_engine(void) { return 0; }
int hal_clock_synced(void) { return 0; }
void hal_clock_set(int y, int mo, int d, int dow, int h, int mi, int s)
{
    (void)y; (void)mo; (void)d; (void)dow; (void)h; (void)mi; (void)s;
}
int hal_clock_hour(void) { return 0; }
int hal_clock_minute(void) { return 0; }
int hal_clock_second(void) { return 0; }
void hal_clock_date(int *y, int *mo, int *d, int *dow)
{
    if (y) *y = 0;
    if (mo) *mo = 0;
    if (d) *d = 0;
    if (dow) *dow = 0;
}
touch_diag_t touch_diag;

/* ---- include the unit under test -------------------------------------- */

#include "../../firmware/nimble_port/src/ble_dfu.c"

/* ---- driver helpers ---------------------------------------------------- */

static int fails;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        fails++; \
    } \
} while (0)

static void reset_dfu(void)
{
    dfu_state = ST_IDLE;
    buf_len = total_len = app_size = 0;
    erased_to = 0;
    expect_crc = 0;
    prn_every = 0;
    pkt_count = bytes_rx = 0;
    g_notif_len = g_notif_count = 0;
    memset(g_flash, 0xA5, sizeof g_flash);   /* garbage: erase must do work */
}

static void write_chr(uint16_t attr, const void *d, uint16_t n)
{
    struct ble_gatt_access_ctxt ctxt = {BLE_GATT_ACCESS_OP_WRITE_CHR,
                                        mbuf_from(d, n)};
    dfu_access(0, attr, &ctxt, NULL);
    os_mbuf_free_chain(ctxt.om);
}

static void write_pkt_chained(uint16_t attr, const void *d, uint16_t n,
                              uint16_t cut)
{
    struct ble_gatt_access_ctxt ctxt = {BLE_GATT_ACCESS_OP_WRITE_CHR,
                                        mbuf_chain(d, n, cut)};
    dfu_access(0, attr, &ctxt, NULL);
    os_mbuf_free_chain(ctxt.om);
}

static void expect_notify(uint8_t a, uint8_t b, uint8_t c, const char *msg)
{
    CHECK(g_notif_len == 3 && g_notif[0] == a && g_notif[1] == b &&
          g_notif[2] == c, msg);
    g_notif_len = 0;
}

static uint8_t *read_file(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("FAIL: cannot open %s\n", path);
        fails++;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) {
        fclose(f);
        free(b);
        fails++;
        return NULL;
    }
    fclose(f);
    *out_len = n;
    return b;
}

/* Runs the complete happy-path DFU from START through VALIDATE and returns
   the final dfu_state. img[img_off] is XORed with corrupt when nonzero. */
static int run_dfu(const uint8_t *dat, long dat_len,
                   const uint8_t *img, long img_len, int corrupt)
{
    reset_dfu();
    ble_dfu_init();

    /* START, application image */
    write_chr(h_ctrl, (const uint8_t *)"\x01\x04", 2);
    CHECK(dfu_state == ST_START, "START accepted");

    /* sizes packet: sd=0, bl=0, app=img_len (chained at byte 5) */
    uint8_t sizes[12] = {0};
    sizes[8] = (uint8_t)img_len;
    sizes[9] = (uint8_t)(img_len >> 8);
    sizes[10] = (uint8_t)(img_len >> 16);
    sizes[11] = (uint8_t)(img_len >> 24);
    write_pkt_chained(h_pkt, sizes, 12, 5);
    expect_notify(0x10, 0x01, 0x01, "sizes accepted");
    CHECK(dfu_state == ST_INIT, "state INIT after sizes");

    /* init-start gets NO response (client must not jump to RECV early) */
    int notifs_before = g_notif_count;
    write_chr(h_ctrl, (const uint8_t *)"\x02\x00", 2);
    CHECK(g_notif_count == notifs_before, "no init-start response");

    /* dat packet, chained mid-header (sd_len field straddles segments) */
    write_pkt_chained(h_pkt, dat, dat_len, 9);

    /* init-complete -> response */
    write_chr(h_ctrl, (const uint8_t *)"\x02\x01", 2);
    expect_notify(0x10, 0x02, 0x01, "init-complete response");

    /* receive image */
    write_chr(h_ctrl, (const uint8_t *)"\x03", 1);
    CHECK(dfu_state == ST_DATA, "state DATA after RECV");

    /* stream image in mixed-size packets, one of them chained */
    long off = 0;
    while (off < img_len) {
        long n = img_len - off;
        if (n > 200)
            n = (off & 0x80) ? 137 : 200;
        uint8_t chunk[256];
        memcpy(chunk, img + off, n);
        if (corrupt && off == 0)
            chunk[n - 1] ^= 0xFF;
        if (off == img_len - n && n > 40)
            write_pkt_chained(h_pkt, chunk, n, 40);
        else
            write_chr(h_pkt, chunk, n);
        off += n;
    }
    expect_notify(0x10, 0x03, 0x01, "image received");
    CHECK(dfu_state == ST_VALIDATE, "state VALIDATE after image");
    CHECK(bytes_rx == (unsigned)img_len, "byte count matches image");

    /* validate */
    write_chr(h_ctrl, (const uint8_t *)"\x04", 1);
    return dfu_state;
}

int main(void)
{
    const char *dir = getenv("DFU_FIXTURE_DIR")
                          ? getenv("DFU_FIXTURE_DIR")
                          : "firmware/voidos-dfu-1.5.53";
    char path[256];
    snprintf(path, sizeof path, "%s/voidos-app-image.dat", dir);
    long dat_len;
    uint8_t *dat = read_file(path, &dat_len);
    snprintf(path, sizeof path, "%s/voidos-app-image.bin", dir);
    long img_len;
    uint8_t *img = read_file(path, &img_len);
    if (!dat || !img)
        return 1;

    /* 1. happy path: real package validates end-to-end */
    int st = run_dfu(dat, dat_len, img, img_len, 0);
    expect_notify(0x10, 0x04, 0x01, "validate OK");
    CHECK(st == ST_VALIDATED, "reaches VALIDATED");

    /* the flash holds the image verbatim + trailer magic */
    CHECK(memcmp(g_flash + 0x40000, img, img_len) == 0,
          "flash image bytes match");
    const uint8_t magic[16] = {0x77, 0xc2, 0x95, 0xf3, 0x60, 0xd2, 0xef, 0x7f,
                               0x35, 0x52, 0x50, 0x0f, 0x2c, 0xb6, 0x79, 0x80};
    CHECK(memcmp(g_flash + 0x40000 + 475136 - 16, magic, 16) == 0,
          "trailer magic written at end of slot");

    /* 2. corrupted image fails CRC and resets to IDLE */
    st = run_dfu(dat, dat_len, img, img_len, 1);
    expect_notify(0x10, 0x04, 0x05, "validate reports CRC error");
    CHECK(st == ST_IDLE, "returns to IDLE after bad CRC");

    /* 3. oversize image rejected at START */
    reset_dfu();
    ble_dfu_init();
    write_chr(h_ctrl, (const uint8_t *)"\x01\x04", 2);
    uint8_t big[12] = {0};
    big[8] = 0x01;
    big[11] = 0x08;   /* 0x08000001 > DFU_MAX_SIZE */
    write_chr(h_pkt, big, 12);
    expect_notify(0x10, 0x01, 0x04, "oversize rejected");
    CHECK(dfu_state == ST_IDLE, "IDLE after oversize");

    /* 4. short/garbage control writes don't crash or change state */
    reset_dfu();
    ble_dfu_init();
    write_chr(h_ctrl, (const uint8_t *)"\x01", 1);
    CHECK(dfu_state == ST_IDLE, "1-byte START ignored");
    write_chr(h_ctrl, (const uint8_t *)"\x09", 1);
    CHECK(dfu_state == ST_IDLE, "unknown opcode ignored");

    /* 5. image whose size is a multiple of the 200-byte flush buffer must
       still complete (buf_len is 0 when the last flush lands) */
    reset_dfu();
    ble_dfu_init();
    write_chr(h_ctrl, (const uint8_t *)"\x01\x04", 2);
    uint8_t sz400[12] = {0};
    sz400[8] = 0x90;
    sz400[9] = 0x01;   /* 400 bytes — exactly two flush buffers */
    write_chr(h_pkt, sz400, 12);
    write_chr(h_ctrl, (const uint8_t *)"\x02\x00", 2);
    write_chr(h_pkt, dat, dat_len);
    write_chr(h_ctrl, (const uint8_t *)"\x02\x01", 2);
    write_chr(h_ctrl, (const uint8_t *)"\x03", 1);
    uint8_t zeros[400] = {0};
    write_chr(h_pkt, zeros, 200);
    write_chr(h_pkt, zeros + 200, 200);
    expect_notify(0x10, 0x03, 0x01, "aligned image completes");
    CHECK(dfu_state == ST_VALIDATE, "VALIDATE at buffer boundary");

    /* 6. revision characteristic reads DFU version 8 */
    struct ble_gatt_access_ctxt rd = {BLE_GATT_ACCESS_OP_READ_CHR,
                                      mbuf_new(64)};
    dfu_access(0, h_rev, &rd, NULL);
    uint8_t rev[2] = {0};
    os_mbuf_copydata(rd.om, 0, 2, rev);
    CHECK(rev[0] == 8 && rev[1] == 0, "DFU revision reads 8");
    os_mbuf_free_chain(rd.om);

    free(dat);
    free(img);
    if (fails == 0)
        printf("PASS: all DFU checks\n");
    else
        printf("%d check(s) failed\n", fails);
    return fails ? 1 : 0;
}
