/* jpgprog.c - progressive JPEGs, decoded small.
 *
 * picojpeg (src/third) reads baseline JPEGs only. A progressive JPEG sends the picture in several passes ("scans"):
 * first the average of every 8x8 block (DC), then bands of finer detail (AC), each band possibly in two halves
 * (successive approximation). A full decoder keeps every coefficient of the whole picture until the last scan:
 * 1.5 MB or more for a 600x450 picture, more than the 3DO can spare.
 *
 * The menu only needs an 88x66 picture, so this keeps, per 8x8 block, just the N x N lowest-frequency coefficients
 * (N = 1, 2, 4 or 8, the smallest that still gives at least 88x66), plus one bit per coefficient saying whether it
 * is non-zero (the refinement scans can only be read with that). A 600x450 picture needs about 100 KB. At the end
 * each block becomes N x N pixels with an N-point inverse DCT (as libjpeg's reduced-size decoding does).
 *
 * Supported: SOF2 (progressive, Huffman), 8-bit, 1 or 3 components (greyscale, YCbCr), any sampling factors,
 * restart intervals. Not: arithmetic coding, 12-bit, CMYK.
 */
#include "common.h"

#define JP_MAXCOMP  3
#define JP_MEM_MAX  (768 * 1024)

typedef struct Huff {
    uint8  look_len[512];       /* first 9 bits -> code length (0: longer code) */
    uint8  look_val[512];
    int32  mincode[17], maxcode[18], valptr[17];
    uint8  vals[256];
    int32  ok;
} Huff;

typedef struct Comp {
    int32  id, h, v, tq;
    int32  bw, bh;              /* block grid as stored (padded to whole MCUs) */
    int32  ubw, ubh;            /* blocks that hold picture */
    int16 *coef;                /* N*N per block, natural order within the N x N corner */
    uint8 *nz;                  /* 8 bytes per block: bit k = zigzag coefficient k is non-zero */
    uint8 *plane;               /* bw*N x bh*N pixels after the IDCT */
    int32  dcpred, td, ta;
} Comp;

typedef struct Bits {
    const uint8 *p, *end;
    uint32 buf;
    int32  bits, marker;
} Bits;

static const uint8 zz_nat[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

/* round(4096 * C(u) * cos((2x+1) u pi / 2N)), C(0) = 1/sqrt(2), for N = 2, 4, 8; [x][u] */
static const int16 cos2[2][2] = { {2896, 2896}, {2896, -2896} };
static const int16 cos4[4][4] = {
    {2896, 3784, 2896, 1567}, {2896, 1567, -2896, -3784}, {2896, -1567, -2896, 3784}, {2896, -3784, 2896, -1567}
};
static const int16 cos8[8][8] = {
    {2896, 4017, 3784, 3406, 2896, 2276, 1567, 799}, {2896, 3406, 1567, -799, -2896, -4017, -3784, -2276},
    {2896, 2276, -1567, -4017, -2896, 799, 3784, 3406}, {2896, 799, -3784, -2276, 2896, 3406, -1567, -4017},
    {2896, -799, -3784, 2276, 2896, -3406, -1567, 4017}, {2896, -2276, -1567, 4017, -2896, -799, 3784, -3406},
    {2896, -3406, 1567, 799, -2896, 4017, -3784, 2276}, {2896, -4017, 3784, -3406, 2896, -2276, 1567, -799}
};

typedef struct Jp {
    const uint8 *f;
    int32  len, pos;
    int32  w, h, ncomp, hmax, vmax, mcux, mcuy, n, nn, ri, want_w, want_h;
    uint16 q[4][64];            /* natural order */
    Huff   dc[4], ac[4];
    Comp   c[JP_MAXCOMP];
    int32  eobrun;
    Bits   b;
    ArtAbort ab;
    int32  ticks;
    int32  memsize;
    uint8 *mem;
} Jp;

static Jp *jp;

/* ---------------------------------------------------------------- bits and Huffman codes */

static void bits_init(Bits *b, const uint8 *p, const uint8 *end)
{
    b->p = p;
    b->end = end;
    b->buf = 0;
    b->bits = 0;
    b->marker = 0;
}

static void fill(Bits *b)
{
    int32 c;

    while (b->bits <= 24) {
        c = 0;
        if (!b->marker && b->p < b->end) {
            c = *b->p;
            if (c == 0xff) {
                if (b->p + 1 < b->end && b->p[1] == 0) {
                    b->p += 2;
                } else {
                    b->marker = 1;          /* a marker: the scan's data ends here; feed zeros */
                    c = 0;
                }
            } else {
                b->p++;
            }
        }
        b->buf |= (uint32)c << (24 - b->bits);
        b->bits += 8;
    }
}

static int32 getbits(Bits *b, int32 n)
{
    uint32 v;

    if (n == 0)
        return 0;
    fill(b);
    v = b->buf >> (32 - n);
    b->buf <<= n;
    b->bits -= n;
    return (int32)v;
}

static int32 getbit(Bits *b)
{
    return getbits(b, 1);
}

static int32 extend(int32 v, int32 s)
{
    return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

static int32 huff_build(Huff *t, const uint8 *counts, const uint8 *vals, int32 nvals)
{
    int32 l, i, k = 0, code = 0, j, n;

    memset(t, 0, sizeof(*t));
    for (l = 1; l <= 16; l++) {
        t->valptr[l] = k;
        t->mincode[l] = code;
        code += counts[l - 1];
        k += counts[l - 1];
        t->maxcode[l] = counts[l - 1] ? code - 1 : -1;
        if (code > (1 << l))
            return -1;
        code <<= 1;
    }
    t->maxcode[17] = 0x7fffffff;
    if (k > 256 || k > nvals)
        return -1;
    memcpy(t->vals, vals, k);
    /* the 9-bit lookup */
    k = 0;
    code = 0;
    for (l = 1; l <= 9; l++) {
        for (i = 0; i < counts[l - 1]; i++, k++, code++) {
            n = 1 << (9 - l);
            for (j = 0; j < n; j++) {
                t->look_len[(code << (9 - l)) + j] = (uint8)l;
                t->look_val[(code << (9 - l)) + j] = vals[k];
            }
        }
        code <<= 1;
    }
    t->ok = 1;
    return 0;
}

static int32 huff_decode(Bits *b, const Huff *t)
{
    int32 idx, l, code;

    fill(b);
    idx = (int32)(b->buf >> 23);
    l = t->look_len[idx];
    if (l) {
        b->buf <<= l;
        b->bits -= l;
        return t->look_val[idx];
    }
    code = 0;
    for (l = 1; l <= 16; l++) {
        code = (code << 1) | getbit(b);
        if (code <= t->maxcode[l])
            return t->vals[t->valptr[l] + code - t->mincode[l]];
    }
    return 0;           /* a bad code: decode as 0 and carry on (the picture is just a thumbnail) */
}

/* ---------------------------------------------------------------- coefficients */

static int16 *coef_at(Comp *c, int32 blk, int32 k)
{
    int32 nat = zz_nat[k], r = nat >> 3, col = nat & 7;

    if (r >= jp->n || col >= jp->n)
        return 0;
    return c->coef + blk * jp->nn + r * jp->n + col;
}

static int32 nz_get(Comp *c, int32 blk, int32 k)
{
    return (c->nz[blk * 8 + (k >> 3)] >> (k & 7)) & 1;
}

static void nz_set(Comp *c, int32 blk, int32 k)
{
    c->nz[blk * 8 + (k >> 3)] |= (uint8)(1 << (k & 7));
}

static void set_coef(Comp *c, int32 blk, int32 k, int32 v)
{
    int16 *p = coef_at(c, blk, k);

    if (p)
        *p = (int16)v;
    if (v)
        nz_set(c, blk, k);
}

static void dc_first(Comp *c, int32 blk, int32 al)
{
    int32 s = huff_decode(&jp->b, &jp->dc[c->td]), d = 0;

    if (s > 16)
        s = 0;                  /* not a valid DC size (a damaged file) */
    if (s)
        d = extend(getbits(&jp->b, s), s);
    c->dcpred += d;
    *coef_at(c, blk, 0) = (int16)(c->dcpred * (1 << al));
}

static void dc_refine(Comp *c, int32 blk, int32 al)
{
    if (getbit(&jp->b))
        *coef_at(c, blk, 0) |= (int16)(1 << al);
}

static void ac_first(Comp *c, int32 blk, int32 ss, int32 se, int32 al)
{
    int32 k, rs, r, s;

    if (jp->eobrun > 0) {
        jp->eobrun--;
        return;
    }
    for (k = ss; k <= se; k++) {
        rs = huff_decode(&jp->b, &jp->ac[c->ta]);
        r = rs >> 4;
        s = rs & 15;
        if (s) {
            k += r;
            if (k > se)
                break;
            set_coef(c, blk, k, extend(getbits(&jp->b, s), s) * (1 << al));
        } else if (r < 15) {
            jp->eobrun = (1 << r) - 1;
            if (r)
                jp->eobrun += getbits(&jp->b, r);
            break;
        } else {
            k += 15;
        }
    }
}

/* one correction bit for an already non-zero coefficient */
static void refine_one(Comp *c, int32 blk, int32 k, int32 p1)
{
    int16 *p;

    if (getbit(&jp->b)) {
        p = coef_at(c, blk, k);
        if (p && (*p & p1) == 0)
            *p = (int16)(*p >= 0 ? *p + p1 : *p - p1);
    }
}

/* AC successive-approximation refinement, after libjpeg's decode_mcu_AC_refine */
static void ac_refine(Comp *c, int32 blk, int32 ss, int32 se, int32 al)
{
    int32 p1 = 1 << al, m1 = -(1 << al), k = ss, rs, r, s;

    if (jp->eobrun == 0) {
        for (; k <= se; k++) {
            rs = huff_decode(&jp->b, &jp->ac[c->ta]);
            r = rs >> 4;
            s = rs & 15;
            if (s) {
                s = getbit(&jp->b) ? p1 : m1;
            } else if (r != 15) {
                jp->eobrun = 1 << r;
                if (r)
                    jp->eobrun += getbits(&jp->b, r);
                break;
            }
            /* skip r zero coefficients, refining the non-zero ones passed on the way */
            for (; k <= se; k++) {
                if (nz_get(c, blk, k))
                    refine_one(c, blk, k, p1);
                else if (--r < 0)
                    break;
            }
            if (s && k <= se)
                set_coef(c, blk, k, s);
        }
    }
    if (jp->eobrun > 0) {
        for (; k <= se; k++)
            if (nz_get(c, blk, k))
                refine_one(c, blk, k, p1);
        jp->eobrun--;
    }
}

/* ---------------------------------------------------------------- markers */

static int32 rd16(int32 at)
{
    return (jp->f[at] << 8) | jp->f[at + 1];
}

/* after a scan: the next marker that is not a restart */
static void skip_to_marker(void)
{
    int32 p = jp->pos;

    while (p + 1 < jp->len) {
        if (jp->f[p] == 0xff && jp->f[p + 1] != 0 && jp->f[p + 1] != 0xff &&
            !(jp->f[p + 1] >= 0xd0 && jp->f[p + 1] <= 0xd7))
            break;
        p++;
    }
    jp->pos = p;
}

static void restart(void)
{
    const uint8 *p = jp->b.p;
    int32 i;

    while (p + 1 < jp->b.end && !(p[0] == 0xff && p[1] >= 0xd0 && p[1] <= 0xd7))
        p++;
    if (p + 1 < jp->b.end)
        p += 2;
    bits_init(&jp->b, p, jp->b.end);
    for (i = 0; i < jp->ncomp; i++)
        jp->c[i].dcpred = 0;
    jp->eobrun = 0;
}

static int32 scan(int32 at, int32 seglen)
{
    Comp *sc[JP_MAXCOMP];
    int32 ns, i, j, ss, se, ah, al, mx, my, bx, by, h, v, blk, todo = 0, total, nb, rcount;

    ns = jp->f[at];
    if (ns < 1 || ns > jp->ncomp || seglen < 4 + 2 * ns)
        return ART_BAD;
    for (i = 0; i < ns; i++) {
        sc[i] = 0;
        for (j = 0; j < jp->ncomp; j++)
            if (jp->c[j].id == jp->f[at + 1 + 2 * i])
                sc[i] = &jp->c[j];
        if (sc[i] == 0)
            return ART_BAD;
        sc[i]->td = jp->f[at + 2 + 2 * i] >> 4 & 3;
        sc[i]->ta = jp->f[at + 2 + 2 * i] & 3;
        sc[i]->dcpred = 0;
    }
    at += 1 + 2 * ns;
    ss = jp->f[at];
    se = jp->f[at + 1];
    ah = jp->f[at + 2] >> 4;
    al = jp->f[at + 2] & 15;
    if (ss > 63 || se > 63 || ss > se || al > 13 || (ss == 0 && se != 0) || (ss > 0 && ns != 1))
        return ART_BAD;
    for (i = 0; i < ns; i++)
        if ((ss == 0 && ah == 0 && !jp->dc[sc[i]->td].ok) || (ss > 0 && !jp->ac[sc[i]->ta].ok))
            return ART_BAD;
    jp->eobrun = 0;
    bits_init(&jp->b, jp->f + jp->pos, jp->f + jp->len);
    total = ns > 1 ? jp->mcux * jp->mcuy : sc[0]->ubw * sc[0]->ubh;
    mx = my = 0;
    rcount = 0;
    for (nb = 0; nb < total; nb++) {
        if (jp->ri && nb > 0 && ++rcount == jp->ri) {
            rcount = 0;
            restart();
        }
        if (ns > 1) {
            for (i = 0; i < ns; i++)
                for (v = 0; v < sc[i]->v; v++)
                    for (h = 0; h < sc[i]->h; h++) {
                        blk = (my * sc[i]->v + v) * sc[i]->bw + mx * sc[i]->h + h;
                        if (ah == 0)
                            dc_first(sc[i], blk, al);
                        else
                            dc_refine(sc[i], blk, al);
                    }
        } else {
            bx = mx;
            by = my;
            blk = by * sc[0]->bw + bx;
            if (ss == 0 && ah == 0)
                dc_first(sc[0], blk, al);
            else if (ss == 0)
                dc_refine(sc[0], blk, al);
            else if (ah == 0)
                ac_first(sc[0], blk, ss, se, al);
            else
                ac_refine(sc[0], blk, ss, se, al);
        }
        if (++mx == (ns > 1 ? jp->mcux : sc[0]->ubw)) {
            mx = 0;
            my++;
        }
        if (++todo >= 256) {
            todo = 0;
            if (jp->ab && jp->ab())
                return ART_ABORTED;
        }
    }
    jp->pos = jp->b.p - jp->f;
    skip_to_marker();
    return ART_OK;
}

static int32 frame(int32 at, int32 seglen)
{
    int32 i, n, mem = 0, nblk;
    uint8 *m;
    Comp *c;

    if (jp->f[at] != 8 || seglen < 9)
        return ART_BAD;
    jp->h = rd16(at + 1);
    jp->w = rd16(at + 3);
    jp->ncomp = jp->f[at + 5];
    if (jp->w < 1 || jp->h < 1 || (jp->ncomp != 1 && jp->ncomp != 3) || seglen < 6 + 3 * jp->ncomp)
        return ART_BAD;
    jp->hmax = jp->vmax = 1;
    for (i = 0; i < jp->ncomp; i++) {
        c = &jp->c[i];
        c->id = jp->f[at + 6 + 3 * i];
        c->h = jp->f[at + 7 + 3 * i] >> 4;
        c->v = jp->f[at + 7 + 3 * i] & 15;
        c->tq = jp->f[at + 8 + 3 * i] & 3;
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4)
            return ART_BAD;
        if (c->h > jp->hmax)
            jp->hmax = c->h;
        if (c->v > jp->vmax)
            jp->vmax = c->v;
    }
    if (jp->ncomp == 1)
        jp->c[0].h = jp->c[0].v = jp->hmax = jp->vmax = 1;   /* one component: never interleaved */
    jp->mcux = (jp->w + 8 * jp->hmax - 1) / (8 * jp->hmax);
    jp->mcuy = (jp->h + 8 * jp->vmax - 1) / (8 * jp->vmax);
    /* the smallest N x N corner that gives twice the frame's size (the scaler then averages, which looks better
     * than a 1:1 copy), or less when memory would run short */
    for (n = 1; n < 8; n *= 2)
        if ((jp->w * n + 7) / 8 >= 2 * jp->want_w && (jp->h * n + 7) / 8 >= 2 * jp->want_h)
            break;
    for (;; n /= 2) {
        mem = 0;
        for (i = 0; i < jp->ncomp; i++) {
            c = &jp->c[i];
            c->bw = jp->mcux * c->h;
            c->bh = jp->mcuy * c->v;
            c->ubw = ((jp->w * c->h + jp->hmax - 1) / jp->hmax + 7) / 8;
            c->ubh = ((jp->h * c->v + jp->vmax - 1) / jp->vmax + 7) / 8;
            nblk = c->bw * c->bh;
            mem += nblk * (3 * n * n + 8);
        }
        if (mem <= JP_MEM_MAX || n == 1)
            break;
    }
    if (mem > JP_MEM_MAX)
        return ART_BIG;
    jp->n = n;
    jp->nn = n * n;
    jp->mem = m = (uint8 *)AllocMem(mem, MEMTYPE_ANY);
    if (m == 0)
        return ART_NOMEM;
    jp->memsize = mem;
    memset(m, 0, mem);
    for (i = 0; i < jp->ncomp; i++) {
        c = &jp->c[i];
        nblk = c->bw * c->bh;
        c->coef = (int16 *)m;
        m += nblk * 2 * jp->nn;
        c->nz = m;
        m += nblk * 8;
        c->plane = m;
        m += nblk * jp->nn;
    }
    return ART_OK;
}

/* ---------------------------------------------------------------- pixels */

static int32 clamp8(int32 v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* coefficients and the IDCT's middle step are kept to what a real picture can hold, so a damaged file cannot
 * overflow the sums */
static int32 clampc(int32 v)
{
    return v < -16384 ? -16384 : (v > 16384 ? 16384 : v);
}

/* block -> N x N pixels of its component's plane */
static void idct_block(Comp *c, int32 bx, int32 by)
{
    int32 n = jp->n, x, y, u, v, s, blk = by * c->bw + bx, pw = c->bw * n;
    int32 f[64], g[64];
    const uint16 *q = jp->q[c->tq];
    const int16 *t;
    int16 *co = c->coef + blk * jp->nn;
    uint8 *out = c->plane + by * n * pw + bx * n;

    if (n == 1) {
        out[0] = (uint8)clamp8((co[0] * q[0] + 4) / 8 + 128);
        return;
    }
    t = n == 2 ? &cos2[0][0] : (n == 4 ? &cos4[0][0] : &cos8[0][0]);
    for (v = 0; v < n; v++)
        for (u = 0; u < n; u++)
            f[v * n + u] = clampc(co[v * n + u] * q[v * 8 + u]);
    /* columns: g[y][u] = sum_v T[y][v] f[v][u], back to scale 1 */
    for (y = 0; y < n; y++)
        for (u = 0; u < n; u++) {
            s = 0;
            for (v = 0; v < n; v++)
                s += t[y * n + v] * f[v * n + u];
            g[y * n + u] = clampc((s + 2048) >> 12);
        }
    /* rows, then / 4 (the IDCT's 1/4) and the level shift */
    for (y = 0; y < n; y++)
        for (x = 0; x < n; x++) {
            s = 0;
            for (u = 0; u < n; u++)
                s += t[x * n + u] * g[y * n + u];
            out[y * pw + x] = (uint8)clamp8(((s + 8192) >> 14) + 128);
        }
}

/* per component: which plane column / row each output pixel takes (worked out once: the ARM has no divide) */
static int32 *xmap[JP_MAXCOMP];

static void make_pixels(ArtBegin begin, ArtPixel px, void *ctx)
{
    int32 i, bx, by, x, y, sw, sh, yy, cb, cr, r, g, b, n = jp->n, row[JP_MAXCOMP];
    int32 *maps;
    Comp *c;

    for (i = 0; i < jp->ncomp; i++) {
        c = &jp->c[i];
        for (by = 0; by < c->bh; by++)
            for (bx = 0; bx < c->bw; bx++)
                idct_block(c, bx, by);
    }
    sw = (jp->w * n + 7) / 8;
    sh = (jp->h * n + 7) / 8;
    maps = (int32 *)AllocMem(jp->ncomp * sw * sizeof(int32), MEMTYPE_ANY);
    if (maps == 0)
        return;
    for (i = 0; i < jp->ncomp; i++) {
        xmap[i] = maps + i * sw;
        for (x = 0; x < sw; x++)
            xmap[i][x] = x * jp->c[i].h / jp->hmax;
    }
    if (begin(ctx, sw, sh) < 0) {
        FreeMem(maps, jp->ncomp * sw * sizeof(int32));
        return;
    }
    for (y = 0; y < sh; y++) {
        for (i = 0; i < jp->ncomp; i++)
            row[i] = (y * jp->c[i].v / jp->vmax) * jp->c[i].bw * n;
        for (x = 0; x < sw; x++) {
            yy = jp->c[0].plane[row[0] + xmap[0][x]];
            if (jp->ncomp == 1) {
                px(ctx, x, y, yy, yy, yy);
                continue;
            }
            cb = jp->c[1].plane[row[1] + xmap[1][x]] - 128;
            cr = jp->c[2].plane[row[2] + xmap[2][x]] - 128;
            r = yy + ((91881 * cr + 32768) >> 16);
            g = yy - ((22554 * cb + 46802 * cr + 32768) >> 16);
            b = yy + ((116130 * cb + 32768) >> 16);
            px(ctx, x, y, clamp8(r), clamp8(g), clamp8(b));
        }
        if ((y & 15) == 15 && jp->ab && jp->ab())
            break;
    }
    FreeMem(maps, jp->ncomp * sw * sizeof(int32));
}

/* ---------------------------------------------------------------- */

int32 jpg_progressive(const uint8 *f, int32 len, int32 want_w, int32 want_h, ArtBegin begin, ArtPixel px, void *ctx,
                      ArtAbort ab)
{
    static Jp state;
    int32 m, seglen, at, rc = ART_BAD, i, k, tc, th, pq, cnt;
    uint8 counts[16];

    jp = &state;
    memset(jp, 0, sizeof(*jp));
    jp->f = f;
    jp->len = len;
    jp->ab = ab;
    jp->want_w = want_w;
    jp->want_h = want_h;
    jp->pos = 2;
    for (;;) {
        while (jp->pos + 1 < len && !(f[jp->pos] == 0xff && f[jp->pos + 1] != 0xff && f[jp->pos + 1] != 0))
            jp->pos++;
        if (jp->pos + 3 >= len)
            break;
        m = f[jp->pos + 1];
        if (m == 0xd9)
            break;
        if (m >= 0xd0 && m <= 0xd7) {
            jp->pos += 2;
            continue;
        }
        seglen = rd16(jp->pos + 2);
        at = jp->pos + 4;
        if (seglen < 2 || jp->pos + 2 + seglen > len)
            break;
        jp->pos += 2 + seglen;
        seglen -= 2;
        switch (m) {
        case 0xdb:                      /* quantisation tables */
            while (seglen > 0) {
                pq = f[at] >> 4;
                th = f[at] & 3;
                if (seglen < 1 + 64 * (pq + 1))
                    goto out;
                for (k = 0; k < 64; k++)
                    jp->q[th][zz_nat[k]] = (uint16)(pq ? rd16(at + 1 + 2 * k) : f[at + 1 + k]);
                at += 1 + 64 * (pq + 1);
                seglen -= 1 + 64 * (pq + 1);
            }
            break;
        case 0xc4:                      /* Huffman tables */
            while (seglen >= 17) {
                tc = f[at] >> 4;
                th = f[at] & 3;
                memcpy(counts, f + at + 1, 16);
                for (cnt = 0, i = 0; i < 16; i++)
                    cnt += counts[i];
                if (seglen < 17 + cnt || huff_build(tc ? &jp->ac[th] : &jp->dc[th], counts, f + at + 17, cnt) < 0)
                    goto out;
                at += 17 + cnt;
                seglen -= 17 + cnt;
            }
            break;
        case 0xdd:                      /* restart interval */
            jp->ri = rd16(at);
            break;
        case 0xc2:
            if (jp->mem || (rc = frame(at, seglen)) != ART_OK)
                goto out;
            rc = ART_BAD;
            break;
        case 0xda:
            if (!jp->mem)
                goto out;
            rc = scan(at, seglen);
            if (rc != ART_OK)
                goto out;
            rc = ART_BAD;
            break;
        default:
            if ((m >= 0xc0 && m <= 0xcf) && m != 0xc4 && m != 0xc8 && m != 0xcc)
                goto out;               /* another kind of frame */
            break;                      /* APPn, COM and the like */
        }
    }
    if (jp->mem) {
        make_pixels(begin, px, ctx);
        rc = ART_OK;
    }
out:
    if (jp->mem)
        FreeMem(jp->mem, jp->memsize);
    jp->mem = 0;
    return rc;
}
