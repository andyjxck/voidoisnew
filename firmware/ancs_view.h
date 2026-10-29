/* ancs_view.h — what the UI reads about one iPhone notification (2.2.2) */
#ifndef VOID_ANCS_VIEW_H
#define VOID_ANCS_VIEW_H
#include <stdint.h>

struct ancs_notif_view {
    uint32_t uid;
    uint8_t flags, has_pos, has_neg, cat;   /* ANCS flags and category */
    const char *app;      /* display name, never empty */
    const char *title;    /* sender or title, may be empty */
    const char *sub;      /* subtitle (group, thread, subject), may be empty */
    const char *msg;      /* body, may be empty */
    uint32_t rx_ms;       /* when the watch heard of it */
    uint32_t date;        /* yyyymmdd from the phone, 0 = unknown */
    int16_t mod;          /* minute of day from the phone, -1 = unknown */
};

int ancs_ready(void), ancs_state(void);
int ancs_count(void);                       /* notifications ready to show */
const struct ancs_notif_view *ancs_get(int i);   /* 0 = newest */
int ancs_take_dirty(void);
int ancs_take_new(void);         /* a new, non-silent one arrived: buzz */
uint32_t ancs_take_popup(void);  /* its text has arrived: uid to show */
void ancs_dismiss(uint32_t uid);
void ancs_positive(uint32_t uid);
void ancs_set_muted(int on);     /* 2.2.5: CC ALERTS off = 1 */

#endif
