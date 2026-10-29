/* AMS PlaybackInfo value parsing (pure C, host-tested). */
#ifndef AMS_PARSE_H
#define AMS_PARSE_H
#include <stdlib.h>
#include <string.h>

/* "state,rate,elapsed", e.g. "1,1.0,42.317". state: 0 paused,
   1 playing, 2 rewinding, 3 fast-forwarding. Returns 0 or -1. */
static inline int ams_parse_playback(const char *v, int *playing,
                                     int *elapsed_s)
{
    if (!v || *v < '0' || *v > '9')
        return -1;
    int state = atoi(v);
    *playing = state == 1 || state == 3;
    const char *e = strchr(v, ',');
    if (e) e = strchr(e + 1, ',');
    *elapsed_s = e ? atoi(e + 1) : -1;
    return 0;
}
#endif
