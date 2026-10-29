/* lists.c - favorites, recently played and the settings, kept as small text files in the drive root.
 *
 *   _odemenu.fav   favorites, one path per line from the drive root ("3DO/USA/Doom (USA).iso")
 *   _odemenu.rec   recently played, newest first
 *   _odemenu.opt   settings, "key=value" lines
 *
 * The firmware opens files relative to its current folder, so the menu takes it up to the root for these
 * (dir_root_begin) and back down after. On the device list (both drives fitted) there is no drive to write to:
 * changes wait until the menu is inside a drive again.
 */
#include "common.h"

#define FAV_FILE "_odemenu.fav"
#define REC_FILE "_odemenu.rec"
#define OPT_FILE "_odemenu.opt"
#define FILE_MAX (FAV_MAX * LIST_PATH_MAX + 2 * ODE_XFER)

PathList g_favs, g_recent;
Settings g_set;

static char fav_store[FAV_MAX][LIST_PATH_MAX];
static char rec_store[REC_MAX][LIST_PATH_MAX];
static char fbuf[FILE_MAX + 1];

void lists_init(void)
{
    g_favs.p = fav_store;
    g_favs.max = FAV_MAX;
    g_favs.file = FAV_FILE;
    g_recent.p = rec_store;
    g_recent.max = REC_MAX;
    g_recent.file = REC_FILE;
    g_favs.n = g_recent.n = 0;
    g_set.pane = 1;
    g_set.bar = 1;
    g_set.shine = 0;
    g_set.index = 1;
    g_set.remember = 1;
    g_set.scanned = 0;
}

const char *path_base(const char *path)
{
    const char *b = path, *p;

    for (p = path; *p; p++)
        if (*p == '/')
            b = p + 1;
    return b;
}

int32 list_split(const char *path, char comps[][DIR_NAME_MAX], int32 maxc)
{
    int32 n = 0, k = 0;
    const char *p;

    for (p = path;; p++) {
        if (*p == '/' || *p == 0) {
            if (k > 0) {
                if (n >= maxc)
                    return -1;
                comps[n][k] = 0;
                n++;
            }
            k = 0;
            if (*p == 0)
                break;
        } else if (n < maxc && k < DIR_NAME_MAX - 1) {
            comps[n][k++] = *p;
        }
    }
    return n;
}

int32 list_path_of(int32 i, char *out, int32 max)
{
    int32 d;

    out[0] = 0;
    for (d = 0; d < g_dir.depth; d++) {
        if ((int32)(strlen(out) + strlen(g_dir.path[d]) + 2) >= max)
            return -1;
        strcat(out, g_dir.path[d]);
        strcat(out, "/");
    }
    if (i < 0)
        return 0;
    if ((int32)(strlen(out) + strlen(dir_name(i)) + 1) >= max)
        return -1;
    strcat(out, dir_name(i));
    return 0;
}

int32 list_find(PathList *l, const char *path)
{
    int32 i;

    for (i = 0; i < l->n; i++)
        if (strcmp(l->p[i], path) == 0)
            return i;
    return -1;
}

void list_remove(PathList *l, int32 i)
{
    if (i < 0 || i >= l->n)
        return;
    memmove(l->p[i], l->p[i + 1], (l->n - i - 1) * LIST_PATH_MAX);
    l->n--;
    l->dirty = 1;
}

void list_add_front(PathList *l, const char *path)
{
    int32 i = list_find(l, path);

    if ((int32)strlen(path) >= LIST_PATH_MAX)
        return;
    if (i >= 0)
        list_remove(l, i);
    if (l->n >= l->max)
        l->n = l->max - 1;
    memmove(l->p[1], l->p[0], l->n * LIST_PATH_MAX);
    strcpy(l->p[0], path);
    l->n++;
    l->dirty = 1;
}

/* ------------------------------------------------------------------ files */

static int32 read_file(const char *name, char *buf, int32 max)
{
    uint32 got;
    int32 n = 0;

    if (ode_file_open(name, 0) < 0)
        return -1;
    while (n + ODE_XFER <= max) {
        if (ode_file_read((uint8 *)buf + n, ODE_XFER, &got) < 0 || got == 0)
            break;
        if (got == ODE_XFER && n >= ODE_XFER && memcmp(buf + n, buf + n - ODE_XFER, ODE_XFER) == 0)
            break;                      /* a clone's end of file: the last chunk again (see art_read_all) */
        n += got;
        if (got < ODE_XFER)
            break;
    }
    ode_file_close();
    buf[n] = 0;
    return n;
}

static int32 write_file(const char *name, const char *buf, int32 len)
{
    int32 off, n, err = 0;

    if (ode_file_open(name, 1) < 0)
        return -1;
    for (off = 0; off < len && err == 0; off += n) {
        n = len - off > ODE_XFER ? ODE_XFER : len - off;
        err = ode_file_write((const uint8 *)buf + off, n);
    }
    if (ode_file_close() < 0 && err == 0)
        err = -1;
    return err;
}

/* the last line of a list file: a clone was seen to answer reads past the end of a file with old data (an
 * earlier version of the same list among it), so the end is written into the file itself */
#define LIST_END "-END-"

static void parse_list(PathList *l, char *buf)
{
    char *p = buf, *e;
    int32 k;

    l->n = 0;
    l->dirty = 0;
    if (strncmp(p, "OML1\n", 5) != 0)
        return;
    p += 5;
    while (*p && l->n < l->max) {
        for (e = p; *e && *e != '\n' && *e != '\r'; e++)
            ;
        if (strncmp(p, "OM", 2) == 0 || (e - p == (int32)strlen(LIST_END) && strncmp(p, LIST_END, e - p) == 0))
            break;                      /* its end mark, or the file again (a clone's end of file) */
        if (e > p && e - p < LIST_PATH_MAX) {
            memcpy(l->p[l->n], p, e - p);
            l->p[l->n][e - p] = 0;
            for (k = 0; k < l->n && strcmp(l->p[k], l->p[l->n]) != 0; k++)
                ;
            if (k == l->n)
                l->n++;                 /* each path once */
        }
        while (*e == '\n' || *e == '\r')
            e++;
        p = e;
    }
}

static int32 opt_value(const char *buf, const char *key, int32 def, int32 maxv)
{
    const char *p = buf;
    int32 n = strlen(key), v;

    while (*p) {
        if (strncmp(p, key, n) == 0 && p[n] == '=') {
            v = p[n + 1] - '0';
            return (v >= 0 && v <= maxv) ? v : def;
        }
        while (*p && *p != '\n')
            p++;
        while (*p == '\n' || *p == '\r')
            p++;
    }
    return def;
}

void lists_load(void)
{
    if (dir_root_begin() < 0)
        return;
    if (read_file(FAV_FILE, fbuf, FILE_MAX) > 0)
        parse_list(&g_favs, fbuf);
    else
        g_favs.n = 0;
    if (read_file(REC_FILE, fbuf, FILE_MAX) > 0)
        parse_list(&g_recent, fbuf);
    else
        g_recent.n = 0;
    if (read_file(OPT_FILE, fbuf, FILE_MAX) > 0 && strncmp(fbuf, "OMO1\n", 5) == 0) {
        g_set.pane = opt_value(fbuf, "pane", 1, 1);
        g_set.bar = opt_value(fbuf, "bar", 1, 3);
        g_set.shine = opt_value(fbuf, "shine", 0, 1);
        g_set.index = opt_value(fbuf, "index", 1, 1);
        g_set.remember = opt_value(fbuf, "remember", 1, 1);
        g_set.scanned = opt_value(fbuf, "scanned", 0, 1);
    }
    dir_root_end();
    g_dir.use_cache = g_set.index;
    g_favs.dirty = g_recent.dirty = g_set.dirty = 0;
}

static int32 save_list(PathList *l)
{
    int32 i, n = 5;

    strcpy(fbuf, "OML1\n");
    for (i = 0; i < l->n; i++) {
        strcpy(fbuf + n, l->p[i]);
        n += strlen(l->p[i]);
        fbuf[n++] = '\n';
    }
    strcpy(fbuf + n, LIST_END "\n");   /* where the list ends, whatever a read returns after it */
    n += strlen(LIST_END) + 1;
    return write_file(l->file, fbuf, n);
}

void lists_save(int32 at_root)
{
    if (!g_favs.dirty && !g_recent.dirty && !g_set.dirty)
        return;
    if (!at_root && dir_root_begin() < 0)
        return;                 /* device list: keep the changes for later */
    if (g_favs.dirty && save_list(&g_favs) == 0)
        g_favs.dirty = 0;
    if (g_recent.dirty && save_list(&g_recent) == 0)
        g_recent.dirty = 0;
    if (g_set.dirty) {
        sprintf(fbuf, "OMO1\npane=%d\nbar=%d\nshine=%d\nindex=%d\nremember=%d\nscanned=%d\n", (int)g_set.pane,
                (int)g_set.bar, (int)g_set.shine, (int)g_set.index, (int)g_set.remember, (int)g_set.scanned);
        if (write_file(OPT_FILE, fbuf, strlen(fbuf)) == 0)
            g_set.dirty = 0;
    }
    if (!at_root)
        dir_root_end();
}

/* ------------------------------------------------------------------ the last launch, in the console's NVRAM
 * Writing to the card just before a launch made games fail to start (the card is still busy when the ODE starts
 * reading the game), so a launch writes only a 256-byte note to NVRAM: the drive and the game's path. The next
 * menu start adds it to RECENT and goes back to it. */

#define NOTE_FILE "/NVRAM/ODEMenu"
#define NOTE_SIZE 256

int32 note_save(int32 drive, const char *path)
{
    static char note[NOTE_SIZE];

    if ((int32)strlen(path) > NOTE_SIZE - 16)
        return -1;
    memset(note, 0, NOTE_SIZE);
    sprintf(note, "ONT1\n%d\n%s\n", (int)drive, path);
    return SaveFile(NOTE_FILE, note, NOTE_SIZE, 0);
}

/* the note, once taken into RECENT: emptied, so a game removed from RECENT does not come back at every start */
int32 note_clear(void)
{
    static char note[NOTE_SIZE];

    memset(note, 0, NOTE_SIZE);
    strcpy(note, "ONT1\n0\n\n");
    return SaveFile(NOTE_FILE, note, NOTE_SIZE, 0);
}

int32 note_load(int32 *drive, char *path, int32 max)
{
    int32 size = 0, n;
    char *p, *e;
    char *buf = (char *)LoadFile(NOTE_FILE, &size, MEMTYPE_ANY);

    path[0] = 0;
    if (buf == 0)
        return -1;
    n = -1;
    if (size >= 8 && strncmp(buf, "ONT1\n", 5) == 0) {
        buf[size - 1] = 0;
        p = buf + 5;
        *drive = *p - '0';
        p = strchr(p, '\n');
        if (p) {
            p++;
            e = strchr(p, '\n');
            if (e && e - p > 0 && e - p < max) {
                memcpy(path, p, e - p);
                path[e - p] = 0;
                n = 0;
            }
        }
    }
    UnloadFile(buf);
    return n;
}

/* ------------------------------------------------------------------ launch trace (LAUNCH_TRACE builds only) */
#define TRACE_FILE "/NVRAM/ODETrace"

#ifdef LAUNCH_TRACE
uint8 g_trace[TRACE_N][12];
int32 g_trace_n;

/* every ODE command and the start of its answer (odeio.c), the last TRACE_N kept */
void ode_trace(const uint8 *cmd, const uint8 *reply, int32 replylen, int32 err)
{
    uint8 *t = g_trace[g_trace_n % TRACE_N];
    int32 k;

    if (cmd[0] == 0xe3 || cmd[0] == 0xe7)
        return;                     /* file reads (covers, lists): many, and not what a launch sends */
    memcpy(t, cmd, 7);
    for (k = 0; k < 4; k++)
        t[7 + k] = k < replylen ? reply[k] : 0;
    t[11] = (uint8)(err < 0 ? 0xee : 0);
    g_trace_n++;
}
#endif

int32 trace_save(void)
{
#ifdef LAUNCH_TRACE
    static uint8 buf[8 + TRACE_N * 12];
    int32 n = g_trace_n < TRACE_N ? g_trace_n : TRACE_N, k, first = g_trace_n - n;

    memcpy(buf, "OTR1", 4);
    buf[4] = (uint8)n;
    buf[5] = buf[6] = buf[7] = 0;
    for (k = 0; k < n; k++)
        memcpy(buf + 8 + k * 12, g_trace[(first + k) % TRACE_N], 12);
    return SaveFile(TRACE_FILE, buf, sizeof(buf), 0);
#else
    return 0;
#endif
}

int32 trace_lines(char lines[][40], int32 max)
{
#ifdef LAUNCH_TRACE
    int32 size = 0, n, k;
    uint8 *b = (uint8 *)LoadFile(TRACE_FILE, &size, MEMTYPE_ANY), *t;

    if (b == 0)
        return 0;
    n = 0;
    if (size >= 8 && memcmp(b, "OTR1", 4) == 0) {
        n = b[4];
        if (n > max)
            n = max;
        if (8 + n * 12 > size)
            n = 0;
        for (k = 0; k < n; k++) {
            t = b + 8 + (b[4] - n + k) * 12;
            sprintf(lines[k], "%02X %02X%02X%02X%02X %02X%02X >%02X%02X%02X%02X%s", t[0], t[1], t[2], t[3], t[4], t[5],
                    t[6], t[7], t[8], t[9], t[10], t[11] ? " ERR" : "");
        }
    }
    UnloadFile(b);
    return n;
#else
    (void)lines;
    (void)max;
    return 0;
#endif
}

/* LAUNCH_TRACE builds: at the start after a launch, that launch's trace goes into _odemenu.log in the drive root
 * (newest last, the oldest dropped when it grows), so a tester only has to send one file */
#define LOG_FILE "_odemenu.log"

int32 trace_log(const char *game)
{
#ifdef LAUNCH_TRACE
    static char lines[TRACE_N][40];
    static uint8 empty[8 + TRACE_N * 12];
    int32 n = trace_lines(lines, TRACE_N), k, len;
    char *e, *cut;

    if (n <= 0)
        return 0;
    len = read_file(LOG_FILE, fbuf, FILE_MAX);
    if (len < 0 || strncmp(fbuf, "ODE LOG\n", 8) != 0) {
        strcpy(fbuf, "ODE LOG\n");
        len = 8;
    }
    fbuf[len] = 0;
    e = strstr(fbuf, LIST_END "\n");
    if (e)
        len = e - fbuf;                 /* (what a clone returns after it is old data) */
    while (len > FILE_MAX - 2048 && (cut = strstr(fbuf + 9, "\n== ")) != 0) {
        memmove(fbuf + 8, cut + 1, len - (cut + 1 - fbuf) + 1);
        len -= cut + 1 - (fbuf + 8);
    }
    len += sprintf(fbuf + len, "== %.200s (MENU %s)\n%s\n", game[0] ? game : "(QUEUE)", MENU_VERSION, g_probe_info);
    for (k = 0; k < n; k++)
        len += sprintf(fbuf + len, "%s\n", lines[k]);
    len += sprintf(fbuf + len, "%s\n", LIST_END);
    write_file(LOG_FILE, fbuf, len);
    memcpy(empty, "OTR1", 4);           /* logged: the trace is emptied */
    return SaveFile("/NVRAM/ODETrace", empty, sizeof(empty), 0);
#else
    (void)game;
    return 0;
#endif
}
