/* ancs_store.h — the iPhone notification store (2.2.2), pure C.
 *
 * ancs.c feeds it Notification Source events and reassembled Data
 * Source replies; the UI reads it through ancs_view.h. No BLE here, so
 * tests/ble runs it on the host.
 *
 * What 2.2.1 got wrong and this fixes:
 *  - a freed or evicted slot was reused without clearing it, so its old
 *    receive time made a NEW notification look old: no buzz after older
 *    ones were cleared (or after a reconnect), and stale text showed;
 *  - a Data Source reply for a notification that had already gone made
 *    a fresh empty slot: the blank "APP" card;
 *  - the fetch queue (6) overflowed silently in the burst of existing
 *    notifications iOS sends on subscribe: slots that were never fetched
 *    showed as blank cards. Each slot now carries its own fetch state,
 *    the newest unfetched slot is fetched next, and a fetch that times
 *    out twice drops the slot;
 *  - AppIdentifier was read as a C string with no length, running into
 *    the next attribute, and then cut at the last '.', so
 *    "ai.sesame.stable" became "STABLE" plus junk bytes. Names now come
 *    from a table of common apps, then a cleaned-up bundle ID;
 *  - Subtitle and Date were never asked for; the time received shown is
 *    now the phone's, not the watch's (right for notifications that were
 *    already on the phone when the watch connected);
 *  - text is folded to what the watch font can draw: curly quotes,
 *    dashes, ellipses and accented letters become plain ASCII, emoji and
 *    other symbols are dropped, line breaks become spaces.
 */
#ifndef VOID_ANCS_STORE_H
#define VOID_ANCS_STORE_H
#include <stdint.h>
#include <string.h>
#include "../../ancs_view.h"

#define AN_MAX 6
#define AN_FETCH_MS 3000
/* ANCS event flags */
#define AN_F_SILENT 0x01
#define AN_F_PREEXISTING 0x04
#define AN_F_POS 0x08
#define AN_F_NEG 0x10
/* request lengths (also the buffer sizes - 1) */
#define AN_TITLE_MAX 40
#define AN_SUB_MAX 32
#define AN_MSG_MAX 119

enum { AN_NEED, AN_INFLIGHT, AN_DONE };

struct an_slot {
    uint32_t uid, rx_ms, sent_ms, date;
    int16_t mod;
    uint8_t flags, cat, has_pos, has_neg, fetch, tries, alert;
    char app[20];
    char title[AN_TITLE_MAX + 1];
    char sub[AN_SUB_MAX + 1];
    char msg[AN_MSG_MAX + 1];
};

struct an_store {
    struct an_slot s[AN_MAX];   /* arrival order, [0] oldest */
    int n;
    uint8_t dirty, buzz;
    uint32_t popup;
};

/* ---- text ------------------------------------------------------------- */

/* fold one code point to ASCII the font has; returns chars written */
static int an_fold(uint32_t c, char *o)
{
    static const char lat[] =   /* U+00C0..U+00FF, '\0' = drop */
        "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs"
        "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";
    if (c >= 0x20 && c < 0x7F) { o[0] = (char)c; return 1; }
    if (c == '\n' || c == '\r' || c == '\t' || c == 0xA0 ||
        c == 0x2028 || c == 0x2029 || (c >= 0x2000 && c <= 0x200A)) {
        o[0] = ' '; return 1;
    }
    if (c >= 0xC0 && c <= 0xFF) {
        if (c == 0xC6 || c == 0xE6) { o[0] = 'A'; o[1] = 'E'; return 2; }
        if (c == 0xDF) { o[0] = 's'; o[1] = 's'; return 2; }
        o[0] = lat[c - 0xC0]; return 1;
    }
    switch (c) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: case 0xB4:
        o[0] = '\''; return 1;
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: case 0xAB: case 0xBB:
        o[0] = '"'; return 1;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014:
    case 0x2015: case 0x2212:
        o[0] = '-'; return 1;
    case 0x2026: o[0] = o[1] = o[2] = '.'; return 3;
    case 0x2022: case 0xB7: o[0] = '*'; return 1;
    case 0x152: case 0x153: o[0] = 'O'; o[1] = 'E'; return 2;
    case 0x141: case 0x142: o[0] = 'L'; return 1;
    case 0x160: case 0x161: case 0x15A: case 0x15B: o[0] = 'S'; return 1;
    case 0x17D: case 0x17E: case 0x179: case 0x17A: case 0x17B: case 0x17C:
        o[0] = 'Z'; return 1;
    case 0x106: case 0x107: case 0x10C: case 0x10D: o[0] = 'C'; return 1;
    case 0x118: case 0x119: case 0x11A: case 0x11B: o[0] = 'E'; return 1;
    case 0x143: case 0x144: case 0x147: case 0x148: o[0] = 'N'; return 1;
    case 0x158: case 0x159: o[0] = 'R'; return 1;
    case 0x20AC: o[0] = 'E'; o[1] = 'U'; o[2] = 'R'; return 3;
    case 0xA3: o[0] = 'G'; o[1] = 'B'; o[2] = 'P'; return 3;
    }
    return 0;                   /* emoji, CJK, marks, controls: dropped */
}

/* UTF-8 (possibly cut mid-character) -> one line of drawable ASCII,
   spaces collapsed and trimmed; never writes past cap-1 */
static void an_text(char *dst, int cap, const uint8_t *s, int len)
{
    int o = 0, sp = 0;
    for (int i = 0; i < len && s[i];) {
        uint32_t c = s[i];
        int k = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 :
                (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (k > 1) {
            if (i + k > len) break;         /* cut character at the end */
            c &= 0x3F >> (k - 1);
            for (int j = 1; j < k; j++) {
                if ((s[i + j] & 0xC0) != 0x80) { c = 0; k = j; break; }
                c = (c << 6) | (s[i + j] & 0x3F);
            }
        } else if (c >= 0x80) c = 0;
        i += k;
        char f[3];
        int n = an_fold(c, f);
        for (int j = 0; j < n; j++) {
            if (f[j] == ' ') { sp = o > 0; continue; }
            if (sp && o < cap - 1) dst[o++] = ' ';
            sp = 0;
            if (o < cap - 1) dst[o++] = f[j];
        }
    }
    dst[o] = 0;
}

/* ---- app names -------------------------------------------------------- */

static const char *const an_apps[] = {
    "com.apple.MobileSMS", "Messages",
    "com.apple.mobilephone", "Phone",
    "com.apple.mobilemail", "Mail",
    "com.apple.mobilecal", "Calendar",
    "com.apple.reminders", "Reminders",
    "com.apple.facetime", "FaceTime",
    "com.apple.Health", "Health",
    "com.apple.findmy", "Find My",
    "com.apple.news", "News",
    "com.apple.mobileslideshow", "Photos",
    "com.apple.Fitness", "Fitness",
    "com.apple.shortcuts", "Shortcuts",
    "com.apple.Passbook", "Wallet",
    "com.apple.AppStore", "App Store",
    "com.apple.mobiletimer", "Clock",
    "com.apple.Home", "Home",
    "com.apple.Music", "Music",
    "com.apple.podcasts", "Podcasts",
    "com.apple.mobilenotes", "Notes",
    "com.apple.Maps", "Maps",
    "com.apple.TestFlight", "TestFlight",
    "com.apple.Preferences", "Settings",
    "com.apple.weather", "Weather",
    "net.whatsapp.WhatsApp", "WhatsApp",
    "net.whatsapp.WhatsAppSMB", "WA Business",
    "com.facebook.Messenger", "Messenger",
    "com.facebook.Facebook", "Facebook",
    "com.burbn.instagram", "Instagram",
    "com.burbn.barcelona", "Threads",
    "com.atebits.Tweetie2", "X",
    "ph.telegra.Telegraph", "Telegram",
    "org.whispersystems.signal", "Signal",
    "com.toyopagroup.picaboo", "Snapchat",
    "com.zhiliaoapp.musically", "TikTok",
    "com.google.ios.youtube", "YouTube",
    "com.google.Gmail", "Gmail",
    "com.google.calendar", "Google Cal",
    "com.google.Maps", "Google Maps",
    "com.google.GoogleMobile", "Google",
    "com.microsoft.Office.Outlook", "Outlook",
    "com.microsoft.skype.teams", "Teams",
    "com.tinyspeck.chatlyio", "Slack",
    "com.hammerandchisel.discord", "Discord",
    "com.spotify.client", "Spotify",
    "com.linkedin.LinkedIn", "LinkedIn",
    "com.reddit.Reddit", "Reddit",
    "com.amazon.Amazon", "Amazon",
    "com.ubercab.UberClient", "Uber",
    "com.ubercab.UberEats", "Uber Eats",
    "com.netflix.Netflix", "Netflix",
    "com.duolingo.DuolingoMobile", "Duolingo",
    "com.openai.chat", "ChatGPT",
    "co.uk.getmondo", "Monzo",
    "com.revolut.revolut", "Revolut",
    "com.paypal.PPClient", "PayPal",
    "com.strava.stravaride", "Strava",
    "com.deliveroo.orderapp", "Deliveroo",
    "com.cardify.tinder", "Tinder",
    "com.bumble.app", "Bumble",
    "com.skype.skype", "Skype",
    "us.zoom.videomeetings", "Zoom",
};

static int an_ieq(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !y) return 0;
    }
    return 1;
}

/* token is one of a NUL-separated list, case-insensitive */
static int an_in(const char *t, int n, const char *list)
{
    for (const char *w = list; *w; w += strlen(w) + 1)
        if ((int)strlen(w) == n && an_ieq(t, w, n))
            return 1;
    return 0;
}

/* bundle ID -> display name: a known app's name, else the ID's most
   telling part: "ai.sesame.stable" -> "Sesame",
   "com.google.ios.youtube" -> "Youtube", "com.foo.bar-app.beta" ->
   "Bar app". Never empty: an app with no ID shows as "iPhone". */
static void an_app_name(char *dst, int cap, const uint8_t *id, int len)
{
    char b[64];
    int n = 0;
    for (int i = 0; i < len && id[i] && n < (int)sizeof b - 1; i++)
        if (id[i] > 0x20 && id[i] < 0x7F)
            b[n++] = (char)id[i];
    b[n] = 0;
    for (unsigned i = 0; i < sizeof an_apps / sizeof an_apps[0]; i += 2)
        if ((int)strlen(an_apps[i]) == n && an_ieq(b, an_apps[i], n)) {
            an_text(dst, cap, (const uint8_t *)an_apps[i + 1], 64);
            return;
        }
    /* split on '.', drop reverse-DNS heads and build-flavour tails */
    static const char heads[] = "com\0net\0org\0io\0ai\0co\0me\0uk\0"
        "de\0fr\0us\0ca\0au\0tv\0app\0info\0ph\0jp\0nl\0se\0ch\0it\0"
        "es\0br\0in\0eu\0ie\0";
    static const char tails[] = "stable\0beta\0alpha\0release\0prod\0"
        "production\0ios\0iphone\0ipad\0app\0apps\0mobile\0client\0"
        "ent\0enterprise\0debug\0dev\0internal\0appstore\0lite\0free\0"
        "main\0store\0testflight\0nightly\0staging\0dist\0global\0intl\0"
        "inhouse\0ai\0";
    const char *tk[12];
    int tl[12], nt = 0;
    for (int i = 0; i < n && nt < 12;) {
        int j = i;
        while (j < n && b[j] != '.') j++;
        if (j > i) { tk[nt] = b + i; tl[nt] = j - i; nt++; }
        i = j + 1;
    }
    int lo = 0, hi = nt - 1;
    while (hi > lo && an_in(tk[lo], tl[lo], heads)) lo++;
    while (hi > lo && (an_in(tk[hi], tl[hi], tails) || tl[hi] <= 2)) hi--;
    if (nt == 0) {
        an_text(dst, cap, (const uint8_t *)"iPhone", 6);
        return;
    }
    char o[24];
    int m = 0;
    for (int i = 0; i < tl[hi] && m < (int)sizeof o - 1; i++) {
        char c = tk[hi][i];
        if (c == '-' || c == '_') c = ' ';
        if (i == 0 && c >= 'a' && c <= 'z') c -= 32;
        o[m++] = c;
    }
    o[m] = 0;
    an_text(dst, cap, (const uint8_t *)o, m);
    if (!dst[0])
        an_text(dst, cap, (const uint8_t *)"iPhone", 6);
}

/* ANCS Date "yyyyMMdd'T'HHmmSS" -> yyyymmdd and minute of day */
static void an_date(struct an_slot *s, const uint8_t *v, int len)
{
    s->date = 0;
    s->mod = -1;
    if (len < 13 || v[8] != 'T')
        return;
    for (int i = 0; i < 13; i++)
        if (i != 8 && (v[i] < '0' || v[i] > '9'))
            return;
    uint32_t d = 0;
    for (int i = 0; i < 8; i++)
        d = d * 10 + (v[i] - '0');
    int h = (v[9] - '0') * 10 + (v[10] - '0');
    int m = (v[11] - '0') * 10 + (v[12] - '0');
    if (h > 23 || m > 59)
        return;
    s->date = d;
    s->mod = (int16_t)(h * 60 + m);
}

/* ---- the store -------------------------------------------------------- */

static struct an_slot *an_find(struct an_store *st, uint32_t uid)
{
    for (int i = 0; i < st->n; i++)
        if (st->s[i].uid == uid)
            return &st->s[i];
    return 0;
}

static void an_drop_at(struct an_store *st, int i)
{
    memmove(&st->s[i], &st->s[i + 1], sizeof st->s[0] * (st->n - i - 1));
    st->n--;
    memset(&st->s[st->n], 0, sizeof st->s[0]);   /* never reuse stale */
    st->dirty = 1;
}

static void an_drop(struct an_store *st, uint32_t uid)
{
    for (int i = 0; i < st->n; i++)
        if (st->s[i].uid == uid) {
            an_drop_at(st, i);
            return;
        }
}

static void an_clear(struct an_store *st)
{
    memset(st, 0, sizeof *st);
    st->dirty = 1;
}

static int an_visible(const struct an_slot *s)
{
    return s->fetch == AN_DONE && (s->title[0] || s->sub[0] || s->msg[0]);
}

/* Notification Source: ev 0 added, 1 modified, 2 removed */
static void an_on_ns(struct an_store *st, int ev, int flags, int cat,
                     uint32_t uid, uint32_t now)
{
    if (ev == 2) {
        an_drop(st, uid);
        return;
    }
    struct an_slot *s = an_find(st, uid);
    if (!s) {
        if (st->n == AN_MAX)
            an_drop_at(st, 0);                  /* oldest goes */
        s = &st->s[st->n++];
        memset(s, 0, sizeof *s);
        s->uid = uid;
        s->rx_ms = now;
        s->mod = -1;
        s->alert = ev == 0 && !(flags & (AN_F_SILENT | AN_F_PREEXISTING));
        if (s->alert)
            st->buzz = 1;
    } else {
        s->tries = 0;
    }
    s->flags = (uint8_t)flags;
    s->cat = (uint8_t)cat;
    s->has_pos = (flags & AN_F_POS) != 0;
    s->has_neg = (flags & AN_F_NEG) != 0;
    s->fetch = AN_NEED;                         /* (re)fetch the text */
    st->dirty = 1;
}

/* the uid to fetch now, or 0: one fetch at a time, newest first; a
   fetch unanswered after AN_FETCH_MS is retried once, then dropped */
static uint32_t an_next_fetch(struct an_store *st, uint32_t now)
{
    for (int i = 0; i < st->n; i++) {
        struct an_slot *s = &st->s[i];
        if (s->fetch != AN_INFLIGHT)
            continue;
        if (now - s->sent_ms < AN_FETCH_MS)
            return 0;
        if (++s->tries >= 2) {
            if (s->title[0] || s->sub[0] || s->msg[0])
                s->fetch = AN_DONE;             /* keep the old text */
            else
                an_drop_at(st, i);
        } else
            s->fetch = AN_NEED;
        break;
    }
    for (int i = st->n - 1; i >= 0; i--)
        if (st->s[i].fetch == AN_NEED) {
            st->s[i].fetch = AN_INFLIGHT;
            st->s[i].sent_ms = now;
            return st->s[i].uid;
        }
    return 0;
}

/* the GetNotificationAttributes request for uid (18 bytes) */
static int an_request(uint8_t *req, uint32_t uid)
{
    int n = 0;
    req[n++] = 0x00;
    req[n++] = uid & 0xFF; req[n++] = (uid >> 8) & 0xFF;
    req[n++] = (uid >> 16) & 0xFF; req[n++] = (uid >> 24) & 0xFF;
    req[n++] = 0x00;                                   /* AppIdentifier */
    req[n++] = 0x01; req[n++] = AN_TITLE_MAX; req[n++] = 0;   /* Title */
    req[n++] = 0x02; req[n++] = AN_SUB_MAX; req[n++] = 0;     /* Subtitle */
    req[n++] = 0x03; req[n++] = AN_MSG_MAX; req[n++] = 0;     /* Message */
    req[n++] = 0x05;                                   /* Date */
    return n;
}

/* a (reassembled) Data Source reply. Returns 0 while more fragments are
   needed, 1 when it was complete and used, -1 when it is malformed. */
static int an_on_ds(struct an_store *st, const uint8_t *d, int len)
{
    if (len < 1 || d[0] != 0x00)
        return -1;
    if (len < 5)
        return 0;
    uint32_t uid = d[1] | d[2] << 8 | (uint32_t)d[3] << 16 |
                   (uint32_t)d[4] << 24;
    const uint8_t *val[6] = {0};
    int vl[6] = {0}, seen = 0, p = 5;
    while (p < len) {
        if (p + 3 > len)
            return 0;
        int id = d[p], al = d[p + 1] | d[p + 2] << 8;
        if (p + 3 + al > len)
            return 0;
        if (id <= 5) {
            val[id] = d + p + 3;
            vl[id] = al;
            seen |= 1 << id;
        }
        p += 3 + al;
    }
    if ((seen & 0x2F) != 0x2F)          /* app, title, sub, msg, date */
        return 0;
    struct an_slot *s = an_find(st, uid);
    if (!s)
        return 1;           /* it was removed meanwhile: make nothing */
    an_app_name(s->app, sizeof s->app, val[0], vl[0]);
    an_text(s->title, sizeof s->title, val[1], vl[1]);
    an_text(s->sub, sizeof s->sub, val[2], vl[2]);
    an_text(s->msg, sizeof s->msg, val[3], vl[3]);
    an_date(s, val[5], vl[5]);
    s->fetch = AN_DONE;
    if (!strcmp(s->sub, s->title))
        s->sub[0] = 0;                  /* some apps repeat the title */
    st->dirty = 1;
    if (!an_visible(s)) {
        an_drop(st, uid);               /* nothing to show */
        return 1;
    }
    /* the same text again (an app re-posting) replaces the older one */
    for (int i = 0; i < st->n; i++) {
        struct an_slot *o = &st->s[i];
        if (o != s && an_visible(o) && !strcmp(o->app, s->app) &&
            !strcmp(o->title, s->title) && !strcmp(o->msg, s->msg)) {
            an_drop_at(st, i);
            s = an_find(st, uid);
            break;
        }
    }
    if (s && s->alert) {
        s->alert = 0;
        st->popup = uid;
    }
    return 1;
}

static int an_count(const struct an_store *st)
{
    int c = 0;
    for (int i = 0; i < st->n; i++)
        c += an_visible(&st->s[i]);
    return c;
}

/* i-th visible notification, newest first */
static const struct an_slot *an_at(const struct an_store *st, int i)
{
    for (int k = st->n - 1; k >= 0; k--)
        if (an_visible(&st->s[k]) && i-- == 0)
            return &st->s[k];
    return 0;
}

#endif
