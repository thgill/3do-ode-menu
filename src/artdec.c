/* artdec.c - the info pane's pictures: decodes a whole picture file held in memory into a w x h RGB555 image.
 *
 *   OART   the menu's own format (tools/mkart.py): "OART", u16 width, u16 height, big-endian RGB555 rows.
 *          Must already be w x h. Fastest to show.
 *   PNG    any color type, bit depths 1-16, not interlaced. Inflated with tinf (src/third, zlib license).
 *   JPEG   baseline only (not progressive), grayscale or YCbCr. Decoded with picojpeg (src/third, public domain).
 *
 * PNG and JPEG pictures of any size are trimmed to the frame's 4:3 shape (equal amounts off both sides, as mkart.py
 * --fit crop does) and averaged down (or repeated up) to w x h. Transparent pixels are drawn over black.
 * JPEGs at least 8 times the frame size are decoded at 1/8 size, which is much faster.
 *
 * The abort callback is asked now and then during long decodes; when it returns non-zero the decode stops with
 * ART_ABORTED and the picture is left untouched.
 */
#include "common.h"
#include "third/tinf.h"
#include "third/picojpeg.h"

#define PNG_MAX_SIDE  4096
#define PNG_RAW_MAX   (1024 * 1024)   /* inflated size: 640 x 480 RGB is 922 KB */

/* ---------------------------------------------------------------- the scaler
 * The whole picture is shrunk to fit the frame (max w x max h), keeping its shape: a 4:3 screenshot becomes
 * 88 x 66, a US long box 63 x 122. Source pixels are summed into the destination pixel they fall in; destination
 * pixels no source pixel fell in (the picture is smaller than the frame) copy the one that holds their source
 * pixel. */

typedef struct Sink {
    int32   sw, sh, w, h;
    int32   x0, y0, cw, ch;
    int32  *colmap, *rowmap;      /* source x / y -> destination x / y, -1 outside the trimmed part */
    uint32 *acc;                  /* r, g, b, count per destination pixel */
} Sink;

static void sink_free(Sink *k)
{
    if (k->colmap)
        FreeMem(k->colmap, k->sw * sizeof(int32));
    if (k->rowmap)
        FreeMem(k->rowmap, k->sh * sizeof(int32));
    if (k->acc)
        FreeMem(k->acc, k->w * k->h * 4 * sizeof(uint32));
    k->colmap = k->rowmap = 0;
    k->acc = 0;
}

/* the size a sw x sh picture is shown at, at most mw x mh, its shape kept */
static void fit_size(int32 sw, int32 sh, int32 mw, int32 mh, int32 *w, int32 *h)
{
    if (sw * mh > sh * mw) {        /* wider than the frame: full width */
        *w = mw;
        *h = (sh * mw + sw / 2) / sw;
    } else {                        /* taller: full height */
        *h = mh;
        *w = (sw * mh + sh / 2) / sh;
    }
    if (*w < 1)
        *w = 1;
    if (*h < 1)
        *h = 1;
}

static int32 sink_begin(Sink *k, int32 sw, int32 sh, int32 mw, int32 mh)
{
    int32 i, w, h;

    fit_size(sw, sh, mw, mh, &w, &h);
    k->sw = sw;
    k->sh = sh;
    k->w = w;
    k->h = h;
    k->x0 = k->y0 = 0;
    k->cw = sw;
    k->ch = sh;
    k->colmap = (int32 *)AllocMem(sw * sizeof(int32), MEMTYPE_ANY);
    k->rowmap = (int32 *)AllocMem(sh * sizeof(int32), MEMTYPE_ANY);
    k->acc = (uint32 *)AllocMem(w * h * 4 * sizeof(uint32), MEMTYPE_ANY);
    if (!k->colmap || !k->rowmap || !k->acc) {
        sink_free(k);
        return -1;
    }
    memset(k->acc, 0, w * h * 4 * sizeof(uint32));
    for (i = 0; i < sw; i++)
        k->colmap[i] = (i >= k->x0 && i < k->x0 + k->cw) ? (i - k->x0) * w / k->cw : -1;
    for (i = 0; i < sh; i++)
        k->rowmap[i] = (i >= k->y0 && i < k->y0 + k->ch) ? (i - k->y0) * h / k->ch : -1;
    return 0;
}

static void sink_px(Sink *k, int32 x, int32 y, uint32 r, uint32 g, uint32 b)
{
    int32 dx = k->colmap[x], dy = k->rowmap[y];
    uint32 *a;

    if (dx < 0 || dy < 0)
        return;
    a = k->acc + (dy * k->w + dx) * 4;
    a[0] += r;
    a[1] += g;
    a[2] += b;
    a[3]++;
}

/* Averages, then a light sharpen (each pixel moves an eighth of the way away from the average of its four
 * neighbors, times four: c + (4c - n) / 8), which takes the edge off the softness of shrinking to cover size. */
static void sink_end(Sink *k, uint16 *out, int32 stride)
{
    int32 x, y, sx, sy, ch, c, nb, v[3];
    uint32 *a, *b;

    for (y = 0; y < k->h; y++)
        for (x = 0; x < k->w; x++) {
            a = k->acc + (y * k->w + x) * 4;
            if (a[3]) {
                a[0] /= a[3];
                a[1] /= a[3];
                a[2] /= a[3];
                a[3] = 1;
            }
        }
    /* destination pixels no source pixel fell in (a picture smaller than the frame) copy the one that holds their
     * source pixel; that one always has a value */
    for (y = 0; y < k->h; y++)
        for (x = 0; x < k->w; x++) {
            a = k->acc + (y * k->w + x) * 4;
            if (a[3] == 0) {
                sx = k->x0 + x * k->cw / k->w;
                sy = k->y0 + y * k->ch / k->h;
                b = k->acc + (k->rowmap[sy] * k->w + k->colmap[sx]) * 4;
                a[0] = b[0];
                a[1] = b[1];
                a[2] = b[2];
                a[3] = 2;               /* a copy: not a source of the hole fill above */
            }
        }
    for (y = 0; y < k->h; y++)
        for (x = 0; x < k->w; x++) {
            a = k->acc + (y * k->w + x) * 4;
            for (ch = 0; ch < 3; ch++) {
                c = (int32)a[ch];
                nb = (int32)k->acc[((y > 0 ? y - 1 : y) * k->w + x) * 4 + ch] +
                     (int32)k->acc[((y < k->h - 1 ? y + 1 : y) * k->w + x) * 4 + ch] +
                     (int32)k->acc[(y * k->w + (x > 0 ? x - 1 : x)) * 4 + ch] +
                     (int32)k->acc[(y * k->w + (x < k->w - 1 ? x + 1 : x)) * 4 + ch];
                c += (4 * c - nb) / 8;
                v[ch] = c < 0 ? 0 : (c > 255 ? 255 : c);
            }
            out[y * stride + x] = (uint16)(((v[0] >> 3) << 10) | ((v[1] >> 3) << 5) | (v[2] >> 3));
        }
    sink_free(k);
}

/* ---------------------------------------------------------------- the menu's own .img formats
 *   OAR8  "OAR8", u16 w, u16 h, u16 n, n palette colors (big-endian RGB555), then w*h one-byte indices:
 *         a picture's own 256 colors: 6.2 KB at 88x66, 8.4 KB for a 63x122 long box. What the scan, mkart.py
 *         and the Cover Maker write. Any size up to the pane's 88x122.
 *   OART  "OART", u16 w, u16 h, then big-endian RGB555 pixels: 11.4 KB. The first format; still read. */

static int32 dec_oar8(const uint8 *f, int32 len, uint16 *out, int32 stride, int32 *pw, int32 *ph)
{
    uint16 pal[256];
    int32 x, y, n, i, w = (f[4] << 8) | f[5], h = (f[6] << 8) | f[7];
    const uint8 *p;

    n = (f[8] << 8) | f[9];
    if (w < 1 || h < 1 || w > *pw || h > *ph || n < 1 || n > 256 || len < 10 + 2 * n + w * h)
        return ART_BAD;
    *pw = w;
    *ph = h;
    for (i = 0; i < 256; i++)
        pal[i] = i < n ? (uint16)((f[10 + 2 * i] << 8) | f[11 + 2 * i]) : 0;
    p = f + 10 + 2 * n;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            out[y * stride + x] = pal[*p++];
    return ART_OK;
}

static int32 dec_oart(const uint8 *f, int32 len, uint16 *out, int32 stride, int32 *pw, int32 *ph)
{
    int32 x, y, w = (f[4] << 8) | f[5], h = (f[6] << 8) | f[7];
    const uint8 *p = f + 8;

    if (w < 1 || h < 1 || w > *pw || h > *ph || len < 8 + w * h * 2)
        return ART_BAD;
    *pw = w;
    *ph = h;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++, p += 2)
            out[y * stride + x] = (uint16)((p[0] << 8) | p[1]);
    return ART_OK;
}

/* ---- the 256 colors: median cut over the picture's own colors (counted in a 32768-entry table), splitting
 * the box with the most pixels along its widest channel at the weighted median, until there are 256 boxes; each
 * color then becomes its box's average. A picture with 256 colors or fewer keeps them exactly. */

typedef struct QBox {
    int32 lo, hi;               /* range in the color list */
    int32 pixels;
} QBox;

static int32 q_chan(uint16 c, int32 ch)
{
    return ch == 0 ? (c >> 10) & 31 : (ch == 1 ? (c >> 5) & 31 : c & 31);
}

int32 art_encode8(const uint16 *pix, int32 stride, int32 w, int32 h, uint8 *out, int32 max)
{
    uint16 *count, *cols;
    uint8 *map;
    QBox box[256];
    int32 nbox = 1, ncol = 0, i, j, k, x, y, best, ch, lo[3], hi[3], wch, acc, half, cut, t, len, hist[32];
    uint32 sum[3], wsum;
    uint16 c, tmpc;

    len = 10 + 2 * 256 + w * h;
    if (len > max)
        return -1;
    count = (uint16 *)AllocMem(32768 * 2 + 32768 + w * h * 2, MEMTYPE_ANY);
    if (count == 0)
        return -1;
    map = (uint8 *)(count + 32768);
    cols = (uint16 *)(map + 32768);
    memset(count, 0, 32768 * 2);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            c = pix[y * stride + x] & 0x7fff;
            if (count[c]++ == 0)
                cols[ncol++] = c;
        }
    box[0].lo = 0;
    box[0].hi = ncol;
    box[0].pixels = w * h;
    while (nbox < 256 && nbox < ncol) {
        /* the box with the most pixels that can still be split */
        best = -1;
        for (i = 0; i < nbox; i++)
            if (box[i].hi - box[i].lo > 1 && (best < 0 || box[i].pixels > box[best].pixels))
                best = i;
        if (best < 0)
            break;
        for (ch = 0; ch < 3; ch++) {
            lo[ch] = 31;
            hi[ch] = 0;
        }
        for (j = box[best].lo; j < box[best].hi; j++)
            for (ch = 0; ch < 3; ch++) {
                t = q_chan(cols[j], ch);
                if (t < lo[ch]) lo[ch] = t;
                if (t > hi[ch]) hi[ch] = t;
            }
        wch = 0;
        for (ch = 1; ch < 3; ch++)
            if (hi[ch] - lo[ch] > hi[wch] - lo[wch])
                wch = ch;
        /* weighted median along that channel, from a 32-step histogram */
        memset(hist, 0, sizeof(hist));
        for (j = box[best].lo; j < box[best].hi; j++)
            hist[q_chan(cols[j], wch)] += count[cols[j]];
        half = box[best].pixels / 2;
        acc = 0;
        for (cut = lo[wch]; cut < hi[wch]; cut++) {
            acc += hist[cut];
            if (acc >= half)
                break;
        }
        if (cut >= hi[wch])
            cut = hi[wch] - 1;      /* most of the box at its top value: split that value off */
        /* partition: values <= cut first */
        i = box[best].lo;
        j = box[best].hi - 1;
        while (i <= j) {
            if (q_chan(cols[i], wch) <= cut) {
                i++;
            } else {
                tmpc = cols[i];
                cols[i] = cols[j];
                cols[j] = tmpc;
                j--;
            }
        }
        if (i == box[best].lo || i == box[best].hi)
            break;              /* (cannot happen: cut is below the box's highest value) */
        box[nbox].lo = i;
        box[nbox].hi = box[best].hi;
        box[best].hi = i;
        box[nbox].pixels = 0;
        for (k = box[nbox].lo; k < box[nbox].hi; k++)
            box[nbox].pixels += count[cols[k]];
        box[best].pixels -= box[nbox].pixels;
        nbox++;
    }
    memcpy(out, "OAR8", 4);
    out[4] = (uint8)(w >> 8); out[5] = (uint8)w;
    out[6] = (uint8)(h >> 8); out[7] = (uint8)h;
    out[8] = (uint8)(nbox >> 8); out[9] = (uint8)nbox;
    for (i = 0; i < nbox; i++) {
        sum[0] = sum[1] = sum[2] = 0;
        wsum = 0;
        for (j = box[i].lo; j < box[i].hi; j++) {
            for (ch = 0; ch < 3; ch++)
                sum[ch] += (uint32)q_chan(cols[j], ch) * count[cols[j]];
            wsum += count[cols[j]];
            map[cols[j]] = (uint8)i;
        }
        if (wsum == 0)
            wsum = 1;
        c = (uint16)((((sum[0] + wsum / 2) / wsum) << 10) | (((sum[1] + wsum / 2) / wsum) << 5) | ((sum[2] + wsum / 2) / wsum));
        out[10 + 2 * i] = (uint8)(c >> 8);
        out[11 + 2 * i] = (uint8)c;
    }
    k = 10 + 2 * nbox;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            out[k++] = map[pix[y * stride + x] & 0x7fff];
    FreeMem(count, 32768 * 2 + 32768 + w * h * 2);
    return k;
}

/* ---------------------------------------------------------------- PNG */

static uint32 be32(const uint8 *p)
{
    return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3];
}

static int32 paeth(int32 a, int32 b, int32 c)
{
    int32 p = a + b - c, pa = p - a, pb = p - b, pc = p - c;

    if (pa < 0) pa = -pa;
    if (pb < 0) pb = -pb;
    if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

static int32 unfilter(uint8 *row, const uint8 *prev, int32 n, int32 bpp, int32 type)
{
    int32 i, a, b, c;

    for (i = 0; i < n; i++) {
        a = i >= bpp ? row[i - bpp] : 0;
        b = prev ? prev[i] : 0;
        c = (prev && i >= bpp) ? prev[i - bpp] : 0;
        switch (type) {
        case 0: break;
        case 1: row[i] = (uint8)(row[i] + a); break;
        case 2: row[i] = (uint8)(row[i] + b); break;
        case 3: row[i] = (uint8)(row[i] + ((a + b) >> 1)); break;
        case 4: row[i] = (uint8)(row[i] + paeth(a, b, c)); break;
        default: return -1;
        }
    }
    return 0;
}

/* sample x of a row: a value of `depth` bits (1/2/4/8) or the high byte of a 16-bit one, as 0..255 */
static uint32 sample(const uint8 *row, int32 idx, int32 depth, int32 raw)
{
    uint32 v;

    if (depth == 8)
        return row[idx];
    if (depth == 16)
        return row[idx * 2];
    v = (row[(idx * depth) >> 3] >> (8 - depth - ((idx * depth) & 7))) & ((1 << depth) - 1);
    return raw ? v : v * 255 / ((1 << depth) - 1);
}

static int32 dec_png(uint8 *f, int32 len, uint16 *out, int32 stride, int32 *w, int32 *h, ArtAbort ab)
{
    int32 pos = 8, iw = 0, ih = 0, depth = 0, ctype = -1, chans, bpp, rowb, need, x, y, nplte = 0, ntrns = 0;
    uint8 *z = 0, *raw, *row, *prev;
    const uint8 *plte = 0, *trns = 0;
    uint32 zlen = 0, clen;
    unsigned int dlen;
    uint32 r, g, b, a;
    int32 rc;
    Sink k;

    while (pos + 12 <= len) {
        clen = be32(f + pos);
        if (clen > (uint32)(len - pos - 12))
            return ART_BAD;
        if (memcmp(f + pos + 4, "IHDR", 4) == 0 && clen >= 13) {
            iw = (int32)be32(f + pos + 8);
            ih = (int32)be32(f + pos + 12);
            depth = f[pos + 16];
            ctype = f[pos + 17];
            if (f[pos + 20] != 0)
                return ART_BAD;     /* interlaced */
        } else if (memcmp(f + pos + 4, "PLTE", 4) == 0) {
            plte = f + pos + 8;
            nplte = clen / 3;
        } else if (memcmp(f + pos + 4, "tRNS", 4) == 0) {
            trns = f + pos + 8;
            ntrns = clen;
        } else if (memcmp(f + pos + 4, "IDAT", 4) == 0) {
            /* gather the IDAT payloads together, over the chunk headers between them */
            if (z == 0)
                z = f + pos + 8;
            memmove(z + zlen, f + pos + 8, clen);
            zlen += clen;
        } else if (memcmp(f + pos + 4, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + clen;
    }
    if (iw < 1 || ih < 1 || iw > PNG_MAX_SIDE || ih > PNG_MAX_SIDE || z == 0 || zlen < 3)
        return ART_BAD;
    switch (ctype) {
    case 0: chans = 1; if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return ART_BAD; break;
    case 2: chans = 3; if (depth != 8 && depth != 16) return ART_BAD; break;
    case 3: chans = 1; if (depth > 8 || (depth & (depth - 1)) || plte == 0) return ART_BAD; break;
    case 4: chans = 2; if (depth != 8 && depth != 16) return ART_BAD; break;
    case 6: chans = 4; if (depth != 8 && depth != 16) return ART_BAD; break;
    default: return ART_BAD;
    }
    /* zlib header: deflate, no preset dictionary */
    if ((z[0] & 0x0f) != 8 || ((z[0] << 8) | z[1]) % 31 != 0 || (z[1] & 0x20))
        return ART_BAD;
    bpp = (chans * depth + 7) / 8;
    rowb = (iw * chans * depth + 7) / 8;
    if ((uint32)ih * (uint32)(rowb + 1) > PNG_RAW_MAX)
        return ART_BIG;
    need = ih * (rowb + 1);
    raw = (uint8 *)AllocMem(need, MEMTYPE_ANY);
    if (raw == 0)
        return ART_NOMEM;
    dlen = need;
    rc = tinf_uncompress(raw, &dlen, z + 2, zlen - 2);
    if ((rc != TINF_OK && rc != TINF_BUF_ERROR) || dlen != (unsigned int)need) {
        FreeMem(raw, need);
        return ART_BAD;
    }
    if (ab && ab()) {
        FreeMem(raw, need);
        return ART_ABORTED;
    }
    k.colmap = k.rowmap = 0;
    k.acc = 0;
    if (sink_begin(&k, iw, ih, *w, *h) < 0) {
        FreeMem(raw, need);
        return ART_NOMEM;
    }
    prev = 0;
    for (y = 0; y < ih; y++) {
        row = raw + y * (rowb + 1) + 1;
        if (unfilter(row, prev, rowb, bpp, row[-1]) < 0) {
            sink_free(&k);
            FreeMem(raw, need);
            return ART_BAD;
        }
        prev = row;
        if (k.rowmap[y] < 0)
            continue;               /* still unfiltered: the next row needs it */
        for (x = 0; x < iw; x++) {
            if (k.colmap[x] < 0)
                continue;
            a = 255;
            switch (ctype) {
            case 0:
                r = g = b = sample(row, x, depth, 0);
                break;
            case 2:
                r = sample(row, x * 3, depth, 0);
                g = sample(row, x * 3 + 1, depth, 0);
                b = sample(row, x * 3 + 2, depth, 0);
                break;
            case 3:
                r = sample(row, x, depth, 1);
                if ((int32)r < ntrns && trns)
                    a = trns[r];
                if ((int32)r < nplte) {
                    g = plte[r * 3 + 1];
                    b = plte[r * 3 + 2];
                    r = plte[r * 3];
                } else {
                    r = g = b = 0;
                }
                break;
            case 4:
                r = g = b = sample(row, x * 2, depth, 0);
                a = sample(row, x * 2 + 1, depth, 0);
                break;
            default:
                r = sample(row, x * 4, depth, 0);
                g = sample(row, x * 4 + 1, depth, 0);
                b = sample(row, x * 4 + 2, depth, 0);
                a = sample(row, x * 4 + 3, depth, 0);
                break;
            }
            if (a != 255) {
                r = r * a / 255;
                g = g * a / 255;
                b = b * a / 255;
            }
            sink_px(&k, x, y, r, g, b);
        }
        if ((y & 15) == 15 && ab && ab()) {
            sink_free(&k);
            FreeMem(raw, need);
            return ART_ABORTED;
        }
    }
    FreeMem(raw, need);
    *w = k.w;
    *h = k.h;
    sink_end(&k, out, stride);
    return ART_OK;
}

/* ---------------------------------------------------------------- JPEG */

typedef struct JpgSrc {
    const uint8 *p;
    int32 left;
} JpgSrc;

static unsigned char jpg_fetch(unsigned char *buf, unsigned char size, unsigned char *got, void *data)
{
    JpgSrc *s = (JpgSrc *)data;
    int32 n = size < s->left ? size : s->left;

    memcpy(buf, s->p, n);
    s->p += n;
    s->left -= n;
    *got = (unsigned char)n;
    return 0;
}

typedef struct ProgCtx {
    Sink  k;
    int32 w, h, started;
} ProgCtx;

static int32 prog_begin(void *ctx, int32 sw, int32 sh)
{
    ProgCtx *p = (ProgCtx *)ctx;

    if (sink_begin(&p->k, sw, sh, p->w, p->h) < 0)
        return -1;
    p->started = 1;
    return 0;
}

static void prog_px(void *ctx, int32 x, int32 y, int32 r, int32 g, int32 b)
{
    sink_px(&((ProgCtx *)ctx)->k, x, y, (uint32)r, (uint32)g, (uint32)b);
}

static int32 dec_jpg_progressive(const uint8 *f, int32 len, uint16 *out, int32 stride, int32 *w, int32 *h, ArtAbort ab)
{
    static ProgCtx pc;
    int32 rc;

    memset(&pc, 0, sizeof(pc));
    pc.w = *w;
    pc.h = *h;
    rc = jpg_progressive(f, len, *w, *h, prog_begin, prog_px, &pc, ab);
    if (rc == ART_OK && pc.started) {
        *w = pc.k.w;
        *h = pc.k.h;
        sink_end(&pc.k, out, stride);
        return ART_OK;
    }
    if (pc.started)
        sink_free(&pc.k);
    return rc == ART_OK ? ART_NOMEM : rc;
}

static int32 dec_jpg(const uint8 *f, int32 len, uint16 *out, int32 stride, int32 *pw, int32 *ph, ArtAbort ab)
{
    pjpeg_image_info_t info;
    JpgSrc src;
    Sink k;
    int32 reduce = 0, sw, sh, mx = 0, my = 0, bx, by, px, py, ox, oy, i, j, st, ofs, w, h;

    src.p = f;
    src.left = len;
    st = pjpeg_decode_init(&info, jpg_fetch, &src, 0);
    if (st == PJPG_UNSUPPORTED_MODE)
        return dec_jpg_progressive(f, len, out, stride, pw, ph, ab);
    if (st != 0)
        return ART_BAD;
    fit_size(info.m_width, info.m_height, *pw, *ph, &w, &h);
    if ((info.m_width + 7) / 8 >= w && (info.m_height + 7) / 8 >= h) {
        reduce = 1;
        src.p = f;
        src.left = len;
        if (pjpeg_decode_init(&info, jpg_fetch, &src, 1) != 0)
            return ART_BAD;
    }
    sw = reduce ? (info.m_width + 7) / 8 : info.m_width;
    sh = reduce ? (info.m_height + 7) / 8 : info.m_height;
    k.colmap = k.rowmap = 0;
    k.acc = 0;
    if (sw < 1 || sh < 1 || sink_begin(&k, sw, sh, *pw, *ph) < 0)
        return ART_NOMEM;
    for (;;) {
        st = pjpeg_decode_mcu();
        if (st == PJPG_NO_MORE_BLOCKS)
            break;
        if (st != 0 || my >= info.m_MCUSPerCol) {
            sink_free(&k);
            return ART_BAD;
        }
        /* each MCU is (m_MCUWidth/8) x (m_MCUHeight/8) blocks of 8 x 8, at byte offsets x*64 + y*128 */
        for (by = 0; by < info.m_MCUHeight / 8; by++)
            for (bx = 0; bx < info.m_MCUWidth / 8; bx++) {
                ofs = bx * 64 + by * 128;
                if (info.m_scanType == PJPG_GRAYSCALE)
                    ofs = 0;
                if (reduce) {
                    px = mx * (info.m_MCUWidth / 8) + bx;
                    py = my * (info.m_MCUHeight / 8) + by;
                    if (px < sw && py < sh) {
                        if (info.m_scanType == PJPG_GRAYSCALE)
                            sink_px(&k, px, py, info.m_pMCUBufR[0], info.m_pMCUBufR[0], info.m_pMCUBufR[0]);
                        else
                            sink_px(&k, px, py, info.m_pMCUBufR[ofs], info.m_pMCUBufG[ofs], info.m_pMCUBufB[ofs]);
                    }
                    continue;
                }
                ox = mx * info.m_MCUWidth + bx * 8;
                oy = my * info.m_MCUHeight + by * 8;
                for (j = 0; j < 8 && oy + j < sh; j++) {
                    if (k.rowmap[oy + j] < 0)
                        continue;
                    for (i = 0; i < 8 && ox + i < sw; i++) {
                        if (info.m_scanType == PJPG_GRAYSCALE)
                            sink_px(&k, ox + i, oy + j, info.m_pMCUBufR[j * 8 + i], info.m_pMCUBufR[j * 8 + i],
                                    info.m_pMCUBufR[j * 8 + i]);
                        else
                            sink_px(&k, ox + i, oy + j, info.m_pMCUBufR[ofs + j * 8 + i],
                                    info.m_pMCUBufG[ofs + j * 8 + i], info.m_pMCUBufB[ofs + j * 8 + i]);
                    }
                }
            }
        if (++mx >= info.m_MCUSPerRow) {
            mx = 0;
            my++;
            if (ab && ab()) {
                sink_free(&k);
                return ART_ABORTED;
            }
        }
    }
    *pw = k.w;
    *ph = k.h;
    sink_end(&k, out, stride);
    return ART_OK;
}

/* ---------------------------------------------------------------- */

int32 art_decode(uint8 *f, int32 len, uint16 *out, int32 stride, int32 *w, int32 *h, ArtAbort ab)
{
    static const uint8 png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

    if (len >= 10 && memcmp(f, "OAR8", 4) == 0)
        return dec_oar8(f, len, out, stride, w, h);
    if (len >= 8 && memcmp(f, "OART", 4) == 0)
        return dec_oart(f, len, out, stride, w, h);
    if (len >= 33 && memcmp(f, png_sig, 8) == 0)
        return dec_png(f, len, out, stride, w, h, ab);
    if (len >= 4 && f[0] == 0xff && f[1] == 0xd8)
        return dec_jpg(f, len, out, stride, w, h, ab);
    return ART_BAD;
}
