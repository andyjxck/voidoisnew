/* rpa_match.h — does a resolvable private address belong to a bonded
   peer? (2.2.5)

   An iPhone never connects from its real (identity) address: it uses a
   resolvable private address that changes every few minutes, and hands
   over its IRK when it pairs. Our controller is built without LL privacy,
   so nothing turned that address back into the identity the keys are
   saved under. After a power-off or an update the phone came back on a
   new address, the watch found no key for it, refused encryption, and
   iOS said "peer removed pairing information".

   ah(k, r) = e(k, 0^104 || r) mod 2^24 (Core spec Vol 3 Part H 2.2.2).
   The IRK is little-endian as SMP sends it; the address is the 6 bytes
   as on air (val[0] first): val[0..2] hash, val[3..5] prand.
   The AES is passed in so this header builds on the host for tests. */
#ifndef RPA_MATCH_H
#define RPA_MATCH_H

#include <stdint.h>
#include <string.h>

/* plain AES-128 in the usual big-endian byte order; 0 on success */
typedef int (*rpa_aes_fn)(const uint8_t key[16], const uint8_t in[16],
                          uint8_t out[16]);

static inline int rpa_is_resolvable(const uint8_t a[6])
{
    return (a[5] & 0xC0) == 0x40;
}

static inline int rpa_matches(const uint8_t irk_le[16], const uint8_t a[6],
                              rpa_aes_fn aes)
{
    uint8_t k[16], p[16], o[16];
    if (!rpa_is_resolvable(a))
        return 0;
    for (int i = 0; i < 16; i++)
        k[i] = irk_le[15 - i];
    memset(p, 0, sizeof p);
    p[13] = a[5];
    p[14] = a[4];
    p[15] = a[3];
    if (aes(k, p, o) != 0)
        return 0;
    return o[15] == a[0] && o[14] == a[1] && o[13] == a[2];
}

#endif
