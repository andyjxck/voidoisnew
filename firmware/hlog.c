/* hlog: history outbox, see hlog.h */
#include "hlog.h"
#include <string.h>

static const struct hlog_flash *F;
static uint32_t log_id;
static unsigned head, tail;          /* sector indexes */
static uint32_t head_base, tail_base; /* seq of slot 1 in each */
static unsigned head_slot;           /* next free slot, 256 = full */
static uint32_t acked, dropped;
static uint8_t erased[HLOG_SECTORS / 8];   /* known erased since boot */
static unsigned ack_slot;            /* next free journal slot */
static int ack_dirty;

#define SLOTS 256u

static uint32_t sec_addr(unsigned s, unsigned slot)
{
    return HLOG_BASE + s * HLOG_SECTOR + slot * HLOG_REC;
}
static int is_erased(unsigned s) { return erased[s >> 3] >> (s & 7) & 1; }
static void set_erased(unsigned s, int on)
{
    if (on)
        erased[s >> 3] |= (uint8_t)(1u << (s & 7));
    else
        erased[s >> 3] &= (uint8_t)~(1u << (s & 7));
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int all_ff(const uint8_t *p, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (p[i] != 0xFF)
            return 0;
    return 1;
}

uint8_t hlog_crc8(const uint8_t *p, unsigned n)
{
    uint8_t c = 0xFF;
    for (unsigned i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

static uint8_t rec_crc(const uint8_t b[16])
{
    uint8_t t[15];
    memcpy(t, b, 9);
    memcpy(t + 9, b + 10, 6);
    return hlog_crc8(t, 15);
}

void hrec_pack(const struct hrec *r, uint8_t o[16])
{
    put32(o, r->seq);
    put32(o + 4, r->t);
    o[8] = r->type;
    o[10] = (uint8_t)r->a;
    o[11] = (uint8_t)(r->a >> 8);
    put32(o + 12, r->b);
    o[9] = rec_crc(o);
}

int hrec_unpack(const uint8_t in[16], struct hrec *r)
{
    r->seq = get32(in);
    r->t = get32(in + 4);
    r->type = in[8];
    r->crc = in[9];
    r->a = (uint16_t)(in[10] | in[11] << 8);
    r->b = get32(in + 12);
    if (r->seq == 0xFFFFFFFFu || r->type == 0xFF || r->seq == 0)
        return 0;
    return rec_crc(in) == in[9];
}

/* sector header: {magic, base_seq, log_id, ~magic} */
static int read_hdr(unsigned s, uint32_t *base, uint32_t *id)
{
    uint8_t b[16];
    F->read(sec_addr(s, 0), b, 16);
    if (get32(b) != HLOG_HDR_MAGIC || get32(b + 12) != ~HLOG_HDR_MAGIC)
        return 0;
    *base = get32(b + 4);
    *id = get32(b + 8);
    return *base >= 1 && (*base - 1) % HLOG_PER_SECTOR == 0;
}

static void erase_now(uint32_t addr)
{
    F->erase_start(addr);
    while (F->busy())
        ;
}

static void start_sector(unsigned s, uint32_t base)
{
    if (!is_erased(s))
        erase_now(sec_addr(s, 0));
    uint8_t b[16];
    put32(b, HLOG_HDR_MAGIC);
    put32(b + 4, base);
    put32(b + 8, log_id);
    put32(b + 12, ~HLOG_HDR_MAGIC);
    F->write(sec_addr(s, 0), b, 16);
    set_erased(s, 0);
}

static uint32_t next_seq(void) { return head_base + head_slot - 1; }

void hlog_init(const struct hlog_flash *fl, uint32_t seed)
{
    F = fl;
    memset(erased, 0, sizeof erased);
    acked = dropped = 0;
    ack_dirty = 0;
    int found = 0;
    uint32_t best = 0, best_id = 0;
    for (unsigned s = 0; s < HLOG_SECTORS; s++) {
        uint32_t base, id;
        if (read_hdr(s, &base, &id) && (!found || base > best)) {
            found = 1;
            best = base;
            best_id = id;
            head = s;
        }
    }
    if (!found) {
        log_id = seed ? seed : 0x56484C31u;
        head = tail = 0;
        head_base = tail_base = 1;
        start_sector(0, 1);
        head_slot = 1;
    } else {
        log_id = best_id;
        head_base = best;
        head_slot = SLOTS;
        uint8_t b[16];
        for (unsigned i = 1; i < SLOTS; i++) {
            F->read(sec_addr(head, i), b, 16);
            if (all_ff(b, 16)) {
                head_slot = i;
                break;
            }
        }
        /* walk back while the chain is unbroken */
        tail = head;
        tail_base = head_base;
        for (unsigned k = 1; k < HLOG_SECTORS; k++) {
            unsigned s = (head + HLOG_SECTORS - k) % HLOG_SECTORS;
            uint32_t base, id;
            if (!read_hdr(s, &base, &id) || id != log_id ||
                base + HLOG_PER_SECTOR != tail_base)
                break;
            tail = s;
            tail_base = base;
        }
    }
    /* ack journal */
    ack_slot = HLOG_ACK_SLOTS;
    for (unsigned i = 0; i < HLOG_ACK_SLOTS; i++) {
        uint8_t b[16];
        F->read(HLOG_ACK_ADDR + i * 16u, b, 16);
        if (all_ff(b, 16)) {
            ack_slot = i;
            break;
        }
        if (get32(b) == HLOG_ACK_MAGIC && get32(b + 4) == log_id &&
            get32(b + 8) == ~get32(b + 12))
            acked = get32(b + 8);
    }
    if (acked >= next_seq())
        acked = next_seq() - 1;
}

/* move to the next sector; drops the oldest when the ring is full */
static void advance(void)
{
    unsigned nx = (head + 1) % HLOG_SECTORS;
    if (nx == tail) {
        uint32_t last = tail_base + HLOG_PER_SECTOR - 1;
        if (acked < last) {
            uint32_t from = acked >= tail_base ? acked + 1 : tail_base;
            dropped += last - from + 1;
        }
        tail = (tail + 1) % HLOG_SECTORS;
        tail_base += HLOG_PER_SECTOR;
    }
    start_sector(nx, head_base + HLOG_PER_SECTOR);
    head = nx;
    head_base += HLOG_PER_SECTOR;
    head_slot = 1;
}

int hlog_append(uint32_t t, uint8_t type, uint16_t a, uint32_t b)
{
    if (!F)
        return -1;
    if (head_slot >= SLOTS)
        advance();
    struct hrec r = {next_seq(), t, type, 0, a, b};
    uint8_t o[16];
    hrec_pack(&r, o);
    F->write(sec_addr(head, head_slot), o, 16);
    head_slot++;
    return 0;
}

void hlog_ack(uint32_t seq)
{
    if (!F)
        return;
    if (seq >= next_seq())
        seq = next_seq() - 1;
    if (seq > acked) {
        acked = seq;
        ack_dirty = 1;
    }
}

void hlog_poll(void)
{
    if (!F || F->busy())
        return;
    if (ack_dirty) {
        if (ack_slot >= HLOG_ACK_SLOTS) {
            F->erase_start(HLOG_ACK_ADDR);   /* written next poll */
            ack_slot = 0;
            return;
        }
        uint8_t b[16];
        put32(b, HLOG_ACK_MAGIC);
        put32(b + 4, log_id);
        put32(b + 8, acked);
        put32(b + 12, ~acked);
        F->write(HLOG_ACK_ADDR + ack_slot * 16u, b, 16);
        ack_slot++;
        ack_dirty = 0;
        return;
    }
    /* the phone has everything in the oldest sector: delete it */
    if (tail != head && acked >= tail_base + HLOG_PER_SECTOR - 1) {
        F->erase_start(sec_addr(tail, 0));
        set_erased(tail, 1);
        tail = (tail + 1) % HLOG_SECTORS;
        tail_base += HLOG_PER_SECTOR;
        return;
    }
    /* get the next sector ready while the head fills */
    unsigned nx = (head + 1) % HLOG_SECTORS;
    if (head_slot >= SLOTS / 2 && nx != tail && !is_erased(nx)) {
        F->erase_start(sec_addr(nx, 0));
        set_erased(nx, 1);
    }
}

int hlog_read(uint32_t seq, struct hrec *out, int max, uint32_t *next)
{
    int n = 0;
    if (!F) {
        *next = seq;
        return 0;
    }
    uint32_t end = next_seq();
    if (seq < tail_base)
        seq = tail_base;
    uint8_t buf[16 * HLOG_REC];
    while (n < max && seq < end) {
        uint32_t off = seq - tail_base;
        unsigned s = (tail + off / HLOG_PER_SECTOR) % HLOG_SECTORS;
        unsigned slot = off % HLOG_PER_SECTOR + 1;
        unsigned run = SLOTS - slot;
        if (run > (unsigned)(max - n))
            run = (unsigned)(max - n);
        if (run > end - seq)
            run = end - seq;
        if (run > 16)
            run = 16;
        F->read(sec_addr(s, slot), buf, run * HLOG_REC);
        for (unsigned i = 0; i < run; i++, seq++) {
            struct hrec r;
            if (hrec_unpack(buf + i * HLOG_REC, &r) && r.seq == seq)
                out[n++] = r;
        }
    }
    *next = seq;
    return n;
}

void hlog_get_info(struct hlog_info *inf)
{
    inf->log_id = log_id;
    inf->first_seq = tail_base;
    inf->next_seq = F ? next_seq() : 1;
    inf->acked_seq = acked < tail_base ? tail_base - 1 : acked;
    inf->dropped = dropped;
}
