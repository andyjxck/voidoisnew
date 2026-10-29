#include "bondblob.h"
#include <string.h>

uint32_t bondblob_crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v)
{ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t get32(const uint8_t *p)
{ return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

void bondblob_begin(struct bondblob_builder *b, uint8_t *buf)
{
    b->buf = buf;
    b->len = BONDBLOB_HDR;
    b->overflow = 0;
    memset(buf, 0, BONDBLOB_HDR);
}

void bondblob_add(struct bondblob_builder *b, uint8_t type,
                  const void *data, uint8_t len)
{
    if (b->len + 2u + len > BONDBLOB_MAX) { b->overflow = 1; return; }
    b->buf[b->len++] = type;
    b->buf[b->len++] = len;
    memcpy(b->buf + b->len, data, len);
    b->len += len;
}

/* header check of one slot; on success leaves the whole copy in buf */
static int slot_valid(const struct bondblob_flash *f, uint32_t addr,
                      uint16_t layout, uint8_t *buf, uint32_t *seq)
{
    if (f->read(addr, buf, BONDBLOB_HDR) != 0) return -1;
    if (get32(buf) != BONDBLOB_MAGIC) return -1;
    uint32_t len = get16(buf + 8);
    if (get16(buf + 10) != layout) return -1;
    if (len > BONDBLOB_MAX - BONDBLOB_HDR) return -1;
    if (f->read(addr + BONDBLOB_HDR, buf + BONDBLOB_HDR, len) != 0) return -1;
    if (bondblob_crc32(buf + BONDBLOB_HDR, len) != get32(buf + 12)) return -1;
    *seq = get32(buf + 4);
    if (*seq == 0 || *seq == 0xFFFFFFFFu) return -1;
    return (int)len;
}

int bondblob_load(const struct bondblob_flash *f, uint16_t layout,
                  uint8_t *buf, uint32_t *seq)
{
    uint32_t sa = 0, sb = 0;
    *seq = 0;
    int lb = slot_valid(f, BONDBLOB_SLOT_B, layout, buf, &sb);
    int la = slot_valid(f, BONDBLOB_SLOT_A, layout, buf, &sa);
    if (la < 0 && lb < 0) return -1;
    if (la >= 0 && (lb < 0 || sa > sb)) {   /* A is in buf already */
        *seq = sa;
        return la;
    }
    *seq = sb;
    return slot_valid(f, BONDBLOB_SLOT_B, layout, buf, &sb);
}

int bondblob_walk(const uint8_t *payload, uint32_t len,
                  int (*cb)(uint8_t, const uint8_t *, uint8_t, void *),
                  void *ctx)
{
    uint32_t i = 0;
    int n = 0;
    while (i < len) {
        if (i + 2u > len) return -1;
        uint8_t type = payload[i], l = payload[i + 1];
        if (i + 2u + l > len) return -1;
        n++;
        if (cb && cb(type, payload + i + 2, l, ctx)) break;
        i += 2u + l;
    }
    return n;
}

uint32_t bondblob_slot_of(const struct bondblob_flash *f, uint16_t layout,
                          uint32_t seq)
{
    static uint8_t hdr[BONDBLOB_HDR];
    if (seq && f->read(BONDBLOB_SLOT_A, hdr, BONDBLOB_HDR) == 0 &&
        get32(hdr) == BONDBLOB_MAGIC && get16(hdr + 10) == layout &&
        get32(hdr + 4) == seq)
        return BONDBLOB_SLOT_A;
    return seq ? BONDBLOB_SLOT_B : 0;
}

uint32_t bondblob_save(const struct bondblob_flash *f, uint16_t layout,
                       struct bondblob_builder *b, uint32_t cur_seq,
                       uint32_t cur_slot)
{
    if (b->overflow) return 0;
    uint32_t seq = cur_seq + 1;
    if (seq == 0xFFFFFFFFu) seq = 1;
    uint32_t len = b->len - BONDBLOB_HDR;
    put32(b->buf, BONDBLOB_MAGIC);
    put32(b->buf + 4, seq);
    put16(b->buf + 8, len);
    put16(b->buf + 10, layout);
    put32(b->buf + 12, bondblob_crc32(b->buf + BONDBLOB_HDR, len));
    uint32_t dst = cur_slot == BONDBLOB_SLOT_A ? BONDBLOB_SLOT_B
                                               : BONDBLOB_SLOT_A;
    if (f->erase(dst) != 0) return 0;
    if (f->write(dst, b->buf, b->len) != 0) return 0;
    /* read it back before calling it saved */
    uint8_t chk[32];
    for (uint32_t o = 0; o < b->len; o += sizeof chk) {
        uint32_t n = b->len - o < sizeof chk ? b->len - o : sizeof chk;
        if (f->read(dst + o, chk, n) != 0 || memcmp(chk, b->buf + o, n))
            return 0;
    }
    return seq;
}
