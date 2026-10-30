/* odeio.c - 3DO side transport to the ODE through Fixel's "ode" driver (System/Drivers/ode, unchanged).
 *
 * Same driver calls as the original menu (reconstruction src/ode.c):
 *   ode-device command 3  = xbus CmdCommand: 7 command bytes out, n status bytes back
 *   ode-device command 12 = data phase (xbus CMD_READ into a buffer) + command, ioi_User -> {unit, buf, len}
 *   ode-device command 13 = xbus dev_MaxUnitNum
 *   "ramp" device         = memory / ROM / NVRAM access (units 0, 2..5)
 * As in the original, every command gets a new IOReq that is deleted after it: the ode driver completes its
 * requests in a way that leaves a reused IOReq "in progress" (on hardware C0 then fails with ER_IONotDone).
 * Difference from the original: no debug printf on every data transfer.
 */
#include "common.h"
#include "time.h"

typedef struct OdeRW {
    uint32  unit;
    uint8  *buf;
    int32   len;
} OdeRW;

static Item   ode_dev = -1;
static Item   ramp_dev = -1;
static uint32 ode_unit = 0;
int32  g_ode_cmds;                  /* commands sent, for the diagnostic build */

static Item new_req(const char *name, int32 pri, Item dev)
{
    return CreateIOReq((char *)name, (uint8)pri, dev, 0);
}

static int32 ode_count_units(void)
{
    IOInfo ioi;
    int32 n = 0;
    Item req;
    Err err;

    req = new_req("odecmd0", 0xff, ode_dev);
    if (req < 0)
        return 1;
    memset(&ioi, 0, sizeof(ioi));
    ioi.ioi_Command = 13;
    ioi.ioi_Recv.iob_Buffer = &n;
    ioi.ioi_Recv.iob_Len = 4;
    ioi.ioi_Flags = IO_QUICK;
    err = DoIO(req, &ioi);
    DeleteItem(req);
    if (err == 0)
        return n + 1;
    return 1;
}

int32 ode_io_open(void)
{
    ode_dev = FindAndOpenDevice("ode");
    if (ode_dev < 0)
        return ode_dev;
    ramp_dev = FindAndOpenDevice("ramp");
    /* with two xbus units the ODE is unit 1 (same rule as the original) */
    ode_unit = (ode_count_units() == 2) ? 1 : 0;
    return 0;
}

uint32 g_ode_io_vbl;



int32 ode_cmd(const uint8 *cmd, uint8 *reply, int32 replylen)
{
    IOInfo ioi;
    Item req;
    Err err;

    g_ode_cmds++;
    g_ode_io_vbl = gfx_vbl_count();
    req = new_req("odecmd0", 0xff, ode_dev);
    if (req < 0)
        return req;
    memset(&ioi, 0, sizeof(ioi));
    ioi.ioi_Command = 3;
    ioi.ioi_User = ode_unit;
    ioi.ioi_Recv.iob_Buffer = reply;
    ioi.ioi_Recv.iob_Len = replylen;
    ioi.ioi_Send.iob_Buffer = (void *)cmd;
    ioi.ioi_Send.iob_Len = 7;
    ioi.ioi_Flags = IO_QUICK;
    err = DoIO(req, &ioi);
    DeleteItem(req);
#ifdef LAUNCH_TRACE
    ode_trace(cmd, reply, replylen, err);
#endif
    return err;
}

int32 ode_cmd_data(const uint8 *cmd, uint8 *reply, int32 replylen, uint8 *data, int32 datalen)
{
    IOInfo ioi;
    OdeRW rw;
    Item req;
    Err err;

    g_ode_cmds++;
    g_ode_io_vbl = gfx_vbl_count();
    req = new_req("odecmd1", 200, ode_dev);
    if (req < 0)
        return req;
    memset(&ioi, 0, sizeof(ioi));
    rw.unit = ode_unit;
    rw.buf = data;
    rw.len = datalen;
    ioi.ioi_Command = 12;
    ioi.ioi_User = (uint32)&rw;
    ioi.ioi_Recv.iob_Buffer = reply;
    ioi.ioi_Recv.iob_Len = replylen;
    ioi.ioi_Send.iob_Buffer = (void *)cmd;
    ioi.ioi_Send.iob_Len = 7;
    err = DoIO(req, &ioi);
    DeleteItem(req);
#ifdef LAUNCH_TRACE
    ode_trace(cmd, reply, replylen, err);
#endif
    return err;
}

/* For units other than 0 the ramp driver (odedrv.c, ramp_cmd_read/ramp_cmd_write) stores the ram device's
 * io_Error in the int32 that ioi_User points to. The original menu returned the pointer value instead, so errors
 * never reached its callers; here the stored error is returned. */
int32 ramp_read(int32 unit, uint32 addr, void *buf, int32 len)
{
    IOInfo ioi;
    int32 u;
    Item req;
    Err err;

    if (ramp_dev < 0)
        return -1;
    req = new_req("ramprq", 0xff, ramp_dev);
    if (req < 0)
        return req;
    memset(&ioi, 0, sizeof(ioi));
    u = unit;
    ioi.ioi_Command = CMD_READ;
    ioi.ioi_Recv.iob_Buffer = buf;
    ioi.ioi_Recv.iob_Len = len;
    ioi.ioi_Offset = addr;
    ioi.ioi_User = (uint32)&u;
    err = DoIO(req, &ioi);
    DeleteItem(req);
    if (err >= 0 && unit != 0)
        err = u;
    return err;
}

int32 ramp_write(int32 unit, uint32 addr, void *buf, int32 len)
{
    IOInfo ioi;
    int32 u;
    Item req;
    Err err;

    if (ramp_dev < 0)
        return -1;
    req = new_req("ramprq", 0xff, ramp_dev);
    if (req < 0)
        return req;
    memset(&ioi, 0, sizeof(ioi));
    u = unit;
    ioi.ioi_Command = CMD_WRITE;
    ioi.ioi_Send.iob_Buffer = buf;
    ioi.ioi_Send.iob_Len = len;
    ioi.ioi_Offset = addr;
    ioi.ioi_User = (uint32)&u;
    err = DoIO(req, &ioi);
    DeleteItem(req);
    if (err >= 0 && unit != 0)
        err = u;
    return err;
}

Item nvram_unmount(void)
{
    return LoadProgram("$c/lmadm -u ram 3 0 nvram");
}

Item run_fscheck(void)
{
    return LoadProgram("$c/fscheck");
}

/* Sleep secs seconds on the timer device, doing nothing else: what the stock menu does after C5 (its
 * timer_delay(7, 0)), so the console is left alone while the ODE swaps the disc and resets it. */
void ode_sleep_quiet(int32 secs)
{
    IOInfo ioi;
    TimeVal tv;
    Item dev, req = -1;

    dev = FindAndOpenDevice("timer");
    if (dev >= 0)
        req = CreateIOReq(0, 0, dev, 0);
    if (req >= 0) {
        tv.tv_sec = secs;
        tv.tv_usec = 0;
        memset(&ioi, 0, sizeof(ioi));
        ioi.ioi_Command = TIMERCMD_DELAY;
        ioi.ioi_Unit = TIMER_UNIT_USEC;
        ioi.ioi_Send.iob_Buffer = &tv;
        ioi.ioi_Send.iob_Len = sizeof(tv);
        DoIO(req, &ioi);
        DeleteItem(req);
    } else {
        int32 f;
        for (f = 0; f < secs * 60; f++)
            gfx_wait_vbl();         /* (no timer: wait on the video fields instead, still drawing nothing) */
    }
    if (dev >= 0)
        CloseItem(dev);
}
