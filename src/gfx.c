/* gfx.c - the menu's drawing layer: an 8x8 face, 1-bpp icons, rectangles and 16-bit pictures, all as cels.
 *
 * The parts of the screen that stay put (background, logo, panel, tabs, frames) are drawn once into a spare VRAM
 * buffer; each frame starts by copying that buffer into the frame being drawn with a SPORT transfer, which runs in
 * the vertical blank (so it also paces the menu, and the buffer it lands in is off screen by then) and costs the
 * cel engine nothing. Everything else is queued as a chain of cels and handed to the cel engine in one DrawCels
 * call at the end. Text is rendered into small 1-bpp cels each frame: with an
 * 8x8 face that is a byte per glyph row, cheaper than keeping cached text around. Colours are palette roles;
 * 1-bpp cels take theirs from a two-entry PLUT per role, so the selection bar's colour can change every frame.
 * A pixel value of 0 is transparent to the cel engine, so no colour here is ever 0 (black is 0x0001).
 */
#include "common.h"
#define UIART_DATA
#include "uiart.h"

static void rect_rgb(int32 x, int32 y, int32 w, int32 h, int32 c);
static uint32 vbl_count;
static uint8 *bgbuf;            /* the cached still parts of the screen, in VRAM */
static int32  bg_key = -1;      /* what bgbuf holds (the caller's key), -1 nothing */
static int32  bg_pending = -1;

#define NCCB        448
#define NTXT        128
#define TXT_CHARS   40
#define TXT_BYTES   (TXT_CHARS * 8)
#define PIXC_DIM3   0x08000800      /* source x 3/16 */

ScreenContext *g_sc = 0;

static Item   vram_ioreq = -1;
static Item   vbl_ioreq = -1;
static Item   display = -1;
static int32  cur_screen;
static int32  dim;

static CCB   *pool;
static int32  npool;
static uint8 *tbufs;
static int32  ntxt;
static CCB   *head, *tail;
static CCB   *rect_tmpl;
static CCB   *spr_tmpl[SPRITE_COUNT];
static uint16 *pluts;               /* [2][P_COUNT][4] */
static int32  colors[2][P_COUNT];

static const uint8 synth[P_COUNT][3] = {
    {8, 2, 20},         /* P_BG */
    {36, 14, 70},       /* P_PANEL */
    {110, 50, 170},     /* P_EDGE */
    {160, 130, 200},    /* P_DIM */
    {245, 235, 255},    /* P_TEXT */
    {60, 230, 255},     /* P_ACCENT */
    {255, 200, 80},     /* P_FOLDER */
    {255, 80, 170},     /* P_HEART */
    {255, 140, 80},     /* P_ORANGE */
    {90, 255, 180},     /* P_GREEN */
    {190, 120, 255},    /* P_VIOLET */
    {30, 8, 60},        /* P_BARTEXT */
    {255, 120, 220},    /* P_BAR */
};

static int32 dimmed(int32 c)
{
    int32 r = ((c >> 10) & 31) * 3 / 16, g = ((c >> 5) & 31) * 3 / 16, b = (c & 31) * 3 / 16;
    c = (r << 10) | (g << 5) | b;
    return c ? c : 1;
}

static void set_role(int32 role, int32 c)
{
    if (c == 0)
        c = 1;
    colors[0][role] = c;
    colors[1][role] = dimmed(c);
    pluts[(0 * P_COUNT + role) * 4 + 1] = (uint16)colors[0][role];
    pluts[(1 * P_COUNT + role) * 4 + 1] = (uint16)colors[1][role];
}

int32 gfx_color(int32 role)
{
    return colors[dim][role];
}

void gfx_set_dim(int32 on)
{
    dim = on ? 1 : 0;
}

void gfx_set_bar(int32 rgb15)
{
    set_role(P_BAR, rgb15);
}

/* ------------------------------------------------------------------ the cel chain */

#ifdef DIAG
static int32 st_frames[2], st_drawerr[2], st_disperr, st_last_draw, st_last_disp, st_slow;
static uint32 st_vbl0;
#endif

static void flush(void)
{
    Err e;

    if (head) {
        e = DrawCels(g_sc->sc_BitmapItems[cur_screen], head);
#ifdef DIAG
        if (e < 0) {
            st_drawerr[cur_screen & 1]++;
            st_last_draw = e;
        }
#endif
        (void)e;
    }
    head = tail = 0;
    npool = 0;
    ntxt = 0;
}

static CCB *new_ccb(void)
{
    if (npool >= NCCB)
        flush();
    return &pool[npool++];
}

static void queue(CCB *c)
{
    c->ccb_NextPtr = 0;
    c->ccb_Flags |= CCB_LAST;
    if (tail) {
        tail->ccb_NextPtr = c;
        tail->ccb_Flags &= ~CCB_LAST;
    } else {
        head = c;
    }
    tail = c;
}

/* ------------------------------------------------------------------ setup */

static CCB *make_sprite(int32 id)
{
    const SpriteDef *d = &sprite_defs[id];
    int32 bpr = (d->w + 7) / 8, rb, y, k;
    uint8 *data;
    CCB *c;

    rb = (((d->w + 31) >> 5) << 2);
    if (rb < 8)
        rb = 8;
    data = (uint8 *)AllocMem(rb * d->h, MEMTYPE_CEL);
    if (data == 0)
        return 0;
    memset(data, 0, rb * d->h);
    for (y = 0; y < d->h; y++)
        for (k = 0; k < bpr; k++)
            data[y * rb + k] = sprite_bits[d->off + y * bpr + k];
    c = CreateCel(d->w, d->h, 1, CREATECEL_CODED, data);
    return c;
}

#ifdef DIAG
/* Diagnostic build: a whole-screen colour written by the CPU (no cel engine, no SPORT), then about 0.75 s.
 * bars = number of white bars, to tell failures of the same stage apart. */
static int32 diag_ready;

static int32 display_open(void);

void gfx_diag(int32 color, int32 bars)
{
    uint16 *p;
    int32 x, y, i, b;

    if (!diag_ready) {
        if (display_open() < 0)
            return;
        diag_ready = 1;
    }
    cur_screen = 1 - cur_screen;
    p = (uint16 *)g_sc->sc_Bitmaps[cur_screen]->bm_Buffer;
    for (y = 0; y < SCR_H; y++)
        for (x = 0; x < SCR_W; x++) {
            b = 0;
            for (i = 0; i < bars; i++)
                if (y >= 40 + i * 24 && y < 52 + i * 24 && x >= 40 && x < 280)
                    b = 1;
            p[((y >> 1) * SCR_W + x) * 2 + (y & 1)] = (uint16)(b ? 0x7fff : color);
        }
    DisplayScreen(g_sc->sc_ScreenItems[cur_screen], 0);
    for (i = 0; i < 45; i++)
        gfx_wait_vbl();
}
#endif

static int32 display_open(void)
{
    int32 type;

    if (display > 0)
        return 0;
    vram_ioreq = GetVRAMIOReq();
    if (vram_ioreq < 0)
        return -1;
    g_sc = (ScreenContext *)AllocMem(sizeof(ScreenContext), MEMTYPE_ANY);
    if (g_sc == 0)
        return -2;
    if (OpenGraphicsFolio() < 0)
        return -3;
    OpenMathFolio();            /* lib3do's cel mapping calls go through it */
    type = GetDisplayType();
    if (type < 0)
        return -4;
    type = (type == DI_TYPE_PAL1 || type == DI_TYPE_PAL2) ? DI_TYPE_PAL1 : DI_TYPE_NTSC;
    display = CreateBasicDisplay(g_sc, type, 2);
    if (display < 0)
        return -5;
    vbl_ioreq = GetVBLIOReq();
    return 0;
}

int32 gfx_init(void)
{
    int32 i, r;

    r = display_open();
    if (r < 0)
        return r;
    DIAG_STEP(0x0010, 1);                       /* gfx sub-steps: dark blue with n bars */
    pool = (CCB *)AllocMem(NCCB * sizeof(CCB), MEMTYPE_CEL);
    DIAG_STEP(0x0010, 2);
    tbufs = (uint8 *)AllocMem(NTXT * TXT_BYTES, MEMTYPE_CEL);
    pluts = (uint16 *)AllocMem(2 * P_COUNT * 4 * sizeof(uint16), MEMTYPE_CEL);
    DIAG_STEP(0x0010, 3);
    /* a 1x1 opaque 16-bit cel whose pixel lives in its own PLUT pointer word, as CreateBackdropCel makes it;
     * built by hand because CreateBackdropCel maps it with MapCelToSRect, which needs the math folio */
    rect_tmpl = CreateCel(1, 1, 16, CREATECEL_UNCODED, (void *)1);
    if (rect_tmpl) {
        rect_tmpl->ccb_Flags |= CCB_BGND;
        rect_tmpl->ccb_PRE0 |= PRE0_BGND;
        rect_tmpl->ccb_PIXC = PIXC_OPAQUE;
        rect_tmpl->ccb_PLUTPtr = (void *)(1u << 16);
        rect_tmpl->ccb_SourcePtr = (CelData *)&rect_tmpl->ccb_PLUTPtr;
    }
    DIAG_STEP(0x0010, 4);
    if (pool == 0 || tbufs == 0 || pluts == 0 || rect_tmpl == 0)
        return -6;
    memset(pluts, 0, 2 * P_COUNT * 4 * sizeof(uint16));
    for (i = 0; i < P_COUNT; i++)
        set_role(i, RGB15(synth[i][0], synth[i][1], synth[i][2]));
    DIAG_STEP(0x0010, 5);
    for (i = 0; i < SPRITE_COUNT; i++) {
        if ((i & 7) == 0)
            DIAG_STEP(0x0200, i / 8 + 1);       /* dark green, n bars: sprites 8(n-1).. */
        spr_tmpl[i] = make_sprite(i);
        if (spr_tmpl[i] == 0)
            return -7;
    }
    cur_screen = 0;
    bgbuf = (uint8 *)AllocMem(g_sc->sc_NumBitmapBytes,
                              MEMTYPE_VRAM | MEMTYPE_STARTPAGE | GetBankBits(g_sc->sc_Bitmaps[0]->bm_Buffer));
    return 0;
}

void gfx_shutdown(void)
{
    if (display > 0)
        DeleteBasicDisplay(g_sc);
}


static void *frame_buffer(void)
{
    return g_sc->sc_Bitmaps[cur_screen]->bm_Buffer;
}

static void next_buffer(void)
{
    cur_screen = 1 - cur_screen;
    g_sc->sc_CurrentScreen = cur_screen;
    head = tail = 0;
    npool = 0;
    ntxt = 0;
    dim = 0;
}

/* Start a frame. key >= 0 names the still parts the caller would draw: if the cache holds them they are copied in
 * (returns 0); otherwise the frame starts on the background colour and the caller draws them, then calls
 * gfx_bg_done() to keep them (returns 1). DisplayScreen takes effect at the next vertical blank, so both ways
 * wait for one before touching the buffer, which until then may still be on screen. */
int32 gfx_frame_begin_bg(int32 key)
{
    next_buffer();
    bg_pending = -1;
    if (key >= 0 && bgbuf && key == bg_key) {
        CopyVRAMPages(vram_ioreq, frame_buffer(), bgbuf, g_sc->sc_NumBitmapPages, -1);
        return 0;
    }
    gfx_wait_vbl();
    rect_rgb(0, 0, SCR_W, SCR_H, colors[0][P_BG]);
    if (key >= 0 && bgbuf)
        bg_pending = key;
    return 1;
}

void gfx_bg_done(void)
{
    if (bg_pending < 0)
        return;
    flush();
    CopyVRAMPages(vram_ioreq, bgbuf, frame_buffer(), g_sc->sc_NumBitmapPages, -1);
    bg_key = bg_pending;
    bg_pending = -1;
}

void gfx_bg_forget(void)
{
    bg_key = -1;
}

void gfx_frame_begin(void)
{
    gfx_frame_begin_bg(-1);
}

void gfx_frame_end(void)
{
    Err e;

    flush();
    e = DisplayScreen(g_sc->sc_ScreenItems[cur_screen], 0);
#ifdef DIAG
    st_frames[cur_screen & 1]++;
    if (e < 0) {
        st_disperr++;
        st_last_disp = e;
    }
#endif
    (void)e;
    dim = 0;
}

#ifdef DIAG
/* one line of counters for the footer: frames drawn into each buffer, DrawCels / DisplayScreen failures (and
 * the last error codes), frames that took more than one video field */
extern int32 g_ode_cmds;

void gfx_stats(char *out)
{
    sprintf(out, "F%d D%d/%d S%d ODE %d", (int)(st_frames[0] + st_frames[1]), (int)st_drawerr[0],
            (int)st_drawerr[1], (int)st_disperr, (int)g_ode_cmds);
}
#endif

void gfx_wait_vbl(void)
{
    if (vbl_ioreq >= 0)
        WaitVBL(vbl_ioreq, 1);
    vbl_count++;
}

/* The number of video fields since boot, the clock for animation, key repeat and the launch settle. The graphics
 * folio's gf_VBLNumber is not quite it: its interrupt forces the number's lowest bit to match the field, so on the
 * console's progressive 240-line picture it goes up by 2 every field (everything ran at double speed). At start the
 * menu waits 8 fields and divides by the step it measured (vbl_div). */
#ifndef HOST_BUILD
static uint32 vbl_div = 1;
#endif

uint32 gfx_vbl_count(void)
{
#ifdef HOST_BUILD
    extern int32 host_frame;
    return (uint32)host_frame;
#else
    return GrafBase->gf_VBLNumber / vbl_div;
#endif
}

void gfx_clock_init(void)
{
#ifndef HOST_BUILD
    uint32 v0, v1;

    if (vbl_ioreq < 0)
        return;
    gfx_wait_vbl();
    v0 = GrafBase->gf_VBLNumber;
    gfx_delay(8);
    v1 = GrafBase->gf_VBLNumber;
    vbl_div = (v1 - v0 >= 12) ? 2 : 1;
#endif
}

void gfx_delay(int32 frames)
{
    while (frames-- > 0)
        gfx_wait_vbl();
}

/* ------------------------------------------------------------------ shapes */

static void rect_rgb(int32 x, int32 y, int32 w, int32 h, int32 c)
{
    CCB *k;

    if (w <= 0 || h <= 0)
        return;
    k = new_ccb();
    *k = *rect_tmpl;
    k->ccb_PLUTPtr = (void *)(size_t)((uint32)c << 16);
    k->ccb_SourcePtr = (CelData *)&k->ccb_PLUTPtr;
    k->ccb_XPos = x << 16;
    k->ccb_YPos = y << 16;
    k->ccb_HDX = w << 20;
    k->ccb_VDY = h << 16;
    queue(k);
}

void gfx_rect(int32 x, int32 y, int32 w, int32 h, int32 role)
{
    rect_rgb(x, y, w, h, colors[dim][role]);
}

/* the colour of pixel (i, j) of a rounded rectangle, -1 for none (PicoPak menu/art.py Canvas.rrect) */
static int32 rr_pixel(int32 i, int32 j, int32 w, int32 h, int32 fill, int32 edge, int32 r, int32 mode)
{
    int32 dx = i < w - 1 - i ? i : w - 1 - i;
    int32 dy = mode == RR_TOP ? j : (mode == RR_BOTTOM ? h - 1 - j : (j < h - 1 - j ? j : h - 1 - j));
    int32 on_edge;

    if (r == 2 && dx + dy < 2 && !(dx == 1 && dy == 1))
        return -1;
    if (r == 1 && dx == 0 && dy == 0)
        return -1;
    on_edge = edge >= 0 && (i == 0 || i == w - 1 || (j == 0 && mode != RR_BOTTOM) ||
                            (j == h - 1 && mode != RR_TOP) || (r == 2 && dx == 1 && dy == 1));
    return on_edge ? edge : fill;
}

void gfx_rrect(int32 x, int32 y, int32 w, int32 h, int32 fill, int32 edge, int32 r, int32 mode)
{
    int32 K = r == 2 ? 2 : (r == 1 ? 1 : 0);
    int32 j, dy, i, c, run_c, run_x, m0 = -1, m1 = -1;
    int32 xs[8], n, k;

    if (w < 2 * K + 1 || h < 1)
        return;
    for (j = 0; j < h; j++) {
        dy = mode == RR_TOP ? j : (mode == RR_BOTTOM ? h - 1 - j : (j < h - 1 - j ? j : h - 1 - j));
        if (dy >= K && !(edge >= 0 && ((j == 0 && mode != RR_BOTTOM) || (j == h - 1 && mode != RR_TOP)))) {
            if (m0 < 0)
                m0 = j;
            m1 = j;
            continue;
        }
        /* a corner row: the K pixels at each end one by one, the span between as one run */
        n = 0;
        for (i = 0; i < K; i++)
            xs[n++] = i;
        xs[n++] = K;                        /* the span */
        for (i = w - K; i < w; i++)
            xs[n++] = i;
        run_c = -2;
        run_x = 0;
        for (k = 0; k < n; k++) {
            i = xs[k];
            c = rr_pixel(i, j, w, h, fill, edge, r, mode);
            if (c != run_c) {
                if (run_c >= 0)
                    gfx_rect(x + run_x, y + j, i - run_x, 1, run_c);
                run_c = c;
                run_x = i;
            }
        }
        if (run_c >= 0)
            gfx_rect(x + run_x, y + j, w - run_x, 1, run_c);
    }
    if (m0 >= 0) {
        if (edge >= 0) {
            gfx_rect(x, y + m0, 1, m1 - m0 + 1, edge);
            gfx_rect(x + 1, y + m0, w - 2, m1 - m0 + 1, fill);
            gfx_rect(x + w - 1, y + m0, 1, m1 - m0 + 1, edge);
        } else {
            gfx_rect(x, y + m0, w, m1 - m0 + 1, fill);
        }
    }
}

/* ------------------------------------------------------------------ text */

/* Latin-1 letters with marks, folded to the plain capital (the face has no accents) */
static const char latin1_fold[65] =
    "AAAAAAACEEEEIIIIDNOOOOOXOUUUUYTSAAAAAAACEEEEIIIIDNOOOOO-OUUUUYTY";

static int32 fold_char(uint32 c)
{
    if (c >= 'a' && c <= 'z')
        return c - 32;
    if (c >= 0x20 && c <= 0x5f)
        return c;
    switch (c) {
    case '`': return '\'';
    case '{': return '(';
    case '}': return ')';
    case '|': return '!';
    case '~': return '-';
    }
    if (c >= 0xc0 && c <= 0xff)
        return latin1_fold[c - 0xc0];
    return '?';
}

int32 gfx_fold(const char *utf8, char *out, int32 max)
{
    const unsigned char *p = (const unsigned char *)utf8;
    int32 n = 0;
    uint32 c;

    while (*p && n < max - 1) {
        c = *p;
        if (c < 0x80) {
            p++;
        } else if ((c & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
            c = ((c & 0x1f) << 6) | (p[1] & 0x3f);
            p += 2;
        } else if ((c & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
            c = ((c & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
            p += 3;
            /* common punctuation from outside Latin-1 */
            if (c >= 0x2010 && c <= 0x2015)
                c = '-';                /* hyphens and dashes */
            else if (c >= 0x2018 && c <= 0x201b)
                c = '\'';
            else if (c >= 0x201c && c <= 0x201f)
                c = '"';
            else if (c == 0x2026)
                c = '.';
            else
                c = '?';
        } else if ((c & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
            c = '?';
            p += 4;
        } else {
            p++;                /* not UTF-8: take the byte as Latin-1 */
        }
        out[n++] = (char)fold_char(c);
    }
    out[n] = 0;
    return n;
}

/* text in a window of maxch characters, starting px pixels into the string: the selected row's long name glides
 * through its column a pixel at a time */
void gfx_text_scroll(int32 x, int32 y, const char *s, int32 maxch, int32 px, int32 role)
{
    static char t[DIR_NAME_MAX + 2];
    int32 n, rb, r, k, start, sh, c0, c1;
    uint8 *buf, *row;
    CCB *c;

    if (maxch <= 0 || maxch > TXT_CHARS - 1)
        maxch = TXT_CHARS - 1;
    n = gfx_fold(s, t, sizeof(t));
    start = px >> 3;
    sh = px & 7;
    if (start >= n)
        return;
    if (ntxt >= NTXT)
        flush();
    c = new_ccb();
    buf = tbufs + (ntxt++) * TXT_BYTES;
    rb = InitCel(c, maxch * 8, 8, 1, INITCEL_CODED) / 8;
    for (r = 0; r < 8; r++) {
        row = buf + r * rb;
        for (k = 0; k < rb; k++) {
            c0 = (k < maxch && start + k < n) ? font8[(uint8)t[start + k] - FONT8_FIRST][r] : 0;
            c1 = (start + k + 1 < n) ? font8[(uint8)t[start + k + 1] - FONT8_FIRST][r] : 0;
            row[k] = (uint8)(k < maxch ? ((c0 << sh) | (c1 >> (8 - sh))) & 0xff : 0);
        }
    }
    c->ccb_SourcePtr = (CelData *)buf;
    c->ccb_PLUTPtr = pluts + (dim * P_COUNT + role) * 4;
    c->ccb_XPos = x << 16;
    c->ccb_YPos = y << 16;
    queue(c);
}

void gfx_text(int32 x, int32 y, const char *s, int32 maxch, int32 role)
{
    char t[TXT_CHARS + 1];
    int32 n, rb, r, k;
    uint8 *buf, *row;
    CCB *c;

    if (maxch <= 0 || maxch > TXT_CHARS)
        maxch = TXT_CHARS;
    n = gfx_fold(s, t, maxch + 1);
    while (n > 0 && t[n - 1] == ' ')
        n--;
    if (n == 0)
        return;
    if (ntxt >= NTXT)
        flush();
    c = new_ccb();
    buf = tbufs + (ntxt++) * TXT_BYTES;
    rb = InitCel(c, n * 8, 8, 1, INITCEL_CODED) / 8;
    for (r = 0; r < 8; r++) {
        row = buf + r * rb;
        for (k = 0; k < n; k++)
            row[k] = font8[(uint8)t[k] - FONT8_FIRST][r];
        for (; k < rb; k++)
            row[k] = 0;
    }
    c->ccb_SourcePtr = (CelData *)buf;
    c->ccb_PLUTPtr = pluts + (dim * P_COUNT + role) * 4;
    c->ccb_XPos = x << 16;
    c->ccb_YPos = y << 16;
    queue(c);
}

/* ------------------------------------------------------------------ sprites */

void gfx_sprite(int32 id, int32 x, int32 y, int32 role)
{
    CCB *c;

    if (id < 0 || id >= SPRITE_COUNT)
        return;
    c = new_ccb();
    *c = *spr_tmpl[id];
    c->ccb_PLUTPtr = pluts + (dim * P_COUNT + role) * 4;
    c->ccb_XPos = x << 16;
    c->ccb_YPos = y << 16;
    queue(c);
}

void gfx_micro(int32 x, int32 y, const char *s, int32 role)
{
    int32 k, ch;

    for (k = 0; s[k]; k++) {
        ch = s[k];
        if (ch >= 'A' && ch <= 'Z')
            gfx_sprite(micro_index[ch - 'A'], x + k * 4, y, role);
    }
}

void gfx_button(int32 x, int32 y, int32 label, int32 role)
{
    char s[2];

    s[0] = (char)label;
    s[1] = 0;
    gfx_sprite(SPR_BUTTON, x, y, role);
    gfx_micro(x + 2, y + 1, s, P_BG);
}

/* ------------------------------------------------------------------ 16-bit pictures */

GfxImage *gfx_image_new(int32 w, int32 h)
{
    GfxImage *im = (GfxImage *)AllocMem(sizeof(GfxImage), MEMTYPE_ANY);
    int32 rb = ((w * 16 + 31) >> 5) << 2;

    if (im == 0)
        return 0;
    memset(im, 0, sizeof(*im));
    if (rb < 8)
        rb = 8;
    im->w = w;
    im->h = h;
    im->stride = rb / 2;
    im->pix = (uint16 *)AllocMem(rb * h, MEMTYPE_CEL);
    if (im->pix == 0) {
        FreeMem(im, sizeof(GfxImage));
        return 0;
    }
    memset(im->pix, 0, rb * h);
    im->ccb = CreateCel(w, h, 16, CREATECEL_UNCODED, im->pix);
    if (im->ccb == 0) {
        FreeMem(im->pix, rb * h);
        FreeMem(im, sizeof(GfxImage));
        return 0;
    }
    im->ccb->ccb_Flags |= CCB_BGND;     /* black pixels are drawn, not see-through */
    return im;
}

static uint32 be16(const uint8 *p)
{
    return (p[0] << 8) | p[1];
}

GfxImage *gfx_image_load(const char *path)
{
    int32 size = 0, w, h, fstride, x, y;
    uint8 *f = (uint8 *)LoadFile((char *)path, &size, MEMTYPE_ANY);
    GfxImage *im = 0;

    if (f == 0)
        return 0;
    if (size < 8 || memcmp(f, "OLG1", 4) != 0)
        goto out;
    w = be16(f + 4);
    h = be16(f + 6);
    fstride = (w * 2 + 3) & ~3;
    if (w <= 0 || h <= 0 || 8 + fstride * h > size)
        goto out;
    im = gfx_image_new(w, h);
    if (im == 0)
        goto out;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            im->pix[y * im->stride + x] = (uint16)be16(f + 8 + y * fstride + x * 2);
    im->mstride = ((w + 31) / 32) * 4;
    if (8 + fstride * h + im->mstride * h <= size) {
        im->mask = (uint8 *)AllocMem(im->mstride * h, MEMTYPE_ANY);
        if (im->mask)
            memcpy(im->mask, f + 8 + fstride * h, im->mstride * h);
    }
out:
    UnloadFile(f);
    return im;
}

#ifndef PRE0_VCNT_MASK
#define PRE0_VCNT_MASK      0x0000FFC0
#define PRE0_VCNT_SHIFT     6
#define PRE0_VCNT_PREFETCH  1
#endif

/* the picture's top rows only (a cover shorter than its slot) */
void gfx_image_rows(GfxImage *img, int32 x, int32 y, int32 rows)
{
    CCB *c;

    if (img == 0 || rows < 1)
        return;
    if (rows > img->h)
        rows = img->h;
    c = new_ccb();
    *c = *img->ccb;
    c->ccb_PRE0 = (c->ccb_PRE0 & ~PRE0_VCNT_MASK) | ((uint32)(rows - PRE0_VCNT_PREFETCH) << PRE0_VCNT_SHIFT);
    c->ccb_Height = rows;
    c->ccb_XPos = x << 16;
    c->ccb_YPos = y << 16;
    c->ccb_PIXC = dim ? PIXC_DIM3 : PIXC_OPAQUE;
    queue(c);
}

void gfx_image(GfxImage *img, int32 x, int32 y)
{
    CCB *c;

    if (img == 0)
        return;
    c = new_ccb();
    *c = *img->ccb;
    c->ccb_XPos = x << 16;
    c->ccb_YPos = y << 16;
    c->ccb_PIXC = dim ? PIXC_DIM3 : PIXC_OPAQUE;
    queue(c);
}
