/* VoidOS 2.2.2 host tests: the notification store (slot reuse, late
   replies, app names, text folding, pop-ups), the midnight reset of
   the daily totals, and the text the alerts screen lays out.
   Build + run: make -C tests/ble store */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../firmware/nimble_port/src/ancs_store.h"
#include "../../firmware/daily.h"

static int fails, checks;
#define CHECK(c, ...)                                                   \
    do {                                                                \
        checks++;                                                       \
        if (!(c)) {                                                     \
            fails++;                                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                              \
        }                                                               \
    } while (0)

/* ---- a Data Source reply as the iPhone sends it ---------------------- */
static int ds_build(uint8_t *b, uint32_t uid, const char *app,
                    const char *title, const char *sub, const char *msg,
                    const char *date)
{
    int n = 0;
    const char *v[6] = {app, title, sub, msg, NULL, date};
    b[n++] = 0;
    b[n++] = uid & 0xFF; b[n++] = uid >> 8 & 0xFF;
    b[n++] = uid >> 16 & 0xFF; b[n++] = uid >> 24 & 0xFF;
    for (int id = 0; id < 6; id++) {
        if (id == 4)
            continue;
        int l = (int)strlen(v[id]);
        b[n++] = (uint8_t)id; b[n++] = l & 0xFF; b[n++] = l >> 8;
        memcpy(b + n, v[id], l);
        n += l;
    }
    return n;
}

static int deliver(struct an_store *st, uint32_t uid, const char *app,
                   const char *title, const char *sub, const char *msg)
{
    uint8_t b[600];
    int n = ds_build(b, uid, app, title, sub, msg, "20261029T094100");
    return an_on_ds(st, b, n);
}

static struct an_slot *slot(struct an_store *st, uint32_t uid)
{
    return an_find(st, uid);
}

static void test_app_names(void)
{
    static const struct { const char *id, *want; } t[] = {
        {"com.apple.MobileSMS", "Messages"},
        {"net.whatsapp.WhatsApp", "WhatsApp"},
        {"com.apple.mobilemail", "Mail"},
        {"com.google.Gmail", "Gmail"},
        {"com.burbn.instagram", "Instagram"},
        {"com.atebits.Tweetie2", "X"},
        {"com.spotify.client", "Spotify"},
    };
    char out[20];
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        an_app_name(out, sizeof out, (const uint8_t *)t[i].id,
                    (int)strlen(t[i].id));
        CHECK(!strcmp(out, t[i].want), "app %s -> \"%s\", want \"%s\"",
              t[i].id, out, t[i].want);
    }
    /* 2.2.1 cut the id at its last dot: "...app.stable" became STABLE */
    const char *st = "com.sesame.app.stable";
    an_app_name(out, sizeof out, (const uint8_t *)st, (int)strlen(st));
    CHECK(strcmp(out, "STABLE") && strcmp(out, "Stable"),
          "%s -> \"%s\" (the 2.2.1 bug)", st, out);
    CHECK(!strcmp(out, "Sesame"), "%s -> \"%s\", want Sesame", st, out);
    /* unknown apps: tidied, never empty, never junk past the id */
    const char *u = "com.example.coolweatherpro";
    an_app_name(out, sizeof out, (const uint8_t *)u, (int)strlen(u));
    CHECK(out[0] && strlen(out) < sizeof out, "unknown -> \"%s\"", out);
    for (const char *p = out; *p; p++)
        CHECK(*p >= 0x20 && *p < 0x7F, "unknown app has byte %02x",
              (unsigned char)*p);
    /* an id with no terminator, longer than the buffer */
    uint8_t raw[64];
    memset(raw, 'z', sizeof raw);
    an_app_name(out, sizeof out, raw, sizeof raw);
    CHECK(strlen(out) < sizeof out, "long id overflows: %zu", strlen(out));
    an_app_name(out, sizeof out, (const uint8_t *)"", 0);
    CHECK(out[0], "empty id gives an empty name");
}

static void test_text_fold(void)
{
    char o[41];
    /* curly quotes, accents, an emoji: all drawable after folding */
    const char *in = "\xE2\x80\x9C" "Caf\xC3\xA9" "\xE2\x80\x9D \xE2\x80\x94 it\xE2\x80\x99s \xF0\x9F\x98\x80 ok";
    an_text(o, sizeof o, (const uint8_t *)in, (int)strlen(in));
    int bad = 0;
    for (const char *p = o; *p; p++)
        bad |= (unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E;
    CHECK(!bad, "folded text has bytes the font cannot draw: \"%s\"", o);
    CHECK(strstr(o, "Cafe") != NULL, "accent folds to plain letter: \"%s\"", o);
    CHECK(strstr(o, "it's") != NULL, "curly apostrophe folds: \"%s\"", o);
    /* never past the buffer, even from a too-long value */
    char big[300];
    memset(big, 'a', sizeof big);
    an_text(o, sizeof o, (const uint8_t *)big, sizeof big);
    CHECK(strlen(o) == sizeof o - 1, "long text length %zu", strlen(o));
}

static void test_store(void)
{
    struct an_store st;
    memset(&st, 0, sizeof st);
    uint32_t now = 1000;

    /* a new notification buzzes, and pops up once its text arrives */
    an_on_ns(&st, 0, AN_F_POS, 4, 10, now);
    CHECK(st.buzz == 1, "new notification buzzes");
    st.buzz = 0;
    CHECK(an_next_fetch(&st, now) == 10, "fetches its text");
    CHECK(deliver(&st, 10, "com.apple.MobileSMS", "Sam", "", "hi") == 1,
          "reply used");
    CHECK(st.popup == 10, "pop-up for uid 10, got %u", (unsigned)st.popup);
    st.popup = 0;

    /* already on the phone when the watch connects: no buzz, no pop-up */
    an_on_ns(&st, 0, AN_F_PREEXISTING, 4, 11, now);
    CHECK(st.buzz == 0, "pre-existing does not buzz");
    an_next_fetch(&st, now);
    deliver(&st, 11, "com.apple.MobileSMS", "Old", "", "older");
    CHECK(st.popup == 0, "pre-existing does not pop up");

    /* clear one, then a new one in the reused slot must buzz (2.2.1
       reused the slot without emptying it: no buzz) */
    an_on_ns(&st, 2, 0, 0, 10, now);
    CHECK(!slot(&st, 10), "removed");
    an_on_ns(&st, 0, 0, 4, 12, now + 10);
    CHECK(st.buzz == 1, "new one after a clear buzzes");
    st.buzz = 0;
    struct an_slot *s = slot(&st, 12);
    CHECK(s && !s->title[0] && !s->msg[0] && s->alert,
          "reused slot starts empty");

    /* a late reply for a notification already cleared makes no card
       (2.2.1: the blank APP card) */
    an_next_fetch(&st, now + 10);
    an_on_ns(&st, 2, 0, 0, 12, now + 20);
    int before = st.n;
    CHECK(deliver(&st, 12, "com.apple.MobileSMS", "Late", "", "x") == 1,
          "late reply consumed");
    CHECK(st.n == before && !slot(&st, 12), "late reply made a card");
    CHECK(st.popup == 0, "late reply popped up");

    /* a reply with no text at all is dropped, not shown blank */
    an_on_ns(&st, 0, 0, 4, 13, now + 30);
    an_next_fetch(&st, now + 30);
    deliver(&st, 13, "com.apple.MobileSMS", "", "", "");
    CHECK(!slot(&st, 13), "empty notification shown");

    /* the connect burst: many more than fit; the store never overflows
       and every kept slot is a real one */
    for (uint32_t u = 100; u < 140; u++)
        an_on_ns(&st, 0, AN_F_PREEXISTING, 4, u, now + 40);
    CHECK(st.n == AN_MAX, "store holds %d, max %d", st.n, AN_MAX);
    for (int i = 0; i < st.n; i++)
        CHECK(st.s[i].uid >= 134, "kept the newest: uid %u",
              (unsigned)st.s[i].uid);
    /* unanswered fetches are retried once then dropped, never shown */
    uint32_t t = now + 50;
    for (int k = 0; k < 40; k++, t += AN_FETCH_MS)
        an_next_fetch(&st, t);
    for (int i = 0; i < st.n; i++)
        CHECK(an_visible(&st.s[i]), "blank slot left after timeouts");

    /* subtitle repeating the title is dropped; the date is parsed */
    memset(&st, 0, sizeof st);
    an_on_ns(&st, 0, 0, 6, 20, now);
    an_next_fetch(&st, now);
    deliver(&st, 20, "com.apple.mobilemail", "GitHub", "GitHub", "PR");
    s = slot(&st, 20);
    CHECK(s && !s->sub[0], "repeated subtitle kept");
    CHECK(s && s->date == 20261029 && s->mod == 9 * 60 + 41,
          "date %u mod %d", s ? (unsigned)s->date : 0, s ? s->mod : -2);
}

/* ---- the midnight reset ------------------------------------------- */
static void test_daily(void)
{
    struct daily d;
    memset(&d, 0, sizeof d);
    /* first sight of a day starts it without inventing a yesterday */
    CHECK(daily_roll(&d, 20261028, 1, 5000) == 0 || d.day == 20261028,
          "first day starts");
    CHECK(d.day == 20261028, "day %u", (unsigned)d.day);
    d.water_ml = 1500;
    d.rest_bpm = 57;
    CHECK(daily_steps(&d, 9000) == 4000, "steps today %u",
          (unsigned)daily_steps(&d, 9000));

    /* same day again (every clock check): nothing resets */
    CHECK(daily_roll(&d, 20261028, 1, 9100) == 0, "rolled twice in a day");
    CHECK(d.water_ml == 1500, "water kept");

    /* midnight: yesterday saved, today starts at zero */
    CHECK(daily_roll(&d, 20261029, 1, 12000) == 1, "midnight roll");
    CHECK(d.y_day == 20261028 && d.y_water == 1500 && d.y_rest == 57,
          "yesterday saved: day %u water %u rest %u", (unsigned)d.y_day,
          (unsigned)d.y_water, (unsigned)d.y_rest);
    CHECK(d.y_steps == 7000, "yesterday steps %u", (unsigned)d.y_steps);
    CHECK(d.water_ml == 0 && daily_steps(&d, 12000) == 0,
          "today zeroed: water %u steps %u", (unsigned)d.water_ml,
          (unsigned)daily_steps(&d, 12000));
    CHECK(daily_have_yesterday(&d), "has yesterday");

    /* the clock going backwards, or a reboot, does not reset */
    d.water_ml = 250;
    CHECK(daily_roll(&d, 20261028, 1, 12100) == 0, "backwards clock reset");
    CHECK(d.water_ml == 250 && d.day == 20261029, "backwards kept today");

    /* the step counter restarting (reboot) mid-day never goes negative */
    CHECK(daily_steps(&d, 10) < 100000u, "steps after counter reset %u",
          (unsigned)daily_steps(&d, 10));

    /* asleep over several midnights: one reset on waking, and
       "yesterday" is the last day it saw, not a blank day */
    d.water_ml = 900;
    CHECK(daily_roll(&d, 20261101, 1, 15000) == 1, "roll after days off");
    CHECK(d.y_day == 20261029 && d.y_water == 900, "last day kept");
    CHECK(!daily_have_yesterday(&d), "a gap is not yesterday");

    /* month and year ends */
    CHECK(daily_roll(&d, 20261231, 1, 15000) == 1, "to 31 Dec");
    CHECK(daily_roll(&d, 20270101, 1, 15000) == 1, "new year");
    CHECK(daily_have_yesterday(&d), "31 Dec is yesterday on 1 Jan");
    daily_roll(&d, 20270228, 1, 15000);
    daily_roll(&d, 20270301, 1, 15000);
    CHECK(daily_have_yesterday(&d), "28 Feb is yesterday on 1 Mar 2027");
}

int main(void)
{
    test_app_names();
    test_text_fold();
    test_store();
    test_daily();
    printf("store/daily: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
