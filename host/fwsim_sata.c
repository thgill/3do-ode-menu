/* fwsim_sata.c - FWSIM_CLONE=7: a model of the clone ODE firmware based on FCare's SataTo3DO
 * (https://github.com/FCare/SataTo3DO, MIT), the firmware the clones seen so far behave like. Written from reading
 * that source (src/interfaces/3DO.c, MSC.c); no code is copied. Test harness only.
 *
 * What it models (each one seen on a tester's clone, or read in the source):
 *  - boot: the firmware is at the drive root ("level 0"), current TOC 0. C0 0 there only sets the drive byte
 *    (current TOC = drive << 24); without it every id it hands out has drive byte 0, and a game queued with drive 0
 *    never starts.
 *  - listing: only folders and *.cue / *.iso files (no '.' names, nothing 128 characters or longer), in directory
 *    order. Ids are drive << 24 | position. Inside a folder (level >= 1) D1 index 0 answers ".." (drive << 24 |
 *    0xffffff) first; D1 index n >= 1 skips n entries, so it answers from entry n + 1. At the root there is no "..",
 *    so index n answers from entry n + 1 too. D1 answers exactly count entries (".." included).
 *  - C0 id: ignored if id is the current TOC; "up" cuts the path at the last '\' (nothing at the root); otherwise it
 *    walks id entries from the start of the folder and goes in if that one is a folder (a file id only becomes the
 *    current TOC).
 *  - C4 id: looked up in the last D1 answer (not on the card): only a file found there is queued, with the path of
 *    the folder the firmware is in at that moment.
 *  - C5: the first queued game starts if its drive byte is the drive and the file is a .cue/.iso that exists.
 *  - E1 name: the file is opened read/write and created if missing ("open always"), relative to the current folder;
 *    it fails if a folder on the way does not exist, or if the file is the running disc (boot.iso is open).
 *    Opening by id is not supported (fails).
 *  - E3: answers the length asked for whenever the read did not fail, even past the end (the buffer keeps old data).
 *  - E4: writes at the current position and never shortens the file.
 */
#define _DEFAULT_SOURCE
#include "h3do.h"
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

extern long fw_readdir_steps, fw_cmds, fw_d1, fw_e6, fw_bytes_written, fw_d1_overflow, fw_dotdot;
extern char fw_launched[2048];

static char   root[1024];
static char   path[2048];            /* the firmware's current folder */
static int32  level;                 /* 0 = drive root */
static uint32 current_toc;
static const uint32 DRIVE = 1;
static uint8  tocbuf[0x800];         /* the last D1 answer */
static uint8  filebuf[0x800];
static int    fd = -1;
static char   queue[16][2048];
static uint32 queue_id[16];
static int32  qn;
long fw_sata_created;                /* files that did not exist and were made by an open */

void fwsim_sata_setup(const char *sd)
{
    strcpy(root, sd);
    strcpy(path, sd);
    level = 0;
    current_toc = 0;
}

void fwsim_sata_start_in(const char *rel)
{
    const char *p;

    strcat(path, "/");
    strcat(path, rel);
    for (p = rel, level = 1; *p; p++)
        if (*p == '/')
            level++;
    current_toc = (DRIVE << 24) | 1;    /* left in a folder: the console was reset, the clone kept its state */
}

static int32 ends_with(const char *s, const char *ext)
{
    int32 n = strlen(s), m = strlen(ext);
    return n > m && !strcasecmp(s + n - m, ext);
}

static int32 is_dir_at(const char *dir, const char *name)
{
    char p[4096];
    struct stat st;
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* the n-th (1-based) valid entry of the current folder */
static int32 nth(int32 n, char *out, int32 *dirp)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int32 i = 0;

    if (!d)
        return 0;
    while ((e = readdir(d))) {
        int32 dp;
        fw_readdir_steps++;
        if (e->d_name[0] == '.' || strlen(e->d_name) >= 128)
            continue;
        dp = is_dir_at(path, e->d_name);
        if (!dp && !ends_with(e->d_name, ".cue") && !ends_with(e->d_name, ".iso"))
            continue;
        if (++i == n) {
            strcpy(out, e->d_name);
            *dirp = dp;
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

static void put32(uint8 *p, uint32 v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static uint32 get32(const uint8 *p)
{
    return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3];
}

static void d1(uint32 index, int32 nb, uint8 *data)
{
    int32 pos = 0, id = 0, n, dp;
    uint32 k, drv = current_toc >> 24;
    char name[256];

    fw_d1++;
    memset(tocbuf, 0xff, sizeof(tocbuf));
    for (k = 1;; k++) {
        if (index == 0 && id == 0 && level != 0) {
            strcpy(name, "..");
            dp = 1;
            k--;                        /* ".." is not an entry of the folder */
        } else if (!nth(index + k, name, &dp)) {
            break;
        }
        n = strlen(name) + 1;
        if (pos + 12 + n > (int32)sizeof(tocbuf))
            break;
        put32(tocbuf + pos, dp ? 2 : 1);
        put32(tocbuf + pos + 4, (drv << 24) | (!strcmp(name, "..") ? 0xffffff : (index + k)));
        put32(tocbuf + pos + 8, n);
        memcpy(tocbuf + pos + 12, name, n);
        pos += 12 + n;
        id++;
        if (nb != -1 && id >= nb)
            break;
        if (pos > (int32)sizeof(tocbuf) - (128 + 13))
            break;
    }
    if (data)
        memcpy(data, tocbuf, 0x800);
}

static void change_toc(uint32 index)
{
    char name[256];
    int32 dp;

    if ((index & 0xffffff) == 0xffffff) {
        char *s = strrchr(path, '/');
        if (level > 0 && s && strlen(path) > strlen(root)) {
            *s = 0;
            level--;
            current_toc &= ~0xffffffu;
        }
        return;
    }
    if (current_toc == index) {
        if (level == 0)
            current_toc = DRIVE << 24;
        return;
    }
    if ((index & 0xffffff) == 0 && level == 0) {
        level++;                        /* (only with several drives in practice: one drive starts at 0) */
        current_toc = index;
        return;
    }
    if ((index & 0xffffff) == 0) {
        host_log("sata: C0 to entry 0 inside a folder (undefined in the firmware)\n");
        current_toc = index;
        return;
    }
    if (nth(index & 0xffffff, name, &dp) && dp) {
        strcat(path, "/");
        strcat(path, name);
        level++;
    }
    current_toc = index;
}

int32 fwsim_sata_cmd(const uint8 *cmd, uint8 *reply, int32 n, uint8 *data, int32 len)
{
    char p[4096], name[2048];
    uint32 v, off;
    int32 pos;
    ssize_t got;

    fw_cmds++;
    memset(reply, 0, n);
    reply[0] = cmd[0];
    switch (cmd[0]) {
    case 0x93:
        reply[1] = 0x01;                /* boot device: microSD (the source always answers 1) */
        memcpy(reply + 2, "3DSATA", 6);
        reply[8] = 1; reply[9] = 1; reply[10] = 0;
        break;
    case 0xc0:
        change_toc(get32(cmd + 1));
        if (getenv("FWSIM_TRACE"))
            host_log("C0 %08x -> %s (level %d, toc %08x)\n", (unsigned)get32(cmd + 1), path, (int)level,
                     (unsigned)current_toc);
        break;
    case 0xc3:
        qn = 0;
        break;
    case 0xc4:
        v = get32(cmd + 1);
        for (pos = 0; pos < 0x800 - 12 && get32(tocbuf + pos) != 0xffffffff; pos += 12 + get32(tocbuf + pos + 8)) {
            if (get32(tocbuf + pos + 4) != v)
                continue;
            if (get32(tocbuf + pos) == 1 && qn < 16) {
                snprintf(queue[qn], sizeof(queue[0]), "%s/%s", path, (char *)tocbuf + pos + 12);
                queue_id[qn++] = v;
                reply[1] = reply[2] = 1;
                host_log("C4 queued: %s\n", (char *)tocbuf + pos + 12);
            }
            break;
        }
        if (!reply[1])
            host_log("sata: C4 %08x is not in the last listing: not queued\n", (unsigned)v);
        break;
    case 0xc5:
        if (qn == 0) {
            strcpy(fw_launched, "boot.iso");
        } else if ((queue_id[0] >> 24) != DRIVE) {
            strcpy(fw_launched, "(nothing: wrong drive byte)");
        } else if (access(queue[0], F_OK) != 0) {
            strcpy(fw_launched, "(nothing: file missing)");
        } else {
            strcpy(fw_launched, queue[0]);
        }
        host_log("C5 launch: %s (queue %d)\n", fw_launched, qn);
        break;
    case 0xd1:
        d1(get32(cmd + 1), (cmd[5] << 8) | cmd[6], data && len >= 0x800 ? data : 0);
        break;
    case 0xe6:
        fw_e6++;
        off = ((cmd[1] << 8) | cmd[2]) * 4;
        if (off + 4 <= 0x800)
            memcpy(filebuf + off, cmd + 3, 4);
        break;
    case 0xe1:
        reply[1] = 0;
        if (cmd[1] != 0) {
            host_log("E1 by id (not supported) -> 0\n");
            break;
        }
        v = (cmd[2] << 8) | cmd[3];
        if (v == 0 || v > 0x800)
            break;
        memcpy(name, filebuf, v);
        name[v < sizeof(name) ? v : sizeof(name) - 1] = 0;
        if (fd >= 0) {
            close(fd);              /* (the firmware reuses its one file object) */
            fd = -1;
        }
        snprintf(p, sizeof(p), "%s/%s", path, name);
        if (level == 0 && !strcasecmp(name, "boot.iso")) {
            host_log("E1 open %s -> 0 (the running disc is open)\n", name);
            break;
        }
        {
            int existed = access(p, F_OK) == 0;
            fd = open(p, O_RDWR | O_CREAT, 0644);
            if (fd >= 0 && !existed) {
                fw_sata_created++;
                host_log("sata: E1 created an empty file: %s\n", name);
            }
        }
        reply[1] = fd >= 0;
        host_log("E1 open %s %s -> %d\n", name, cmd[6] ? "write" : "read", fd >= 0);
        break;
    case 0xe3:
        v = (cmd[1] << 8) | cmd[2];
        if (v > 0x800)
            v = 0x800;
        got = fd >= 0 ? read(fd, filebuf, v) : -1;
        if (got < 0)
            v = 0;
        reply[1] = v >> 8;              /* the length asked for: the rest of the buffer is old data */
        reply[2] = v;
        break;
    case 0xe4:
        v = (cmd[1] << 8) | cmd[2];
        if (v > 0x800)
            v = 0x800;
        got = fd >= 0 ? write(fd, filebuf, v) : -1;
        if (got < 0)
            v = 0;
        fw_bytes_written += got > 0 ? got : 0;
        reply[1] = v >> 8;
        reply[2] = v;
        break;
    case 0xe5:
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
        break;
    case 0xe7:
        if (data && len >= 0x800)
            memcpy(data, filebuf, 0x800);
        break;
    default:
        break;
    }
    return 0;
}
