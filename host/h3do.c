/* h3do.c - host implementation of the lib3do subset (rendering into a 320x240 RGB555 framebuffer),
 * scripted control pad, screenshots. Test harness only; mimics textlib/CreateBackdropCel closely enough to
 * judge layout and colors, not bit-exact.
 */
#include "h3do.h"

#define W 320
#define HMAX 288
static int32 H = 240;                  /* 288 with FWSIM_PAL=1 (a PAL console) */

static uint16 fb[2][W * HMAX];
static uint16 shown[W * HMAX];
int32  host_frame;
static const char *g_outdir = ".";
static FILE *g_script;
static uint32 pad_state;
static int32  next_event_frame = -1;
static char   next_line[256];
static int32  shots;
static int32  pending_shot;
static char   pending_name[128];
static int32  disp_count = -1;
static int32  rec_left, rec_n;
static char   rec_name[128];
static char   disp_name[128];

void host_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%6d] ", host_frame);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void *AllocMem(int32 size, uint32 type)
{
    /* Portfolio does not clear memory unless asked (MEMTYPE_FILL): fill with junk so the tests see it */
    void *p = malloc(size > 0 ? size : 1);
    (void)type;
    memset(p, 0xa5, size > 0 ? size : 1);
    return p;
}

void FreeMem(void *p, int32 size)
{
    (void)size;
    free(p);
}

Item LoadProgram(char *path)
{
    host_log("LoadProgram %s\n", path);
    return 1;
}

Item GetVRAMIOReq(void) { return 1; }
Err OpenGraphicsFolio(void) { return 0; }
int32 GetDisplayType(void) { return getenv("FWSIM_PAL") ? DI_TYPE_PAL1 : DI_TYPE_NTSC; }
Item GetVBLIOReq(void) { return 2; }

Item CreateBasicDisplay(ScreenContext *sc, uint32 type, uint32 n)
{
    sc->sc_DisplayType = type;
    sc->sc_NumScreens = n;
    sc->sc_CurrentScreen = 0;
    sc->sc_ScreenItems[0] = 0;
    sc->sc_ScreenItems[1] = 1;
    sc->sc_BitmapItems[0] = 0;
    sc->sc_BitmapItems[1] = 1;
    {
        static Bitmap bm[2];
        H = type == DI_TYPE_NTSC ? 240 : 288;
        bm[0].bm_Width = bm[1].bm_Width = W;
        bm[0].bm_Height = bm[1].bm_Height = H;
        bm[0].bm_Buffer = (uint8 *)fb[0];
        bm[1].bm_Buffer = (uint8 *)fb[1];
        sc->sc_Bitmaps[0] = &bm[0];
        sc->sc_Bitmaps[1] = &bm[1];
        sc->sc_NumBitmapPages = W * H * 2 / 2048;
        sc->sc_NumBitmapBytes = W * H * 2;
    }
    return 1;
}

Err DeleteBasicDisplay(ScreenContext *sc) { (void)sc; return 0; }

/* ---------------------------------------------------------------- screenshots */

static void write_ppm(const char *name, const uint16 *px)
{
    char path[512];
    FILE *f;
    int32 i;

    snprintf(path, sizeof(path), "%s/%s.ppm", g_outdir, name);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (i = 0; i < W * H; i++) {
        uint16 v = px[i];
        fputc(((v >> 10) & 31) * 255 / 31, f);
        fputc(((v >> 5) & 31) * 255 / 31, f);
        fputc((v & 31) * 255 / 31, f);
    }
    fclose(f);
    shots++;
    host_log("screenshot %s\n", path);
}

static void script_next(void)
{
    char line[256];

    next_event_frame = -1;
    if (!g_script)
        return;
    while (fgets(line, sizeof(line), g_script)) {
        int32 f;
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "%d", &f) == 1) {
            next_event_frame = f;
            strcpy(next_line, line);
            return;
        }
    }
}

static uint32 parse_buttons(const char *s)
{
    uint32 b = 0;
    for (; *s; s++) {
        switch (*s) {
        case 'U': b |= 0x40000000; break;
        case 'D': b |= 0x80000000; break;
        case 'L': b |= 0x10000000; break;
        case 'R': b |= 0x20000000; break;
        case 'A': b |= 0x08000000; break;
        case 'B': b |= 0x04000000; break;
        case 'C': b |= 0x02000000; break;
        case 'P': b |= 0x01000000; break;
        case 'X': b |= 0x00800000; break;
        case 'r': b |= 0x00400000; break;
        case 'l': b |= 0x00200000; break;
        }
    }
    return b;
}

/* script lines: "<frame> pad <buttons|->"  "<frame> shot <name>"  "<frame> end" */
static void script_run(void)
{
    while (next_event_frame >= 0 && host_frame >= next_event_frame) {
        char cmd[32], arg[128];
        int32 f;
        arg[0] = 0;
        sscanf(next_line, "%d %31s %127s", &f, cmd, arg);
        if (strcmp(cmd, "pad") == 0) {
            pad_state = (arg[0] == '-') ? 0 : parse_buttons(arg);
        } else if (strcmp(cmd, "shot") == 0) {
            pending_shot = 1;
            strcpy(pending_name, arg);
        } else if (strcmp(cmd, "shotdisp") == 0) {
            /* "<frame> shotdisp <name> <n>": capture the n-th DisplayScreen from now (mid-operation screens) */
            int32 n = 1;
            sscanf(next_line, "%*d %*s %*s %d", &n);
            disp_count = n;
            strcpy(disp_name, arg);
        } else if (strcmp(cmd, "rec") == 0) {
            /* "<frame> rec <name> <n>": write every video field for n fields as <name>_00000.ppm ... */
            sscanf(next_line, "%*d %*s %*s %d", &rec_left);
            strcpy(rec_name, arg);
            rec_n = 0;
        } else if (strcmp(cmd, "end") == 0) {
            write_ppm("last", shown);
            host_log("script end, %d screenshots\n", shots);
            exit(0);
        }
        script_next();
    }
}

void host_setup(const char *outdir, const char *script)
{
    g_outdir = outdir;
    g_script = script ? fopen(script, "r") : 0;
    script_next();
}

Err WaitVBL(Item io, uint32 n)
{
    (void)io;
    while (n--) {
        host_frame++;
        if (pending_shot) {
            write_ppm(pending_name, shown);
            pending_shot = 0;
        }
        if (rec_left > 0) {
            char nm[160];
            snprintf(nm, sizeof(nm), "%s_%05d", rec_name, (int)rec_n++);
            write_ppm(nm, shown);
            rec_left--;
        }
        script_run();
        if (host_frame > 200000) {
            host_log("frame limit\n");
            exit(1);
        }
    }
    return 0;
}

Err InitEventUtility(int32 pads, int32 mice, int32 focus) { (void)pads; (void)mice; (void)focus; return 0; }

Err GetControlPad(int32 pad, int32 wait, ControlPadEventData *d)
{
    (void)pad; (void)wait;
    script_run();
    d->cped_ButtonBits = pad_state;
    return 1;
}

/* ---------------------------------------------------------------- files, images */

void *LoadFile(const char *path, int32 *size, uint32 mem)
{
    char hp[256];
    FILE *f = fopen(strncmp(path, "/NVRAM/", 7) == 0 ? (sprintf(hp, "nvram_%s", path + 7), hp) : path, "rb");
    long n;
    void *p;

    (void)mem;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = malloc(n);
    if (fread(p, 1, n, f) != (size_t)n) {
        fclose(f);
        free(p);
        return 0;
    }
    fclose(f);
    if (size)
        *size = n;
    return p;
}

void UnloadFile(void *p) { free(p); }
void *LookupItem(Item i) { (void)i; return 0; }

/* NVRAM files on the host: "/NVRAM/x" -> "nvram_x" in the current (disc) directory */
static const char *host_path(const char *path, char *buf)
{
    if (strncmp(path, "/NVRAM/", 7) == 0) {
        sprintf(buf, "nvram_%s", path + 7);
        return buf;
    }
    return path;
}

int32 SaveFile(const char *path, void *buf, int32 size, int32 extra)
{
    char hp[256];
    FILE *f = fopen(host_path(path, hp), "wb");
    (void)extra;
    if (!f)
        return -1;
    fwrite(buf, 1, size, f);
    fclose(f);
    host_log("SaveFile %s %d\n", path, (int)size);
    return 0;
}
Item CreateMsgPort(const char *name, uint8 pri, uint32 signal) { (void)name; (void)pri; (void)signal; return 100; }
Item FindMsgPort(const char *name) { (void)name; return 100; }

static uint32 be32(const uint8 *p) { return ((uint32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

void *LoadImage(char *path, uint8 *dest, void **vdl, ScreenContext *sc)
{
    int32 n;
    uint8 *d = LoadFile(path, &n, 0);
    uint16 *img;
    int32 x, y, w, h;

    (void)dest; (void)vdl; (void)sc;
    if (!d || n < 0x24 || memcmp(d, "IMAG", 4) != 0) {
        host_log("LoadImage %s failed\n", path);
        return 0;
    }
    w = be32(d + 8);
    h = be32(d + 12);
    img = calloc(W * H, 2);
    for (y = 0; y < h && y < H; y++)
        for (x = 0; x < w && x < W; x++) {
            int32 i = ((y / 2) * w + x) * 2 + (y & 1);
            img[y * W + x] = (d[0x24 + i * 2] << 8) | d[0x25 + i * 2];
        }
    free(d);
    return img;
}

void UnloadImage(void *img) { free(img); }

boolean DrawImage(Item screen, uint8 *img, ScreenContext *sc)
{
    (void)sc;
    memcpy(fb[screen & 1], img, sizeof(fb[0]));
    return 1;
}

Err DisplayScreen(Item s0, Item s1)
{
    (void)s1;
    memcpy(shown, fb[s0 & 1], sizeof(shown));
    if (disp_count > 0 && --disp_count == 0)
        write_ppm(disp_name, shown);
    return 0;
}

/* ---------------------------------------------------------------- cels
 * What the menu uses of the cel engine: chains of cels (CCB_LAST ends one), 1-bpp coded cels through a PLUT,
 * 16-bpp uncoded cels, 1x1 backdrop cels scaled by HDX/VDY whose pixel sits in the PLUTPtr word, a zero
 * pixel being transparent unless CCB_BGND, and the menu's two PIXC values (opaque, and source x 3/16). */

int32 InitCel(CCB *c, int32 w, int32 h, int32 bpp, int32 opt)
{
    int32 rb = (((w * bpp) + 31) >> 5) << 2;

    if (rb < 8)
        rb = 8;
    memset(c, 0, sizeof(*c));
    c->ccb_Flags = CCB_LAST | CCB_NPABS;
    c->ccb_HDX = 1 << 20;
    c->ccb_VDY = 1 << 16;
    c->ccb_PIXC = PIXC_OPAQUE;
    c->ccb_Width = w;
    c->ccb_Height = h;
    c->h_bpp = bpp;
    c->h_coded = (opt & INITCEL_CODED) || bpp < 8;
    c->h_rowbytes = rb;
    return h * rb;
}

CCB *CreateCel(int32 w, int32 h, int32 bpp, int32 opt, void *data)
{
    CCB *c = calloc(1, sizeof(CCB) + 64);
    int32 n = InitCel(c, w, h, bpp, opt);

    c->ccb_PLUTPtr = c->h_coded ? (void *)(c + 1) : 0;
    c->ccb_SourcePtr = data ? data : calloc(1, n);
    return c;
}

CCB *CreateBackdropCel(int32 w, int32 h, int32 color, int32 opacity)
{
    CCB *c = CreateCel(1, 1, 16, CREATECEL_UNCODED, (void *)1);

    (void)opacity;
    c->ccb_HDX = w << 20;
    c->ccb_VDY = h << 16;
    c->ccb_PLUTPtr = (void *)(size_t)((uint32)(color & 0x7fff) << 16);
    c->ccb_SourcePtr = (CelData *)&c->ccb_PLUTPtr;
    return c;
}

Err OpenMathFolio(void) { return 0; }

Err CopyVRAMPages(Item io, void *dest, void *src, uint32 pages, uint32 mask)
{
    (void)pages; (void)mask;
    WaitVBL(io, 1);             /* SPORT transfers happen in the vertical blank */
    memcpy(dest, src, W * H * 2);
    return 0;
}

Err SetVRAMPages(Item io, void *dest, int32 val, int32 pages, int32 mask)
{
    uint16 *p = dest;
    int32 i;

    (void)mask; (void)pages;
    WaitVBL(io, 1);                 /* SPORT transfers happen in the vertical blank */
    for (i = 0; i < W * H; i++)
        p[i] = (uint16)val;
    return 0;
}

static void put_px(uint16 *dst, CCB *c, int32 X, int32 Y, uint16 v)
{
    if (v == 0 && !(c->ccb_Flags & CCB_BGND))
        return;
    if (X < 0 || X >= W || Y < 0 || Y >= H)
        return;
    if (c->ccb_PIXC != PIXC_OPAQUE) {
        int32 r = ((v >> 10) & 31) * 3 / 16, g = ((v >> 5) & 31) * 3 / 16, b = (v & 31) * 3 / 16;
        v = (uint16)((r << 10) | (g << 5) | b);
    }
    dst[Y * W + X] = v & 0x7fff;
}

static void draw_one(uint16 *dst, CCB *c)
{
    int32 x0 = c->ccb_XPos >> 16, y0 = c->ccb_YPos >> 16, x, y, w, h;
    const uint8 *src = c->ccb_SourcePtr;
    const uint16 *plut = c->ccb_PLUTPtr;

    if (c->h_bpp == 16 && c->ccb_SourcePtr == (CelData *)&c->ccb_PLUTPtr) {
        uint16 v = (uint16)((uint32)(size_t)c->ccb_PLUTPtr >> 16);
        w = c->ccb_HDX >> 20;
        h = c->ccb_VDY >> 16;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                put_px(dst, c, x0 + x, y0 + y, v);
        return;
    }
    for (y = 0; y < c->ccb_Height; y++)
        for (x = 0; x < c->ccb_Width; x++) {
            uint16 v;
            if (c->h_bpp == 16) {
                v = ((const uint16 *)(src + y * c->h_rowbytes))[x];
            } else if (c->h_bpp == 1) {
                int32 bit = (src[y * c->h_rowbytes + (x >> 3)] >> (7 - (x & 7))) & 1;
                v = plut[bit];
            } else {
                continue;
            }
            put_px(dst, c, x0 + x, y0 + y, v);
        }
}

Err DrawCels(Item bitmap, CCB *c)
{
    uint16 *dst = fb[bitmap & 1];

    while (c) {
        draw_one(dst, c);
        if (c->ccb_Flags & CCB_LAST)
            break;
        c = c->ccb_NextPtr;
    }
    return 0;
}
