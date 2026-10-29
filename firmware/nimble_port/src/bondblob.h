/* Persistent bond storage format (pure C, host-testable).
 *
 * Two 4 KB sectors of the external SPI flash hold alternating copies:
 *   BONDBLOB_SLOT_A 0x3F3000, BONDBLOB_SLOT_B 0x3F4000
 * They sit inside the 64 KB block 0x3F0000-0x3FFFFF, next to the
 * settings sector (0x3F0000, untouched), above the planned history log
 * (0x2F0000-0x3EFFFF) and far from the bootloader's regions and the
 * update slot (0x040000-0x0B3FFF).
 *
 * Each copy: a 16-byte header, then records (type, len, bytes).
 *   magic 'VBN1' | seq u32 | payload_len u16 | layout u16 | crc32 u32
 * The newer valid copy (higher seq, good CRC, same layout) wins; a save
 * erases and writes the other sector, so a power cut mid-write leaves
 * the previous copy intact.
 */
#ifndef BONDBLOB_H
#define BONDBLOB_H
#include <stdint.h>
#include <stddef.h>

#define BONDBLOB_SLOT_A   0x3F3000u
#define BONDBLOB_SLOT_B   0x3F4000u
#define BONDBLOB_SECTOR   4096u
#define BONDBLOB_MAGIC    0x314E4256u   /* "VBN1" little-endian */
#define BONDBLOB_HDR      16u
#define BONDBLOB_MAX      1024u         /* header + payload */

struct bondblob_flash {
    int (*read)(uint32_t addr, void *buf, uint32_t len);
    int (*erase)(uint32_t addr);                       /* one sector */
    int (*write)(uint32_t addr, const void *buf, uint32_t len);
};

uint32_t bondblob_crc32(const uint8_t *p, uint32_t n);

/* builder over a caller buffer of BONDBLOB_MAX bytes */
struct bondblob_builder { uint8_t *buf; uint32_t len; int overflow; };
void bondblob_begin(struct bondblob_builder *b, uint8_t *buf);
void bondblob_add(struct bondblob_builder *b, uint8_t type,
                  const void *data, uint8_t len);

/* Find the newest valid copy and read it into buf (BONDBLOB_MAX bytes).
 * Returns the payload length (0 allowed) or -1 when neither copy is
 * valid. *seq gets its sequence number (0 when none). */
int bondblob_load(const struct bondblob_flash *f, uint16_t layout,
                  uint8_t *buf, uint32_t *seq);

/* Walk the records of a loaded payload (buf + BONDBLOB_HDR). The
 * callback returns nonzero to stop. Returns records walked, or -1 if a
 * record runs past the end. */
int bondblob_walk(const uint8_t *payload, uint32_t len,
                  int (*cb)(uint8_t type, const uint8_t *d, uint8_t n,
                            void *ctx), void *ctx);

/* Seal the builder's buffer with seq/layout/crc and write it to the slot
 * that does not hold copy number cur_seq. Returns the new seq, or 0 on a
 * flash error / overflow. */
uint32_t bondblob_save(const struct bondblob_flash *f, uint16_t layout,
                       struct bondblob_builder *b, uint32_t cur_seq,
                       uint32_t cur_slot);

/* the slot that holds copy cur_seq after load/save (A or B) */
uint32_t bondblob_slot_of(const struct bondblob_flash *f, uint16_t layout,
                          uint32_t seq);
#endif
