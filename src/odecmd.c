/* odecmd.c - ODE command set used by the menu (opcodes as documented in the firmware project's
 * docs/commands.md). Portable: it only calls ode_cmd / ode_cmd_data.
 */
#include "common.h"

static char   ode_name_buf[16];
static uint32 ext_b1;
static uint32 ver_major, ver_minor, ver_count;

static void clear7(uint8 *c)
{
    int32 i;
    for (i = 0; i < 8; i++)
        c[i] = 0;
}

static void put32(uint8 *c, uint32 v)
{
    c[0] = (uint8)(v >> 24);
    c[1] = (uint8)(v >> 16);
    c[2] = (uint8)(v >> 8);
    c[3] = (uint8)v;
}

static int32 ext_id(void)
{
    uint8 cmd[8];
    uint8 reply[12];
    int32 err;
    uint32 i;

    clear7(cmd);
    memset(reply, 0, sizeof(reply));
    cmd[0] = 0x93;
    err = ode_cmd(cmd, reply, 12);
    if (err != 0)
        return err;
    memset(ode_name_buf, 0, sizeof(ode_name_buf));
    for (i = 2; i < 12 && reply[i] >= 0x20; i++)
        ode_name_buf[i - 2] = reply[i];
    ext_b1 = reply[1];
    ver_major = reply[8];
    ver_minor = reply[9];
    ver_count = reply[10];
    return 0;
}

char *ode_init(void)
{
    if (ode_io_open() < 0)
        return 0;
    ext_id();
    return ode_name_buf;
}

void ode_refresh_ext_id(void)
{
    ext_id();
}

uint32 ode_ext_b1(void)
{
    return ext_b1;
}

void ode_version(int32 *major, int32 *minor, int32 *count)
{
    *major = ver_major & 0xff;
    *minor = ver_minor & 0xff;
    *count = ver_count & 0xff;
}

int32 ode_drives(void)
{
    return ext_b1 & 3;
}

#define C0_UNKNOWN 0xfffffffeu          /* not known: after power-on 0, but a clone keeps its own through a reset */
static uint32 c0_last = C0_UNKNOWN;     /* the id the firmware takes as its current one (a clone's "current TOC") */

static int32 c0_send(uint32 id)
{
    uint8 cmd[8];
    uint8 reply[4];

    clear7(cmd);
    cmd[0] = 0xc0;
    put32(cmd + 1, id);
    if ((id & 0x00ffffffu) == 0x00ffffffu)
        c0_last = id & 0xff000000u;     /* "up": the clone's current id becomes the folder's entry 0 */
    else
        c0_last = id;
    return ode_cmd(cmd, reply, 2);
}

static uint32 get32b(const uint8 *p)
{
    return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3];
}

/* A clone (SataTo3DO-based: its ids carry a drive byte) ignores C0 for the id it already takes as current, so
 * folder 1 inside a folder that was itself entry 1 would not open. First C0 another entry of this folder: a file
 * only becomes the current id (the firmware stays where it is), a folder is entered and left again. Returns 0 if
 * the firmware will now take id, -1 if this folder has no other entry. */
static int32 c0_unstick(uint32 id)
{
    static uint8 buf[ODE_XFER];
    uint32 flags, toc, other = 0, other_dir = 0, len;
    int32 pos = 0;

    if (ode_d1_list(0, 14, buf) < 0)
        return -1;
    while (pos + 12 <= ODE_XFER) {
        flags = get32b(buf + pos);
        if (flags == 0xffffffffu)
            break;
        toc = get32b(buf + pos + 4);
        len = get32b(buf + pos + 8);
        if ((toc & 0x00ffffffu) != 0x00ffffffu && toc != id) {
            if (!(flags & 2)) {
                other = toc;
                break;
            }
            if (!other_dir)
                other_dir = toc;
        }
        if (len == 0 || len > 256)
            break;
        pos += 12 + len;
    }
    if (other) {
        c0_send(other);
        return 0;
    }
    if (other_dir) {
        c0_send(other_dir);
        c0_send((other_dir & 0xff000000u) | 0x00ffffffu);
        return 0;
    }
    return -1;
}

int32 ode_c0_change_dir(uint32 id)
{
    if ((id == c0_last || c0_last == C0_UNKNOWN) && (id & 0xff000000u) != 0 && (id & 0x00ffffffu) != 0 &&
        (id & 0x00ffffffu) != 0x00ffffffu && c0_unstick(id) < 0 && id == c0_last)
        return ODE_C0_STUCK;            /* it would be ignored: the caller must not go on as if it had opened */
    return c0_send(id);
}


int32 ode_c3_queue_clear(void)
{
    uint8 cmd[8];
    uint8 reply[4];

    clear7(cmd);
    cmd[0] = 0xc3;
    return ode_cmd(cmd, reply, 2);
}

int32 ode_c4_queue_add(uint32 toc_id)
{
    uint8 cmd[8];
    uint8 reply[4];
    int32 err;

    clear7(cmd);
    reply[1] = reply[2] = 0;
    cmd[0] = 0xc4;
    put32(cmd + 1, toc_id);
    err = ode_cmd(cmd, reply, 4);
    if (err < 0)
        return -1;
    if (reply[1] == 0)
        return -1;          /* entry not found */
    if (reply[2] == 0)
        return -2;          /* found but not queued (queue full, or the image could not be opened) */
    return 0;
}

int32 ode_c5_launch(void)
{
    uint8 cmd[8];
    uint8 reply[4];

    clear7(cmd);
    cmd[0] = 0xc5;
    return ode_cmd(cmd, reply, 2);
}

/* D1: entries from index, up to count, into a 2 KB buffer. The firmware does not check the buffer end until
 * after it has written an entry, so count must stay <= 14 (14 x (12 + 128) fits in 2 KB). */
int32 ode_d1_list(uint32 index, uint32 count, uint8 *buf)
{
    uint8 cmd[8];
    uint8 reply[4];
    int32 err;

    if (count > 14)
        count = 14;
    clear7(cmd);
    reply[0] = 0;
    memset(buf, 0xff, ODE_XFER);
    cmd[0] = 0xd1;
    put32(cmd + 1, index);
    cmd[5] = (uint8)(count >> 8);
    cmd[6] = (uint8)count;
    err = ode_cmd_data(cmd, reply, 2, buf, ODE_XFER);
    if (err < 0)
        return err;
    if (reply[0] != 0xd1)
        return -0x1000 - reply[0];      /* -0x10xx: the reply byte was xx */
    return 0;
}

/* E6: store 4 bytes at word index in the ODE's 2 KB file buffer */
static int32 fill_buffer(const uint8 *buf, uint32 len)
{
    uint8 cmd[8];
    uint8 reply[4];
    uint32 i, w;
    int32 err = 0;

    clear7(cmd);
    cmd[0] = 0xe6;
    for (i = 0; i < len; i += 4) {
        w = i >> 2;
        cmd[1] = (uint8)(w >> 8);
        cmd[2] = (uint8)w;
        cmd[3] = buf[i];
        cmd[4] = (i + 1 < len) ? buf[i + 1] : 0;
        cmd[5] = (i + 2 < len) ? buf[i + 2] : 0;
        cmd[6] = (i + 3 < len) ? buf[i + 3] : 0;
        err = ode_cmd(cmd, reply, 2);
        if (err < 0)
            return err;
    }
    return 0;
}

/* E1 by name, in the ODE's current directory. write != 0 creates or truncates. */
uint32 g_sd_write_vbl;           /* video field of the last file write on the card */
static int32 writing;

int32 ode_file_open(const char *name, int32 write)
{
    uint8 cmd[8];
    uint8 reply[4];
    uint32 len;
    int32 err;

    len = strlen(name) + 1;
    err = fill_buffer((const uint8 *)name, len);
    if (err < 0)
        return err;
    clear7(cmd);
    reply[1] = 0;
    cmd[0] = 0xe1;
    cmd[1] = 0;
    cmd[2] = (uint8)(len >> 8);     /* E1 reads the length as cmd[2] << 8 | cmd[3] */
    cmd[3] = (uint8)len;
    cmd[6] = write ? 1 : 0;
    writing = write;
    if (write)
        g_sd_write_vbl = gfx_vbl_count();
    err = ode_cmd(cmd, reply, 3);
    if (err < 0)
        return err;
    return reply[1] ? 0 : -1;
}

int32 ode_file_write(const uint8 *buf, uint32 len)
{
    uint8 cmd[8];
    uint8 reply[4];
    int32 err;
    uint32 n;

    if (len > ODE_XFER)
        return -1;
    err = fill_buffer(buf, len);
    if (err < 0)
        return err;
    clear7(cmd);
    cmd[0] = 0xe4;
    cmd[1] = (uint8)(len >> 8);
    cmd[2] = (uint8)len;
    err = ode_cmd(cmd, reply, 4);
    if (err < 0)
        return err;
    n = (reply[1] << 8) | reply[2];
    return (n == len) ? 0 : -1;
}

static uint8 xfer[ODE_XFER];

int32 ode_file_read(uint8 *buf, uint32 len, uint32 *got)
{
    uint8 cmd[8];
    uint8 reply[4];
    int32 err;
    uint32 n;

    static const uint8 mark[8] = { 0xfe, 0xed, 0x3d, 0x0e, 0x4d, 0x61, 0x72, 0x6b };

    *got = 0;
    if (len > ODE_XFER)
        len = ODE_XFER;
    /* A clone (SataTo3DO-based) answers every read with the length asked for, even an empty file or the end of
     * one, and its buffer still holds what was read before (another file). So 8 marker bytes go into the buffer
     * first: if they are still there after the read, nothing was read. (Fixel's firmware answers the true length,
     * and its read overwrites them.) */
    err = fill_buffer(mark, sizeof(mark));
    if (err < 0)
        return err;
    clear7(cmd);
    cmd[0] = 0xe3;
    cmd[1] = (uint8)(len >> 8);
    cmd[2] = (uint8)len;
    err = ode_cmd(cmd, reply, 4);
    if (err < 0)
        return err;
    n = (reply[1] << 8) | reply[2];
    if (n > len)
        return -1;
    if (n == 0)
        return 0;
    clear7(cmd);
    cmd[0] = 0xe7;
    reply[0] = 0;
    err = ode_cmd_data(cmd, reply, 2, xfer, ODE_XFER);
    if (err < 0)
        return err;
    if (reply[0] != 0xe7)
        return -1;
    if (memcmp(xfer, mark, n < sizeof(mark) ? n : sizeof(mark)) == 0)
        return 0;                       /* nothing was read: the end of the file (n > 0 here) */
    memcpy(buf, xfer, n);
    *got = n;
    return 0;
}

int32 ode_file_close(void)
{
    int32 err;
    uint8 cmd[8];
    uint8 reply[4];

    clear7(cmd);
    cmd[0] = 0xe5;
    err = ode_cmd(cmd, reply, 4);
    if (writing)
        g_sd_write_vbl = gfx_vbl_count();
    writing = 0;
    return err;
}

int32 ode_f0_update(void)
{
    uint8 cmd[8];
    uint8 reply[4];
    int32 err;

    clear7(cmd);
    reply[1] = 0;
    cmd[0] = 0xf0;
    err = ode_cmd(cmd, reply, 3);
    if (err != 0)
        return err < 0 ? err : -1;
    return reply[1] ? 1 : 0;
}
