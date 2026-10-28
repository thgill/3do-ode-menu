/* rampatch.c - reconstructed from LaunchMe 0x5200-0x5388 (object boundary: docs/objects.md).
 * File name, identifier names and parameter types are guesses; the code bytes are what is matched.
 */
#include "types.h"

extern int32 ramp_read(int32 unit, uint32 addr, void *buf, int32 len);     /* ode file, 0x2954 */
extern int32 ramp_write(int32 unit, uint32 addr, void *buf, int32 len);    /* ode file, 0x2a54 */
extern uint32 crc32_update(uint32 crc, const uint8 *buf, int32 len);       /* crc.c */

typedef struct RamPatch {
    uint32 crc;         /* ram_crc_0x200() of the running OS */
    uint32 addr;        /* RAM address to patch */
    uint32 old_value;   /* expected word at addr */
    uint32 new_value;   /* word written */
} RamPatch;

/* LaunchMe 0x5338 (const data: end of the code area). Terminated by crc == 0. */
static const RamPatch ram_patches[] = {
    { 0x00984c6a, 0x00005dfc, 0x98aecc85, 0xe3a00001 },
    { 0x00984c6a, 0x00005e00, 0xd6edc52d, 0xe1a0f00e },
    { 0x00984c6a, 0x00005df8, 0xfc747ccf, 0x00005dfc },
    { 0x00984c6a, 0x00005dd8, 0xc90a8c33, 0xf4840010 },
    { 0, 0, 0, 0 }
};

static uint8 ram_buf[0x200];    /* LaunchMe ZI 0x129f4 */

/* LaunchMe 0x5200: ~CRC-32 of RAM 0x200..0x3ff, 0 if it cannot be read */
uint32 ram_crc_0x200(void)
{
    int32 err;

    err = ramp_read(0, 0x200, ram_buf, 0x200);
    if (err != 0)
        return 0;
    return ~crc32_update(0xffffffff, ram_buf, 0x200);
}

/* LaunchMe 0x525c: apply every patch whose crc matches and whose old word is present; returns the number applied.
 * Caller: browser_init (XPort only), argument 1, unused here. */
int32 apply_ram_patches(int32 arg)
{
    int32 n = 0;
    uint32 crc;
    int32 i;
    uint32 v;

    crc = ram_crc_0x200();
    for (i = 0; ram_patches[i].crc != 0; i++) {
        if (ram_patches[i].crc == crc) {
            ramp_read(0, ram_patches[i].addr, &v, 4);
            if (ram_patches[i].old_value == v) {
                v = ram_patches[i].new_value;
                ramp_write(0, ram_patches[i].addr, &v, 4);
                n++;
            }
        }
    }
    return n;
}
