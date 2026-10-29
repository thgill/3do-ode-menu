/* common.h - shared declarations for the revised FZ-1 ODE menu (LaunchMe).
 *
 * Builds two ways:
 *   - for the 3DO with the 3do-devkit (ARM SDT 2.51 + Portfolio 2.5 libraries)
 *   - for the host (HOST_BUILD defined) against host/h3do.h, a small stand-in for the lib3do/Portfolio calls used
 *     here, plus a firmware simulator, so the logic and the screen layout can be tested without a console.
 */
#ifndef MENU_COMMON_H
#define MENU_COMMON_H

#ifdef HOST_BUILD
#include "h3do.h"
#else
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "mem.h"
#include "kernel.h"
#include "nodes.h"
#include "kernelnodes.h"
#include "item.h"
#include "io.h"
#include "device.h"
#include "operror.h"
#include "graphics.h"
#include "filefunctions.h"
#include "displayutils.h"
#include "celutils.h"
#include "fontlib.h"
#include "textlib.h"
#include "event.h"
#include "blockfile.h"
#include "getvideoinfo.h"
#include "msgport.h"
#include "operamath.h"
#endif

/* the menu's version is its build date: build_3do.sh passes -DMENU_DATE="YYYY-MM-DD" */
#ifndef MENU_DATE
#define MENU_DATE       "DEV BUILD"
#endif
#define MENU_VERSION    MENU_DATE

/* ---- screen (320 x 240, laid out on a 40 x 30 grid of 8-pixel cells, PicoPak-SMS style) ---- */
#define SCR_W           320
#define SCR_H           240

/* palette roles (gfx.c holds the SYNTHWAVE colors for each, and a dimmed copy for behind dialogs) */
enum {
    P_BG, P_PANEL, P_EDGE, P_DIM, P_TEXT, P_ACCENT, P_FOLDER, P_HEART, P_ORANGE, P_GREEN, P_VIOLET,
    P_BARTEXT, P_BAR, P_COUNT
};
#define RGB15(r, g, b)  ((((r) >> 3) << 10) | (((g) >> 3) << 5) | ((b) >> 3))

/* ---- diagnostic build (-DDIAG): whole-screen colors between startup steps ---- */
#ifdef DIAG
void gfx_diag(int32 color, int32 bars);
void gfx_stats(char *out);
#endif
#ifdef DIAG_STEPS
#define DIAG_STEP(c, n) gfx_diag((c), (n))
#define DIAG_STOP(c, n) do { for (;;) gfx_diag((c), (n)); } while (0)
#else
#define DIAG_STEP(c, n)
#define DIAG_STOP(c, n)
#endif

/* ---- ODE (odeio.c / host transport) ---- */
int32  ode_io_open(void);                       /* opens "ode" and "ramp", returns 0 or < 0 */
int32  ode_cmd(const uint8 *cmd, uint8 *reply, int32 replylen);
int32  ode_cmd_data(const uint8 *cmd, uint8 *reply, int32 replylen, uint8 *data, int32 datalen);
int32  ramp_read(int32 unit, uint32 addr, void *buf, int32 len);
int32  ramp_write(int32 unit, uint32 addr, void *buf, int32 len);
Item   nvram_unmount(void);
Item   run_fscheck(void);

/* ---- ODE commands (odecmd.c) ---- */
#define ODE_XFER        0x800
#define TOC_UP          0xffffffff
#define TF_FILE         0x01
#define TF_DIR          0x02
#define TF_NVM          0x08
#define TF_ODE          0x10
char  *ode_init(void);                          /* EXT_ID; returns the ODE name or 0 */
void   ode_refresh_ext_id(void);
uint32 ode_ext_b1(void);
void   ode_version(int32 *major, int32 *minor, int32 *count);
int32  ode_drives(void);                        /* bit 0 microSD, bit 1 USB */
int32  ode_c0_change_dir(uint32 id);
int32  ode_c3_queue_clear(void);
int32  ode_c4_queue_add(uint32 toc_id);         /* 0 added, -1 error, -2 not added (full) */
int32  ode_c5_launch(void);
int32  ode_d1_list(uint32 index, uint32 count, uint8 *buf);   /* 2 KB into buf */
int32  ode_file_open(const char *name, int32 write);
int32  ode_file_write(const uint8 *buf, uint32 len);           /* <= 2 KB per call */
int32  ode_file_read(uint8 *buf, uint32 len, uint32 *got);     /* <= 2 KB per call */
int32  ode_file_close(void);
extern uint32 g_ode_io_vbl;                    /* field of the last ODE command of any kind */
extern uint32 g_sd_write_vbl;                  /* gfx_vbl_count() at the last card write */
int32  ode_f0_update(void);                     /* 1 staged, 0 no update file, < 0 error */

/* ---- directory model (dirlist.c) ---- */
#define DIR_MAX_ENTRIES 4096
#define DIR_POOL_BYTES  (256 * 1024)
#define DIR_MAX_DEPTH   16
#define DIR_NAME_MAX    128

typedef struct DirEntry {
    uint32 toc_id;
    uint32 name_off;
    uint32 inner_off;   /* a game folder: its game file's name, in the pool */
    uint16 inner_toc;   /* and that file's entry number inside the folder */
    uint8  flags;
    uint8  name_len;
    uint8  queued;      /* 0, or 1..5 = position in the play queue */
    uint8  art;         /* for the ui: picture state of this visit (DE_NOART...), favorite mark */
    uint8  gtype;       /* folders: GT_UNKNOWN, GT_GAME (one game inside: shown and started as that game), GT_PLAIN */
    uint8  inner_len;
} DirEntry;
#define DE_NOART    0x01
#define GT_UNKNOWN  0
#define GT_GAME     1
#define GT_PLAIN    2

typedef int32 (*DirProgress)(int32 count, int32 phase);   /* return non-zero to cancel */

typedef struct DirState {
    DirEntry *ent;
    int32     count;
    char     *pool;
    int32     pool_used;
    int32     devlist;          /* top-level device list (/sd, /usb) */
    int32     truncated;
    int32     from_cache;
    int32     depth;            /* 0 = drive root (or the device list) */
    char      path[DIR_MAX_DEPTH][DIR_NAME_MAX];
    uint32    path_toc[DIR_MAX_DEPTH];
    int32     drive;            /* 1 microSD, 2 USB, 0 unknown */
    int32     use_cache;
    int32     both_drives;
    int32     max_entries;      /* set by dir_init from available memory */
    int32     pool_bytes;
    int32     hidden;           /* entries after count that are not shown (the menu disc, unusable files) */
} DirState;

extern DirState g_dir;
extern char     g_dir_err[96];

int32       dir_init(void);
const char *dir_name(int32 i);
int32       dir_load(DirProgress progress, int32 force_scan);
int32       dir_enter(int32 i, DirProgress progress);
int32       dir_back(DirProgress progress, char *came_from);   /* 2 = already at the top */
int32       dir_go_root(void);
int32       dir_verify(int32 i);               /* 1 = the entry is still at its index */
int32       dir_find(const char *name);
void        dir_path_string(char *out, int32 max);
int32       dir_write_cache(void);
int32       dir_write_resume(const char *selected, int32 write);   /* firmware must be in the drive root */
void        dir_restore_path(void);
int32       dir_find_nocase(const char *name);
int32       dir_resume(DirProgress progress, char *selected);
int32       name_compare(const char *a, const char *b);
int32       dir_to_root(DirProgress progress);  /* up to the drive root and list it (stays on the device list) */
int32       dir_to_devlist(DirProgress progress);
int32       dir_root_begin(void);               /* firmware to the drive root for a file access (-1 on the device list) */
void        dir_root_end(void);                 /* and back to the folder the menu is in */
int32       dir_ode_test(char lines[][40], int32 max);    /* TOOLS > ODE TEST: raw answers, for clones */
int32       dir_open_path(int32 drive, const char *path, DirProgress progress);  /* index of the file, or < 0 */
int32       name_is_game(const char *name);     /* .iso or .cue */
int32       dir_is_game(int32 i);               /* a game file, or a game folder */
int32       dir_is_game_folder(int32 i);
const char *dir_inner(int32 i);                 /* a game folder's game file */
int32       dir_game_enter(int32 i, uint32 *toc);   /* firmware into game folder i; *toc = its game. 0 or < 0 */
void        dir_game_leave(void);               /* and back out */
extern int32 g_probe_ok;                        /* single-entry D1 answers can be trusted (-1 not tested yet) */
extern char  g_probe_info[40];

/* ---- input (input.c) ---- */
#define BTN_UP      0x40000000
#define BTN_DOWN    0x80000000
#define BTN_LEFT    0x10000000
#define BTN_RIGHT   0x20000000
#define BTN_A       0x08000000
#define BTN_B       0x04000000
#define BTN_C       0x02000000
#define BTN_P       0x01000000
#define BTN_X       0x00800000
#define BTN_R       0x00400000
#define BTN_L       0x00200000
int32  input_init(void);
uint32 input_poll(uint32 *held);                /* returns press + auto-repeat events */
void   input_flush(void);                       /* ignore buttons until released */
int32  input_fast(void);                        /* 1 while a held direction is scrolling fast */
uint32 input_pressed(void);                     /* fresh presses of the last poll (no repeats) */
int32  input_repeats(void);                     /* steps the held direction moved in the last poll (>= 1) */
int32  input_interrupt(void);                   /* 1 if a button went down since the last poll (checked once a field) */

/* ---- pictures (artdec.c) ---- */
#define ART_OK       0
#define ART_BAD     -1                          /* not a picture, or a kind it cannot read (progressive JPEG...) */
#define ART_BIG     -2
#define ART_NOMEM   -3
#define ART_ABORTED -4
#define ART_MISSING -5                          /* no picture file found */
typedef int32 (*ArtAbort)(void);
int32  art_decode(uint8 *file, int32 len, uint16 *out, int32 stride, int32 *w, int32 *h, ArtAbort ab);
                                                /* *w x *h: the most in, the picture's size out */
int32  art_encode8(const uint16 *pix, int32 stride, int32 w, int32 h, uint8 *out, int32 max);   /* OAR8; length */
typedef int32 (*ArtBegin)(void *ctx, int32 w, int32 h);          /* the decoded picture's size; < 0 to give up */
typedef void  (*ArtPixel)(void *ctx, int32 x, int32 y, int32 r, int32 g, int32 b);
int32  jpg_progressive(const uint8 *f, int32 len, int32 want_w, int32 want_h, ArtBegin begin, ArtPixel px,
                       void *ctx, ArtAbort ab);                 /* jpgprog.c: decoded at 1/8..1/1 size */

/* ---- graphics (gfx.c) ---- */
#define RR_ALL      0
#define RR_TOP      1       /* bottom corners square, no bottom outline (a tab) */
#define RR_BOTTOM   2

typedef struct GfxImage {
    CCB    *ccb;
    uint16 *pix;            /* w x h RGB555, rows of stride pixels */
    uint8  *mask;           /* optional 1-bpp mask of solid pixels (the logo's shine) */
    int32   w, h, stride, mstride;
} GfxImage;

extern ScreenContext *g_sc;
int32  gfx_init(void);
void   gfx_shutdown(void);
void   gfx_frame_begin(void);                   /* starts a frame on the background color */
int32  gfx_frame_begin_bg(int32 key);           /* starts a frame on cached still parts; 1: draw them now */
void   gfx_bg_done(void);                       /* keep what was drawn so far as the still parts */
void   gfx_bg_forget(void);
void   gfx_text_scroll(int32 x, int32 y, const char *s, int32 maxch, int32 px, int32 role);
void   gfx_frame_end(void);                     /* draws everything queued and shows the frame */
void   gfx_wait_vbl(void);
void   gfx_clock_init(void);                    /* measures the field clock; after the screen is up */
uint32 gfx_vbl_count(void);                     /* video fields waited for so far: the clock for animation */
void   gfx_delay(int32 frames);
void   gfx_set_dim(int32 on);                   /* draw what follows in the dimmed palette */
void   gfx_set_bar(int32 rgb15);                /* the selection bar's color this frame */
int32  gfx_color(int32 role);
void   gfx_rect(int32 x, int32 y, int32 w, int32 h, int32 role);
void   gfx_rrect(int32 x, int32 y, int32 w, int32 h, int32 fill, int32 edge, int32 r, int32 mode);
void   gfx_text(int32 x, int32 y, const char *s, int32 maxch, int32 role);   /* UTF-8 in, upper case out */
int32  gfx_fold(const char *utf8, char *out, int32 max);   /* to the font's characters; returns length */
void   gfx_sprite(int32 id, int32 x, int32 y, int32 role);
void   gfx_micro(int32 x, int32 y, const char *s, int32 role);
void   gfx_button(int32 x, int32 y, int32 label, int32 role);
GfxImage *gfx_image_new(int32 w, int32 h);
GfxImage *gfx_image_load(const char *path);     /* "OLG1" file from the disc */
void   gfx_image(GfxImage *img, int32 x, int32 y);
void   gfx_image_rows(GfxImage *img, int32 x, int32 y, int32 rows);  /* only its top rows */

/* ---- ui (ui.c) ---- */
void  ui_init(const char *ode_name);
void  ui_start(void);
int32 ui_frame(uint32 events, uint32 held);    /* returns 1 if the screen needs redrawing */
void  ui_draw(void);
void  ui_toast(const char *msg);
int32 ui_progress(const char *title, const char *msg, int32 pct);   /* draws now; 1 = B pressed */
int32 ui_confirm(const char *title, const char *msg);
void  ui_message(const char *title, const char *msg);

/* ---- favorites, recently played and settings (lists.c), stored in the drive root ---- */
#define LIST_PATH_MAX   200
#define FAV_MAX         64
#define REC_MAX         12
typedef struct PathList {
    int32 n, max, dirty;
    char  (*p)[LIST_PATH_MAX];      /* paths from the drive root, '/' separated */
    const char *file;
} PathList;
extern PathList g_favs, g_recent;
typedef struct Settings {
    int32 pane;         /* info pane shown */
    int32 bar;          /* bar color cycle: 0 off, 1 slow, 2 medium, 3 fast */
    int32 shine;        /* logo shine */
    int32 index;        /* folder index files */
    int32 remember;     /* start in the last folder */
    int32 scanned;      /* the card scan was offered */
    int32 dirty;
} Settings;
extern Settings g_set;
void  lists_init(void);
void  lists_load(void);                         /* from the root of the drive the menu is in */
void  lists_save(int32 at_root);                /* writes what changed (at_root: the firmware is already there) */
int32 list_find(PathList *l, const char *path);
void  list_add_front(PathList *l, const char *path);
void  list_remove(PathList *l, int32 i);
int32 list_path_of(int32 i, char *out, int32 max);     /* path of folder entry i from the drive root (-1: the folder); -1 if too long */
int32 list_split(const char *path, char comps[][DIR_NAME_MAX], int32 maxc);   /* components; returns count */
const char *path_base(const char *path);
int32 note_save(int32 drive, const char *path);   /* the launched game, in NVRAM */
int32 note_load(int32 *drive, char *path, int32 max);

/* ---- tools (tools.c), the X-menu actions of the original menu ---- */
void  tool_storage_manager(void);
int32 tool_save_nvram(void);
int32 tool_load_nvram(void);
int32 tool_dump(int32 which);       /* 0 SYSROM, 1 FONTROM, 2 FMVROM, 3 RAM */
int32 tool_update_ode(void);
void  tool_device_info(char lines[8][40], int32 *n);
int32 tool_patches_applied(void);
void  tool_set_patches(int32 n);

/* ---- main.c ---- */

/* crc.c, rampatch.c (unchanged from the reconstruction) */
uint32 crc32_update(uint32 crc, const uint8 *buf, int32 len);
uint32 ram_crc_0x200(void);
int32  apply_ram_patches(int32 arg);

#endif
