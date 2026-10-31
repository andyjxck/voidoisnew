/* host test for firmware/hlog.c on a simulated NOR flash: bits only go
   1 -> 0 on write, an erase sets a 4 KB sector back to 0xFF. The region
   starts full of junk, as it is on a watch that ran InfiniTime. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hlog.h"

#define FLASH_SIZE 0x400000u
static uint8_t *flash;
static int erases;

static void f_read(uint32_t a, uint8_t *b, unsigned n) { memcpy(b, flash + a, n); }
static void f_write(uint32_t a, const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        flash[a + i] &= b[i];
}
static void f_erase(uint32_t a)
{
    memset(flash + (a & ~0xFFFu), 0xFF, 4096);
    erases++;
}
static int f_busy(void) { return 0; }
static const struct hlog_flash FL = {f_read, f_write, f_erase, f_busy};

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
    printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct hlog_info info(void)
{
    struct hlog_info i;
    hlog_get_info(&i);
    return i;
}
static void polls(int n) { while (n--) hlog_poll(); }

/* read everything from seq on, checking order; returns count */
static int read_all(uint32_t from, uint32_t *first, uint32_t *last)
{
    struct hrec r[11];
    uint32_t seq = from, nx;
    int total = 0;
    *first = *last = 0;
    for (;;) {
        int n = hlog_read(seq, r, 11, &nx);
        for (int i = 0; i < n; i++) {
            if (!total)
                *first = r[i].seq;
            else if (r[i].seq <= *last)
                return -1;
            *last = r[i].seq;
            if (r[i].t != r[i].seq * 300u || r[i].a != (uint16_t)r[i].seq)
                return -2;
            total++;
        }
        if (nx == seq)
            break;
        seq = nx;
    }
    return total;
}

static void add(int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t s = info().next_seq;
        hlog_append(s * 300u, HREC_SAMPLE, (uint16_t)s, 0x12345678u);
    }
}

int main(void)
{
    flash = malloc(FLASH_SIZE);
    for (uint32_t i = 0; i < FLASH_SIZE; i++)
        flash[i] = (uint8_t)(i * 37u + (i >> 9));

    /* pack / unpack */
    {
        struct hrec r = {42, 123456, HREC_WORKOUT | HREC_F_CLOCK_GUESS, 0, 0xBEEF, 0xCAFEF00Du}, q;
        uint8_t b[16];
        hrec_pack(&r, b);
        CHECK(hrec_unpack(b, &q) && q.seq == 42 && q.t == 123456 &&
              q.type == r.type && q.a == 0xBEEF && q.b == 0xCAFEF00Du, "roundtrip");
        int caught = 0;
        for (int i = 0; i < 16 * 8; i++) {
            if (i / 8 == 9) continue;
            b[i / 8] ^= (uint8_t)(1 << (i % 8));
            caught += !hrec_unpack(b, &q);
            b[i / 8] ^= (uint8_t)(1 << (i % 8));
        }
        CHECK(caught == 120, "crc catches every single-bit flip (%d/120)", caught);
        memset(b, 0xFF, 16);
        CHECK(!hrec_unpack(b, &q), "erased slot is not a record");
    }

    /* fresh log on junk */
    hlog_init(&FL, 0xA1B2C3D4u);
    struct hlog_info i0 = info();
    CHECK(i0.log_id == 0xA1B2C3D4u && i0.first_seq == 1 && i0.next_seq == 1 &&
          i0.acked_seq == 0, "new log %u %u %u", i0.first_seq, i0.next_seq, i0.acked_seq);
    add(10);
    uint32_t a, b;
    CHECK(read_all(0, &a, &b) == 10 && a == 1 && b == 10, "read 10 back");

    /* reboot keeps records and seq */
    hlog_init(&FL, 999);
    CHECK(info().log_id == 0xA1B2C3D4u && info().next_seq == 11, "reboot next=%u", info().next_seq);
    add(5);
    CHECK(read_all(1, &a, &b) == 15 && b == 15, "continues after reboot");

    /* ack survives reboot */
    hlog_ack(7);
    polls(3);
    hlog_init(&FL, 999);
    CHECK(info().acked_seq == 7, "acked after reboot %u", info().acked_seq);
    CHECK(read_all(8, &a, &b) == 8 && a == 8, "resume from acked+1");

    /* ack beyond the end is clamped */
    hlog_ack(1000);
    CHECK(info().acked_seq == 15, "ack clamp %u", info().acked_seq);

    /* fill past three sectors, ack part: fully acked sectors are erased */
    add(800 - 15);                       /* seq 1..800, sectors 0..3 */
    CHECK(info().next_seq == 801, "next 801 got %u", info().next_seq);
    CHECK(read_all(1, &a, &b) == 800, "800 across sectors");
    hlog_ack(600);
    polls(10);
    struct hlog_info i1 = info();
    CHECK(i1.first_seq == 511 && i1.acked_seq == 600, "after delete first=%u acked=%u",
          i1.first_seq, i1.acked_seq);
    int ff = 1;
    for (unsigned k = 0; k < 2 * 4096; k++)
        ff &= flash[HLOG_BASE + k] == 0xFF;
    CHECK(ff, "sectors 0 and 1 wiped");
    CHECK(read_all(1, &a, &b) == 290 && a == 511 && b == 800, "read from oldest kept (%u..%u)", a, b);
    hlog_init(&FL, 999);
    i1 = info();
    CHECK(i1.first_seq == 511 && i1.next_seq == 801 && i1.acked_seq == 600,
          "reboot after delete %u %u %u", i1.first_seq, i1.next_seq, i1.acked_seq);

    /* torn record is skipped, nothing else lost */
    {
        struct hrec r[11];
        uint32_t nx;
        uint32_t off = 700 - 511;                 /* sector 2 slot... */
        uint32_t addr = HLOG_BASE + (2 + off / 255) * 4096 + (off % 255 + 1) * 16;
        flash[addr + 5] &= 0x0F;
        int n = hlog_read(699, r, 3, &nx);
        CHECK(n == 3 && r[0].seq == 699 && r[1].seq == 701 && r[2].seq == 702 && nx == 703,
              "torn skipped n=%d nx=%u", n, nx);
    }

    /* ack journal wraps: 300 acks */
    for (uint32_t s = 601; s <= 800; s++) {
        hlog_ack(s);
        polls(2);
    }
    add(200);
    for (uint32_t s = 801; s <= 900; s++) {
        hlog_ack(s);
        polls(2);
    }
    polls(10);
    hlog_init(&FL, 999);
    CHECK(info().acked_seq == 900, "journal wrap acked=%u", info().acked_seq);

    /* lost journal: acked drops back, records resent, never deleted early */
    f_erase(HLOG_ACK_ADDR);
    hlog_init(&FL, 999);
    struct hlog_info i2 = info();
    CHECK(i2.acked_seq == i2.first_seq - 1, "lost journal acked=%u first=%u",
          i2.acked_seq, i2.first_seq);

    /* never synced: the ring overwrites the oldest and counts it */
    hlog_ack(info().next_seq - 1);
    polls(600);
    uint32_t start = info().next_seq;
    int before = erases;
    add(HLOG_CAPACITY + 300);
    struct hlog_info i3 = info();
    CHECK(i3.next_seq == start + HLOG_CAPACITY + 300, "ring next");
    CHECK(i3.first_seq > start &&
          i3.dropped == HLOG_CAPACITY + 300 - (i3.next_seq - i3.first_seq),
          "dropped=%u stored=%u", i3.dropped, i3.next_seq - i3.first_seq);
    CHECK(i3.next_seq - i3.first_seq <= HLOG_CAPACITY &&
          i3.next_seq - i3.first_seq > HLOG_CAPACITY - 255,
          "ring holds a full ring (%u)", i3.next_seq - i3.first_seq);
    CHECK(read_all(0, &a, &b) == (int)(i3.next_seq - i3.first_seq) && a == i3.first_seq,
          "whole ring readable");
    CHECK(erases - before <= 258, "one erase per sector (%d)", erases - before);
    hlog_init(&FL, 999);
    struct hlog_info i4 = info();
    CHECK(i4.first_seq == i3.first_seq && i4.next_seq == i3.next_seq,
          "full ring reboot %u..%u vs %u..%u", i4.first_seq, i4.next_seq,
          i3.first_seq, i3.next_seq);

    /* acknowledge everything: all but the sector being written is wiped */
    hlog_ack(i4.next_seq - 1);
    polls(1000);
    struct hlog_info i5 = info();
    CHECK(i5.next_seq - i5.first_seq <= 255, "all acked -> one sector left (%u)",
          i5.next_seq - i5.first_seq);
    hlog_init(&FL, 999);
    CHECK(info().next_seq == i5.next_seq && info().log_id == 0xA1B2C3D4u,
          "seq survives a wiped log");

    printf("hlog: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
