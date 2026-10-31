/* 2.4.2: automatic sleep detection (firmware/sleepdet.h) on synthetic
   nights built from 5-minute slots, plus its flash journal on a
   simulated NOR flash. */
#include <stdio.h>
#include <string.h>
#include "sleepdet.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, \
                      __LINE__, #c); fails++; } } while (0)

#define DAY0  (2496u * 86400u)              /* a day, midnight */
#define HM(d, h, m) (DAY0 + (uint32_t)(d) * 86400u + (uint32_t)(h) * 3600u + \
                     (uint32_t)(m) * 60u)

/* ---- simulated flash: 2 sectors of the journal ---------------------- */
static uint8_t fl_mem[2 * SD_J_SECTOR];
static uint8_t fl_snap[2 * SD_J_SECTOR];
static int fl_busy_n, fl_erases, fl_writes;
static void fl_read(uint32_t a, uint8_t *b, unsigned n)
{ memcpy(b, fl_mem + (a - SD_J_BASE), n); }
static void fl_write(uint32_t a, const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        fl_mem[a - SD_J_BASE + i] &= b[i];       /* NOR: 1 -> 0 only */
    fl_writes++;
}
static void fl_erase(uint32_t a)
{
    memset(fl_mem + ((a - SD_J_BASE) & ~(SD_J_SECTOR - 1)), 0xFF, SD_J_SECTOR);
    fl_busy_n = 2;
    fl_erases++;
}
static int fl_busy(void) { if (fl_busy_n) { fl_busy_n--; return 1; } return 0; }
static const struct sd_flash FL = { fl_read, fl_write, fl_erase, fl_busy };

/* ---- the watch -------------------------------------------------------- */
struct sim {
    struct sd_state s;
    struct sd_journal j;
    struct sd_night nights[8];
    int n;
    uint32_t ms;           /* uptime */
    int journal;
    int hr_on;
    int rest;
};

static void sim_init(struct sim *w, int journal)
{
    memset(w, 0, sizeof *w);
    w->hr_on = 1;
    w->rest = 58;
    w->ms = 5000;
    w->journal = journal;
    if (journal) {
        memset(fl_mem, 0xFF, sizeof fl_mem);
        sd_j_load(&w->j, &FL, &w->s);
    }
}

/* one slot at clock time t (slot start); `real` advances uptime */
static void slot(struct sim *w, uint32_t t, int steps, int move, int bpm,
                 int chg, int guess)
{
    struct sd_epoch e;
    struct sd_night out[2];
    w->ms += 300000u;
    e.t = t;
    e.ms = w->ms;
    e.steps = (uint16_t)steps;
    e.move = (uint8_t)move;
    e.bpm = (uint8_t)(w->hr_on ? bpm : 0);
    e.charging = (uint8_t)chg;
    e.hr_on = (uint8_t)w->hr_on;
    e.guess = (uint8_t)guess;
    e.rest = (uint8_t)w->rest;
    int k = sd_epoch(&w->s, &e, out);
    for (int i = 0; i < k && w->n < 8; i++)
        w->nights[w->n++] = out[i];
    if (w->journal) {
        sd_j_poll(&w->j);
        sd_j_save(&w->j, &w->s);
        sd_j_poll(&w->j);
    }
}

/* fill [from, to) with one kind of slot */
enum { AWAKE, UP, STILL, TOSS, TABLE, CHARGER, RESTLESS, SITTING };
static void fill(struct sim *w, uint32_t from, uint32_t to, int kind)
{
    for (uint32_t t = from; t < to; t += SD_EPOCH) {
        int i = (int)((t / SD_EPOCH) % 7);
        switch (kind) {
        case AWAKE:    slot(w, t, 120, 50, 76, 0, 0); break;
        case UP:       slot(w, t, 260, 90, 84, 0, 0); break;
        /* asleep: a twitch now and then, pulse missed in 1 of 7 */
        case STILL:    slot(w, t, 0, i == 3 ? 2 : (i == 5 ? 1 : 0),
                            i == 6 ? 0 : 54 + i, 0, 0); break;
        case TOSS:     slot(w, t, 0, 22, 66, 0, 0); break;   /* awake in bed */
        case RESTLESS: slot(w, t, 0, 9, 60, 0, 0); break;    /* below awake */
        case TABLE:    slot(w, t, 0, 0, 0, 0, 0); break;
        case CHARGER:  slot(w, t, 0, 0, 0, 1, 0); break;
        case SITTING:  slot(w, t, 4, 3, 82, 0, 0); break;    /* sofa, TV */
        }
    }
}

static int awake_min(const struct sd_night *n) { return n->a >> SD_A_AWAKE_SH; }

int main(void)
{
    struct sim w;

    /* ---- normal night: 23:00 to 07:00, one 5-minute wake at 03:00 ---- */
    sim_init(&w, 0);
    fill(&w, HM(0, 20, 0), HM(0, 23, 0), AWAKE);
    fill(&w, HM(0, 23, 0), HM(1, 3, 0), STILL);
    fill(&w, HM(1, 3, 0), HM(1, 3, 5), TOSS);
    fill(&w, HM(1, 3, 5), HM(1, 7, 0), STILL);
    CHECK(w.s.state == SD_ASLEEP);          /* the watch shows ASLEEP */
    CHECK(w.n == 0);
    fill(&w, HM(1, 7, 0), HM(1, 9, 0), UP);
    CHECK(w.n == 1);
    CHECK(w.nights[0].t == HM(0, 23, 0));
    CHECK(w.nights[0].secs == 8u * 3600u);
    CHECK(w.nights[0].a & SD_A_AUTO);
    CHECK(!(w.nights[0].a & SD_A_NAP));
    CHECK(awake_min(&w.nights[0]) == 5);
    CHECK(!w.nights[0].guess);
    CHECK(w.s.state == SD_IDLE);

    /* awake in bed from 06:00, up at 06:20: the night ends at 06:00 */
    sim_init(&w, 0);
    fill(&w, HM(0, 22, 0), HM(0, 23, 0), AWAKE);
    fill(&w, HM(0, 23, 0), HM(1, 6, 0), STILL);
    fill(&w, HM(1, 6, 0), HM(1, 6, 20), TOSS);
    fill(&w, HM(1, 6, 20), HM(1, 8, 0), UP);
    CHECK(w.n == 1);
    CHECK(w.nights[0].t + w.nights[0].secs == HM(1, 6, 0));
    CHECK(awake_min(&w.nights[0]) == 0);
    /* heart rate switched off: movement alone still finds the night */
    sim_init(&w, 0);
    w.hr_on = 0;
    fill(&w, HM(0, 22, 0), HM(0, 23, 0), AWAKE);
    fill(&w, HM(0, 23, 0), HM(1, 7, 0), STILL);
    fill(&w, HM(1, 7, 0), HM(1, 8, 0), UP);
    CHECK(w.n == 1);
    CHECK(w.nights[0].t == HM(0, 23, 0) && w.nights[0].secs == 8u * 3600u);

    /* ---- restless night: tossing every hour, a bathroom trip ---- */
    sim_init(&w, 0);
    fill(&w, HM(0, 21, 0), HM(0, 23, 30), AWAKE);
    int aw = 0;
    uint32_t t = HM(0, 23, 30);
    for (int h = 0; h < 7; h++) {           /* 23:30 .. 06:30 */
        fill(&w, t, t + 2400, STILL);
        fill(&w, t + 2400, t + 2700, RESTLESS);
        if (h == 3) {
            /* 03:15 up to the bathroom for 10 minutes, back by 03:25 */
            fill(&w, t + 2700, t + 3300, UP);
            aw += 2;
            fill(&w, t + 3300, t + 3600, STILL);
        } else {
            fill(&w, t + 2700, t + 3300, TOSS);   /* 10 min awake */
            aw += 2;
            fill(&w, t + 3300, t + 3600, STILL);
        }
        t += 3600;
    }
    fill(&w, HM(1, 6, 30), HM(1, 8, 30), UP);
    CHECK(w.n == 1);
    CHECK(w.nights[0].t == HM(0, 23, 30));
    CHECK(w.nights[0].secs == 7u * 3600u);
    CHECK(awake_min(&w.nights[0]) == aw * 5);
    CHECK(!(w.nights[0].a & SD_A_NAP));

    /* ---- watch on a table all night ---- */
    sim_init(&w, 0);
    fill(&w, HM(0, 18, 0), HM(0, 22, 0), AWAKE);
    fill(&w, HM(0, 22, 0), HM(1, 8, 0), TABLE);
    fill(&w, HM(1, 8, 0), HM(1, 9, 0), UP);
    CHECK(w.n == 0);
    CHECK(w.s.state == SD_IDLE);
    /* ... and with heart rate switched off */
    sim_init(&w, 0);
    w.hr_on = 0;
    fill(&w, HM(0, 22, 0), HM(1, 8, 0), TABLE);
    fill(&w, HM(1, 8, 0), HM(1, 9, 0), UP);
    CHECK(w.n == 0);

    /* ---- on the charger all night ---- */
    sim_init(&w, 0);
    fill(&w, HM(0, 18, 0), HM(0, 22, 0), AWAKE);
    fill(&w, HM(0, 22, 0), HM(1, 8, 0), CHARGER);
    fill(&w, HM(1, 8, 0), HM(1, 9, 0), UP);
    CHECK(w.n == 0);
    /* asleep, watch off at 06:20, on the charger at 06:35: ends 06:20 */
    sim_init(&w, 0);
    fill(&w, HM(0, 22, 0), HM(0, 23, 0), AWAKE);
    fill(&w, HM(0, 23, 0), HM(1, 6, 20), STILL);
    fill(&w, HM(1, 6, 20), HM(1, 6, 35), TABLE);
    CHECK(w.s.state == SD_ASLEEP);          /* 15 min: not enough yet */
    fill(&w, HM(1, 6, 35), HM(1, 8, 0), CHARGER);
    CHECK(w.n == 1);
    CHECK(w.nights[0].t == HM(0, 23, 0));
    CHECK(w.nights[0].t + w.nights[0].secs == HM(1, 6, 20));
    /* taken off at 06:20 and left on the bedside table */
    sim_init(&w, 0);
    fill(&w, HM(0, 22, 0), HM(0, 23, 0), AWAKE);
    fill(&w, HM(0, 23, 0), HM(1, 6, 20), STILL);
    fill(&w, HM(1, 6, 20), HM(1, 9, 0), TABLE);
    CHECK(w.n == 1);
    CHECK(w.nights[0].t + w.nights[0].secs == HM(1, 6, 20));

    /* ---- naps ---- */
    sim_init(&w, 0);
    fill(&w, HM(0, 9, 0), HM(0, 14, 0), AWAKE);
    fill(&w, HM(0, 14, 0), HM(0, 15, 20), STILL);    /* 80 min nap */
    fill(&w, HM(0, 15, 20), HM(0, 16, 30), UP);
    fill(&w, HM(0, 16, 30), HM(0, 17, 10), STILL);   /* 40 min: too short */
    fill(&w, HM(0, 17, 10), HM(0, 19, 30), AWAKE);
    fill(&w, HM(0, 19, 30), HM(0, 21, 30), SITTING); /* TV, pulse 82 */
    fill(&w, HM(0, 21, 30), HM(0, 23, 0), AWAKE);
    CHECK(w.n == 1);
    CHECK(w.nights[0].t == HM(0, 14, 0));
    CHECK(w.nights[0].secs == 80u * 60u);
    CHECK(w.nights[0].a & SD_A_NAP);
    CHECK(w.nights[0].a & SD_A_AUTO);
    /* then the main night is not a nap */
    fill(&w, HM(0, 23, 0), HM(1, 6, 30), STILL);
    fill(&w, HM(1, 6, 30), HM(1, 8, 0), UP);
    CHECK(w.n == 2);
    CHECK(!(w.nights[1].a & SD_A_NAP));
    CHECK(w.nights[1].t == HM(0, 23, 0));
    /* an evening doze that runs into the night is a night */
    CHECK(!sd_is_nap(HM(0, 19, 0), 3u * 3600u));  /* middle 20:30 */
    CHECK(sd_is_nap(HM(0, 13, 0), 3u * 3600u));
    CHECK(!sd_is_nap(HM(0, 12, 0), 5u * 3600u));  /* 4 h or more */

    /* ---- reboot mid-night, phone sets the clock soon after ---- */
    sim_init(&w, 1);
    fill(&w, HM(0, 22, 0), HM(0, 23, 0), AWAKE);
    fill(&w, HM(0, 23, 0), HM(1, 2, 0), STILL);
    CHECK(w.s.state == SD_ASLEEP);
    memcpy(fl_snap, fl_mem, sizeof fl_mem);    /* the flash at the restart */
    {
        /* restart: RAM gone, uptime from 0, the clock restored from the
           last settings save, 21:40, until the phone sets it */
        struct sim r = w;
        memset(&r.s, 0xA5, sizeof r.s);
        sd_j_load(&r.j, &FL, &r.s);
        CHECK(r.s.state == SD_ASLEEP);
        CHECK(r.s.start_t == HM(0, 23, 0));
        /* the watch screen right after the restart */
        CHECK(sd_now(&r.s, HM(0, 21, 40), 1) == HM(1, 2, 0));
        r.ms = 2000;
        slot(&r, HM(0, 21, 40), 0, 0, 55, 0, 1);
        slot(&r, HM(0, 21, 45), 0, 1, 55, 0, 1);
        CHECK(r.s.state == SD_ASLEEP);
        CHECK(r.s.flags & SD_F_GUESS);
        /* the phone connects: real time 02:15 */
        fill(&r, HM(1, 2, 15), HM(1, 7, 0), STILL);
        CHECK(!(r.s.flags & SD_F_GUESS));
        fill(&r, HM(1, 7, 0), HM(1, 8, 0), UP);
        CHECK(r.n == 1);
        CHECK(r.nights[0].t == HM(0, 23, 0));
        CHECK(r.nights[0].secs == 8u * 3600u);
        CHECK(!r.nights[0].guess);
    }
    {
        /* same restart, the phone never connects: the night still ends
           at 07:00 on the carried-on clock, marked as a guess */
        struct sim r = w;
        memcpy(fl_mem, fl_snap, sizeof fl_mem);
        sd_j_load(&r.j, &FL, &r.s);
        r.ms = 2000;
        uint32_t rt = HM(0, 21, 40);
        for (uint32_t u = HM(1, 2, 0); u < HM(1, 7, 0); u += SD_EPOCH, rt += SD_EPOCH)
            slot(&r, rt, 0, (u / SD_EPOCH) % 7 == 3 ? 2 : 0, 55, 0, 1);
        for (int k = 0; k < 12; k++, rt += SD_EPOCH)
            slot(&r, rt, 260, 90, 84, 0, 1);
        CHECK(r.n == 1);
        CHECK(r.nights[0].t == HM(0, 23, 0));
        CHECK(r.nights[0].secs == 8u * 3600u);
        CHECK(r.nights[0].guess);
    }
    {
        /* battery ran out at 02:00, back at 05:00 with the phone's time:
           the night ended when the watch went off */
        struct sim r = w;
        memcpy(fl_mem, fl_snap, sizeof fl_mem);
        sd_j_load(&r.j, &FL, &r.s);
        r.ms = 2000;
        fill(&r, HM(1, 5, 0), HM(1, 6, 0), CHARGER);
        CHECK(r.n == 1);
        CHECK(r.nights[0].t == HM(0, 23, 0));
        CHECK(r.nights[0].t + r.nights[0].secs == HM(1, 2, 0));
    }
    {
        /* off for 3 h, back on a restored clock, the phone sets it later */
        struct sim r = w;
        memcpy(fl_mem, fl_snap, sizeof fl_mem);
        sd_j_load(&r.j, &FL, &r.s);
        r.ms = 2000;
        slot(&r, HM(0, 21, 40), 0, 0, 55, 0, 1);
        slot(&r, HM(0, 21, 45), 0, 0, 55, 0, 1);
        fill(&r, HM(1, 5, 10), HM(1, 6, 0), AWAKE);
        CHECK(r.n == 1);
        CHECK(r.nights[0].t + r.nights[0].secs == HM(1, 2, 0));
    }

    /* ---- the phone moves the clock an hour mid-night (same boot) ---- */
    sim_init(&w, 0);
    fill(&w, HM(0, 22, 0), HM(0, 23, 0), AWAKE);
    fill(&w, HM(0, 23, 0), HM(1, 2, 0), STILL);
    fill(&w, HM(1, 3, 0), HM(1, 8, 0), STILL);      /* clock now +1 h */
    fill(&w, HM(1, 8, 0), HM(1, 9, 0), UP);
    CHECK(w.n == 1);
    CHECK(w.nights[0].secs == 8u * 3600u);
    CHECK(w.nights[0].t == HM(1, 0, 0));

    /* ---- manual BEDTIME / WAKE UP overrides detection ---- */
    sim_init(&w, 1);
    {
        struct sd_night o;
        fill(&w, HM(0, 21, 0), HM(0, 22, 30), AWAKE);
        CHECK(sd_bed(&w.s, HM(0, 22, 31), &o) == 0);
        sd_j_save(&w.j, &w.s);
        fill(&w, HM(0, 22, 30), HM(1, 1, 0), STILL);
        CHECK(w.s.state == SD_MANUAL);
        /* restart during a manual night keeps it */
        sd_j_load(&w.j, &FL, &w.s);
        CHECK(w.s.state == SD_MANUAL);
        CHECK(w.s.start_t == HM(0, 22, 31));
        w.ms = 2000;
        fill(&w, HM(1, 1, 0), HM(1, 6, 0), STILL);
        fill(&w, HM(1, 6, 0), HM(1, 6, 45), UP);    /* not auto-ended */
        CHECK(w.s.state == SD_MANUAL);
        CHECK(w.n == 0);
        CHECK(sd_wake(&w.s, HM(1, 6, 46), 1, &o) == 1);
        CHECK(o.t == HM(0, 22, 31));
        CHECK(o.secs == HM(1, 6, 46) - HM(0, 22, 31));
        CHECK(o.a == 0);                           /* as older builds */
        CHECK(w.s.state == SD_IDLE);
        /* clock unknown at bedtime: the uptime gives the length */
        sd_bed(&w.s, 0, &o);
        CHECK(sd_wake(&w.s, HM(1, 9, 0), 1800, &o) == 1);
        CHECK(o.secs == 1800 && o.t == HM(1, 9, 0) - 1800);
        /* WAKE UP on an auto night ends it now */
        sim_init(&w, 0);
        fill(&w, HM(0, 22, 0), HM(0, 23, 0), AWAKE);
        fill(&w, HM(0, 23, 0), HM(1, 6, 0), STILL);
        CHECK(sd_wake(&w.s, HM(1, 6, 2), 0, &o) == 1);
        CHECK(o.t == HM(0, 23, 0) && o.secs == 7u * 3600u + 120u);
        CHECK(o.a & SD_A_AUTO);
        /* tracking switched off mid-night writes what there is */
        sim_init(&w, 0);
        fill(&w, HM(0, 22, 0), HM(0, 23, 0), AWAKE);
        fill(&w, HM(0, 23, 0), HM(1, 3, 0), STILL);
        CHECK(sd_off(&w.s, HM(1, 3, 0), &o) == 1);
        CHECK(o.secs == 4u * 3600u);
        CHECK(w.s.state == SD_IDLE);
        /* BEDTIME while a night is held after a wake writes it first */
        sim_init(&w, 0);
        fill(&w, HM(0, 22, 0), HM(0, 23, 0), AWAKE);
        fill(&w, HM(0, 23, 0), HM(1, 5, 0), STILL);
        fill(&w, HM(1, 5, 0), HM(1, 5, 10), UP);
        CHECK(w.s.state == SD_PENDING);
        CHECK(sd_bed(&w.s, HM(1, 5, 12), &o) == 1);
        CHECK(o.t == HM(0, 23, 0) && o.secs == 6u * 3600u);
        CHECK(w.s.state == SD_MANUAL);
    }

    /* ---- journal: wraps between its sectors, survives a torn write ---- */
    sim_init(&w, 1);
    fl_erases = 0;
    for (int k = 0; k < 3 * (int)SD_J_PER + 7; k++) {
        w.s.state = SD_ASLEEP;
        w.s.start_t = 1000u + (uint32_t)k;
        sd_j_poll(&w.j);
        sd_j_save(&w.j, &w.s);
        sd_j_poll(&w.j);
    }
    while (w.j.pend)
        sd_j_poll(&w.j);
    CHECK(fl_erases == 3);
    {
        struct sd_state l;
        struct sd_journal j2;
        sd_j_load(&j2, &FL, &l);
        CHECK(l.state == SD_ASLEEP);
        CHECK(l.start_t == 1000u + 3 * SD_J_PER + 6);
        /* a torn newest entry: the one before wins */
        uint32_t a = sd_j_addr(j2.sec, j2.slot - 1) - SD_J_BASE;
        fl_mem[a + 8] = 0x00;             /* start_t, low byte */
        sd_j_load(&j2, &FL, &l);
        CHECK(l.start_t == 1000u + 3 * SD_J_PER + 5);
        /* the next save goes after the torn one */
        l.start_t = 77;
        sd_j_save(&j2, &l);
        sd_j_load(&j2, &FL, &l);
        CHECK(l.start_t == 77);
        /* blank flash: idle */
        memset(fl_mem, 0xFF, sizeof fl_mem);
        sd_j_load(&j2, &FL, &l);
        CHECK(l.state == SD_IDLE);
        /* junk everywhere: idle, and the first save erases before use */
        memset(fl_mem, 0x12, sizeof fl_mem);
        sd_j_load(&j2, &FL, &l);
        CHECK(l.state == SD_IDLE);
        l.state = SD_MANUAL;
        l.start_t = 4242;
        sd_j_save(&j2, &l);
        while (j2.pend)
            sd_j_poll(&j2);
        sd_j_load(&j2, &FL, &l);
        CHECK(l.state == SD_MANUAL && l.start_t == 4242);
    }

    if (fails) {
        printf("test_sleep: %d failed\n", fails);
        return 1;
    }
    printf("test_sleep: all passed\n");
    return 0;
}
