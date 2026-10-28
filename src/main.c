/* main.c - startup and main loop of the menu.
 * Order: ode driver, graphics, the USB-source wait, the XPort RAM patch, then the menu.
 */
#include "common.h"


#ifndef HOST_BUILD
extern void kprintf(const char *fmt, ...);
#endif

/* AppStartup runs $boot/LaunchMe, and the system then starts $boot/LaunchMe again after the script, so two copies
 * of the menu can end up running side by side, each drawing its own screens and talking to the ODE. Only one may
 * stay: each copy registers a named message port, and every copy but the oldest leaves. */
#define INSTANCE_PORT "FZ1ODEMenu"

static int32 another_copy_runs(void)
{
    Item mine, first;

    mine = CreateMsgPort(INSTANCE_PORT, 0, 0);
    if (mine < 0)
        return 0;
    first = FindMsgPort(INSTANCE_PORT);
    return first >= 0 && first != mine;
}

static void splash(const char *msg)
{
    gfx_frame_begin();
    gfx_text((SCR_W - (int32)strlen(msg) * 8) / 2, 116, msg, 0, P_DIM);
    gfx_frame_end();
}

#ifdef RULER
/* test build (-DRULER): 8-pixel bands numbered with their first line (rows) and column, to read off how much of
 * the picture a TV hides. A goes on to the menu. */
static void ruler(void)
{
    char t[8];
    int32 y, x;

    input_flush();
    for (;;) {
        gfx_frame_begin();
        for (y = 0; y < SCR_H; y += 8) {
            gfx_rect(0, y, SCR_W, 8, (y / 8) & 1 ? P_PANEL : P_BG);
            sprintf(t, "%d", (int)y);
            gfx_text(56, y, t, 0, P_TEXT);
            gfx_text(SCR_W / 2 - 12, y, t, 0, P_ACCENT);
            gfx_text(SCR_W - 88, y, t, 0, P_TEXT);
        }
        gfx_rect(0, 104, SCR_W, 40, P_BG);
        for (x = 0; x < SCR_W; x += 8)
            gfx_rect(x, 106, 8, 12, (x / 8) & 1 ? P_ORANGE : P_VIOLET);
        for (x = 0; x < SCR_W; x += 32) {
            sprintf(t, "%d", (int)x);
            gfx_text(x, 120, t, 0, P_ORANGE);
        }
        gfx_text(SCR_W / 2 - 68, 132, "A: ON TO THE MENU", 0, P_ACCENT);
        gfx_frame_end();
        if (input_poll(0) & BTN_A)
            break;
    }
    input_flush();
}
#endif

int main(void)
{
    char *name;
    int32 s;
    uint32 ev, held;

    if (another_copy_runs())
        return 0;
    DIAG_STEP(0x001f, 0);                       /* blue: the menu program started */
    name = ode_init();
    if (name == 0) {
#ifndef HOST_BUILD
        kprintf("ode init failed\n");
#endif
        DIAG_STOP(0x7c00, 1);                   /* red, 1 bar: the ode driver did not open */
        return 0;
    }
    DIAG_STEP(0x03e0, 0);                       /* green: ODE answered */
    s = gfx_init();
    if (s < 0) {
#ifndef HOST_BUILD
        kprintf("Graph init failed\n");
#endif
        DIAG_STOP(0x7c1f, -s);                  /* magenta, n bars: graphics step n failed */
        gfx_shutdown();
        return 0;
    }
    DIAG_STEP(0x7fe0, 0);                       /* yellow: graphics ready */
    gfx_clock_init();
    input_init();
#ifdef RULER
    ruler();
#endif

    if (ode_ext_b1() & 0x20) {
        /* the menu disc was mounted from USB: the original waited 2 s here */
        splash("WAITING FOR USB STORAGE");
        gfx_delay(120);
    }
    if (name[0] == 'X')
        tool_set_patches(apply_ram_patches(1));
    DIAG_STEP(0x03ff, 0);                       /* cyan: music player and patches done */
    if (dir_init() < 0) {
        DIAG_STOP(0x7e00, 2);                   /* orange, 2 bars: no memory for the folder list */
        splash("NOT ENOUGH MEMORY");
        for (;;)
            gfx_wait_vbl();
    }
    ui_init(name);
    DIAG_STEP(0x7fff, 0);                       /* white: about to read the card */
    ui_start();
    input_flush();
    for (;;) {
        /* a frame that draws waits for the vertical blank in gfx_frame_begin; one that does not, waits here */
        ev = input_poll(&held);
        if (ui_frame(ev, held))
            ui_draw();          /* waits for the vertical blank in gfx_frame_begin */
        else
            gfx_wait_vbl();
    }
    return 0;
}
