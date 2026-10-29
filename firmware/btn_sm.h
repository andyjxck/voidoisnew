/* Side-button press decoder (2.2.3): single press, double press.
 *
 * Header-only so the host test (tests/test_btn.c) runs the same code.
 *
 * Feed it every UI tick with the button level and the time. It returns
 * BTN_EV_SINGLE / BTN_EV_DOUBLE on the tick an action should happen.
 *
 *  - A single press acts on release. If the screen is asleep (`fast`)
 *    it acts at once, so waking never waits; otherwise it waits out the
 *    double-press window (BTN_DBL_MS) first, so a double never fires the
 *    single action too.
 *  - A second press that starts within BTN_DBL_MS of the first release
 *    is a double press (acts on its release). If the first press already
 *    acted (the wake), the double still fires: wake + toggle.
 *  - A press held longer than BTN_TAP_MAX_MS never starts a double: it
 *    acts as a single on release, as before. The 3 s hold reset in
 *    main.c is untouched (it reads the pin itself).
 */
#ifndef VOIDOS_BTN_SM_H
#define VOIDOS_BTN_SM_H
#include <stdint.h>

#define BTN_DBL_MS      300u
#define BTN_TAP_MAX_MS  600u

enum { BTN_EV_NONE = 0, BTN_EV_SINGLE = 1, BTN_EV_DOUBLE = 2 };
enum { BTN_S_IDLE, BTN_S_DOWN1, BTN_S_WAIT2, BTN_S_DOWN2 };

typedef struct {
    uint8_t state;
    uint8_t acted;        /* first press already acted (fast path) */
    uint32_t t_down;
    uint32_t t_up;
} btn_sm_t;

static inline int btn_sm_step(btn_sm_t *s, int down, uint32_t now, int fast)
{
    switch (s->state) {
    case BTN_S_IDLE:
        if (down) {
            s->state = BTN_S_DOWN1;
            s->t_down = now;
        }
        return BTN_EV_NONE;
    case BTN_S_DOWN1:
        if (down)
            return BTN_EV_NONE;
        if (now - s->t_down > BTN_TAP_MAX_MS) {
            s->state = BTN_S_IDLE;     /* long press: plain single */
            return BTN_EV_SINGLE;
        }
        s->state = BTN_S_WAIT2;
        s->t_up = now;
        s->acted = fast ? 1 : 0;
        return fast ? BTN_EV_SINGLE : BTN_EV_NONE;
    case BTN_S_WAIT2:
        if (down && now - s->t_up <= BTN_DBL_MS) {
            s->state = BTN_S_DOWN2;
            s->t_down = now;
            return BTN_EV_NONE;
        }
        if (now - s->t_up > BTN_DBL_MS || down) {
            int ev = s->acted ? BTN_EV_NONE : BTN_EV_SINGLE;
            s->state = down ? BTN_S_DOWN1 : BTN_S_IDLE;
            s->t_down = now;
            return ev;
        }
        return BTN_EV_NONE;
    case BTN_S_DOWN2:
        if (down)
            return BTN_EV_NONE;
        s->state = BTN_S_IDLE;
        return BTN_EV_DOUBLE;
    }
    s->state = BTN_S_IDLE;
    return BTN_EV_NONE;
}

#endif
