/* h3do.h - host stand-in for the parts of Portfolio / lib3do the menu uses. Test harness only. */
#ifndef H3DO_H
#define H3DO_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

typedef signed char      int8;
typedef unsigned char    uint8;
typedef short            int16;
typedef unsigned short   uint16;
typedef int              int32;
typedef unsigned int     uint32;
typedef int32            s32;
typedef uint32           u32;
typedef int32            Item;
typedef int32            Err;
typedef int32            Coord;
typedef unsigned char    boolean;
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#define MEMTYPE_ANY   0
#define MEMTYPE_CEL   0x00040000
#define MEMTYPE_DRAM  0x00080000
#define MEMTYPE_VRAM  0x00010000
#define MEMTYPE_STARTPAGE 0x02000000
#define GetBankBits(a) 0
#define DI_TYPE_NTSC  1
#define DI_TYPE_PAL1  2
#define DI_TYPE_PAL2  3
#define LC_ISFOCUSED  1
#define LC_Observer   2
#define INITCEL_UNCODED   0
#define INITCEL_CODED     1
#define CREATECEL_UNCODED INITCEL_UNCODED
#define CREATECEL_CODED   INITCEL_CODED
#define CCB_LAST    0x40000000
#define CCB_NPABS   0x20000000
#define CCB_BGND    0x00000020
#define PRE0_BGND   0x40000000
#define PIXC_OPAQUE 0x1F001F00

typedef void CelData;
typedef struct CCB CCB;
struct CCB {
    uint32   ccb_Flags;
    CCB     *ccb_NextPtr;
    CelData *ccb_SourcePtr;
    void    *ccb_PLUTPtr;
    Coord    ccb_XPos, ccb_YPos;
    int32    ccb_HDX, ccb_HDY, ccb_VDX, ccb_VDY, ccb_HDDX, ccb_HDDY;
    uint32   ccb_PIXC, ccb_PRE0, ccb_PRE1;
    int32    ccb_Width, ccb_Height;
    /* host-only: what InitCel put in the preamble words */
    int32    h_bpp, h_coded, h_rowbytes;
};

typedef struct Bitmap { uint8 *bm_Buffer; } Bitmap;

typedef struct ScreenContext {
    Item   sc_ScreenGroup;
    uint32 sc_DisplayType, sc_NumScreens, sc_CurrentScreen;
    Item   sc_ScreenItems[2];
    Item   sc_BitmapItems[2];
    Bitmap *sc_Bitmaps[2];
    uint32 sc_NumBitmapPages;
    uint32 sc_NumBitmapBytes;
} ScreenContext;

typedef struct ControlPadEventData { uint32 cped_ButtonBits; } ControlPadEventData;

void  *AllocMem(int32 size, uint32 type);
void   FreeMem(void *p, int32 size);
Item   LoadProgram(char *path);
Item   GetVRAMIOReq(void);
Err    OpenGraphicsFolio(void);
int32  GetDisplayType(void);
Item   CreateBasicDisplay(ScreenContext *sc, uint32 type, uint32 n);
Err    DeleteBasicDisplay(ScreenContext *sc);
Item   GetVBLIOReq(void);
Err    WaitVBL(Item io, uint32 n);
void  *LoadImage(char *path, uint8 *dest, void **vdl, ScreenContext *sc);
void   UnloadImage(void *img);
boolean DrawImage(Item screen, uint8 *img, ScreenContext *sc);
Err    DisplayScreen(Item s0, Item s1);
Err    DrawCels(Item bitmap, CCB *ccb);
void  *LookupItem(Item i);
Item   CreateMsgPort(const char *name, uint8 pri, uint32 signal);
Item   FindMsgPort(const char *name);
void  *LoadFile(const char *path, int32 *size, uint32 mem);
void   UnloadFile(void *p);
int32  SaveFile(const char *path, void *buf, int32 size, int32 extra);
CCB   *CreateCel(int32 w, int32 h, int32 bpp, int32 opt, void *data);
CCB   *CreateBackdropCel(int32 w, int32 h, int32 color, int32 opacity);
int32  InitCel(CCB *c, int32 w, int32 h, int32 bpp, int32 opt);
Err    OpenMathFolio(void);
Err    CopyVRAMPages(Item io, void *dest, void *src, uint32 pages, uint32 mask);
Err    SetVRAMPages(Item io, void *dest, int32 val, int32 pages, int32 mask);
Err    InitEventUtility(int32 pads, int32 mice, int32 focus);
Err    GetControlPad(int32 pad, int32 wait, ControlPadEventData *d);

/* harness */
int32  host_fw_cmd(const uint8 *cmd, uint8 *reply, int32 n, uint8 *data, int32 len);
void   host_log(const char *fmt, ...);

#endif
