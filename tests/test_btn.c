/* Side-button decoder (firmware/btn_sm.h): single vs double press. */
#include <stdio.h>
#include "../firmware/btn_sm.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; \
    printf("FAIL: %s\n", msg); } } while (0)

/* drive a press pattern: list of (level, duration ms) at a 10 ms tick;
   records each event and the time it fired */
struct ev { int ev; unsigned t; };
static int run(const int *pat, int n, int fast, struct ev *out, int max)
{
    btn_sm_t s = {0};
    unsigned t = 1000;
    int k = 0;
    for (int i = 0; i < n; i += 2)
        for (int d = 0; d < pat[i + 1]; d += 10, t += 10) {
            int e = btn_sm_step(&s, pat[i], t, fast);
            if (e && k < max) { out[k].ev = e; out[k].t = t; k++; }
        }
    for (int d = 0; d < 1000; d += 10, t += 10) {    /* settle */
        int e = btn_sm_step(&s, 0, t, fast);
        if (e && k < max) { out[k].ev = e; out[k].t = t; k++; }
    }
    return k;
}

int main(void)
{
    struct ev e[8];
    int n;

    /* awake single tap: one SINGLE, after the 300 ms window */
    { int p[] = {0, 50, 1, 100, 0, 10};
      n = run(p, 6, 0, e, 8);
      CHECK(n == 1 && e[0].ev == BTN_EV_SINGLE, "awake tap -> one single");
      CHECK(n == 1 && e[0].t - 1150 >= BTN_DBL_MS && e[0].t - 1150 <= BTN_DBL_MS + 20,
            "awake single waits only the double window"); }

    /* asleep single tap: SINGLE on the release tick, nothing after */
    { int p[] = {0, 50, 1, 100, 0, 10};
      n = run(p, 6, 1, e, 8);
      CHECK(n == 1 && e[0].ev == BTN_EV_SINGLE && e[0].t == 1150,
            "asleep tap -> single at once (no wake lag)"); }

    /* awake double: one DOUBLE, never a SINGLE */
    { int p[] = {0, 50, 1, 100, 0, 150, 1, 100, 0, 10};
      n = run(p, 10, 0, e, 8);
      CHECK(n == 1 && e[0].ev == BTN_EV_DOUBLE, "awake double -> double only"); }

    /* asleep double: wake (single) then DOUBLE */
    { int p[] = {0, 50, 1, 100, 0, 150, 1, 100, 0, 10};
      n = run(p, 10, 1, e, 8);
      CHECK(n == 2 && e[0].ev == BTN_EV_SINGLE && e[1].ev == BTN_EV_DOUBLE,
            "asleep double -> wake then toggle"); }

    /* gap longer than the window: two singles */
    { int p[] = {0, 50, 1, 100, 0, 500, 1, 100, 0, 10};
      n = run(p, 10, 0, e, 8);
      CHECK(n == 2 && e[0].ev == BTN_EV_SINGLE && e[1].ev == BTN_EV_SINGLE,
            "slow presses -> two singles"); }

    /* long press (900 ms) then quick tap: single (at release) + single */
    { int p[] = {0, 50, 1, 900, 0, 100, 1, 100, 0, 10};
      n = run(p, 10, 0, e, 8);
      CHECK(n == 2 && e[0].ev == BTN_EV_SINGLE && e[0].t == 1950 &&
            e[1].ev == BTN_EV_SINGLE, "long press never starts a double"); }

    /* a 3 s hold (the reset in main.c) produces exactly one single at
       release and no double; the reset itself reads the pin directly */
    { int p[] = {0, 50, 1, 3200, 0, 10};
      n = run(p, 6, 0, e, 8);
      CHECK(n == 1 && e[0].ev == BTN_EV_SINGLE, "3 s hold -> no double"); }

    /* triple press: double, then the third starts afresh -> single */
    { int p[] = {0, 50, 1, 80, 0, 100, 1, 80, 0, 100, 1, 80, 0, 10};
      n = run(p, 14, 0, e, 8);
      CHECK(n == 2 && e[0].ev == BTN_EV_DOUBLE && e[1].ev == BTN_EV_SINGLE,
            "triple -> double + single"); }

    /* second press exactly at the window edge still counts */
    { int p[] = {0, 50, 1, 100, 0, 300, 1, 100, 0, 10};
      n = run(p, 10, 0, e, 8);
      CHECK(n == 1 && e[0].ev == BTN_EV_DOUBLE, "300 ms gap -> double"); }

    printf("test_btn: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
