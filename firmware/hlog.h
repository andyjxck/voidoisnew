/* hlog: the watch's history outbox for the VoidHealth phone app.
 *
 * Everything the watch records (5-minute samples, water taps, workouts,
 * sleep, day totals) is appended here as 16-byte records with a rising
 * sequence number. The phone streams them over BLE (ble_hist.c), stores
 * them, then acknowledges a sequence number. Sectors whose records are
 * all acknowledged are erased: the phone holds the history, the watch
 * only holds what has not reached the phone yet.
 *
 * Layout: the 1 MiB region bondblob.h reserved for the history log,
 * 0x2F0000-0x3EFFFF, 256 sectors of 4 KB used as a ring. Slot 0 of every
 * sector is a header {magic, base_seq, log_id, ~magic}; slots 1..255 hold
 * records, so record seq = base_seq + slot - 1. The acknowledged seq is
 * journalled in its own sector (0x3F2000, unused until now) as 16-byte
 * entries {magic, log_id, acked, ~acked}.
 *
 * Power loss is safe in the direction that matters: a lost ack journal
 * only makes the phone receive records again (it drops duplicates by
 * log_id + seq); a record is never erased before its ack is known.
 * If the phone never syncs, the ring holds 65,280 records (about 220
 * days of 5-minute samples) and then overwrites the oldest, counting
 * what it dropped.
 *
 * Pure C on a flash interface so tests/hlog runs it on the host. */
#ifndef VOIDOS_HLOG_H
#define VOIDOS_HLOG_H

#include <stdint.h>

#define HLOG_BASE        0x2F0000u
#define HLOG_SECTOR      4096u
#define HLOG_SECTORS     256u
#define HLOG_REC         16u
#define HLOG_PER_SECTOR  255u            /* slot 0 is the header */
#define HLOG_CAPACITY    (HLOG_SECTORS * HLOG_PER_SECTOR)
#define HLOG_ACK_ADDR    0x3F2000u
#define HLOG_ACK_SLOTS   (HLOG_SECTOR / 16u)

#define HLOG_HDR_MAGIC   0x314C4856u     /* "VHL1" */
#define HLOG_ACK_MAGIC   0x314B4156u     /* "VAK1" */

/* record types (low 7 bits of type; bit 7 = clock was restored from
   flash, not set by the phone, so the time may be off) */
enum {
    HREC_SAMPLE  = 1,  /* t = start of a 5-min slot; a = steps in it;
                          b = avg bpm | max bpm<<8 | batt%<<16 (bit 23 =
                          charging) | moving 2-s units<<24 (0..150) */
    HREC_WATER   = 2,  /* t = tap time; a = (int16) ml added (negative =
                          undo); b = day total ml after it */
    HREC_WORKOUT = 3,  /* t = start; a = active seconds (capped 65535);
                          b = sets | routine<<8 (0xFF quick) |
                          avg bpm<<16 | max bpm<<24 */
    HREC_SLEEP   = 4,  /* t = bedtime; a = 0; b = seconds in bed */
    HREC_DAY     = 5,  /* t = local midnight of the day; a = water ml;
                          b = steps (24 bits) | lowest resting bpm<<24 */
};
#define HREC_F_CLOCK_GUESS 0x80u

/* t is seconds since 2020-01-01 00:00 LOCAL wall time */
#define HLOG_EPOCH_KEY   20200101u

struct hrec {
    uint32_t seq;
    uint32_t t;
    uint8_t  type;
    uint8_t  crc;      /* crc8 over the other 15 bytes */
    uint16_t a;
    uint32_t b;
};

struct hlog_flash {
    void (*read)(uint32_t addr, uint8_t *buf, unsigned len);
    void (*write)(uint32_t addr, const uint8_t *buf, unsigned len);
    void (*erase_start)(uint32_t addr);   /* returns at once */
    int  (*busy)(void);                   /* 1 while an erase runs */
};

struct hlog_info {
    uint32_t log_id, first_seq, next_seq, acked_seq, dropped;
};

/* scan flash; creates a new log (id = seed) when none is found */
void hlog_init(const struct hlog_flash *fl, uint32_t seed);
/* append one record (seq and crc are filled in); 0 on success */
int hlog_append(uint32_t t, uint8_t type, uint16_t a, uint32_t b);
/* the phone has stored everything up to and including seq */
void hlog_ack(uint32_t seq);
/* background work: pre-erase the next sector, erase acknowledged
   sectors, write the ack journal. Call often; never blocks on erase. */
void hlog_poll(void);
/* read up to max records from seq on (seq below the oldest stored is
   moved up to it). Returns how many; *next = the seq to ask for next.
   Torn records are skipped. */
int hlog_read(uint32_t seq, struct hrec *out, int max, uint32_t *next);
void hlog_get_info(struct hlog_info *inf);

/* little-endian wire form, 16 bytes */
void hrec_pack(const struct hrec *r, uint8_t out[16]);
int hrec_unpack(const uint8_t in[16], struct hrec *r);   /* 1 = crc ok */
uint8_t hlog_crc8(const uint8_t *p, unsigned n);

#endif
