/* Host renderer for the device UI: builds every screen of firmware/ui_nav.c
   with the real LVGL config and fonts and writes each one to a PPM, so
   layout changes can be checked without flashing. Hardware calls are
   stubbed with plausible values.
   make -C tests/uisnap && ./tests/uisnap/snap out_dir

   It also audits every screen and exits non-zero on a failure:
   - tap targets: each clickable object's click area (ext click area
     included) must be at least TAP_MIN px on its short side, or a
     full-width row (>= TAP_ROW_W wide) at least TAP_ROW_H tall; no two
     click areas may overlap.
   - canvas screens (launcher, control centre, stats) are hit-tested
     pixel by pixel through their real hit functions; every target's
     region must hold a TAP_MIN x TAP_MIN square.
   - text: labels must sit inside their button and on screen; canvas
     text must fit the box draw_txt() gives it (it wraps otherwise).
   The findings go to stdout and <out_dir>/taps.txt. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define TAP_MIN   40
#define TAP_ROW_W 150
#define TAP_ROW_H 30
struct _lv_font_t;
static void snap_text(const struct _lv_font_t *f, const char *s, int x, int y,
                      int w);
#define UISNAP_TEXT_HOOK(f, s, x, y, w) snap_text(f, s, x, y, w)
static uint32_t fake_mmio[0x4000];
#define REG32(a) (fake_mmio[((uintptr_t)(a) >> 2) & 0x3FFF])
#include "../../firmware/ui_nav.c"

/* ---- hardware stubs ---- */
#include "stubs.inc"

/* ---- framebuffer ---- */
static uint16_t fb[240 * 240];
static uint16_t buf[240 * VOIDOS_DRAW_BUF_LINES];   /* same strip as the watch */

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const uint16_t *p = (const uint16_t *)px;
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++) {
            uint16_t v = *p++;
            fb[y * 240 + x] = (uint16_t)((v >> 8) | (v << 8));
        }
    lv_display_flush_ready(d);
}

static void save(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n240 240\n255\n");
    for (int i = 0; i < 240 * 240; i++) {
        uint16_t v = fb[i];
        unsigned char rgb[3] = {(unsigned char)((v >> 11) << 3),
                                (unsigned char)(((v >> 5) & 63) << 2),
                                (unsigned char)((v & 31) << 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void settle(void)
{
    for (int i = 0; i < 20; i++) {
        now_ms += 50;
        lv_timer_handler();
        if (cur == SCR_FACE)
            ui_face_tick();          /* the firmware's ui_tick does this */
    }
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
}

static FILE *audit, *taps;
static const char *cur_name = "";
static int fails, checks;

static void finding(int bad, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void finding(int bad, const char *fmt, ...)
{
    char m[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    checks++;
    if (bad)
        fails++;
    if (bad || getenv("SNAP_VERBOSE")) {
        printf("%-18s %s %s\n", cur_name, bad ? "FAIL" : "ok  ", m);
        if (taps)
            fprintf(taps, "%-18s %s %s\n", cur_name, bad ? "FAIL" : "ok  ", m);
    }
}

/* ---- text layout audit (2.2.2) ----
   Every piece of text on a shot becomes an ink box: labels from their
   laid-out text size, alignment and clip, canvas text from draw_txt().
   A shot fails when two text boxes share a pixel, when text is cut by
   its own box or an ancestor's, when text sits partly on a bar or
   divider, or when text inside a card or button comes closer than
   TXT_PAD px to the card's edge. */
#define TXT_PAD 2
struct tbox { int x1, y1, x2, y2; const void *obj; char s[40]; };
static struct tbox tb[96];
static int ntb;
static struct tbox shp[48];
static int nshp;
struct shape { int k, a, b, c, d, e; };
static struct shape shapes[160];
static int nshape;
static void shape_bbox(const struct shape *s, struct tbox *b)
{
    b->obj = NULL; b->s[0] = 0;
    if (s->k == 3) { b->x1 = s->a; b->y1 = s->b; b->x2 = s->c; b->y2 = s->d; }
    else if (s->k == 1) { b->x1 = s->a; b->y1 = s->b;
        b->x2 = s->a + s->c - 1; b->y2 = s->b + s->d - 1; }
    else { int hw = s->c * 866 / 1000; b->x1 = s->a - hw; b->x2 = s->a + hw;
        b->y1 = s->b - s->c; b->y2 = s->b + s->c; }
}
static void tb_add(int x1, int y1, int x2, int y2, const void *obj,
                   const char *s)
{
    for (int i = 0; i < ntb; i++)
        if (tb[i].x1 == x1 && tb[i].y1 == y1 && tb[i].x2 == x2 &&
            tb[i].y2 == y2 && !strncmp(tb[i].s, s, sizeof tb[i].s - 1))
            return;                       /* canvas redrawn: same text */
    if (ntb >= 96)
        return;
    struct tbox *b = &tb[ntb++];
    b->x1 = x1; b->y1 = y1; b->x2 = x2; b->y2 = y2; b->obj = obj;
    snprintf(b->s, sizeof b->s, "%s", s);
}

/* rows of ink inside a line box: top and bottom glyph pixel rows of
   the text, measured from the line's top */
static void ink_rows(const lv_font_t *f, const char *s, int *top, int *bot)
{
    int base = f->line_height - f->base_line, t = f->line_height, b = -1;
    for (const char *p = s; *p; p++) {
        if (*p == ' ' || *p == '\n')
            continue;
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(f, &g, (uint8_t)*p, 0) || !g.box_h)
            continue;
        int gt = base - (g.ofs_y + g.box_h), gb = base - g.ofs_y - 1;
        if (gt < t) t = gt;
        if (gb > b) b = gb;
    }
    if (b < 0) { t = 0; b = f->line_height - 1; }
    *top = t; *bot = b;
}

static void snap_text(const struct _lv_font_t *f, const char *s, int x, int y,
                      int w)
{
    int tw = (int)lv_text_get_width(s, strlen(s), (const lv_font_t *)f, 0);
    int cx = x + w / 2;
    if (tw > 0) {
        int it, ib;
        ink_rows((const lv_font_t *)f, s, &it, &ib);
        tb_add(cx - tw / 2, y + it, cx - tw / 2 + tw - 1, y + ib, NULL, s);
    }
    finding(tw > w || cx - tw / 2 < 0 || cx + tw / 2 > 240,
            "canvas text \"%s\" %d px in a %d px box at %d,%d", s, tw, w,
            cx - tw / 2, y);
}

static void label_box(lv_obj_t *o)
{
    const char *t = lv_label_get_text(o);
    const lv_font_t *f = lv_obj_get_style_text_font(o, LV_PART_MAIN);
    lv_area_t c;
    lv_obj_get_content_coords(o, &c);
    int cw = c.x2 - c.x1 + 1, ch = c.y2 - c.y1 + 1;
    lv_label_long_mode_t lm = lv_label_get_long_mode(o);
    int ls = lv_obj_get_style_text_line_space(o, LV_PART_MAIN);
    int lsp = lv_obj_get_style_text_letter_space(o, LV_PART_MAIN);
    lv_point_t sz;
    int maxw = (lm == LV_LABEL_LONG_MODE_WRAP || lm == LV_LABEL_LONG_MODE_DOTS)
               && lv_obj_get_style_width(o, LV_PART_MAIN) != LV_SIZE_CONTENT
               ? cw : LV_COORD_MAX;
    lv_text_get_size(&sz, t, f, lsp, ls, maxw, LV_TEXT_FLAG_NONE);
    int tw = sz.x < cw ? sz.x : cw, th = sz.y;
    int cut = th > ch + 1 && lm != LV_LABEL_LONG_MODE_DOTS &&
              lm != LV_LABEL_LONG_MODE_SCROLL &&
              lm != LV_LABEL_LONG_MODE_SCROLL_CIRCULAR;
    if (th > ch)
        th = ch;
    lv_text_align_t al = lv_obj_get_style_text_align(o, LV_PART_MAIN);
    int x1 = c.x1;
    if (al == LV_TEXT_ALIGN_CENTER)
        x1 = c.x1 + (cw - tw) / 2;
    else if (al == LV_TEXT_ALIGN_RIGHT)
        x1 = c.x2 - tw + 1;
    int it, ib;
    ink_rows(f, t, &it, &ib);
    int lh = f->line_height;
    int x2 = x1 + tw - 1, y1 = c.y1 + it, y2 = c.y1 + th - lh + ib;
    if (y2 > c.y2) y2 = c.y2;
    finding(cut, "text \"%.30s\" needs %d px, its box is %d px: cut off",
            t, sz.y, ch);
    /* ancestors clip their children */
    for (lv_obj_t *p = lv_obj_get_parent(o); p && lv_obj_get_parent(p);
         p = lv_obj_get_parent(p)) {
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_OVERFLOW_VISIBLE))
            continue;
        lv_area_t pa;
        lv_obj_get_coords(p, &pa);
        if (x1 < pa.x1 || x2 > pa.x2 || y1 < pa.y1 || y2 > pa.y2) {
            finding(1, "text \"%.30s\" at %d,%d-%d,%d is cut by its container"
                    " %d,%d-%d,%d", t, x1, y1, x2, y2, pa.x1, pa.y1, pa.x2, pa.y2);
            break;
        }
        /* padding inside the nearest visible card or button */
        if (lv_obj_get_style_bg_opa(p, LV_PART_MAIN) > 0 ||
            lv_obj_get_style_border_width(p, LV_PART_MAIN) > 0) {
            int w = pa.x2 - pa.x1 + 1;
            if (w < 236) {
                int m = x1 - pa.x1;
                if (pa.x2 - x2 < m) m = pa.x2 - x2;
                if (y1 - pa.y1 < m) m = y1 - pa.y1;
                if (pa.y2 - y2 < m) m = pa.y2 - y2;
                finding(m < TXT_PAD, "text \"%.30s\" is %d px from its card's"
                        " edge (min %d)", t, m, TXT_PAD);
            }
            break;
        }
    }
    tb_add(x1, y1, x2, y2, o, t);
}

static int ovl(const struct tbox *a, const struct tbox *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

/* drop screen text that an opaque overlay object covers */
static void occlude(lv_obj_t *o)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
        return;
    /* wl_note paints its own opaque card in a draw callback */
    if (lv_obj_get_parent(o) &&
        (lv_obj_get_style_bg_opa(o, LV_PART_MAIN) >= LV_OPA_COVER ||
         o == wl_note)) {
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        struct tbox ob = {a.x1, a.y1, a.x2, a.y2, o, ""};
        int k = 0;
        for (int i = 0; i < ntb; i++)
            if (!ovl(&tb[i], &ob))
                tb[k++] = tb[i];
        ntb = k;
        k = 0;
        for (int i = 0; i < nshp; i++)
            if (!ovl(&shp[i], &ob))
                shp[k++] = shp[i];
        nshp = k;
        k = 0;
        for (int i = 0; i < nshape; i++) {
            struct tbox sb;
            shape_bbox(&shapes[i], &sb);
            if (!ovl(&sb, &ob))
                shapes[k++] = shapes[i];
        }
        nshape = k;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
        occlude(lv_obj_get_child(o, i));
}

/* ---- shapes (2.2.2): every dk_oct card, dk_hex and dk_rect bar the
   canvases draw. Text must sit wholly inside a shape (2 px clear of its
   outline) or wholly outside it (1 px clear): text half on an outline
   is what "RECOVERY" spilling out of its hex looked like. */
void uisnap_shape(int k, int a, int b, int c, int d, int e)
{
    for (int i = 0; i < nshape; i++)
        if (shapes[i].k == k && shapes[i].a == a && shapes[i].b == b &&
            shapes[i].c == c && shapes[i].d == d && shapes[i].e == e)
            return;                   /* drawn again for the next strip */
    if (nshape < 160)
        shapes[nshape++] = (struct shape){k, a, b, c, d, e};
}

static int poly_in(const int *px, const int *py, int n, int x, int y)
{
    int in = 0;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if ((py[i] > y) != (py[j] > y) &&
            x < (px[j] - px[i]) * (y - py[i]) / (double)(py[j] - py[i]) + px[i])
            in = !in;
    return in;
}

static int shape_in(const struct shape *s, int x, int y)
{
    if (s->k == 3)
        return x >= s->a && x <= s->c && y >= s->b && y <= s->d;
    if (s->k == 1) {
        int x0 = s->a, y0 = s->b, x2 = s->a + s->c - 1, y2 = s->b + s->d - 1,
            cut = s->e;
        int px[8] = {x0 + cut, x2 - cut, x2, x2, x2 - cut, x0 + cut, x0, x0};
        int py[8] = {y0, y0, y0 + cut, y2 - cut, y2, y2, y2 - cut, y0 + cut};
        return poly_in(px, py, 8, x, y);
    }
    int cx = s->a, cy = s->b, r = s->c, hw = r * 866 / 1000;
    int px[6] = {cx, cx + hw, cx + hw, cx, cx - hw, cx - hw};
    int py[6] = {cy - r, cy - r / 2, cy + r / 2, cy + r, cy + r / 2, cy - r / 2};
    return poly_in(px, py, 6, x, y);
}

/* pixels of the box grown by g that fall inside the shape, and the total */
static void box_in(const struct shape *s, const struct tbox *t, int g,
                   int *in, int *all)
{
    *in = *all = 0;
    for (int y = t->y1 - g; y <= t->y2 + g; y++)
        for (int x = t->x1 - g; x <= t->x2 + g; x++) {
            (*all)++;
            *in += shape_in(s, x, y);
        }
}

static int text_straddles;
static void shape_audit(void)
{
    static const char *kn[4] = {"", "card", "hex", "bar"};
    for (int i = 0; i < ntb; i++)
        for (int k = 0; k < nshape; k++) {
            const struct shape *s = &shapes[k];
            int in2, all2, in1, all1;
            box_in(s, &tb[i], 2, &in2, &all2);
            if (in2 == all2)
                continue;                     /* wholly inside */
            box_in(s, &tb[i], 1, &in1, &all1);
            if (in1 == 0)
                continue;                     /* wholly outside */
            /* a small bar (icon stroke, progress fill) never holds text */
            text_straddles++;
            finding(1, "text \"%s\" %d,%d-%d,%d sits on the edge of a %s"
                    " (%d,%d,%d,%d)", tb[i].s, tb[i].x1, tb[i].y1, tb[i].x2,
                    tb[i].y2, kn[s->k], s->a, s->b, s->c, s->d);
        }
    finding(0, "shapes: %d", nshape);
}

static int text_overlaps;
static void text_audit(void)
{
    for (int i = 0; i < ntb; i++)
        for (int j = i + 1; j < ntb; j++)
            if (ovl(&tb[i], &tb[j])) {
                text_overlaps++;
                finding(1, "text \"%s\" %d,%d-%d,%d overlaps \"%s\" %d,%d-%d,%d",
                        tb[i].s, tb[i].x1, tb[i].y1, tb[i].x2, tb[i].y2,
                        tb[j].s, tb[j].x1, tb[j].y1, tb[j].x2, tb[j].y2);
            }
    for (int i = 0; i < ntb; i++)
        for (int k = 0; k < nshp; k++)
            if (ovl(&tb[i], &shp[k]))
                finding(1, "text \"%s\" %d,%d-%d,%d is drawn over a bar at"
                        " %d,%d-%d,%d", tb[i].s, tb[i].x1, tb[i].y1, tb[i].x2,
                        tb[i].y2, shp[k].x1, shp[k].y1, shp[k].x2, shp[k].y2);
    finding(0, "text layout: %d text boxes", ntb);
}

static lv_area_t tgt[64];
static int ntgt;
static void walk(lv_obj_t *o, int depth)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int w = a.x2 - a.x1 + 1, h = a.y2 - a.y1 + 1;
    int hidden = lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
    if (hidden) return;
    int click = lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE);
    if (click && depth > 1 && !(w >= 200 && h >= 200)) {
        lv_area_t c;
        lv_obj_get_click_area(o, &c);
        int cw = c.x2 - c.x1 + 1, ch = c.y2 - c.y1 + 1;
        int sh = cw < ch ? cw : ch, lg = cw < ch ? ch : cw;
        const char *txt = lv_obj_get_child_count(o) &&
            lv_obj_check_type(lv_obj_get_child(o, 0), &lv_label_class)
            ? lv_label_get_text(lv_obj_get_child(o, 0)) : "";
        finding(!(sh >= TAP_MIN || (lg >= TAP_ROW_W && sh >= TAP_ROW_H)),
                "tap \"%s\" %dx%d at %d,%d", txt, cw, ch, c.x1, c.y1);
        for (int i = 0; i < ntgt; i++) {
            lv_area_t *t = &tgt[i];
            if (c.x1 <= t->x2 && t->x1 <= c.x2 && c.y1 <= t->y2 && t->y1 <= c.y2)
                finding(1, "tap \"%s\" at %d,%d overlaps another at %d,%d",
                        txt, c.x1, c.y1, t->x1, t->y1);
        }
        if (ntgt < 64)
            tgt[ntgt++] = c;
    }
    if (lv_obj_check_type(o, &lv_label_class)) {
        lv_obj_t *par = lv_obj_get_parent(o);
        lv_area_t pa;
        lv_obj_get_coords(par, &pa);
        int inside_btn = lv_obj_check_type(par, &lv_button_class);
        int off = a.x1 < 0 || a.x2 > 239 || a.y1 < 0 || a.y2 > 239;
        int spill = inside_btn && (a.x1 < pa.x1 || a.x2 > pa.x2 ||
                                   a.y1 < pa.y1 || a.y2 > pa.y2);
        if (off || spill)
            finding(1, "label \"%s\" %s", lv_label_get_text(o),
                    off ? "runs off screen" : "spills out of its button");
    }
    if (lv_obj_check_type(o, &lv_label_class) && *lv_label_get_text(o))
        label_box(o);
    else if (!lv_obj_check_type(o, &lv_label_class) &&
             lv_obj_get_child_count(o) == 0 && depth > 1 &&
             lv_obj_get_style_bg_opa(o, LV_PART_MAIN) > 0 &&
             (lv_obj_check_type(o, &lv_bar_class) || h <= 6 || w <= 6) &&
             nshp < 48) {
        struct tbox *b = &shp[nshp++];      /* bar, divider, tick */
        b->x1 = a.x1; b->y1 = a.y1; b->x2 = a.x2; b->y2 = a.y2; b->obj = o;
        b->s[0] = 0;
    }
    const char *kind = lv_obj_check_type(o, &lv_label_class) ? "label" :
                       lv_obj_check_type(o, &lv_button_class) ? "button" : "obj";
    if (!strcmp(kind, "label")) {
        const lv_font_t *f = lv_obj_get_style_text_font(o, LV_PART_MAIN);
        fprintf(audit, "%*s%s %d,%d %dx%d font_h=%d%s \"%s\"\n", depth * 2, "", kind,
                a.x1, a.y1, w, h, f ? f->line_height : 0, click ? " CLICK" : "",
                lv_label_get_text(o));
    } else {
        fprintf(audit, "%*s%s %d,%d %dx%d%s\n", depth * 2, "", kind, a.x1, a.y1, w, h,
                click ? " CLICK" : "");
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
        walk(lv_obj_get_child(o, i), depth + 1);
}

/* largest axis-aligned square inside each region id of a 240x240 map */
static int big_square(const signed char *map, int id)
{
    static short dp[240 * 240];
    int best = 0;
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 240; x++) {
            int i = y * 240 + x, v = 0;
            if (map[i] == id) {
                v = 1;
                if (x && y) {
                    int a = dp[i - 1], b = dp[i - 240], c = dp[i - 241];
                    int m = a < b ? a : b;
                    v = 1 + (m < c ? m : c);
                }
            }
            dp[i] = (short)v;
            if (v > best)
                best = v;
        }
    return best;
}

static void canvas_audit(int (*hit)(int, int), int n, int hub,
                         const char *const *names)
{
    static signed char map[240 * 240];
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 240; x++)
            map[y * 240 + x] = (signed char)hit(x, y);
    for (int id = -2; id <= n; id++) {
        if (id == -1 || (id == -2 && hub != -2 && !names))
            continue;
        int sq = big_square(map, id);
        if (id == -2 && sq == 0)
            continue;
        finding(sq < (id == -2 ? TAP_ROW_H : TAP_MIN),
                "canvas %s holds a %dx%d square",
                id == n ? "hub" : id == -2 ? "strip" : names[id], sq, sq);
    }
}

static unsigned heap_peak, heap_total;

static void shot(const char *dir, scr_t s, const char *name)
{
    cur_name = name;
    ntgt = 0;
    ntb = 0;
    nshp = 0;
    nshape = 0;
    cur = s;
    rebuild();
    settle();
    save(dir, name);
    char path[512];
    snprintf(path, sizeof path, "%s/%s.txt", dir, name);
    audit = fopen(path, "w");
    walk(lv_screen_active(), 0);
    if (lv_obj_get_child_count(lv_layer_top())) {
        occlude(lv_layer_top());        /* overlay hides the screen under it */
        walk(lv_layer_top(), 0);
    }
    text_audit();
    shape_audit();
    {
        static const char *lcn[LC_N], *ccn[CCW_DEFN], *stn[16];
        for (int i = 0; i < LC_N; i++) lcn[i] = lc_apps[i].name;
        for (int i = 0; i < ccw_n; i++) ccn[i] = ccw_def[ccw_list[i]].name;
        for (int i = 0; i < ST_N; i++) stn[i] = st_def[i].name;
        if (s == SCR_LAUNCHER) canvas_audit(lc_hit, LC_N, LC_N, lcn);
        if (s == SCR_CC)       canvas_audit(ccw_hit, ccw_n, ccw_n, ccn);
        if (s == SCR_STATS)    canvas_audit(st_hit, ST_N, -2, stn);
    }
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    fprintf(audit, "LVGL heap: used %u%% max_used %u free_biggest %u\n",
            (unsigned)m.used_pct, (unsigned)m.max_used, (unsigned)m.free_biggest_size);
    fclose(audit);
    /* heap.txt: live use after the shot and the running peak; a shot
       that raises the peak is the one that set it */
    static FILE *heap;
    static unsigned peak;
    if (!heap) {
        snprintf(path, sizeof path, "%s/heap.txt", dir);
        heap = fopen(path, "w");
        fprintf(heap, "LV_MEM_SIZE %u, pool %u B usable\n",
                (unsigned)LV_MEM_SIZE, (unsigned)m.total_size);
    }
    unsigned used = (unsigned)(m.total_size - m.free_size);
    fprintf(heap, "%-30s now %5u peak %5u%s\n", name, used,
            (unsigned)m.max_used, m.max_used > peak ? "  <- new peak" : "");
    if (m.max_used > peak)
        peak = m.max_used;
    fflush(heap);
    heap_peak = peak;
    heap_total = (unsigned)m.total_size;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    lv_init();
    if (argc > 2 && !strcmp(argv[1], "-m")) {
        const lv_font_t *fs[] = {&kenney9, &kenney12, &kenney16, &kenney24};
        for (int i = 2; i < argc; i++) {
            printf("%-16s", argv[i]);
            for (int k = 0; k < 4; k++)
                printf(" %4d", (int)lv_text_get_width(argv[i], strlen(argv[i]), fs[k], 0));
            printf("\n");
        }
        return 0;
    }
    lv_tick_set_cb(hal_millis_raw);
    lv_display_t *d = lv_display_create(240, 240);
    lv_display_set_flush_cb(d, flush);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(d, buf, NULL, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    ui_init();
    char tp[512];
    snprintf(tp, sizeof tp, "%s/taps.txt", dir);
    taps = fopen(tp, "w");
    shot(dir, SCR_FACE, "00_face");
    face_dig = 0; shot(dir, SCR_FACE, "01_face_analog"); face_dig = 1;
    snap_stale = 1; shot(dir, SCR_FACE, "08_face_bpm_held");
    face_dig = 0; shot(dir, SCR_FACE, "09_face_analog_bpm_held"); face_dig = 1;
    snap_stale = 0;
    snap_bpm = 0; shot(dir, SCR_FACE, "02_face_measuring");
    snap_hr_on = 0; shot(dir, SCR_FACE, "03_face_hr_off");
    snap_hr_on = 1; snap_bpm = 72;
    snap_music_data.pos_ms = now_ms;
    /* face states: music chip (long title, no artist, paused, gone),
       water chip, unsynced / restored clock */
    snap_music_data.playing = 0; snap_music_recent = 0;
    shot(dir, SCR_FACE, "04_face_no_music");
    snap_music_recent = 1;
    shot(dir, SCR_FACE, "04_face_music_paused");
    snap_music_data.playing = 1;
    {
        struct music keep = snap_music_data;
        snprintf(snap_music_data.track, sizeof snap_music_data.track, "%s", "Harder, Better, Faster, Stronger (Alive 2007 Live)");
        snprintf(snap_music_data.artist, sizeof snap_music_data.artist, "%s", "Daft Punk and The Very Long Orchestra of Everything");
        shot(dir, SCR_FACE, "04_face_music_long");
        shot(dir, SCR_MUSIC, "33_music_long_title");
        snap_music_data.artist[0] = 0; snap_music_data.album[0] = 0;
        shot(dir, SCR_FACE, "04_face_music_no_artist");
        shot(dir, SCR_MUSIC, "33_music_no_artist");
        snprintf(snap_music_data.track, sizeof snap_music_data.track, "%s", "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
        snprintf(snap_music_data.artist, sizeof snap_music_data.artist, "%s", "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
        snap_music_data.len_s = 5999; snap_music_data.pos_s = 5998;
        shot(dir, SCR_MUSIC, "33_music_widest");
        snap_music_data.playing = 0;
        shot(dir, SCR_FACE, "04_face_music_widest_paused");
        snap_music_data = keep;
        snap_music_data.track[0] = snap_music_data.artist[0] = 0;
        snap_music_data.album[0] = 0; snap_music_data.playing = 0;
        snap_music_data.len_s = 0;
        shot(dir, SCR_MUSIC, "34_music_stopped");
        snap_music_data = keep;
    }
    DL.water_ml = 1250; shot(dir, SCR_FACE, "05_face_water_1250");
    DL.water_ml = 10000; shot(dir, SCR_FACE, "05_face_water_max");
    DL.water_ml = 0;
    snap_synced = 0; snap_restored = 1;
    shot(dir, SCR_FACE, "06_face_clock_restored");
    snap_restored = 0; shot(dir, SCR_FACE, "07_face_clock_unset");
    snap_synced = 1;
    snap_music_data.pos_ms = now_ms;
    shot(dir, SCR_MUSIC, "33_music_playing");
    snap_music_data.playing = 0; shot(dir, SCR_MUSIC, "34_music_paused");
    snap_music_data.playing = 1;
    /* overlays live on the top layer, over whatever screen is up */
    torch_set(1); shot(dir, SCR_FACE, "35_torch_on"); torch_set(0);
    wlock_set(1); shot(dir, SCR_FACE, "36_water_lock_notice");
    wlock_set(0);
    shot(dir, SCR_LAUNCHER, "10_launcher");
    shot(dir, SCR_CC, "11_cc");
    cc_hide = 7; shot(dir, SCR_CC, "11_cc_min"); cc_hide = 0;
    for (int f = 0; f < CCW_DEFN; f++) {     /* hub contents per control */
        ccw_focus = f; char nm[40];
        snprintf(nm, sizeof nm, "11_cc_f%d_%s", f, ccw_def[f].name);
        for (char *q = nm; *q; q++)
            if (*q == ' ') *q = '_';
        shot(dir, SCR_CC, nm);
    }
    /* toggles switched the other way, low brightness */
    dnd_on = 1; airplane_on = 1; bt_on = 0; brightness = 30;
    ccw_focus = 2; shot(dir, SCR_CC, "11_cc_toggles_flipped");
    dnd_on = 0; airplane_on = 0; bt_on = 1; brightness = 100;
    torch_set(1); ccw_focus = 5; shot(dir, SCR_CC, "11_cc_torch_on");
    torch_set(0);
    ccw_focus = 0;
    for (int f = 0; f < LC_N; f++) {
        lc_focus = f; char nm[24]; snprintf(nm, sizeof nm, "10_launcher_f%d", f);
        cur_name = nm; cur = SCR_LAUNCHER; rebuild(); settle();
    }
    lc_focus = 0;
    shot(dir, SCR_WORKOUT, "12_workout");
    w_active = 1; w_start_ms = now_ms - 754000; shot(dir, SCR_W_RUN, "13_w_run");
    w_last_ms = 1754000; w_last_sets = 2; shot(dir, SCR_W_DONE, "14_w_done");
    w_active = 0;
    shot(dir, SCR_SLEEP, "15_sleep");
    s_in_bed = 1; s_start_ms = now_ms - (6u * 3600000u + 25u * 60000u);
    shot(dir, SCR_SLEEP, "15_sleep_in_bed"); s_in_bed = 0;
    shot(dir, SCR_S_DONE, "16_s_done");
    DL.water_ml = 750; shot(dir, SCR_WATER, "17_water_no_yesterday");
    DL.y_day = 20261027; DL.y_water = 2250;
    DL.y_steps = 11234; DL.y_rest = 58;
    shot(dir, SCR_WATER, "17_water");
    DL.water_ml = 10000; shot(dir, SCR_WATER, "17_water_max");
    DL.water_ml = 750;
    clock_tab = 0; shot(dir, SCR_CLOCK, "18_clock_sw");
    sw_running = 1; sw_t0 = now_ms - 83400; sw_acc = 0;
    shot(dir, SCR_CLOCK, "18_clock_sw_running"); sw_running = 0; sw_t0 = 0;
    clock_tab = 1; shot(dir, SCR_CLOCK, "19_clock_timer");
    clock_tab = 2; shot(dir, SCR_CLOCK, "20_clock_alarm");
    alarm_ringing = 1; shot(dir, SCR_CLOCK, "20_clock_ringing"); alarm_ringing = 0;
    clock_tab = 1; timer_end_ms = now_ms + 300000;
    shot(dir, SCR_CLOCK, "19_clock_timer_run"); timer_end_ms = 0;
    timer_done = 1; shot(dir, SCR_CLOCK, "19_clock_timer_done"); timer_done = 0;
    for (set_page = 0; set_page < 3; set_page++) {
        char nm[24]; snprintf(nm, sizeof nm, "21_settings_p%d", set_page + 1);
        shot(dir, SCR_SETTINGS, nm);
    }
    set_page = 0;
    shot(dir, SCR_STATS, "22_stats");
    shot(dir, SCR_SETTIME, "23_settime");
    shot(dir, SCR_NOTIF, "24_notif");
    notif_sel_uid = 100; shot(dir, SCR_NOTIF, "25_notif_open");
    notif_reply = 1; shot(dir, SCR_NOTIF, "25_notif_reply_confirm");
    notif_reply = 0;
    notif_sel_uid = 102; shot(dir, SCR_NOTIF, "25_notif_open_no_reply");
    /* worst case the phone can send: 6 notifications, every field full */
    snap_notif_stress = 1; notif_sel_uid = 0; notif_lpage = 0;
    shot(dir, SCR_NOTIF, "24_notif_full_p1");
    notif_lpage = 1; shot(dir, SCR_NOTIF, "24_notif_full_p2");
    notif_lpage = 0;
    for (int u = 200; u < 206; u++) {
        char nm[40];
        notif_sel_uid = u;
        for (notif_dpage = 0; notif_dpage < 4; notif_dpage++) {
            snprintf(nm, sizeof nm, "25_notif_long_%d_p%d", u - 199,
                     notif_dpage + 1);
            shot(dir, SCR_NOTIF, nm);
            if (notif_dpage + 1 >= notif_dpages)
                break;
        }
        notif_dpage = 0;
    }
    notif_sel_uid = 0; snap_notif_stress = 0;
    snap_notif_none = 1; shot(dir, SCR_NOTIF, "24_notif_empty");
    snap_notif_none = 0;
    /* a notification arriving pops up over the face */
    shot(dir, SCR_FACE, "00_face_before_popup");
    notif_popup_show(101);
    shot(dir, cur, "24_notif_popup");
    finding(cur == SCR_NOTIF && notif_sel_uid == 101 ? 0 : 1,
            "new notification opens its detail (screen %d uid %u)", cur,
            (unsigned)notif_sel_uid);
    notif_close();
    finding(cur == SCR_FACE ? 0 : 1, "back from the pop-up returns to the face");
    notif_sel_uid = 0;
    for (int bp = 0; bp < 6; bp++) {
        static const int bpms[6] = {0, 62, 104, 128, 147, 188};
        char nm[32];
        snap_bpm = bpms[bp];
        snprintf(nm, sizeof nm, "26_cardio_%d", bpms[bp]);
        shot(dir, SCR_CARDIO, nm);
    }
    snap_bpm = 72;
    shot(dir, SCR_CARDIO, "26_cardio");
    snap_stale = 1; shot(dir, SCR_CARDIO, "26_cardio_bpm_held");
    shot(dir, SCR_STATS, "22_stats_bpm_held"); snap_stale = 0;
    shot(dir, SCR_DIAG, "27_diag");
    cur_name = "";
    printf("wrote screens to %s; tap/text audit: %d checks, %d failed\n",
           dir, checks, fails);
    printf("LVGL heap: peak %u B of %u B usable (LV_MEM_SIZE %u)\n",
           heap_peak, heap_total, (unsigned)LV_MEM_SIZE);
    if (taps) {
        fprintf(taps, "%d checks, %d failed\n", checks, fails);
        fclose(taps);
    }
    return fails ? 1 : 0;
}
