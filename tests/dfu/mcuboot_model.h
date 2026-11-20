/* 2.4.1: a small model of what the PineTime's MCUBoot does with the
   secondary slot after an update from the running OS: reads the image
   header and the TLVs, checks the SHA-256, reads the trailer and decides
   the swap type. Used by test_ble_dfu to check the whole handoff, from
   the first DFU packet to the bootloader booting the new image.

   Layout (same as the bootloader and as imgtool --pad-header 32):
   secondary slot at external flash 0x40000, 475136 bytes; trailer magic
   in the last 16 bytes; image_ok, copy_done and swap_info just before it
   in 8-byte aligned fields; the swap status area before those. */
#ifndef MCUBOOT_MODEL_H
#define MCUBOOT_MODEL_H
#include <stdint.h>
#include <string.h>

/* ---- SHA-256 (FIPS 180-4), compact -------------------------------------- */
static const uint32_t mb_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define MB_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void mb_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, hh;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 |
               (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = MB_ROR(w[i-15], 7) ^ MB_ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = MB_ROR(w[i-2], 17) ^ MB_ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (MB_ROR(e, 6) ^ MB_ROR(e, 11) ^ MB_ROR(e, 25)) +
                      ((e & f) ^ (~e & g)) + mb_k[i] + w[i];
        uint32_t t2 = (MB_ROR(a, 2) ^ MB_ROR(a, 13) ^ MB_ROR(a, 22)) +
                      ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
static void mb_sha256(const uint8_t *m, uint32_t n, uint8_t out[32])
{
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint32_t i = 0;
    for (; i + 64 <= n; i += 64)
        mb_block(h, m + i);
    uint8_t t[128] = {0};
    uint32_t r = n - i;
    memcpy(t, m + i, r);
    t[r] = 0x80;
    uint32_t L = r + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int k = 0; k < 8; k++)
        t[L - 1 - k] = (uint8_t)(bits >> (8 * k));
    mb_block(h, t);
    if (L == 128)
        mb_block(h, t + 64);
    for (int k = 0; k < 8; k++) {
        out[4*k] = h[k] >> 24; out[4*k+1] = h[k] >> 16;
        out[4*k+2] = h[k] >> 8; out[4*k+3] = h[k];
    }
}

/* ---- the bootloader's decision ------------------------------------------ */
#define MB_SLOT_SIZE   475136u
#define MB_HDR_MAGIC   0x96f3b83du
#define MB_TLV_INFO    0x6907u
#define MB_TLV_SHA256  0x10u
#define MB_ALIGN       8u      /* BOOT_MAX_ALIGN of the PineTime build */

enum { MB_NONE = 1, MB_TEST = 2, MB_PERM = 3, MB_FAIL = 5 };

static uint32_t mb_le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* why: set to a short reason when the result is MB_FAIL or MB_NONE */
static int mcuboot_decide(const uint8_t *slot, const char **why)
{
    static const uint8_t magic[16] = {0x77,0xc2,0x95,0xf3,0x60,0xd2,0xef,0x7f,
                                      0x35,0x52,0x50,0x0f,0x2c,0xb6,0x79,0x80};
    const uint8_t *tr = slot + MB_SLOT_SIZE;
    *why = "";
    if (memcmp(tr - 16, magic, 16) != 0) {
        *why = "no trailer magic: no swap requested";
        return MB_NONE;
    }
    uint8_t image_ok = *(tr - 16 - MB_ALIGN);
    uint8_t copy_done = *(tr - 16 - 2 * MB_ALIGN);
    uint8_t swap_info = *(tr - 16 - 3 * MB_ALIGN);
    if (copy_done != 0xFF || swap_info != 0xFF) {
        *why = "stale copy_done/swap_info in the trailer";
        return MB_FAIL;
    }
    /* swap status area (3 states x max sectors x align) must be erased,
       or MCUBoot thinks a swap was interrupted and resumes it */
    const uint32_t status = 3u * 128u * MB_ALIGN;
    for (const uint8_t *p = tr - 16 - 3 * MB_ALIGN - status;
         p < tr - 16 - 3 * MB_ALIGN; p++)
        if (*p != 0xFF) {
            *why = "swap status area not erased";
            return MB_FAIL;
        }
    /* image header */
    if (mb_le32(slot) != MB_HDR_MAGIC) {
        *why = "bad image header magic";
        return MB_FAIL;
    }
    uint32_t load = mb_le32(slot + 4);
    uint16_t hsz = slot[8] | slot[9] << 8;
    uint16_t prot = slot[10] | slot[11] << 8;
    uint32_t isz = mb_le32(slot + 12);
    if (load != 0 || hsz != 32) {
        *why = "header: load address or header size not what the bootloader expects";
        return MB_FAIL;
    }
    uint32_t tlv = hsz + isz + prot;
    if (tlv + 4 > MB_SLOT_SIZE - 16 - 3 * MB_ALIGN - status) {
        *why = "image runs into the trailer";
        return MB_FAIL;
    }
    /* the vector table the bootloader jumps to */
    uint32_t sp = mb_le32(slot + hsz), rv = mb_le32(slot + hsz + 4);
    if (sp < 0x20000000u || sp > 0x20010000u || (rv & 1) == 0 ||
        rv < 0x8020u || rv >= 0x8020u + isz) {
        *why = "vector table: stack or reset vector out of range";
        return MB_FAIL;
    }
    if ((slot[tlv] | slot[tlv + 1] << 8) != MB_TLV_INFO) {
        *why = "no TLV info block after the image";
        return MB_FAIL;
    }
    uint16_t tlen = slot[tlv + 2] | slot[tlv + 3] << 8;
    int hash_ok = 0;
    for (uint32_t p = tlv + 4; p + 4 <= tlv + tlen;) {
        uint8_t type = slot[p];
        uint16_t l = slot[p + 2] | slot[p + 3] << 8;
        if (type == MB_TLV_SHA256 && l == 32) {
            uint8_t h[32];
            mb_sha256(slot, hsz + isz + prot, h);
            hash_ok = memcmp(h, slot + p + 4, 32) == 0;
        }
        p += 4 + l;
    }
    if (!hash_ok) {
        *why = "SHA-256 of the image doesn't match its TLV";
        return MB_FAIL;
    }
    return image_ok == 0x01 ? MB_PERM : MB_TEST;
}
#endif
