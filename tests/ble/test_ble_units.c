/* Host tests for the 2.2.1 BLE pieces that need no radio:
 *  - which characteristic owns a discovered descriptor (ANCS/AMS CCCDs)
 *  - AMS PlaybackInfo parsing
 *  - the persistent bond blob: round trip, alternating slots, a torn
 *    write and a corrupt copy falling back to the previous one
 *  - 2.2.5: resolving a phone's private address with its IRK
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "nimble_port/src/gatt_owner.h"
#include "nimble_port/src/ams_parse.h"
#include "nimble_port/src/bondblob.h"
#include "nimble_port/src/bondblob.c"
#include "nimble_port/src/rpa_match.h"
#include "tinycrypt/aes.h"
#include "tinycrypt/constants.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, \
    __LINE__, #c); fails++; } } while (0)

/* ---- descriptor owner ---------------------------------------------- */
static void test_owner(void)
{
    /* ANCS as iOS lays it out: service 0x30..0x3A
       NS def 0x31 val 0x32 cccd 0x33
       CP def 0x34 val 0x35 (no CCCD, extended props 0x36)
       DS def 0x37 val 0x38 cccd 0x39 */
    struct gatt_spans s;
    gatt_spans_reset(&s);
    gatt_spans_add(&s, 0x31, 0x32);
    gatt_spans_add(&s, 0x34, 0x35);
    gatt_spans_add(&s, 0x37, 0x38);
    CHECK(gatt_owner_of(&s, 0x33) == 0x32);   /* NS CCCD */
    CHECK(gatt_owner_of(&s, 0x36) == 0x35);   /* CP ext props */
    CHECK(gatt_owner_of(&s, 0x39) == 0x38);   /* DS CCCD */
    CHECK(gatt_owner_of(&s, 0x30) == 0);      /* before any char */

    /* What 2.2.0 did: NimBLE gives every descriptor callback the
       start handle we passed (0x30), so comparing it with the NS/DS
       value handles never matched. */
    uint16_t chr_val_handle_from_nimble = 0x30;
    CHECK(chr_val_handle_from_nimble != 0x32 &&
          chr_val_handle_from_nimble != 0x38);

    /* AMS: RC def 0x41 val 0x42 cccd 0x43, EU def 0x44 val 0x45
       cccd 0x46, EA def 0x47 val 0x48 */
    gatt_spans_reset(&s);
    gatt_spans_add(&s, 0x41, 0x42);
    gatt_spans_add(&s, 0x44, 0x45);
    gatt_spans_add(&s, 0x47, 0x48);
    CHECK(gatt_owner_of(&s, 0x43) == 0x42);   /* RC CCCD, not EU's */
    CHECK(gatt_owner_of(&s, 0x46) == 0x45);   /* EU CCCD */

    /* a characteristic missing from the table (table full) must not
       hand its descriptor to the previous one */
    gatt_spans_reset(&s);
    gatt_spans_add(&s, 0x10, 0x11);
    gatt_spans_add(&s, 0x13, 0x14);
    s.c[1].val = 0x20;   /* pretend char 0x13 recorded with a far val */
    CHECK(gatt_owner_of(&s, 0x15) == 0);
}

/* ---- AMS PlaybackInfo -------------------------------------------- */
static void test_playback(void)
{
    int p, e;
    CHECK(ams_parse_playback("1,1.0,42.317", &p, &e) == 0 && p == 1 && e == 42);
    CHECK(ams_parse_playback("0,0.0,7.9", &p, &e) == 0 && p == 0 && e == 7);
    CHECK(ams_parse_playback("3,2.0,100", &p, &e) == 0 && p == 1 && e == 100);
    CHECK(ams_parse_playback("2,-2.0,5", &p, &e) == 0 && p == 0 && e == 5);
    CHECK(ams_parse_playback("1", &p, &e) == 0 && p == 1 && e == -1);
    CHECK(ams_parse_playback("", &p, &e) == -1);
    CHECK(ams_parse_playback(",1,2", &p, &e) == -1);
}

/* ---- bond blob ----------------------------------------------------- */
static uint8_t flash[0x400000];
static int tear_after = -1;     /* bytes a write may store before power cut */
static int fr(uint32_t a, void *b, uint32_t n) { memcpy(b, flash + a, n); return 0; }
static int fe(uint32_t a) { memset(flash + a, 0xFF, BONDBLOB_SECTOR); return 0; }
static int fw(uint32_t a, const void *b, uint32_t n)
{
    if (tear_after >= 0 && (int)n > tear_after) n = tear_after;
    for (uint32_t i = 0; i < n; i++) flash[a + i] &= ((const uint8_t *)b)[i];
    return 0;
}
static const struct bondblob_flash F = { fr, fe, fw };

struct walk { int n; uint8_t last_type; uint8_t last[64]; uint8_t last_n; };
static int wcb(uint8_t t, const uint8_t *d, uint8_t n, void *ctx)
{
    struct walk *w = ctx;
    w->n++; w->last_type = t; w->last_n = n; memcpy(w->last, d, n);
    return 0;
}

static uint32_t save_with(uint8_t tag, uint32_t cur_seq, uint32_t cur_slot)
{
    static uint8_t buf[BONDBLOB_MAX];
    struct bondblob_builder b;
    uint8_t rec[40];
    memset(rec, tag, sizeof rec);
    bondblob_begin(&b, buf);
    bondblob_add(&b, 1, rec, sizeof rec);
    bondblob_add(&b, 2, rec, 16);
    return bondblob_save(&F, 7, &b, cur_seq, cur_slot);
}

static int load_tag(uint32_t *seq)
{
    static uint8_t buf[BONDBLOB_MAX];
    int len = bondblob_load(&F, 7, buf, seq);
    if (len < 0) return -1;
    struct walk w = {0};
    if (bondblob_walk(buf + BONDBLOB_HDR, len, wcb, &w) != 2) return -2;
    return w.last[0];
}

static void test_bondblob(void)
{
    uint32_t seq;
    memset(flash, 0xFF, sizeof flash);
    CHECK(bondblob_load(&F, 7, (uint8_t[BONDBLOB_MAX]){0}, &seq) == -1);

    uint32_t s1 = save_with(0xA1, 0, 0);
    CHECK(s1 != 0);
    CHECK(load_tag(&seq) == 0xA1 && seq == s1);
    uint32_t slot1 = bondblob_slot_of(&F, 7, s1);

    uint32_t s2 = save_with(0xB2, s1, slot1);
    CHECK(s2 == s1 + 1);
    CHECK(load_tag(&seq) == 0xB2 && seq == s2);
    uint32_t slot2 = bondblob_slot_of(&F, 7, s2);
    CHECK(slot2 != slot1);                     /* alternates */
    CHECK(slot1 == BONDBLOB_SLOT_A || slot1 == BONDBLOB_SLOT_B);

    /* power cut half way through the next save: previous copy wins */
    tear_after = 30;
    save_with(0xC3, s2, slot2);
    tear_after = -1;
    CHECK(load_tag(&seq) == 0xB2 && seq == s2);

    /* a wrong layout (other firmware's GATT table) loads nothing */
    CHECK(bondblob_load(&F, 8, (uint8_t[BONDBLOB_MAX]){0}, &seq) == -1);

    /* flip a payload bit in the newest copy: CRC rejects it */
    flash[slot2 + BONDBLOB_HDR + 5] ^= 0x01;
    int t = load_tag(&seq);
    CHECK(t == 0xA1 || t == -1);               /* older copy, if intact */

    /* nothing outside the two sectors was touched */
    for (uint32_t a = 0x3F0000; a < 0x3F3000; a++)
        if (flash[a] != 0xFF) { CHECK(!"settings sector touched"); break; }
    for (uint32_t a = 0x3F5000; a < 0x400000; a++)
        if (flash[a] != 0xFF) { CHECK(!"flash above bonds touched"); break; }
}

/* ---- 2.2.5 RPA resolution ------------------------------------------ */
static int aes_tc(const uint8_t k[16], const uint8_t in[16], uint8_t out[16])
{
    struct tc_aes_key_sched_struct ks;
    if (tc_aes128_set_encrypt_key(&ks, k) != TC_CRYPTO_SUCCESS)
        return -1;
    return tc_aes_encrypt(out, in, &ks) == TC_CRYPTO_SUCCESS ? 0 : -1;
}

static void test_rpa(void)
{
    /* AES-128 itself: FIPS-197 appendix C.1 */
    static const uint8_t fk[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    static const uint8_t fp[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                                   0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    static const uint8_t fc[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
                                   0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
    uint8_t o[16];
    CHECK(aes_tc(fk, fp, o) == 0 && memcmp(o, fc, 16) == 0);

    /* Core spec Vol 3 Part H appendix D.7 (ah): IRK
       ec0234a357c8ad05341010a60a397d9b, prand 708194 -> hash 0dfbaa.
       IRK little-endian as SMP carries it, address bytes as on air. */
    static const uint8_t be[16] = {0xec,0x02,0x34,0xa3,0x57,0xc8,0xad,0x05,
                                   0x34,0x10,0x10,0xa6,0x0a,0x39,0x7d,0x9b};
    uint8_t irk[16];
    for (int i = 0; i < 16; i++)
        irk[i] = be[15 - i];
    uint8_t rpa[6] = {0xaa, 0xfb, 0x0d, 0x94, 0x81, 0x70};
    CHECK(rpa_is_resolvable(rpa));
    CHECK(rpa_matches(irk, rpa, aes_tc));
    /* a different hash, a different IRK, a non-resolvable address */
    uint8_t bad[6];
    memcpy(bad, rpa, 6);
    bad[0] ^= 1;
    CHECK(!rpa_matches(irk, bad, aes_tc));
    uint8_t irk2[16];
    memcpy(irk2, irk, 16);
    irk2[7] ^= 0x80;
    CHECK(!rpa_matches(irk2, rpa, aes_tc));
    uint8_t stat[6] = {0xaa, 0xfb, 0x0d, 0x94, 0x81, 0xF0};  /* static */
    CHECK(!rpa_is_resolvable(stat));
    CHECK(!rpa_matches(irk, stat, aes_tc));
}

int main(void)
{
    test_owner();
    test_playback();
    test_bondblob();
    test_rpa();
    if (fails) { printf("%d BLE unit checks failed\n", fails); return 1; }
    printf("PASS: all BLE unit checks\n");
    return 0;
}
