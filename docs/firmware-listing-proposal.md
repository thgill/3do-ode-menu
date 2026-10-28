# Proposal: linear folder listing in the ODE firmware (not built, not tested)

## The cost today

The menu lists a folder with D1. For every entry, the firmware's `dir_get_nth_entry(n)` opens the folder and
reads from the first entry until it reaches entry `n`. The source is `source/src/ode.c` in
fixel-3do-fz1-ode-decomp. Listing a whole folder of N entries therefore costs about N×N/2 directory reads.

| Folder | Reads to list it once |
|---|---|
| 100 entries | about 5,000 |
| 500 entries | about 125,000 |
| 1,500 entries | about 1,130,000 |

The revised menu hides this cost on return visits with its `_odemenu.idx` index, but the first visit still pays it.

## The change

Keep the directory open between calls and continue from where the last call stopped, whenever the next request is
for a later entry. The menu always asks in increasing order, so a full listing becomes one pass.

```c
static DIR nth_dir;
static int nth_open;      /* nth_dir is open on the current folder */
static int nth_next;      /* index (1-based, visible entries only) that the next readdir will produce */

void dir_listing_reset(void)
{
    if (nth_open)
        f_closedir(&nth_dir);
    nth_open = 0;
}

int dir_get_nth_entry(int n, FILINFO *fi)
{
    if (!n || !fi)
        return 0;
    if (!nth_open || n < nth_next) {
        dir_listing_reset();
        if (f_opendir(&nth_dir, ".") != FR_OK)
            return 0;
        nth_open = 1;
        nth_next = 1;
    }
    while (f_readdir(&nth_dir, fi) == FR_OK) {
        if (fi->fname[0] == 0)
            break;
        if (!ode_dir_list_filter(fi) && nth_next++ == n)
            return 1;
    }
    dir_listing_reset();
    return 0;
}
```

## Where the cached position must be dropped

`dir_listing_reset()` has to be called wherever the current folder, or the contents of the current folder, can
change:
- C0, and anything else that calls `ode_cd`, `f_chdir` or `f_chdrive`.
- E0, and E1 in write mode (both create files).
- Drive mount and unmount.
- Firmware update staging.

## Open questions

These need checking before it goes on a board:
- `ode_dir_list_filter` compares the global `g_fno` for "System Volume Information", not its argument. Keep
  that behaviour, or fix it in the same change.
- `FF_FS_LOCK` is 0 in the firmware's `ffconf.h`, so an open DIR takes no lock slot. It must not be left
  open across a file create or delete in the same folder, which the reset points above cover.
- Test on the Rev B board, together with the recovery path in `docs/update.md`.

The revised menu needs no change to benefit: its scan and its index checks simply become fast.
