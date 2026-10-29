/* fwsim.c - host simulation of the ODE firmware's menu commands, following the firmware reconstruction
 * (fixel-3do-fz1-ode-decomp: source/src/ode.c, cdrom.c): same filter, same dir_get_nth_entry walk from the
 * start of the folder for every entry, same D1 packing, same file commands. Counts directory reads and
 * commands so access patterns can be compared. Test harness only.
 */
#define _DEFAULT_SOURCE
#include "h3do.h"
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static char   roots[2][1024];
static int32  boot_device = 1;      /* 1 sd, 2 usb, 3 both */
int32 fw_boot_source = 1;           /* EXT_ID high nibble: 1 sd, 2 usb, 4 flash */
static int32  toc_dir = 0;          /* -1 = device list */
static int32  toc_level = 0;
static int32  drive = 1;
static char   cur[2048];
static char   queue[5][2048];
static int32  qn;
static FILE  *fops;
static uint8  fbuf[0x800];
static uint8  tocbuf[0x800 + 512];

long fw_readdir_steps, fw_cmds, fw_d1, fw_e6, fw_bytes_written, fw_d1_overflow, fw_dotdot;
char fw_launched[2048];

void fwsim_setup(const char *sd, const char *usb)
{
    strcpy(roots[0], sd ? sd : "");
    strcpy(roots[1], usb ? usb : "");
    boot_device = (sd ? 1 : 0) | (usb ? 2 : 0);
    drive = sd ? 1 : 2;
    strcpy(cur, roots[drive - 1]);
    toc_dir = (boot_device == 3) ? -1 : 0;
    toc_level = 0;
}

static int32 path_valid(void);

static int32 ext_ok(const char *e)
{
    return !strcasecmp(e, "iso") || !strcasecmp(e, "cue") || !strcasecmp(e, "nvm") || !strcasecmp(e, "ode");
}

static int32 is_dir(const char *dir, const char *name)
{
    char p[4096];
    struct stat st;
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* 1 = skip (ode_dir_list_filter) */
static int32 filtered(const char *name, int32 dirp)
{
    int32 n = strlen(name);
    if (name[0] == '.' || name[0] == '_')
        return 1;
    if (dirp && !strcasecmp(name, "System Volume Information"))
        return 1;
    if (dirp)
        return 0;
    if (n >= 4 && name[n - 4] == '.')
        return !ext_ok(name + n - 3);
    return 0;
}

/* n >= 1: the n-th visible entry of the current folder, walking from the start like the firmware */
static int32 nth_entry(int32 n, char *out, int32 *dirp)
{
    DIR *d;
    struct dirent *e;
    int32 i = 1;

    if (n <= 0 || !path_valid() || !(d = opendir(cur)))
        return 0;
    while ((e = readdir(d))) {
        int32 dp;
        fw_readdir_steps++;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        dp = is_dir(cur, e->d_name);
        if (!filtered(e->d_name, dp)) {
            if (i == n) {
                strncpy(out, e->d_name, 255);
                out[255] = 0;
                *dirp = dp;
                closedir(d);
                return 1;
            }
            i++;
        }
    }
    fw_readdir_steps++;     /* the final empty read */
    closedir(d);
    return 0;
}

typedef struct { uint32 flags, toc; char name[256]; } TE;

static int32 read_dir(uint32 idx, TE *te)
{
    int32 dp;
    te->flags = 0xffffffff;
    te->toc = 0xffffffff;
    if (toc_dir == -1 && boot_device == 3) {
        if (idx == 0) { te->flags = 2; te->toc = 1; strcpy(te->name, "/sd"); }
        else if (idx == 1) { te->flags = 2; te->toc = 2; strcpy(te->name, "/usb"); }
        return 1;
    }
    if (idx == 0) {
        te->flags = 2;
        te->toc = 0xffffffff;
        strcpy(te->name, "..");
        return 1;
    }
    if (!nth_entry(idx, te->name, &dp))
        return -1;
    te->flags = dp ? 2 : 1;
    if (!dp) {
        int32 n = strlen(te->name);
        if (n >= 4 && te->name[n - 4] == '.') {
            if (!strcasecmp(te->name + n - 3, "nvm")) te->flags |= 8;
            if (!strcasecmp(te->name + n - 3, "ode")) te->flags |= 0x10;
        }
    }
    te->toc = idx;
    return 1;
}

/* like the firmware's ode_cd(".."): cut at the last '/', no clamp at the drive root. Below the root the path is
 * not a valid volume any more ("" or "/x"), so later opens fail, as with FatFs FF_STR_VOLUME_ID 2. */
static int32 path_valid(void)
{
    return strncmp(cur, roots[drive - 1], strlen(roots[drive - 1])) == 0;
}

static void cd_up(void)
{
    char *s = strrchr(cur, '/');
    if (s)
        *s = 0;
    if (!path_valid())
        host_log("firmware path is now invalid (went above the drive root)\n");
}

void fwsim_start_in(const char *rel)
{
    char *p, buf[1024];
    strcat(cur, "/");
    strcat(cur, rel);
    strcpy(buf, rel);
    for (p = buf, toc_level = 1; *p; p++)
        if (*p == '/')
            toc_level++;
    toc_dir = 1;
}

/* FWSIM_CLONE=4: a clone seen on hardware: ids carry the drive in the top byte, ".." is 0x01ffffff, and C0
 * ignores ids without it (0xffffffff included) */
static int32 clone4(void)
{
    return getenv("FWSIM_CLONE") && atoi(getenv("FWSIM_CLONE")) == 4;
}

static void change_dir(uint32 id)
{
    char name[256];
    int32 dp;

    if (id == 0 && boot_device == 3) { toc_dir = -1; return; }
    if (toc_dir == -1) {
        if (id == 1 || id == 2) {
            toc_dir = 0;
            drive = id;
            strcpy(cur, roots[id - 1]);
            toc_level = 1;
            return;
        }
    }
    if (id == 0xffffffff) {
        cd_up();
        toc_dir = 0;
        if (toc_level == 1) {
            if (boot_device == 3) toc_dir = -1;
            toc_level = 0;
        } else if (toc_level < 1) {
            toc_level = 0;
            if (boot_device == 3) toc_dir = -1;
        } else {
            toc_level--;
        }
        return;
    }
    if (nth_entry(id, name, &dp) && dp) {
        toc_level++;
        toc_dir = id;
        strcat(cur, "/");
        strcat(cur, name);
    }
}

static void d1(const uint8 *cmd, uint8 *data)
{
    uint32 idx = ((uint32)cmd[1] << 24) | (cmd[2] << 16) | (cmd[3] << 8) | cmd[4];
    int32 count = (cmd[5] << 8) | cmd[6];
    uint32 pos = 0, j, nl;
    TE te;

    fw_d1++;
    memset(tocbuf, 0xff, sizeof(tocbuf));
    /* FWSIM_CLONE=1: a firmware that answers a 1-entry D1 with nothing; =2: one that answers it one entry late */
    if (count == 1 && getenv("FWSIM_CLONE") && atoi(getenv("FWSIM_CLONE")) == 1) {
        memcpy(data, tocbuf, 0x800);
        return;
    }
    if (count < 14 && getenv("FWSIM_CLONE") && atoi(getenv("FWSIM_CLONE")) == 2)
        idx++;
    /* =3: no single-entry answer is right, whatever the count (a listing from 0, 14, 28... still is) */
    if (getenv("FWSIM_CLONE") && atoi(getenv("FWSIM_CLONE")) == 3 && (count < 14 || idx % 14 != 0))
        idx++;
    do {
        if (read_dir(idx, &te) <= 0)
            break;
        tocbuf[pos] = te.flags >> 24; tocbuf[pos + 1] = te.flags >> 16; tocbuf[pos + 2] = te.flags >> 8; tocbuf[pos + 3] = te.flags;
        if (clone4())
            te.toc = te.toc == 0xffffffff ? 0x01ffffff : (0x01000000 | te.toc);
        tocbuf[pos + 4] = te.toc >> 24; tocbuf[pos + 5] = te.toc >> 16; tocbuf[pos + 6] = te.toc >> 8; tocbuf[pos + 7] = te.toc;
        if (te.flags == 0xffffffff)
            break;
        te.name[127] = 0;
        nl = strlen(te.name) + 1;
        tocbuf[pos + 8] = nl >> 24; tocbuf[pos + 9] = nl >> 16; tocbuf[pos + 10] = nl >> 8; tocbuf[pos + 11] = nl;
        pos += 12;
        for (j = 0; j < nl; j++)
            tocbuf[pos++] = te.name[j];
        idx++;
    } while (--count > 0 && pos < 0x800);
    if (pos > 0x800)
        fw_d1_overflow++;
    memcpy(data, tocbuf, 0x800);
}

int32 host_fw_cmd(const uint8 *cmd, uint8 *reply, int32 n, uint8 *data, int32 len)
{
    char path[4096], name[256];
    uint32 v;
    int32 dp;
    TE te;

    fw_cmds++;
    memset(reply, 0, n);
    reply[0] = cmd[0];
    switch (cmd[0]) {
    case 0x93:
        reply[1] = boot_device | (fw_boot_source << 4);
        memcpy(reply + 2, "FZ1ODE", 6);
        reply[8] = 1;
        reply[9] = 0;
        reply[10] = 7;
        break;
    case 0xc0:
        v = ((uint32)cmd[1] << 24) | (cmd[2] << 16) | (cmd[3] << 8) | cmd[4];
        if (clone4()) {             /* only its own ids: 0x01000000 + entry, 0x01ffffff for ".." */
            if (v == 0x01ffffff)
                v = 0xffffffff;
            else if ((v >> 24) == 1)
                v &= 0x00ffffff;
            else
                break;
        }
        change_dir(v);
        if (getenv("FWSIM_TRACE"))
            host_log("C0 %08x -> %s\n", (unsigned)v, cur);
        break;
    case 0xc3:
        qn = 0;
        break;
    case 0xc4:
        v = ((uint32)cmd[1] << 24) | (cmd[2] << 16) | (cmd[3] << 8) | cmd[4];
        if (clone4()) {
            if ((v >> 24) != 1)
                break;
            v &= 0x00ffffff;
        }
        if (read_dir(v, &te) > 0 && te.flags != 0xffffffff) {
            reply[1] = 1;
            if (!(te.flags & 2) && qn < 5) {
                snprintf(queue[qn++], sizeof(queue[0]), "%s/%s", cur, te.name);
                reply[2] = 1;
                host_log("C4 queued: %s\n", te.name);
            }
        }
        break;
    case 0xc5:
        strcpy(fw_launched, qn ? queue[0] : "boot.iso");
        host_log("C5 launch: %s (queue %d)\n", fw_launched, qn);
        break;
    case 0xd1:
        if (data && len >= 0x800)
            d1(cmd, data);
        break;
    case 0xe6:
        fw_e6++;
        v = ((cmd[1] << 8) | cmd[2]) * 4;
        if (v < 0x800)
            memcpy(fbuf + v, cmd + 3, 4);
        break;
    case 0xe1:
        if (fops) { fclose(fops); fops = 0; }
        if (cmd[1] == 0) {
            v = (cmd[2] << 8) | cmd[3];
            if (v >= 0x800) break;
            fbuf[v] = 0;
            strcpy(name, (char *)fbuf);
        } else {
            v = ((uint32)cmd[2] << 24) | (cmd[3] << 16) | (cmd[4] << 8) | cmd[5];
            if (!nth_entry(v, name, &dp)) break;
        }
        snprintf(path, sizeof(path), "%s/%s", cur, name);
        if (strstr(name, "..")) {
            /* FatFs does not resolve ".." on exFAT: the menu must not rely on it */
            host_log("E1 name with '..' (fails on exFAT): %s\n", name);
            fw_dotdot++;
        }
        fops = path_valid() ? fopen(path, cmd[6] ? "wb" : "rb") : 0;
        reply[1] = fops != 0;
        host_log("E1 open %s %s -> %d\n", name, cmd[6] ? "write" : "read", fops != 0);
        break;
    case 0xe3:
        v = (cmd[1] << 8) | cmd[2];
        if (v > 0x800) v = 0x800;
        v = fops ? fread(fbuf, 1, v, fops) : 0;
        reply[1] = v >> 8;
        reply[2] = v;
        break;
    case 0xe4:
        v = (cmd[1] << 8) | cmd[2];
        if (v > 0x800) v = 0x800;
        v = fops ? fwrite(fbuf, 1, v, fops) : 0;
        fw_bytes_written += v;
        reply[1] = v >> 8;
        reply[2] = v;
        break;
    case 0xe5:
        if (fops) { fclose(fops); fops = 0; }
        break;
    case 0xe7:
        if (data && len >= 0x800)
            memcpy(data, fbuf, 0x800);
        break;
    case 0xf0:
        reply[1] = 0;
        break;
    default:
        break;
    }
    return 0;
}

/* ODE transport for the host build */
int32 ode_io_open(void) { return 0; }
int32 g_ode_cmds;
uint32 g_ode_io_vbl;
uint32 gfx_vbl_count(void);
int32 ode_cmd(const uint8 *cmd, uint8 *reply, int32 n) { g_ode_cmds++; g_ode_io_vbl = gfx_vbl_count(); return host_fw_cmd(cmd, reply, n, 0, 0); }
int32 ode_cmd_data(const uint8 *cmd, uint8 *reply, int32 n, uint8 *data, int32 len) { g_ode_io_vbl = gfx_vbl_count(); return host_fw_cmd(cmd, reply, n, data, len); }
int32 ramp_read(int32 unit, uint32 addr, void *buf, int32 len) { (void)unit; (void)addr; memset(buf, 0, len); return 0; }
int32 ramp_write(int32 unit, uint32 addr, void *buf, int32 len) { (void)unit; (void)addr; (void)buf; (void)len; return 0; }
Item nvram_unmount(void) { return 0; }
Item run_fscheck(void) { return 0; }
