/* tools.c - the X-menu tools of the original menu (reconstruction src/ui.c), with progress shown on screen.
 * Behaviour kept: same files (nvram.nvm, sysrom.nvm, fontrom.nvm, fmvrom.nvm, ram3b.nvm) in the current folder,
 * same ramp units and sizes, same update sequence (F0, 15 s wait, EXT_ID again), same Device info values.
 * Changed: Load NVRAM only writes the console's NVRAM after the whole 32 KB file was read without error (the
 * original wrote whatever was in its buffer even after a failed read), and nothing prints debug output.
 */
#include "common.h"

#define BIG 0x8000

static uint8 big_buf[BIG];
static int32 patches_applied;
static uint32 sysrom_crc, fontrom_crc, fmvrom_crc;

int32 tool_patches_applied(void)
{
    return patches_applied;
}

void tool_set_patches(int32 n)
{
    patches_applied = n;
}

void tool_storage_manager(void)
{
    uint32 held = 0;

    Item task;

    ui_progress("STORAGE MANAGER", "LOADING", -1);
    /* the SDK's Storage Tuner (Fixel's build) takes over the screen; it runs until X, like the stock menu's */
    task = LoadProgram("assets/storagetuner");
    if (task < 0) {
        ui_message("Storage Manager", "Could not start the Storage Tuner (assets/storagetuner on the menu disc).");
        return;
    }
    input_flush();
    do {
        gfx_wait_vbl();
        input_poll(&held);
    } while (held != BTN_X && LookupItem(task) != 0);
    input_flush();
}

/* Opera file system volume label at the start of a formatted NVRAM: record type 1, five 0x5A sync bytes,
 * structure version 1 */
static int32 looks_like_nvram(const uint8 *b)
{
    return b[0] == 1 && b[1] == 0x5a && b[2] == 0x5a && b[3] == 0x5a && b[4] == 0x5a && b[5] == 0x5a && b[6] == 1;
}

int32 tool_save_nvram(void)
{
    char msg[40];
    uint32 off;
    int32 err;

    ui_progress("Saving NVRAM", "Reading console memory", 0);
    memset(big_buf, 0, BIG);
    err = ramp_read(3, 0, big_buf, BIG);
    if (err < 0)
        return err;
    if (!looks_like_nvram(big_buf))
        return -2;                      /* do not replace a backup with something that is not NVRAM */
    if (ode_file_open("nvram.nvm", 1) < 0)
        return -1;
    for (off = 0; off < BIG; off += ODE_XFER) {
        sprintf(msg, "nvram.nvm  %d%%", (int)(off * 100 / BIG));
        ui_progress("Saving NVRAM", msg, off * 100 / BIG);
        err = ode_file_write(big_buf + off, ODE_XFER);
        if (err < 0)
            break;
    }
    if (ode_file_close() < 0 && err == 0)
        err = -1;
    return err;
}

int32 tool_load_nvram(void)
{
    char msg[40];
    uint32 off, n;
    int32 err = 0;
    uint8 extra[4];

    /* read and check the whole file before touching the console's NVRAM */
    if (ode_file_open("nvram.nvm", 0) < 0)
        return -1;
    memset(big_buf, 0, BIG);
    for (off = 0; off < BIG; off += ODE_XFER) {
        sprintf(msg, "nvram.nvm  %d%%", (int)(off * 100 / BIG));
        ui_progress("Loading NVRAM", msg, off * 100 / BIG);
        err = ode_file_read(big_buf + off, ODE_XFER, &n);
        if (err < 0 || n != ODE_XFER) {
            err = -1;
            break;
        }
    }
    if (err == 0 && (ode_file_read(extra, 4, &n) < 0 || n != 0))
        err = -1;                       /* not exactly 32 KB */
    ode_file_close();
    if (err == 0 && !looks_like_nvram(big_buf))
        err = -2;
    if (err < 0)
        return err;
    ui_progress("Loading NVRAM", "Unmounting NVRAM", 100);
    if (nvram_unmount() < 0)
        return -1;
    gfx_delay(30);                      /* let lmadm finish (the original waited 5 ms) */
    ui_progress("Loading NVRAM", "Writing console memory", 100);
    err = ramp_write(3, 0, big_buf, BIG);
    run_fscheck();
    gfx_delay(30);
    return err < 0 ? -3 : 0;
}

int32 tool_dump(int32 which)
{
    static const char *files[4] = { "sysrom.nvm", "fontrom.nvm", "fmvrom.nvm", "ram3b.nvm" };
    static const char *titles[4] = { "Dumping System ROM", "Dumping Font ROM", "Dumping FMV ROM", "Dumping RAM" };
    static const int32 units[4] = { 2, 5, 4, 0 };
    static const uint32 sizes[4] = { 0x100000, 0x100000, 0x100000, 0x300000 };
    char msg[40];
    uint32 addr, off, size = sizes[which];
    int32 err = 0;

    if (ode_file_open(files[which], 1) < 0)
        return -1;
    for (addr = 0; addr < size && err == 0; addr += BIG) {
        memset(big_buf, 0, BIG);
        err = ramp_read(units[which], addr, big_buf, BIG);
        if (err < 0)
            break;
        err = 0;
        sprintf(msg, "%s  %d%%", files[which], (int)(addr * 100 / size));
        if (ui_progress(titles[which], msg, addr * 100 / size)) {
            err = -1;
            break;
        }
        for (off = 0; off < BIG; off += ODE_XFER) {
            err = ode_file_write(big_buf + off, ODE_XFER);
            if (err < 0)
                break;
        }
    }
    if (ode_file_close() < 0 && err == 0)
        err = -1;
    return err;
}

int32 tool_update_ode(void)
{
    int32 r, s;
    char msg[40];

    r = ode_f0_update();
    if (r != 1)
        return r;
    for (s = 15; s > 0; s--) {
        sprintf(msg, "Do not power off (%d s)", (int)s);
        ui_progress("Updating the ODE", msg, (15 - s) * 100 / 15);
        gfx_delay(60);
    }
    ode_refresh_ext_id();
    return 1;
}

static uint32 crc_unit(int32 unit, const char *what)
{
    uint32 v = 0xffffffff, off;
    char msg[40];

    for (off = 0; off < 0x100000; off += BIG) {
        sprintf(msg, "%s  %d%%", what, (int)(off * 100 / 0x100000));
        ui_progress("Device info", msg, off * 100 / 0x100000);
        ramp_read(unit, off, big_buf, BIG);
        v = crc32_update(v, big_buf, BIG);
    }
    return ~v;
}

/* same values as the original Device info screen */
void tool_device_info(char lines[8][40], int32 *n)
{
    uint32 v;
    uint8 hdr[16];
    int32 err;

    ui_progress("Device info", "Reading ...", 0);
    ramp_read(0, 0x03300000, &v, 4);
    sprintf(lines[0], "MADAM    %.8X", (unsigned)v);
    ramp_read(0, 0x03400000, &v, 4);
    sprintf(lines[1], "CLIO     %.8X", (unsigned)v);
    sprintf(lines[2], "DIPIR    %.8X", (unsigned)ram_crc_0x200());
    if (sysrom_crc == 0)
        sysrom_crc = crc_unit(2, "System ROM");
    sprintf(lines[3], "SYSROM   %.8X", (unsigned)sysrom_crc);
    if (fontrom_crc == 0) {
        ramp_read(2, 0, big_buf, 16);
        ramp_read(5, 0, hdr, 16);
        if (memcmp(big_buf, hdr, 16) != 0)
            fontrom_crc = crc_unit(5, "Font ROM");
    }
    if (fontrom_crc != 0)
        sprintf(lines[4], "FONTROM  %.8X", (unsigned)fontrom_crc);
    else
        sprintf(lines[4], "FONTROM  not present");
    if (fmvrom_crc == 0) {
        err = ramp_read(4, 0, hdr, 16);
        if (err == 0)
            fmvrom_crc = crc_unit(4, "FMV ROM");
    }
    if (fmvrom_crc != 0)
        sprintf(lines[5], "FMVROM   %.8X", (unsigned)fmvrom_crc);
    else
        sprintf(lines[5], "FMVROM   not present");
    sprintf(lines[6], "PATCHES  %d", (int)patches_applied);
    strcpy(lines[7], g_probe_info);
    *n = 8;
}
