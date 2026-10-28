/* input.c - control pad with auto-repeat.
 * The original menu waited for every button to be released before reading the next press, so a list could only
 * be moved one row per press. Here directions repeat while held and speed up the longer they are held.
 * Called once per video field (about 60 Hz NTSC / 50 Hz PAL).
 */
#include "common.h"

#define REPEAT_MASK   (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT | BTN_L | BTN_R)

static uint32 prev;
static uint32 blocked;      /* buttons that must be released first */
static int32  hold_frames;
static uint32 hold_btn;
static int32  next_fire;
static uint32 last_pressed;
static uint32 last_vbl;
static int32  repeats;          /* how many times the held direction fired in the last poll */

int32 input_init(void)
{
    prev = 0;
    blocked = 0;
    hold_frames = 0;
    hold_btn = 0;
#ifdef HOST_BUILD
    return 0;
#else
    /* observer, like ExamplesLib's InitControlPad used by the original: events arrive even while another
     * program (Storage Manager) has the focus */
    return InitEventUtility(1, 0, LC_Observer);
#endif
}

static uint32 read_raw(void)
{
    ControlPadEventData cp;

    cp.cped_ButtonBits = 0;
    if (GetControlPad(1, 0, &cp) < 0)
        return 0;
    return cp.cped_ButtonBits;
}

void input_flush(void)
{
    blocked = read_raw();
    prev = blocked;
    hold_btn = 0;
    hold_frames = 0;
}

int32 input_fast(void)
{
    return hold_btn != 0 && hold_frames > 40;
}

/* Timing runs on video fields (gfx_vbl_count), not on polls: the menu polls once per drawn frame, and a frame
 * can take more than one field. When several repeats fall due in one poll they are all counted, so holding a
 * direction moves at the same speed however long a frame takes. */
uint32 input_poll(uint32 *held)
{
    uint32 now = read_raw();
    uint32 pressed, events, dir, vbl = gfx_vbl_count();
    int32 interval, elapsed;

    elapsed = (int32)(vbl - last_vbl);
    last_vbl = vbl;
    if (elapsed < 1)
        elapsed = 1;
    if (elapsed > 10)
        elapsed = 10;
    repeats = 0;
    blocked &= now;
    now &= ~blocked;
    pressed = now & ~prev;
    events = pressed;
    if (pressed & REPEAT_MASK)
        repeats = 1;

    /* one repeating button at a time: the most recently pressed direction wins */
    dir = pressed & REPEAT_MASK;
    if (dir) {
        hold_btn = dir & (~dir + 1);    /* lowest set bit */
        hold_frames = 0;
        next_fire = (hold_btn & (BTN_L | BTN_R | BTN_LEFT | BTN_RIGHT)) ? 20 : 15;
    } else if (hold_btn && (now & hold_btn)) {
        hold_frames += elapsed;
        while (hold_frames >= next_fire) {
            events |= hold_btn;
            repeats++;
            if (hold_btn & (BTN_UP | BTN_DOWN)) {
                /* 15 fields delay, then 20 rows a second, 30 after 0.6 s, 60 after 1.4 s */
                interval = hold_frames > 84 ? 1 : (hold_frames > 36 ? 2 : 3);
            } else {
                interval = hold_frames > 80 ? 3 : 6;
            }
            next_fire += interval;
        }
    } else {
        hold_btn = 0;
        hold_frames = 0;
    }
    prev = now;
    last_pressed = pressed;
    if (held)
        *held = now;
    return events;
}

uint32 input_pressed(void)
{
    return last_pressed;
}

int32 input_repeats(void)
{
    return repeats > 0 ? repeats : 1;
}

/* asked during long jobs (decoding a picture): a fresh press means the user has moved on */
int32 input_interrupt(void)
{
    static uint32 last_check = 0xffffffff;
    uint32 v = gfx_vbl_count();

    if (v == last_check)
        return 0;
    last_check = v;
    return (read_raw() & ~prev & ~blocked) != 0;
}
