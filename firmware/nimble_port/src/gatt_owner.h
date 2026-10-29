/* Which characteristic owns a descriptor (pure C, host-tested).
 *
 * NimBLE's ble_gattc_disc_all_dscs() hands every descriptor callback
 * the same chr_val_handle: the start handle we passed in. When we
 * discover over a whole service range that is the service start, never
 * one of the characteristic value handles, so comparing it against the
 * NS/DS/Entity Update value handles can never match (2.2.0 never found
 * a CCCD this way: ANCS gave up, AMS retried four times and stopped).
 *
 * A descriptor sits after its characteristic's value handle and before
 * the next characteristic's declaration. So record every characteristic
 * found during discovery (declaration + value handle, known or not), and
 * a descriptor belongs to the one with the closest value handle below
 * it, unless another characteristic declaration sits in between.
 */
#ifndef GATT_OWNER_H
#define GATT_OWNER_H
#include <stdint.h>

#define GATT_SPANS_MAX 8

struct gatt_spans {
    uint8_t n;
    struct { uint16_t def, val; } c[GATT_SPANS_MAX];
};

static inline void gatt_spans_reset(struct gatt_spans *s) { s->n = 0; }

static inline void gatt_spans_add(struct gatt_spans *s, uint16_t def,
                                  uint16_t val)
{
    if (s->n < GATT_SPANS_MAX) {
        s->c[s->n].def = def;
        s->c[s->n].val = val;
        s->n++;
    }
}

/* value handle of the characteristic that owns descriptor h, 0 if none */
static inline uint16_t gatt_owner_of(const struct gatt_spans *s, uint16_t h)
{
    uint16_t best = 0;
    for (int i = 0; i < s->n; i++)
        if (s->c[i].val < h && s->c[i].val > best)
            best = s->c[i].val;
    if (!best)
        return 0;
    for (int i = 0; i < s->n; i++)
        if (s->c[i].def > best && s->c[i].def < h)
            return 0;          /* an unrecorded char's descriptor */
    return best;
}
#endif
