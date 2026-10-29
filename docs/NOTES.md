# Technical notes

The README covers using the menu. This is the detail behind it: how things work, what was measured on an FZ-1,
and why some things are the way they are.

The menu is a rewrite of `LaunchMe` on Fixel's menu disc (`boot.iso`, V5), started from the byte-exact
reconstruction of Fixel's menu. Everything else on the disc stays as Fixel shipped it.

## Browsing and speed

**Browsing**
- Folders are sorted: folders first, then images, in natural order (`Disc 2` before `Disc 10`).
- Holding a direction scrolls, and it speeds up the longer you hold it. The old menu needed one press per row.
- **L / R** switch tabs. **Left / Right** jump to the previous or next starting letter.
- **B** goes up a folder, and the cursor lands back on the folder you came from.
- The footer shows where you are (`/3DO/USA/`); chips under the panel show the position (`8/114`), the drive
  and the queue. Long names scroll on the selected row.
- With two drives, the top level shows *SD card* and *USB drive*.
- The menu always starts in the drive root; the last game played is at the top of RECENT.
- **A game in its own folder is the game.** A folder holding exactly one `.iso` or `.cue` (the `.bin` files don't
  count) and no folders is listed, sorted and started as that game, and goes into FAVS, RECENT and the queue like
  one. Its dialog adds OPEN FOLDER, for anything else kept with it. Folders with more games, or with folders in
  them, open as before. The menu looks inside each folder once (on the first visit, or during the card scan) and
  keeps the answer in the folder index.
- **Games on several discs.** Starting disc 1 of a game whose discs sit side by side (`Name (Disc 1)`,
  `Name (Disc 2)`..., as files or as game folders; "Disk" and "CD1" work too) offers START WITH ALL DISCS, which
  queues the other discs behind it, or DISC 1 ONLY.
- Files the menu has no use for (the firmware lists any file whose extension isn't three letters, like `.jpeg`)
  are not shown.

**Speed**
- The firmware lists folders slowly. For every entry the menu asks for, it reopens the folder and walks it from
  the start. A page near the end of a 1,500-file folder costs about 21,000 directory reads.
- The new menu reads a folder once and saves the sorted listing in a hidden `_odemenu.idx` file in that folder.
  The firmware hides names starting with `_`, so the file never shows in the menu. The next visit loads that
  file with the fast sequential file commands and checks it against the card with a few probes.
- Simulated cost for a 1,500-file folder: about **3,800** directory reads on a return visit, against about
  **1,130,000** to page through it in the stock menu. A 114-file folder costs a few hundred reads on return.
- Safety: before anything is queued, launched or entered, the menu re-checks that entry's position on the card.
  If the card changed behind the index, it rescans and carries on with the same name. It never acts on a stale
  position.
- No debug output on every ODE transfer. The ODE commands reuse their I/O requests instead of creating and
  deleting one per command.

**Look**
- 320x240 laid out on PicoPak-SMS's 8-pixel grid: logo, tab strip, a rounded panel with an 18-row list, footer
  and chips. 8x8 capitals (Press Start 2P bitmaps, as on PicoPak-SMS) and PicoPak-SMS's icons, badges and pad
  buttons.
- Tabs: **GAMES** (the card), **FAVS** (favorites), **RECENT** (last played, newest first), **TOOLS**, **OPTIONS**.
- An **info pane** beside GAMES, FAVS and RECENT shows the selected game's screenshot, its name, the image type
  (ISO / CUE badge), whether it is a favorite and its place in the play queue. **C** shows or hides it; without
  it the list is full width with type badges on every row.
- **A on a game opens a dialog**: START GAME, ADD TO / REMOVE FROM FAVS, ADD TO QUEUE, and START / CLEAR QUEUE
  once something is queued; a game folder adds OPEN FOLDER. RECENT adds REMOVE FROM LIST.
- The selection bar's color drifts round a ten-color ring in 180 steps (OPTIONS: SLOW 3 s, MED 1.5 s, FAST 1 s,
  or OFF). OPTIONS also has an optional shine across the logo (off by default).
- Dialogs dim everything behind them; reading a folder, dumps, NVRAM and launching show a progress box.

**TOOLS** keeps every original tool and works on the folder open in GAMES (shown in the footer): rescan this
folder, **scan card (games + art)**, clear queue, save / load NVRAM here, Storage Manager, dump System / Font /
FMV ROM and RAM, update ODE firmware, device info. **OPTIONS**: info pane, bar color cycle, logo shine, folder
index, and About.

**Card scan.** Offered once at the first start, and in TOOLS. It visits every folder of the drive, so each
folder's index knows which of its folders are games, and gets every cover picture it finds ready as an `.img`.
Folders already indexed and covers already made are skipped, so running it again only does what's new. B stops
it. It needs OPTIONS > FOLDER INDEX on.

## Artwork

While browsing, the menu opens exactly one file per game, its `.img`: the picture shrunk whole to fit
the info pane's 88x122 with its shape kept, in its own 256 colors (a US long box 63x122, 8.2 KB; a European case
86x122, 11 KB; a square Japanese cover 88x88, 8.2 KB; a 4:3 screenshot 88x66, 5.8 KB; the older 15-bit 88x66
`.img` files still work):

- a game in its own folder (`3DO/Doom/`): `Doom/_cover.img`;
- a game file among others: `_art/<game name>.img` beside it (`Doom (USA).iso` uses `_art/Doom (USA).img`).

How the pane lays it out depends only on the picture's height. Box art taller than 100 lines (long boxes,
European cases) fills the pane, centered, with dark sides beside a narrow box, and the CUE / favorite / queue badges on a darkened strip along its foot (the name is in the list).
Shorter pictures (square and wide covers, screenshots) sit at the top with the name under them, a word per line;
the badges go under the name when there are 40 lines to spare, else on the picture's foot.

No `.img`: NO ART. Nothing is decoded or written while browsing. (Measured on an FZ-1: a file that isn't there
costs about 0.08 s to look for; writing the card costs about 2.3 ms per 4 bytes, because the firmware only takes
4 bytes per command, so one 6.2 KB `.img` takes about 3.5 s to save.)

**Making the `.img` files**, from a `.jpg` or `.png` named after the game file (`Doom/doom_1994.jpg` beside
`doom_1994.cue`, or `_art/Doom (USA).png`) or, in a game folder, after the folder (`Doom/Doom.jpg`):

- **Downloaded, no install:** open `tools/covermaker.html` (the 3DO ODE Cover Maker) in Chrome or Edge on a PC,
  choose the SD card, check the plan and press MAKE COVERS. It does what `covermaker.py` does below, inside the
  browser, using libretro's GitHub copy of the thumbnails. If the browser won't let you choose the whole card,
  choose the folder holding your games instead. Firefox, Safari and phones can't write to a folder, so they can't run it; Brave can once `brave://flags/#file-system-access-api` is turned on.
  It also writes every folder's index, the same bytes the console's SCAN CARD writes, so no folder needs its slow
  first listing (the index numbers entries in the order the browser lists them, which on FAT32/exFAT is the order
  the firmware uses; if a card disagrees, the menu notices and lists that folder itself, as with any stale index).
  An index the console already made is left alone. Games it finds no picture for get FIND (another name in
  libretro's collection), PICTURE (a file from the PC, or drop one) and WEB (an image search in a new tab).
- **Downloaded, any computer with Python (Firefox/Safari users):** `python3 tools/covermaker.py` asks for the card
  (drag it onto the window), shows the plan and asks before writing. It does everything the web page does: your
  own pictures first, then libretro's (`--picture box|screenshot|title`, fallbacks `--then`, `--then2`, "none" to
  stop; default box art, then screenshot, then title screen), the folder indexes (byte for byte the console's;
  `--no-index` to skip) and the first-start mark. `--dry-run` shows the plan only, `--redo` remakes covers,
  `--yes` asks nothing, and `--fix` goes through the games with no picture: search under another name, give a
  picture file, or open an image search in the browser. Needs Python 3.8+ and Pillow (`pip install pillow`);
  downloads are cached. The pictures belong to the publishers: fine for your own card, don't redistribute them.
- **On a PC (recommended for many games):** `python3 tools/mkart.py --card /path/to/sd` does the whole card in
  seconds and keeps `.img` files that already exist (`--force` remakes them). Needs Python 3 and Pillow.
- **On the console:** TOOLS > SCAN CARD (also offered at the first start). It decodes each picture on the
  console and saves the `.img`: expect about 5 seconds per cover, so it suits a few new games better than
  a whole collection. Games that already have an `.img` are skipped; B stops it.

Pictures are shrunk whole to fit 88x122 (nothing is trimmed), lightly sharpened and reduced to their own 256
colors (median cut; at this size the difference can't be seen). libretro's box art PNGs (512 pixels wide, up to
1051 tall) are too big for the console's own decoder: convert them on a PC (Cover Maker, covermaker.py, mkart.py). JPG: baseline
or progressive (progressive is decoded by the menu's own small decoder, `src/jpgprog.c`, which keeps only the
detail a cover needs); PNG: any color type and bit depth, not interlaced, up to about 1 MB unpacked.
Files over 512 KB are skipped. A picture that can't be used is counted in the scan's summary.

`.img` files can also be made one at a time: `python3 tools/mkart.py doom.png "Doom (USA).img"`.

A game without a picture shows a disc. The menu reads the picture once the selection rests (about 0.2 s) and
keeps the last eight in memory, reading the ones around the selection ahead.

## Fixes to the original tools

- **Load NVRAM** asks first. It then reads the whole file and checks it:
  - the file must be exactly 32 KB;
  - it must start with an Opera volume label.

  Only then does it unmount NVRAM and write it. The original wrote its buffer even after a failed read.
- **Save NVRAM** asks before overwriting an existing `nvram.nvm`, and refuses to save console memory that isn't a
  formatted NVRAM.
- **Device errors now count.** The original never saw errors from the `ramp` device: it returned the address of
  its error variable instead of the error. Now a failed ROM or NVRAM read or write is reported, not saved as a
  good file.
- **Update ODE** asks first. After the ODE accepts the update, the menu stops on a "turn the console off and on"
  screen, because the ODE resets itself to flash.
- **Dumps** can be canceled with B.
- **File tools at the device list** are refused (choose a drive first), because the firmware would write into a
  leftover folder.
- `nvram.nvm` is matched without regard to case.

**Firmware folder handling.** The firmware's "up" command is not clamped at the drive root: one "up" too many
empties its path, and file access stays broken until power-off. The menu therefore:
- sends exactly as many "up" commands as it went down (a game folder is entered just before its launch and left
  again if the game doesn't start);
- at startup, when the firmware was left in a folder (for example by a console reset while browsing), searches
  upward only if it knows the root holds `boot.iso`. Otherwise it stays put, as the stock menu did.

The hidden extras (both Konami codes, the eject-count picture, and the XPort RAM patch) are kept unchanged.

## Hardware findings (FZ-1, October 2026)

- **Two copies of the menu.** `AppStartup` runs `$boot/LaunchMe`, and the system starts it again after the script,
  so two copies ran side by side. With the stock menu that is invisible; with an animated menu the display jumps
  between both copies' screens. The menu now registers a named message port and every copy but the first quits.
- **One IOReq per ODE command**, as in the stock menu. A reused IOReq stays "in progress" in the ode driver and the
  next command fails with ER_IONotDone (seen as "could not open this folder").
- **Nothing written on the card at launch.** Card writes (or the trip to the root to make them) just before C3/C4/C5
  made games hang after the 3DO logo, or start only sometimes. A launch now sends C3, C4, C5 only; the launched game
  is noted in NVRAM (`/NVRAM/ODEMenu`, 256 bytes) and goes into RECENT at the next menu start. A launch also waits
  until 3 s have passed since the last card write (folder index, favorites, settings).
- **The math folio must be opened** before lib3do's cel mapping calls (`CreateBackdropCel` crashed without it);
  the menu now builds its rectangle cel itself and opens the folio anyway.
- **The boot banner is required**: a disc without `BannerScreen` goes to "insert disc". The menu disc carries its
  own banner (`tools/mkbanner.py`).
- **Frames wait for the vertical blank** before drawing, so the buffer being drawn is never the one on screen.
- **Key repeat runs on video fields**, so holding a direction scrolls at the same speed whatever the frame rate.
- **`gf_VBLNumber` is not a field count.** The graphics interrupt forces its lowest bit to match the video field,
  so on the 3DO's progressive picture it goes up by 2 per field. Everything timed by it ran at double speed,
  including the launch's settle wait (1.5 s instead of 3 s), and games stopped booting. At start the menu
  now measures its step over 8 waited fields and divides by it.
- **Clones may answer a one-entry listing differently.** A tester with a clone ODE (which runs Fixel's stock menu
  fine) got "That item is no longer on the card" for every game: the menu checks an entry with a one-entry D1
  before acting on it, which the stock menu never does. The menu now tries that once at startup (counts 1, 2 and
  14) and, if no answer matches the listing, works without it: no folder index, entries trusted from a fresh
  read of the folder. TOOLS > DEVICE INFO shows the result on its last line.
  The same tester's clone (seen with ODE TEST) puts a drive number in the top byte of every entry id
  (`0x01000005` for entry 5, `0x01ffffff` for `..`) and ignores the "up" id `0xffffffff` that Fixel's firmware
  uses. The menu now takes the "up" id from the folder's own `..` entry, treats any id ending in `ffffff` as `..`,
  asks D1 for the entry's position (the id's low bits), and stores only the entry number in the folder index,
  putting the drive byte back when it reads it. The simulator's `FWSIM_CLONE=4` behaves like that clone.
  It also answers D1 index n with the entry after it (index 0 is `..` then the first entry), so the menu measures
  that on the first folder it reads ("D1 CHECK OK (COUNT n, SHIFTED)" in DEVICE INFO) and pages by entry
  position; `FWSIM_CLONE=5` adds that. And it reset into a black screen with one game when a game folder was
  started from the list, while picking the file inside worked, so a game folder is now started exactly that way:
  the menu opens the folder as usual and starts the game from its listing (RECENT still records the folder).
  Its file reads also seem not to end: covers showed NO MEMORY and RECENT filled with copies, as if a read at the
  end of a file answered the full length again. A `.img` is now read only up to its own size, other reads stop
  when a chunk repeats the one before, and a list file stops at a second header and keeps each path once
  (`FWSIM_CLONE=6`). And the NVRAM launch note is emptied once it has gone into RECENT, so a game removed from
  RECENT stays removed.
  TOOLS > ODE TEST shows the raw answers to the commands the menu relies on (into a folder and back up, into a
  game folder, a queue entry that is cleared again, single-entry listings), for a photo when a clone misbehaves.
- **The clones seen so far run firmware based on FCare's SataTo3DO** (RP2040, MIT,
  <https://github.com/FCare/SataTo3DO>). Every oddity above is in its source, and so are a few more. The simulator's
  `FWSIM_CLONE=7` (`host/fwsim_sata.c`) follows that source; the menu passes the whole test suite against it.
  What the firmware does, and what the menu does about it:
  - **`C0 0` sets the drive byte only once.** After power-on the firmware's current id is 0, and `C0 0` makes it
    `drive << 24`. Sent again at the root, it opens the drive afresh with current id 0: every id has drive byte 0
    from then on, and no game starts (the firmware looks the game's drive up from that byte) until power-off. The
    stock menu sends `C0 0` at every start, so a console reset (not power) can break the stock menu too. The menu
    lists the folder first and sends `C0 0` only if the ids carry no drive byte yet (Fixel's firmware, or a clone
    just switched on).
  - **`C0` to the current id is ignored.** The firmware keeps the id of the last folder it entered; a subfolder
    with the same id (entry 1 inside a folder that was entry 1) does not open. The menu tracks that id and first
    sends `C0` to another entry of the folder (a file only becomes the current id; a folder is entered and left).
    A folder whose only entry is such a folder can not be opened at all: the menu says so instead of going on as
    if it had (which once left it a folder off from the firmware). After a console reset the menu does not know
    the firmware's id, so it does this before the first folder it opens.
  - **The firmware keeps its folder through a console reset**, and its EXT_ID always says the menu disc came from
    drive 1. So at startup the menu follows a `..` that carries a drive byte up to the root (it stays put only on
    Fixel's firmware with an unknown boot source).
  - **`C4` queues only an entry of the last listing.** It looks the id up in the buffer of the last `D1` answer,
    not on the card. The menu lists the entry on its own just before each `C4` (the stock menu always queues from
    the page it has just listed).
  - **The root has no `..`, folders do.** D1 index 0 inside a folder answers `..` first; index n answers from entry
    n + 1. At the root index n answers from entry n + 1 as well. The menu measures the shift on the entry with the
    highest number (the first entry answers the same either way) and tries both settings.
  - **Listings show only folders, `.cue` and `.iso`**, and folders whatever their name: `_art` was listed. The menu
    hides names starting with `_` itself.
  - **Opening a file creates it.** `E1` opens read/write with "open always", so looking for a cover that is not
    there leaves an empty one. An empty cover counts as none, in the menu and in both Cover Makers. The card
    scan does not look for `.jpg`/`.png` pictures on a clone (each name tried would leave an empty file, and the
    ODE has no delete); covers come from the Cover Maker there. `boot.iso` can not be opened at all (the firmware
    has it open as the running disc), so the drive root is found by `boot.iso` being listed, not opened.
  - **A read answers the length asked for, even past the end**, with the buffer's old contents (often another
    file). Before each read the menu puts 8 marker bytes at the start of the buffer (2 `E6`); if they are still
    there after the read, nothing was read.
  - **Writes never shorten a file**, so every file the menu writes says where it ends: list files end with
    `-END-`, indexes and covers carry their size.
  - Not modelled yet: what the firmware does once the last queued disc has loaded (it clears its folder state), so
    coming back to the menu after a launch that did not boot is untested on the clone model.
- **Writing the card is slow**: the firmware takes data from the 3DO only 4 bytes per command (E6), about
  2.3 ms each, so writes run at about 1.7 KB/s (a 8.4 KB folder index took 4.9 s). Reading is fast (2 KB per
  command); a file that isn't there costs about 0.08 s to look for. So the menu writes as little as it can, and
  never while browsing.
- **The launch waits for an idle card**: 1.5 s since the last ODE command of any kind and 3 s since the last write.
- The ODE's access LED blinks while the menu is idle because the 3DO polls its drive; the menu itself sends no
  commands when idle.

## Card files

To read or write the root files from inside a folder, the menu takes the firmware up to the root and back
down, the same way it does around a launch. It doesn't use `../` paths, because FatFs can't follow `..` on exFAT
cards. With two drives, each drive keeps its own files. Changes you make on the device list are written to their
drive when you next enter one.

## Limits in more detail

- **First visits to huge folders are still slow.** Listing a folder once costs the firmware N×N/2 directory reads.
  Subfolders of a few hundred games are fine. B stops a long scan and shows what was read so far. The firmware
  fix is to keep the directory open between consecutive entries in `dir_get_nth_entry`; that would make every
  listing linear.
- Names are shown in capitals (the face has no lower case); accented letters lose their accents and characters
  outside Latin-1 (for example Japanese) show as `?`.
- The whole screen is redrawn every frame while the bar color cycles. The frame starts with a VRAM fill that
  runs in the vertical blank, which also paces the menu. If that assumption is wrong on hardware the cycle runs
  faster than set; OPTIONS > BAR COLOR CYCLE > OFF redraws only on changes.
- The layout keeps 16 lines clear at the top and bottom of the 240-line picture (TVs hide part of the edges;
  the first layout's logo started on line 2 and was cut off on CRTs). OPTIONS can't move it.
- Up to 4,096 entries per folder (fewer if memory is short), names up to 127 characters (the firmware's limit).
- PAL consoles get the same 320×240 layout as the original.

## Source layout

| Path | What |
|---|---|
| `src/` | The menu. `crc.c` and `rampatch.c` are unchanged from the reconstruction |
| `tools/mkui.py` | Builds `src/uiart.h` (face, icons, micro letters) from PicoPak-SMS's `menu/` and `assets_gen/logo.img` from the logo PNG |
| `tools/mkart.py` | PC tool: pictures -> `.img` covers (one, a folder, or the whole card) |
| `tools/covermaker.html` | Cover Maker web page: covers and folder indexes in Chrome/Edge, nothing to install |
| `tests/web_index_test.sh` | checks the web page's and covermaker.py's folder indexes byte for byte against the menu's own |
| `tools/covermaker.py` | the Cover Maker in Python, for any computer: covers and folder indexes |
| `build_3do.sh` | Compile and link with the 3do-devkit (`DEVKIT=/path/to/3do-devkit`) |
| `make_disc.sh` | Original `boot.iso` + new files -> new signed `boot.iso` (uses `3dt`) |
| `host/`, `tests/run_tests.sh` | PC harness: the real menu code against a lib3do stand-in and a firmware simulator, with screenshots |
| `docs/firmware-listing-proposal.md` | Untested proposal: make the firmware's folder listing linear |
| `tools/elf32run.c` | Runs the devkit's 32-bit Linux `armcc`/`armlink` on a 64-bit system without 32-bit libraries |

The compile flags follow the devkit's release settings (`-O2 -zpno_check_stack`, APCS 3/32/nofp/swst/wide/softfp).
The disc is packed fresh by `3dt` with the original volume label and a BILLSTUFF ROM tag. That is the same signing
scheme `3dt` uses when it re-signs the original layout, which is how the reconstruction's modified menu disc was
booted on an FZ-1.

## License details

- `src/` started from a reconstruction of Fixel's menu, whose license is still "TBD"; it is published in line with
  Fixel's stated aim of open-sourcing the menu. Nothing here grants rights to Fixel's software or to the 3DO SDK,
  and no license is given for `src/` until that is settled.
- `boot.iso` is not in this repository: it is built from Fixel's original disc (3DO system files, Fixel's ODE
  driver, the 3DO SDK's Storage Tuner) with `make_disc.sh`.
- The reconstruction's own tools are licensed for non-commercial use only.
- The 3do-devkit ships ARM SDT and 3DO SDK material that belongs to its owners.
- The 8x8 face is Press Start 2P (c) 2011 Cody "CodeMan38" Boisclair, SIL Open Font License 1.1, taken from
  PicoPak-SMS's extraction. The bitmaps are a Modified Version: they must not be called "Press Start" (a Reserved
  Font Name), and the notice and license must travel with them. `OFL.txt` is on the menu disc and About credits it.
- Icons, badges and pad buttons are PicoPak-SMS's own drawings (Todd's repo). The logo is Todd's.
- PNG decoding uses tinf 1.2.1 by Joergen Ibsen (zlib license, altered as in PicoPak-SMS); JPG decoding uses
  picojpeg 1.1 by Rich Geldreich (public domain / MIT), altered only to build with Norcroft and to fix one
  undefined evaluation order. Both are in `src/third/` with `README.third` and the tinf license.
- Screenshots you put in `_art/` belong to the games' publishers: fine on your own card, not for sharing.
