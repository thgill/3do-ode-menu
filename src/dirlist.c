/* dirlist.c - the folder model behind the browser.
 *
 * Why it works the way it does: the firmware answers D1 (list entries) by re-opening the folder and walking it
 * from the start for every single entry (dir_get_nth_entry), so listing a folder of N entries costs about N*N/2
 * directory reads on the SD card, and a request may not ask for more than 14 entries (larger requests can run
 * past the firmware's 2 KB buffer). The stock menu therefore listed 14 entries per page, and every page further
 * down a big folder got slower.
 *
 * Here a folder is listed once, sorted (folders first, natural order), and stored in a small hidden index file
 * in that folder (_odemenu.idx; the firmware hides names starting with '_'). The next visit reads the index with
 * the fast sequential file commands and checks it against the firmware with three cheap probes (first, middle and
 * last entry, plus the end of the list). Before anything is queued or launched, its index is checked again, so a
 * stale index can never start the wrong game.
 */
#include "common.h"

#define CACHE_NAME   "_odemenu.idx"
#define RESUME_NAME  "_odemenu.cfg"
#define CACHE_MAGIC  0x4f4d4333     /* "OMC3": OMC2 plus game-folder fields (older indexes are rebuilt) */
#define REC_HEAD     8              /* flags, name length, toc (2), gtype, inner length, inner toc (2) */
#define GF_MAX_SEEN  32             /* a folder with more entries than this is never a game folder */

DirState g_dir;
int32    g_dir_no_write;            /* set while a game folder is opened to start its game: nothing is written */
uint32   g_toc_up = TOC_UP;
static int32 up_learned;            /* g_toc_up has been seen in a listing */
#define TOC_PREFIX  (g_toc_up == TOC_UP ? 0u : g_toc_up & 0xff000000u)     /* 0 on Fixel's firmware; the drive byte on the clone */
static void learn_up(void);
char     g_dir_err[96];            /* what went wrong last, for the error message */
static uint8 d1buf[ODE_XFER];
static uint8 iobuf[ODE_XFER];

/* ------------------------------------------------------------------ helpers */

static uint32 get32(const uint8 *p)
{
    return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3];
}

static void put32(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v >> 24);
    p[1] = (uint8)(v >> 16);
    p[2] = (uint8)(v >> 8);
    p[3] = (uint8)v;
}

static int32 lower(int32 c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int32 is_digit(int32 c)
{
    return c >= '0' && c <= '9';
}

/* case-insensitive natural order: "Disc 2" < "Disc 10" */
int32 name_compare(const char *a, const char *b)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    int32 la, lb, i, ca, cb;

    for (;;) {
        if (is_digit(*p) && is_digit(*q)) {
            while (*p == '0' && is_digit(p[1]))
                p++;
            while (*q == '0' && is_digit(q[1]))
                q++;
            for (la = 0; is_digit(p[la]); la++)
                ;
            for (lb = 0; is_digit(q[lb]); lb++)
                ;
            if (la != lb)
                return la - lb;
            for (i = 0; i < la; i++)
                if (p[i] != q[i])
                    return p[i] - q[i];
            p += la;
            q += lb;
            continue;
        }
        ca = lower(*p);
        cb = lower(*q);
        if (ca != cb)
            return ca - cb;
        if (ca == 0)
            return 0;
        p++;
        q++;
    }
}

const char *dir_name(int32 i)
{
    return g_dir.pool + g_dir.ent[i].name_off;
}

static void dir_clear(void)
{
    g_dir.count = 0;
    g_dir.hidden = 0;
    g_dir.pool_used = 0;
    g_dir.truncated = 0;
    g_dir.from_cache = 0;
}

static int32 add_entry(uint32 flags, uint32 toc, const char *name, int32 len)
{
    DirEntry *e;

    if (len > DIR_NAME_MAX - 1)
        len = DIR_NAME_MAX - 1;
    if (g_dir.count >= g_dir.max_entries || g_dir.pool_used + len + 1 > g_dir.pool_bytes) {
        g_dir.truncated = 1;
        return -1;
    }
    e = &g_dir.ent[g_dir.count++];
    e->toc_id = toc;
    e->flags = (uint8)flags;
    e->name_len = (uint8)len;
    e->queued = 0;
    e->art = 0;
    e->gtype = GT_UNKNOWN;
    e->inner_off = 0;
    e->inner_len = 0;
    e->inner_toc = 0;
    e->name_off = g_dir.pool_used;
    memcpy(g_dir.pool + g_dir.pool_used, name, len);
    g_dir.pool[g_dir.pool_used + len] = 0;
    g_dir.pool_used += len + 1;
    return 0;
}

/* a game folder's game file name, kept in the pool after the entries' names */
static int32 set_inner(DirEntry *e, const char *name, int32 len, uint32 toc)
{
    if (len < 1 || len > DIR_NAME_MAX - 1 || g_dir.pool_used + len + 1 > g_dir.pool_bytes)
        return -1;
    e->inner_off = g_dir.pool_used;
    e->inner_len = (uint8)len;
    e->inner_toc = toc;
    memcpy(g_dir.pool + g_dir.pool_used, name, len);
    g_dir.pool[g_dir.pool_used + len] = 0;
    g_dir.pool_used += len + 1;
    return 0;
}

static int32 ext_matches(const char *name, int32 len, const char *ext)
{
    int32 k;

    if (len < 5 || name[len - 4] != '.')
        return 0;
    for (k = 0; k < 3; k++)
        if (lower(name[len - 3 + k]) != ext[k])
            return 0;
    return 1;
}

static int32 name_is_game_n(const char *name, int32 len)
{
    return ext_matches(name, len, "iso") || ext_matches(name, len, "cue");
}

int32 name_is_game(const char *name)
{
    return name_is_game_n(name, strlen(name));
}

/* one D1 record at buf[pos]; returns its size, 0 at the end marker, -1 if malformed */
static int32 d1_record(const uint8 *buf, int32 pos, uint32 *flags, uint32 *toc, const char **name, int32 *len)
{
    uint32 n;

    if (pos + 8 > ODE_XFER)
        return -1;
    *flags = get32(buf + pos);
    *toc = get32(buf + pos + 4);
    if (*flags == 0xffffffff)
        return 0;
    if (TOC_IS_UP(*toc)) {
        g_toc_up = *toc;
        up_learned = 1;
    }            /* learnt from every listing: the clone's ".." carries its drive number */
    if (pos + 12 > ODE_XFER)
        return -1;
    n = get32(buf + pos + 8);
    if (n == 0 || n > DIR_NAME_MAX || pos + 12 + (int32)n > ODE_XFER)
        return -1;
    *name = (const char *)(buf + pos + 12);
    *len = n;
    while (*len > 0 && (*name)[*len - 1] == 0)
        (*len)--;
    return 12 + n;
}

/* ---- single-entry D1 ("probe"): how the menu checks an entry before acting on it, and checks a folder index.
 * Fixel's firmware answers a D1 for one entry exactly like the same entry in a longer listing. A clone may not (a
 * tester's could not launch anything: every check failed). So on the first listing read from the card the menu
 * asks for its first entry alone, with 1, 2 and 14 as the count, and keeps the first count that answers right.
 * If none does, it stops relying on single-entry answers: no folder index, and entries are trusted as listed by
 * a fresh read of the folder (as the stock menu does). */
static int32 probe_count = 1;
static int32 d1_off;                /* a clone answers D1 index n with the entry at n + 1 (index 0 is ".." then
                                     * the first entry): 1 there, 0 on Fixel's firmware */
int32 g_probe_ok = -1;              /* -1 not tested yet, 1 works (with probe_count), 0 does not */
char  g_probe_info[40] = "D1 CHECK: NO FOLDER READ YET";

/* the D1 index that answers with the entry whose id is id (a clone's ids carry a drive byte) */
static uint32 d1_index(uint32 id)
{
    uint32 pos = id & 0x00ffffff;
    return pos >= (uint32)d1_off ? pos - d1_off : 0;
}

static int32 probe_n(uint32 idx, int32 count, uint32 *flags, uint32 *toc, char *name)
{
    const char *nm;
    int32 len, r, pos = 0;
    uint32 at = d1_index(idx);

    if (at == 0 && count < 2)
        count = 2;                      /* index 0 starts with "..": the entry is the one after it */
    r = ode_d1_list(at, count, d1buf);
    if (r < 0)
        return r;
    for (;;) {
        r = d1_record(d1buf, pos, flags, toc, &nm, &len);
        if (r <= 0)
            return r;
        if (!TOC_IS_UP(*toc))
            break;
        pos += r;
    }
    memcpy(name, nm, len);
    name[len] = 0;
    return 1;
}

/* D1 for a single index: fills name (NUL-terminated); returns 1 found, 0 end of list, < 0 error */
static int32 probe(uint32 idx, uint32 *flags, char *name)
{
    uint32 toc;
    int32 r = probe_n(idx, probe_count, flags, &toc, name);

    if (r == 1 && toc != idx)
        return -1;
    return r;
}

static void probe_calibrate(void)
{
    static const int32 counts[3] = { 1, 2, 14 };
    char name[DIR_NAME_MAX];
    uint32 flags = 0, toc = 0;
    DirEntry *e;
    int32 k, r = 0;

    int32 off;

    if (g_probe_ok != -1 || g_dir.devlist || g_dir.count == 0)
        return;
    e = &g_dir.ent[0];
    for (off = 0; off < 2; off++) {
        d1_off = off;
        for (k = 0; k < 3; k++) {
            name[0] = 0;
            r = probe_n(e->toc_id, counts[k], &flags, &toc, name);
            if (r == 1 && toc == e->toc_id && (flags & 0xff) == e->flags && strcmp(name, dir_name(0)) == 0) {
                probe_count = counts[k];
                g_probe_ok = 1;
                sprintf(g_probe_info, "D1 CHECK OK (COUNT %d%s)", (int)counts[k], off ? ", SHIFTED" : "");
                return;
            }
        }
        if (off == 0 && !(r == 1 && (toc & 0x00ffffff) == (e->toc_id & 0x00ffffff) + 1))
            break;                      /* not the clone's one-late answer: no point trying the shift */
    }
    d1_off = 0;
    g_probe_ok = 0;
    sprintf(g_probe_info, "D1 #%u: %d #%u F%02X", (unsigned)e->toc_id, (int)r, (unsigned)toc, (unsigned)(flags & 0xff));
}

/* ------------------------------------------------------------------ sorting */

/* ordinary folders first; game folders sort among the games */
static int32 entry_less(const DirEntry *a, const DirEntry *b)
{
    int32 ad = (a->flags & TF_DIR) && a->gtype != GT_GAME;
    int32 bd = (b->flags & TF_DIR) && b->gtype != GT_GAME;

    if (ad != bd)
        return ad > bd;
    return name_compare(g_dir.pool + a->name_off, g_dir.pool + b->name_off) < 0;
}

/* bottom-up merge sort (stable, no recursion) */
static void sort_entries(void)
{
    DirEntry *a = g_dir.ent, *tmp, *src, *dst, *t;
    int32 n = g_dir.count, width, i, l, m, r, x, y, k;

    if (n < 2)
        return;
    tmp = (DirEntry *)AllocMem(n * sizeof(DirEntry), MEMTYPE_ANY);
    if (tmp == 0) {
        /* fall back to insertion sort */
        for (i = 1; i < n; i++) {
            DirEntry v = a[i];
            for (k = i - 1; k >= 0 && entry_less(&v, &a[k]); k--)
                a[k + 1] = a[k];
            a[k + 1] = v;
        }
        return;
    }
    src = a;
    dst = tmp;
    for (width = 1; width < n; width *= 2) {
        for (l = 0; l < n; l += 2 * width) {
            m = l + width < n ? l + width : n;
            r = l + 2 * width < n ? l + 2 * width : n;
            x = l;
            y = m;
            k = l;
            while (x < m && y < r)
                dst[k++] = entry_less(&src[y], &src[x]) ? src[y++] : src[x++];
            while (x < m)
                dst[k++] = src[x++];
            while (y < r)
                dst[k++] = src[y++];
        }
        t = src;
        src = dst;
        dst = t;
    }
    if (src != a)
        memcpy(a, src, n * sizeof(DirEntry));
    FreeMem(tmp, n * sizeof(DirEntry));
}

/* ------------------------------------------------------------------ full scan */

static int32 scan(DirProgress progress)
{
    uint32 idx = 0, flags, toc, last_real;
    const char *name;
    int32 len, pos, sz, got, err;

    dir_clear();
    g_dir.devlist = 0;
    for (;;) {
        err = ode_d1_list(idx, 14, d1buf);
        if (err < 0) {
            sprintf(g_dir_err, "D1 at %d failed: %d", (int)idx, (int)err);
            return err;
        }
        pos = 0;
        got = 0;
        last_real = 0;
        for (;;) {
            sz = d1_record(d1buf, pos, &flags, &toc, &name, &len);
            if (sz == 0)
                goto done;
            if (sz < 0)
                break;
            if (!TOC_IS_UP(toc))
                last_real = toc;
            if (idx == 0 && got == 0 && name[0] == '/' && !TOC_IS_UP(toc))
                g_dir.devlist = 1;
            if (!TOC_IS_UP(toc) && add_entry(flags, toc, name, len) < 0)
                goto done;
            pos += sz;
            got++;
            if (got >= 14)
                break;
        }
        if (got == 0) {
            sprintf(g_dir_err, "D1 at %d: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                    (int)idx, d1buf[0], d1buf[1], d1buf[2], d1buf[3], d1buf[4], d1buf[5], d1buf[6], d1buf[7],
                    d1buf[8], d1buf[9], d1buf[10], d1buf[11], d1buf[12], d1buf[13], d1buf[14], d1buf[15]);
            return -1;
        }
        if (g_probe_ok == -1 && !g_dir.devlist && got >= 14)
            probe_calibrate();          /* before the next page: how this ODE numbers its D1 answers */
        idx = last_real && !g_dir.devlist ? d1_index(last_real) + 1 : idx + got;
        if (progress && progress(g_dir.count, 0)) {
            g_dir.truncated = 1;
            return 1;           /* canceled: keep what we have, do not cache it */
        }
    }
done:
    return 0;
}

/* ------------------------------------------------------------------ game folders
 * A folder holding exactly one game (.iso or .cue; the firmware does not list .bin files) and no folders is shown
 * and started as that game. To tell, the menu goes into each folder of a freshly read listing, reads the start of
 * its listing (one D1 request; it stops early once the answer is clear) and comes back up. The result is kept in
 * the folder's index, so this happens once per folder. */

static int32 detect_folder(int32 i)
{
    DirEntry *e = &g_dir.ent[i];
    uint32 idx = 0, flags, toc, gtoc = 0, last_real;     /* from 0 in pages of 14, as the stock menu reads a folder */
    const char *name;
    char gname[DIR_NAME_MAX];
    int32 len, pos, sz, got, games = 0, seen = 0, glen = 0, done = 0, plain = 0;

    e->gtype = GT_PLAIN;
    if (ode_c0_change_dir(e->toc_id) < 0)
        return -1;
    while (!done && !plain) {
        if (ode_d1_list(idx, 14, d1buf) < 0)
            break;
        pos = 0;
        got = 0;
        last_real = 0;
        for (;;) {
            sz = d1_record(d1buf, pos, &flags, &toc, &name, &len);
            if (sz == 0) {
                done = 1;
                break;
            }
            if (sz < 0)
                break;
            pos += sz;
            got++;
            if (TOC_IS_UP(toc))
                continue;
            last_real = toc;
            seen++;
            if ((flags & TF_DIR) || seen > GF_MAX_SEEN) {
                plain = 1;
                break;
            }
            if ((flags & TF_FILE) && !(flags & (TF_NVM | TF_ODE)) && name_is_game_n(name, len)) {
                if (++games > 1) {
                    plain = 1;
                    break;
                }
                glen = len < DIR_NAME_MAX ? len : DIR_NAME_MAX - 1;
                memcpy(gname, name, glen);
                gtoc = toc;
            }
            if (got >= 14)
                break;
        }
        if (got == 0)
            break;
        idx = last_real ? d1_index(last_real) + 1 : idx + got;
    }
    ode_c0_change_dir(g_toc_up);
    if (done && !plain && games == 1 && set_inner(e, gname, glen, gtoc) == 0)
        e->gtype = GT_GAME;
    return 0;
}

/* every folder of the listing; 1 if canceled (the rest stay GT_UNKNOWN and nothing is cached) */
static int32 detect_all(DirProgress progress)
{
    int32 i, n = 0, k = 0;

    for (i = 0; i < g_dir.count; i++)
        if (g_dir.ent[i].flags & TF_DIR)
            n++;
    for (i = 0; i < g_dir.count; i++) {
        if (!(g_dir.ent[i].flags & TF_DIR))
            continue;
        if (progress && progress(k * 100 / n, 4))
            return 1;
        detect_folder(i);
        k++;
    }
    return 0;
}

int32 dir_is_game_folder(int32 i)
{
    return i >= 0 && i < g_dir.count && !g_dir.devlist && (g_dir.ent[i].flags & TF_DIR) &&
           g_dir.ent[i].gtype == GT_GAME;
}

int32 dir_is_game(int32 i)
{
    DirEntry *e;

    if (i < 0 || i >= g_dir.count || g_dir.devlist)
        return 0;
    e = &g_dir.ent[i];
    if (e->flags & TF_DIR)
        return e->gtype == GT_GAME;
    return (e->flags & TF_FILE) && !(e->flags & (TF_NVM | TF_ODE)) && name_is_game_n(dir_name(i), e->name_len);
}

const char *dir_inner(int32 i)
{
    return g_dir.pool + g_dir.ent[i].inner_off;
}

/* Into game folder i, ready for C4 with *toc. Its game is checked by name first; if the folder changed since it
 * was indexed it is looked at again. On failure the firmware is back where it was. */
int32 dir_game_enter(int32 i, uint32 *toc)
{
    DirEntry *e;
    uint32 idx = 0, flags, t, gtoc = 0, last_real;
    const char *name;
    int32 len, pos, sz, got, games = 0, seen = 0, found = 0, done = 0;

    if (!dir_is_game_folder(i))
        return -1;
    e = &g_dir.ent[i];
    if (ode_c0_change_dir(e->toc_id) < 0)
        return -1;
    /* the folder's listing, read as when you open it yourself (pages of 14 from the start): the game's id comes
     * from it, so the start is the same as picking the file inside the folder (a clone did not start a game
     * folder's game after the single-entry check) */
    while (!done && !found && seen <= GF_MAX_SEEN) {
        if (ode_d1_list(idx, 14, d1buf) < 0)
            break;
        pos = 0;
        got = 0;
        last_real = 0;
        for (;;) {
            sz = d1_record(d1buf, pos, &flags, &t, &name, &len);
            if (sz == 0) {
                done = 1;
                break;
            }
            if (sz < 0)
                break;
            pos += sz;
            got++;
            if (!TOC_IS_UP(t)) {
                last_real = t;
                seen++;
                if ((flags & TF_FILE) && !(flags & (TF_NVM | TF_ODE)) && name_is_game_n(name, len)) {
                    games++;
                    gtoc = t;
                    if (len == (int32)e->inner_len && memcmp(name, dir_inner(i), len) == 0) {
                        *toc = t;
                        found = 1;
                        break;
                    }
                }
            }
            if (got >= 14)
                break;
        }
        if (got == 0)
            break;
        idx = last_real ? d1_index(last_real) + 1 : idx + got;
    }
    if (found)
        return 0;
    if (done && games == 1) {           /* renamed since it was read: still the folder's one game */
        *toc = gtoc;
        return 0;
    }
    ode_c0_change_dir(g_toc_up);
    return -2;
}

void dir_game_leave(void)
{
    ode_c0_change_dir(g_toc_up);
}

/* ------------------------------------------------------------------ index file */

static int32 serialize(int32 i, uint8 *out)
{
    DirEntry *e = &g_dir.ent[i];

    int32 il = e->gtype == GT_GAME ? e->inner_len : 0;

    out[0] = e->flags;
    out[1] = e->name_len;
    out[2] = (uint8)(e->toc_id >> 8);     /* the entry number; a clone's drive byte is put back on reading */
    out[3] = (uint8)e->toc_id;
    out[4] = e->gtype;
    out[5] = (uint8)il;
    out[6] = (uint8)(e->inner_toc >> 8);
    out[7] = (uint8)e->inner_toc;
    memcpy(out + REC_HEAD, g_dir.pool + e->name_off, e->name_len);
    memcpy(out + REC_HEAD + e->name_len, g_dir.pool + e->inner_off, il);
    return REC_HEAD + e->name_len + il;
}

int32 dir_write_cache(void)
{
    uint8 rec[REC_HEAD + 2 * DIR_NAME_MAX];
    uint32 crc = 0xffffffff, body = 0;
    int32 i, n, fill, err, total;

    if (g_dir.devlist || g_dir.truncated)
        return -1;
    total = g_dir.count + g_dir.hidden;         /* the index keeps every entry, shown or not */
    for (i = 0; i < total; i++) {
        if ((g_dir.ent[i].toc_id & 0x00ffffff) > 0xffff || (g_dir.ent[i].toc_id & 0xff000000) != TOC_PREFIX ||
            (g_dir.ent[i].gtype == GT_GAME && ((g_dir.ent[i].inner_toc & 0x00ffffff) > 0xffff ||
                                               (g_dir.ent[i].inner_toc & 0xff000000) != TOC_PREFIX)))
            return -1;
        n = serialize(i, rec);
        crc = crc32_update(crc, rec, n);
        body += n;
    }
    crc = ~crc;
    if (ode_file_open(CACHE_NAME, 1) < 0)
        return -1;
    put32(iobuf, CACHE_MAGIC);
    put32(iobuf + 4, total);
    put32(iobuf + 8, body);
    put32(iobuf + 12, crc);
    fill = 16;
    err = 0;
    for (i = 0; i < total && err == 0; i++) {
        n = serialize(i, rec);
        if (fill + n > ODE_XFER) {
            int32 part = ODE_XFER - fill;
            memcpy(iobuf + fill, rec, part);
            err = ode_file_write(iobuf, ODE_XFER);
            memcpy(iobuf, rec + part, n - part);
            fill = n - part;
        } else {
            memcpy(iobuf + fill, rec, n);
            fill += n;
        }
    }
    if (err == 0 && fill > 0)
        err = ode_file_write(iobuf, fill);
    ode_file_close();
    return err;
}

static int32 read_cache(DirProgress progress)
{
    uint32 count, body, crc, got, have = 0;
    uint8 *buf = 0;
    int32 err = -1, pos, len, ilen;
    uint32 total = 0;
    DirEntry *e;

    if (ode_file_open(CACHE_NAME, 0) < 0)
        return -1;
    if (ode_file_read(iobuf, ODE_XFER, &got) < 0 || got < 16)
        goto out;
    if (get32(iobuf) != CACHE_MAGIC)
        goto out;
    count = get32(iobuf + 4);
    body = get32(iobuf + 8);
    crc = get32(iobuf + 12);
    if (count > (uint32)g_dir.max_entries || body > (uint32)g_dir.pool_bytes + count * REC_HEAD ||
        body < count * (REC_HEAD + 1))
        goto out;
    total = body ? body : 4;
    buf = (uint8 *)AllocMem(total, MEMTYPE_ANY);
    if (buf == 0)
        goto out;
    have = got - 16;
    if (have > body)
        goto out;
    memcpy(buf, iobuf + 16, have);
    while (have < body) {
        if (ode_file_read(iobuf, ODE_XFER, &got) < 0 || got == 0)
            goto out;
        if (have + got > body)
            got = body - have;
        memcpy(buf + have, iobuf, got);
        have += got;
        if (progress)
            progress(have * 100 / body, 3);
    }
    if (~crc32_update(0xffffffff, buf, body) != crc)
        goto out;
    dir_clear();
    pos = 0;
    while ((uint32)pos < body) {
        len = buf[pos + 1];
        ilen = buf[pos + 5];
        if (pos + REC_HEAD + len + ilen > (int32)body || len == 0)
            goto out;
        if (add_entry(buf[pos], TOC_PREFIX | ((uint32)buf[pos + 2] << 8) | buf[pos + 3], (const char *)buf + pos + REC_HEAD, len) < 0)
            goto out;
        e = &g_dir.ent[g_dir.count - 1];
        e->gtype = buf[pos + 4] & 0x0f;   /* (one build kept a "no picture" mark in bit 5: ignored) */
        if (e->gtype == GT_GAME && set_inner(e, (const char *)buf + pos + REC_HEAD + len, ilen,
                                             TOC_PREFIX | ((uint32)buf[pos + 6] << 8) | buf[pos + 7]) < 0)
            e->gtype = GT_UNKNOWN;
        pos += REC_HEAD + len + ilen;
    }
    if ((uint32)g_dir.count != count)
        goto out;
    err = 0;
out:
    ode_file_close();
    if (buf)
        FreeMem(buf, total);
    if (err < 0)
        dir_clear();
    return err;
}

static int32 find_toc(uint32 toc)
{
    int32 i;
    for (i = 0; i < g_dir.count; i++)
        if (g_dir.ent[i].toc_id == toc)
            return i;
    return -1;
}

static int32 check_one(uint32 toc)
{
    char name[DIR_NAME_MAX];
    uint32 flags;
    int32 i = find_toc(toc);

    if (i < 0)
        return 0;
    if (probe(toc, &flags, name) != 1)
        return 0;
    if ((flags & 0xff) != g_dir.ent[i].flags)
        return 0;
    return strcmp(name, dir_name(i)) == 0;
}

/* three probes plus the end of the list; about 2.5 passes over the folder in the firmware */
static int32 cache_matches(void)
{
    char name[DIR_NAME_MAX];
    uint32 flags, n = g_dir.count;
    int32 i;

    for (i = 0; i < g_dir.count; i++)
        if (g_dir.ent[i].toc_id < (TOC_PREFIX | 1) || g_dir.ent[i].toc_id > (TOC_PREFIX | n))
            return 0;               /* ids must be exactly 1..n */
    if (n == 0)
        return probe(1, &flags, name) == 0;
    if (!check_one(TOC_PREFIX | n))
        return 0;
    if (probe(TOC_PREFIX | (n + 1), &flags, name) != 0)
        return 0;
    if (n > 1 && !check_one(TOC_PREFIX | 1))
        return 0;
    if (n > 2 && !check_one(TOC_PREFIX | (n / 2 + 1)))
        return 0;
    return 1;
}

/* ------------------------------------------------------------------ public */

int32 dir_init(void)
{
    int32 n;

    memset(&g_dir, 0, sizeof(g_dir));
    /* take what memory allows: full size first, then smaller listings */
    for (n = DIR_MAX_ENTRIES; n >= 256; n /= 2) {
        g_dir.ent = (DirEntry *)AllocMem(n * sizeof(DirEntry), MEMTYPE_ANY);
        g_dir.pool = (char *)AllocMem(n * (DIR_POOL_BYTES / DIR_MAX_ENTRIES), MEMTYPE_ANY);
        if (g_dir.ent && g_dir.pool)
            break;
        if (g_dir.ent)
            FreeMem(g_dir.ent, n * sizeof(DirEntry));
        if (g_dir.pool)
            FreeMem(g_dir.pool, n * (DIR_POOL_BYTES / DIR_MAX_ENTRIES));
        g_dir.ent = 0;
        g_dir.pool = 0;
    }
    if (g_dir.ent == 0)
        return -1;
    g_dir.max_entries = n;
    g_dir.pool_bytes = n * (DIR_POOL_BYTES / DIR_MAX_ENTRIES);
    g_dir.use_cache = 1;
    g_dir.both_drives = (ode_drives() == 3);
    g_dir.drive = g_dir.both_drives ? 0 : ode_drives();
    return 0;
}

/* The menu disc itself (boot.iso in the drive root) is not shown: it is moved past the end of the list, so
 * g_dir.count covers only what the menu shows. The index file keeps every entry. */
static int32 is_menu_disc(int32 i)
{
    const char *a = dir_name(i), *b = "boot.iso";
    int32 k;

    for (k = 0; a[k] && lower(a[k]) == b[k]; k++)
        ;
    return a[k] == 0 && b[k] == 0;
}

/* Files the menu has no use for are not shown either: the firmware lists any file whose extension is not three
 * letters long (pictures named .jpeg, files without an extension), and those are neither games nor saves. */
static int32 is_hidden(int32 i)
{
    DirEntry *e = &g_dir.ent[i];

    if (g_dir.depth == 0 && is_menu_disc(i))
        return 1;
    return !(e->flags & TF_DIR) && !(e->flags & (TF_NVM | TF_ODE)) && !name_is_game_n(dir_name(i), e->name_len);
}

static void hide_menu_disc(void)
{
    DirEntry keep;
    int32 i, total = g_dir.count;

    if (g_dir.devlist)
        return;
    for (i = 0; i < g_dir.count;) {
        if (is_hidden(i)) {
            keep = g_dir.ent[i];
            memmove(&g_dir.ent[i], &g_dir.ent[i + 1], (total - i - 1) * sizeof(DirEntry));
            g_dir.ent[total - 1] = keep;
            g_dir.count--;
            g_dir.hidden++;
        } else {
            i++;
        }
    }
}

static int32 load_listing(DirProgress progress, int32 force_scan);

int32 dir_load(DirProgress progress, int32 force_scan)
{
    int32 err = load_listing(progress, force_scan);

    if (err >= 0)
        hide_menu_disc();
    return err;
}

static int32 load_listing(DirProgress progress, int32 force_scan)
{
    int32 err;
    int32 at_devlist = g_dir.both_drives && g_dir.drive == 0;

    /* a game folder opened to start its game is always read from the ODE: its saved listing is not used for the
     * entry number that is sent to start the game */
    if (!force_scan && !g_dir_no_write && g_dir.use_cache && g_probe_ok != 0 && !at_devlist) {
        if (!up_learned)
            learn_up();                 /* the drive byte the index's entry numbers need (a clone) */
        if (progress)
            progress(0, 3);
        if (read_cache(progress) == 0) {
            if (progress)
                progress(0, 1);
            if (cache_matches()) {
                g_dir.devlist = 0;
                g_dir.from_cache = 1;
                probe_calibrate();      /* (the index's checks just passed, so this will too: it is for DEVICE INFO) */
                return 0;
            }
        }
    }
    err = scan(progress);
    if (err < 0)
        return err;
    probe_calibrate();
    if (!g_dir.devlist && err == 0 && detect_all(progress))
        err = 1;                /* canceled: shown, not cached */
    if (!g_dir.devlist)
        sort_entries();
    if (err == 0 && g_dir.use_cache && g_probe_ok != 0 && !g_dir.devlist && !g_dir.truncated && g_dir.count > 0 &&
        !g_dir_no_write) {
        if (progress)
            progress(0, 2);
        dir_write_cache();
    }
    return err;
}

int32 dir_verify(int32 i)
{
    if (i < 0 || i >= g_dir.count)
        return 0;
    if (g_dir.devlist)
        return 1;
    if (g_probe_ok == 0)
        return !g_dir.from_cache;       /* no single-entry checks on this ODE: a fresh listing is trusted */
    return check_one(g_dir.ent[i].toc_id);
}

int32 dir_find(const char *name)
{
    int32 i;
    for (i = 0; i < g_dir.count; i++)
        if (strcmp(dir_name(i), name) == 0)
            return i;
    return -1;
}

int32 dir_find_nocase(const char *name)
{
    int32 i, k;
    const char *a;

    for (i = 0; i < g_dir.count; i++) {
        a = dir_name(i);
        for (k = 0; lower(a[k]) == lower(name[k]) && a[k]; k++)
            ;
        if (a[k] == 0 && name[k] == 0)
            return i;
    }
    return -1;
}

int32 dir_enter(int32 i, DirProgress progress)
{
    DirEntry *e = &g_dir.ent[i];
    int32 r;

    if (!(e->flags & TF_DIR))
        return -1;
    if (g_dir.devlist) {
        if (ode_c0_change_dir(e->toc_id) < 0)
            return -1;
        g_dir.drive = e->toc_id;
        g_dir.depth = 0;
        g_dir.devlist = 0;
        return dir_load(progress, 0);
    }
    if (g_dir.depth >= DIR_MAX_DEPTH)
        return -2;
    if (g_dir.from_cache && !check_one(e->toc_id))
        return -3;              /* index is stale: the caller rescans and tries again */
    r = ode_c0_change_dir(e->toc_id);
    if (r < 0) {
        sprintf(g_dir_err, "C0 %d failed: %d", (int)e->toc_id, (int)r);
        return -1;
    }
    strncpy(g_dir.path[g_dir.depth], dir_name(i), DIR_NAME_MAX - 1);
    g_dir.path[g_dir.depth][DIR_NAME_MAX - 1] = 0;
    g_dir.path_toc[g_dir.depth] = e->toc_id;
    g_dir.depth++;
    return dir_load(progress, 0);
}

int32 dir_back(DirProgress progress, char *came_from)
{
    came_from[0] = 0;
    if (g_dir.devlist)
        return 2;
    if (g_dir.depth == 0) {
        if (!g_dir.both_drives)
            return 2;
        ode_c0_change_dir(g_toc_up);
        strcpy(came_from, g_dir.drive == 2 ? "/usb" : "/sd");
        g_dir.drive = 0;
        return dir_load(progress, 0);
    }
    ode_c0_change_dir(g_toc_up);
    g_dir.depth--;
    strcpy(came_from, g_dir.path[g_dir.depth]);
    return dir_load(progress, 0);
}

int32 g_boot_open = 99;                 /* what opening boot.iso last answered (0 = opened), for the test log */

static int32 at_boot_root(void)
{
    int32 r = ode_file_open("boot.iso", 0);
    ode_file_close();
    g_boot_open = r;
    return r == 0;
}

/* boot.iso is in this listing (a firmware that will not open the menu disc while it is the running disc still
 * lists it): this is the drive root */
int32 dir_has_menu_disc(void)
{
    int32 i;

    if (g_dir.depth != 0 || g_dir.devlist)
        return 0;
    for (i = 0; i < g_dir.count + g_dir.hidden; i++)
        if (is_menu_disc(i))
            return 1;
    return 0;
}

/* Startup. With both drives C0(0) selects the device list. With one drive the firmware may still be in a folder
 * (an earlier stock-menu session, or a console reset while browsing), and its "up" is not clamped at the root: one
 * "up" too many empties its path and breaks file access until power-off. So the menu only searches upward when it
 * knows the root it is looking for: EXT_ID says the menu disc (boot.iso) was served from a drive (source code 1 or
 * 2; the firmware swaps the two after a double click, and with a single drive either one means this drive), so
 * that drive's root holds boot.iso. It then goes up one level at a time until boot.iso opens. Without that
 * knowledge it stays where it is, as the stock menu did. */
/* this folder's ".." id, from the first entry of its listing (see TOC_IS_UP) */
static void learn_up(void)
{
    uint32 flags, toc;
    const char *nm;
    int32 len;

    if (ode_d1_list(0, 1, d1buf) >= 0)
        d1_record(d1buf, 0, &flags, &toc, &nm, &len);
}

int32 dir_go_root(void)
{
    int32 i;

    ode_c0_change_dir(0);
    g_dir.depth = 0;
    g_dir.drive = g_dir.both_drives ? 0 : ode_drives();
    if (g_dir.both_drives)
        return 0;
    if (at_boot_root())
        return 0;
    if (((ode_ext_b1() >> 4) & 3) == 0)
        return -1;
    for (i = 0; i < DIR_MAX_DEPTH; i++) {
        learn_up();
        ode_c0_change_dir(g_toc_up);
        if (at_boot_root())
            return 0;
    }
    return -1;
}

/* "/3DO/USA/" (the drive is shown separately); the device list is "/" */
void dir_path_string(char *out, int32 max)
{
    int32 i, n;

    strcpy(out, "/");
    if (g_dir.devlist)
        return;
    for (i = 0; i < g_dir.depth; i++) {
        n = strlen(out);
        if (n + strlen(g_dir.path[i]) + 2 >= (uint32)max)
            break;
        strcat(out, g_dir.path[i]);
        strcat(out, "/");
    }
}

/* ------------------------------------------------------------------ moving around by path */

/* Take the firmware to the drive root for a file access there, and back. The firmware joins its current path and
 * the file name, and FatFs does not resolve ".." on exFAT cards, so root files are never reached as "../x".
 * Exactly depth "up" commands are sent, and the way back re-enters the same entries (as after a launch). */
int32 dir_root_begin(void)
{
    int32 i;

    if (g_dir.devlist)
        return -1;
    for (i = 0; i < g_dir.depth; i++)
        ode_c0_change_dir(g_toc_up);
    return 0;
}

void dir_root_end(void)
{
    dir_restore_path();
}

static void go_up(int32 levels)
{
    while (levels-- > 0 && g_dir.depth > 0) {
        ode_c0_change_dir(g_toc_up);
        g_dir.depth--;
    }
}

int32 dir_to_root(DirProgress progress)
{
    if (g_dir.devlist)
        return 0;
    if (g_dir.depth == 0)
        return 0;
    go_up(g_dir.depth);
    return dir_load(progress, 0);
}

int32 dir_to_devlist(DirProgress progress)
{
    if (!g_dir.both_drives)
        return -1;
    if (g_dir.devlist)
        return 0;
    go_up(g_dir.depth);
    ode_c0_change_dir(g_toc_up);
    g_dir.drive = 0;
    return dir_load(progress, 0);
}

static int32 enter_named(const char *name, DirProgress progress)
{
    int32 i, r;

    i = dir_find(name);
    if (i < 0 || !(g_dir.ent[i].flags & TF_DIR))
        return -2;
    r = dir_enter(i, progress);
    if (r == -3) {                              /* stale index: rescan and look again */
        if (dir_load(progress, 1) != 0)
            return -1;
        i = dir_find(name);
        if (i < 0 || !(g_dir.ent[i].flags & TF_DIR))
            return -2;
        r = dir_enter(i, progress);
    }
    return r != 0 ? -1 : 0;     /* 1: the read was canceled */
}

/* Go to the folder of path (from the root of drive) and return the index of its last component there.
 * Only the levels that differ from where the menu is are left and entered. */
int32 dir_open_path(int32 drive, const char *path, DirProgress progress)
{
    static char comps[DIR_MAX_DEPTH + 1][DIR_NAME_MAX];
    int32 n, c, i, r;

    n = list_split(path, comps, DIR_MAX_DEPTH + 1);
    if (n < 1)
        return -1;
    if (g_dir.both_drives && (g_dir.devlist || g_dir.drive != drive)) {
        if (dir_to_devlist(progress) < 0)
            return -1;
        for (i = 0; i < g_dir.count; i++)
            if (g_dir.ent[i].toc_id == (uint32)drive)
                break;
        if (i >= g_dir.count || dir_enter(i, progress) != 0)
            return -1;
    }
    for (c = 0; c < g_dir.depth && c < n - 1 && strcmp(g_dir.path[c], comps[c]) == 0; c++)
        ;
    if (c < g_dir.depth) {
        go_up(g_dir.depth - c);
        if (dir_load(progress, 0) != 0)
            return -1;
    }
    for (; c < n - 1; c++) {
        r = enter_named(comps[c], progress);
        if (r < 0)
            return r;
    }
    i = dir_find(comps[n - 1]);
    return i < 0 ? -3 : i;
}

/* ------------------------------------------------------------------ resume (last folder) */

/* Called just before C5 (the queue is already built, so moving the firmware's current folder is safe).
 * Takes the firmware back to the drive root, so the next menu start finds it there, and when write is set stores
 * "<drive>\n<depth>\n<name>...\n<selected>\n" in _odemenu.cfg in that root. Exactly depth "up" commands are
 * sent: the firmware's "up" is not clamped at the root (it empties its path), so it must never go one too far.
 * dir_restore_path() goes back down if the console does not start the game. */
int32 dir_write_resume(const char *selected, int32 write)
{
    char *p = (char *)iobuf;
    int32 i, len, err = 0;

    if (g_dir.devlist)
        return -1;
    sprintf(p, "OMR1\n%d\n%d\n", (int)g_dir.drive, (int)g_dir.depth);
    for (i = 0; i < g_dir.depth; i++) {
        if (strlen(p) + strlen(g_dir.path[i]) + 2 >= ODE_XFER - DIR_NAME_MAX)
            write = 0;
        else {
            strcat(p, g_dir.path[i]);
            strcat(p, "\n");
        }
    }
    strcat(p, selected ? selected : "");
    strcat(p, "\n");
    len = strlen(p);
    if (write) {
        err = ode_file_open(RESUME_NAME, 1);
        if (err == 0) {
            err = ode_file_write(iobuf, len);
            ode_file_close();
        }
    }
    return err;
}

void dir_restore_path(void)
{
    int32 i;
    for (i = 0; i < g_dir.depth; i++)
        ode_c0_change_dir(g_dir.path_toc[i]);
}

static int32 read_resume(char lines[DIR_MAX_DEPTH + 4][DIR_NAME_MAX], int32 *nlines)
{
    uint32 got;
    int32 i, n = 0, k = 0;

    *nlines = 0;
    if (ode_file_open(RESUME_NAME, 0) < 0)
        return -1;
    if (ode_file_read(iobuf, ODE_XFER - 1, &got) < 0)
        got = 0;
    ode_file_close();
    if (got < 5)
        return -1;
    iobuf[got] = 0;
    for (i = 0; i < (int32)got && n < DIR_MAX_DEPTH + 4; i++) {
        if (iobuf[i] == '\n') {
            lines[n][k] = 0;
            n++;
            k = 0;
        } else if (k < DIR_NAME_MAX - 1) {
            lines[n][k++] = iobuf[i];
        }
    }
    if (n < 4 || strcmp(lines[0], "OMR1") != 0)
        return -1;
    *nlines = n;
    return 0;
}

static char resume_lines[DIR_MAX_DEPTH + 4][DIR_NAME_MAX];

/* At startup, after dir_go_root(): walk back into the folder of the last launch. selected receives the name
 * that was launched (empty if none). The listing of the final folder is loaded. */
int32 dir_resume(DirProgress progress, char *selected)
{
    int32 n = 0, i, depth, k, drive;

    selected[0] = 0;
    if (g_dir.both_drives) {
        /* the device list cannot hold files: look in the root of each drive */
        for (drive = 1; drive <= 2; drive++) {
            ode_c0_change_dir(drive);
            if (read_resume(resume_lines, &n) == 0)
                break;
            ode_c0_change_dir(g_toc_up);
            n = 0;
        }
        if (n == 0) {
            g_dir.drive = 0;
            return dir_load(progress, 0);
        }
        g_dir.drive = drive;
    } else if (read_resume(resume_lines, &n) < 0) {
        return dir_load(progress, 0);
    }
    depth = 0;
    for (k = 0; resume_lines[2][k]; k++)
        depth = depth * 10 + (resume_lines[2][k] - '0');
    if (depth < 0 || depth > DIR_MAX_DEPTH || n < depth + 4)
        depth = 0;
    g_dir.depth = 0;
    if (dir_load(progress, 0) < 0)
        return -1;
    for (k = 0; k < depth; k++) {
        i = dir_find(resume_lines[3 + k]);
        if (i < 0 || !(g_dir.ent[i].flags & TF_DIR))
            return 0;
        if (dir_enter(i, progress) < 0)
            return -1;
    }
    strcpy(selected, resume_lines[3 + depth]);
    return 0;
}

/* ------------------------------------------------------------------ ODE TEST (TOOLS)
 * For clone ODEs that do not behave like Fixel's: the raw answers to the commands the menu relies on, from the
 * folder open in GAMES, for a photo. Down and up again into a folder, into a game folder, a queue entry that is
 * cleared again (no launch), and single-entry listings. Every step is undone, if the ODE behaves. */

/* one D1 page from index 0 here: "<n> REC 1:#<toc> F<flags> <name>" (the first real entry); *cnt, *ftoc, fname */
static void t_line(char lines[][40], int32 *k, int32 max, char *l)
{
    l[38] = 0;
    if (*k < max)
        strcpy(lines[(*k)++], l);
}

static void t_list(char *out, int32 *cnt, uint32 *ftoc, char *fname)
{
    uint32 flags, toc;
    const char *nm;
    char t[64];
    int32 len, pos = 0, sz, n = 0, err, got1 = 0;

    *cnt = 0;
    *ftoc = 0;
    fname[0] = 0;
    err = ode_d1_list(0, 14, d1buf);
    if (err < 0) {
        sprintf(out, "D1 ERR %d", (int)err);
        return;
    }
    for (;;) {
        sz = d1_record(d1buf, pos, &flags, &toc, &nm, &len);
        if (sz <= 0)
            break;
        if (!got1 && !TOC_IS_UP(toc)) {
            got1 = 1;
            *ftoc = toc;
            if (len > 8) len = 8;
            memcpy(fname, nm, len);
            fname[len] = 0;
            sprintf(out, "%d REC 1ST #%u F%02X %s", 0, (unsigned)toc, (unsigned)(flags & 0xff), fname);
        }
        pos += sz;
        n++;
        if (n >= 14)
            break;
    }
    *cnt = n;
    if (!got1)
        sprintf(out, "%d REC, NONE BUT ..", (int)n);
    else {
        strcpy(t, strchr(out, ' '));
        sprintf(out, "%d%s", (int)n, t);
    }
}

int32 dir_ode_test(char lines[][40], int32 max)
{
    char l[128], lb[64], n0[16], n1[16], n2[16];
    int32 k = 0, i, f = -1, g = -1, c0, c1, c2, r;
    uint32 t0, t1, t2, flags, toc;
    uint8 cmd[8], reply[8];

    sprintf(l, "DEPTH %d  2 DRIVES %d  LIST %d", (int)g_dir.depth, (int)g_dir.both_drives, (int)g_dir.devlist); t_line(lines, &k, max, l);
    sprintf(l, "ENTRIES %d  HIDDEN %d  D1 OK %d", (int)g_dir.count, (int)g_dir.hidden, (int)g_probe_ok); t_line(lines, &k, max, l);
    sprintf(l, "BOOT.ISO OPEN %d  LISTED %d", (int)g_boot_open, (int)dir_has_menu_disc()); t_line(lines, &k, max, l);
    if (g_dir.devlist) {
        sprintf(l, "OPEN A DRIVE FIRST, THEN RUN AGAIN"); t_line(lines, &k, max, l);
        return k;
    }
    t_list(lb, &c0, &t0, n0);
    sprintf(l, "HERE: %s", lb); t_line(lines, &k, max, l);
    for (i = 0; i < g_dir.count; i++)
        if ((g_dir.ent[i].flags & TF_DIR) && g_dir.ent[i].gtype != GT_GAME) {
            f = i;
            break;
        }
    for (i = 0; i < g_dir.count; i++)
        if (dir_is_game_folder(i)) {
            g = i;
            break;
        }
    if (f >= 0) {
        r = ode_c0_change_dir(g_dir.ent[f].toc_id);
        t_list(lb, &c1, &t1, n1);
        sprintf(l, "C0 #%u %.8s: %d", (unsigned)g_dir.ent[f].toc_id, dir_name(f), (int)r); t_line(lines, &k, max, l);
        sprintf(l, " IN: %s", lb); t_line(lines, &k, max, l);
        r = ode_c0_change_dir(g_toc_up);
        t_list(lb, &c2, &t2, n2);
        sprintf(l, "UP %d: %s %s", (int)r, (c2 == c0 && t2 == t0 && strcmp(n2, n0) == 0) ? "SAME" : "DIFF", lb); t_line(lines, &k, max, l);
    } else {
        sprintf(l, "NO PLAIN FOLDER HERE TO TRY"); t_line(lines, &k, max, l);
    }
    if (g >= 0) {
        r = ode_c0_change_dir(g_dir.ent[g].toc_id);
        t_list(lb, &c1, &t1, n1);
        sprintf(l, "GAME C0 #%u %.8s: %d", (unsigned)g_dir.ent[g].toc_id, dir_name(g), (int)r); t_line(lines, &k, max, l);
        sprintf(l, " IN: %s", lb); t_line(lines, &k, max, l);
        ode_c3_queue_clear();
        memset(cmd, 0, sizeof(cmd));
        memset(reply, 0, sizeof(reply));
        cmd[0] = 0xc4;
        toc = g_dir.ent[g].inner_toc;
        cmd[1] = (uint8)(toc >> 24); cmd[2] = (uint8)(toc >> 16); cmd[3] = (uint8)(toc >> 8); cmd[4] = (uint8)toc;
        r = ode_cmd(cmd, reply, 4);
        ode_c3_queue_clear();
        sprintf(l, "C4 #%u: %d  %02X %02X %02X %02X", (unsigned)toc, (int)r, reply[0], reply[1], reply[2], reply[3]); t_line(lines, &k, max, l);
        r = ode_c0_change_dir(g_toc_up);
        t_list(lb, &c2, &t2, n2);
        sprintf(l, "UP %d: %s %s", (int)r, (c2 == c0 && t2 == t0 && strcmp(n2, n0) == 0) ? "SAME" : "DIFF", lb); t_line(lines, &k, max, l);
    } else {
        for (i = 0; i < g_dir.count && !dir_is_game(i); i++)
            ;
        if (i < g_dir.count) {
            ode_c3_queue_clear();
            memset(cmd, 0, sizeof(cmd));
            memset(reply, 0, sizeof(reply));
            cmd[0] = 0xc4;
            toc = g_dir.ent[i].toc_id;
            cmd[1] = (uint8)(toc >> 24); cmd[2] = (uint8)(toc >> 16); cmd[3] = (uint8)(toc >> 8); cmd[4] = (uint8)toc;
            r = ode_cmd(cmd, reply, 4);
            ode_c3_queue_clear();
            sprintf(l, "C4 #%u %.8s: %d %02X %02X %02X %02X", (unsigned)toc, dir_name(i), (int)r, reply[0], reply[1],
                  reply[2], reply[3]); t_line(lines, &k, max, l);
        } else {
            sprintf(l, "NO GAME HERE TO TRY"); t_line(lines, &k, max, l);
        }
    }
    {
        static const int32 counts[3] = { 1, 2, 14 };
        char part[3][16], nm[DIR_NAME_MAX];
        for (i = 0; i < 3; i++) {
            nm[0] = 0;
            flags = toc = 0;
            r = g_dir.count ? probe_n(g_dir.ent[0].toc_id, counts[i], &flags, &toc, nm) : -9;
            sprintf(part[i], "%d:%d#%u", (int)counts[i], (int)r, (unsigned)toc);
        }
        sprintf(l, "ONE #%u %s %s %s", g_dir.count ? (unsigned)g_dir.ent[0].toc_id : 0u, part[0], part[1], part[2]); t_line(lines, &k, max, l);
    }
    t_list(lb, &c2, &t2, n2);
    sprintf(l, "END: %s %s", (c2 == c0 && t2 == t0 && strcmp(n2, n0) == 0) ? "SAME" : "DIFF", lb); t_line(lines, &k, max, l);
    return k;
}

