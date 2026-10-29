#!/bin/sh
# Host regression tests: real menu code + firmware simulator. Usage: tests/run_tests.sh
set -u
cd "$(dirname "$0")/.."
ROOT=$(pwd)
M=$ROOT/build/host/menu_host
T=$(mktemp -d)
FAIL=0
case "${FWSIM_CLONE:-0}" in 4|5|6|7) CLONE=1 ;; *) CLONE= ;; esac    # clone ODE models (see host/fwsim*.c)
ok()   { echo "PASS  $1"; }
bad()  { echo "FAIL  $1"; FAIL=1; }
run()  { o=$($M --sd "$T/sd" --out "$T" --disc "$T/disc" --script "$@" 2>&1); echo "$o"; echo "$o" | grep -q "path is now invalid" && echo "INVALID-PATH" >> "$T/invalid.log"; echo "$o" | grep "fails on exFAT" >> "$T/dotdot.log"; true; }

mkdir -p "$T/disc/assets" "$T/sd/Games/Sub"
touch "$T/sd/boot.iso"          # the menu disc itself, as on a real card
cp assets_gen/logo.img "$T/disc/assets/"
for i in $(seq 1 60); do touch "$T/sd/Games/Game $i (USA).iso"; done
touch "$T/sd/Games/Sub/Deep (USA).iso" "$T/sd/Games/Sub/Other (USA).iso"   # two games: an ordinary folder
printf 'OMO1\npane=1\nbar=1\nshine=0\nindex=1\nscanned=1\n' > "$T/sd/_odemenu.opt"   # the first-start scan offer was seen
LONG=$(python3 -c "print('L'*120)")
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do touch "$T/sd/Games/$LONG$i.iso"; done

# enter Games (first entry of root), wait, end
printf '20 pad A\n22 pad -\n40 end\n' > "$T/enter.txt"

out=$(run "$T/enter.txt")
r1=$(echo "$out" | sed -n 's/.* \([0-9]*\) directory reads.*/\1/p')
[ -f "$T/sd/Games/_odemenu.idx" ] && ok "index written on first visit" || bad "index written on first visit"
echo "$out" | grep -q " 0 D1 overflows" && ok "no D1 buffer overrun with 120-char names" || bad "D1 overrun"
out=$(run "$T/enter.txt")
r2=$(echo "$out" | sed -n 's/.* \([0-9]*\) directory reads.*/\1/p')
[ "$r2" -lt $((r1 / 4)) ] && ok "return visit cheaper ($r1 -> $r2 directory reads)" || bad "return visit cheaper ($r1 -> $r2)"

# sorted natural order: Game 2 before Game 10; A on row 3 (Game 2? rows: Sub, Game 1, Game 2 ...) plays Game 2
printf '20 pad A\n22 pad -\n30 pad D\n32 pad -\n34 pad D\n36 pad -\n40 pad A\n42 pad -\n50 pad A\n52 pad -\n600 end\n' > "$T/play.txt"
out=$(run "$T/play.txt")
echo "$out" | grep -q "C5 launch: .*Games/Game 2 (USA).iso" && ok "natural sort + play sends the right image" || { bad "play right image"; echo "$out" | grep C5; }
grep -q "^Games/Game 2 (USA).iso$" "$T/disc/nvram_ODEMenu" && ok "launch noted in NVRAM, nothing written on the card at launch" || bad "launch note"

# reboot: the menu starts in the drive root (no resume), and the launch is in RECENT
printf '20 shot boot\n30 pad r\n32 pad -\n36 pad r\n38 pad -\n50 pad A\n52 pad -\n60 pad A\n62 pad -\n700 end\n' > "$T/resume.txt"
out=$(run "$T/resume.txt")
echo "$out" | grep -q "C5 launch: .*Games/Game 2 (USA).iso" && ok "after a launch the menu starts in the root; RECENT starts the game again" || { bad "recent after reboot"; echo "$out" | grep -E "C5|SaveFile"; }

# folder changed behind the index (rename shifts order): queue still gets the right image
mv "$T/sd/Games/Game 1 (USA).iso" "$T/sd/Games/Game 1 renamed to something much longer (USA).iso"
printf '20 pad A\n22 pad -\n50 pad D\n52 pad -\n54 pad D\n56 pad -\n60 pad A\n62 pad -\n70 pad D\n72 pad -\n74 pad D\n76 pad -\n80 pad A\n82 pad -\n140 end\n' > "$T/queue.txt"
out=$(run "$T/queue.txt")
echo "$out" | grep -q "C4 queued: Game 2 (USA).iso" && ok "stale index: action goes to the right image after rescan" || { bad "stale index queue"; echo "$out" | grep C4; }

# a removed folder entry: entering by stale index must not open another folder
rm -f "$T/sd/_odemenu.cfg" "$T/disc/nvram_ODEMenu"
printf '20 pad A\n22 pad -\n30 pad A\n32 pad -\n60 end\n' > "$T/sub.txt"
run "$T/sub.txt" > /dev/null
mkdir "$T/sd/Games/AAA New Folder"
out=$(run "$T/sub.txt")
echo "$out" | grep -q "C5" && bad "entering a folder launched something" || ok "folder entry after change does not launch"

# percent signs and odd bytes in names are shown, not interpreted
touch "$T/sd/100% Fun %s %d (USA).iso"
printf '20 shot pct\n30 end\n' > "$T/pct.txt"
run "$T/pct.txt" > /dev/null && ok "names with %% render (see $T/pct.ppm)" || bad "percent names"

# firmware left deep in a folder by an earlier session (stock menu, or a launch that went wrong):
# startup must find the drive root without going above it
rm -f "$T/sd/_odemenu.cfg" "$T/disc/nvram_ODEMenu"
printf '20 pad A\n22 pad -\n30 pad D\n32 pad -\n40 pad A\n42 pad -\n50 pad A\n52 pad -\n60 pad A\n62 pad -\n600 end\n' > "$T/deep.txt"
out=$(run "$T/deep.txt" --start "Games/Sub")
echo "$out" | grep -q "C5 launch: .*Games/Sub/Deep (USA).iso" && ok "startup from a deep folder finds the root (Games > Sub > Deep)" || { bad "startup from deep folder"; echo "$out" | grep -E "C5|invalid"; }

# boot source unknown (flash image): the menu must not move the firmware, so it starts in the folder it was left in
printf '20 pad A\n22 pad -\n30 pad A\n32 pad -\n600 end\n' > "$T/flash.txt"
out=$(run "$T/flash.txt" --start "Games/Sub" --source 4)
# (Fixel's firmware only: a clone's ".." carries its drive byte and is followed up to the root; its EXT_ID always says 1)
if [ -z "$CLONE" ]; then
echo "$out" | grep -q "C5 launch: .*Games/Sub/Deep (USA).iso" && ok "unknown boot source: stays in the current folder, no overshoot" || { bad "unknown boot source"; echo "$out" | grep -E "C5|invalid"; }
fi

# a launch that does not boot: the menu waits for B, and must then still be in a valid folder (B, then play another)
printf '20 pad A\n22 pad -\n30 pad D\n32 pad -\n34 pad D\n36 pad -\n40 pad A\n42 pad -\n50 pad A\n52 pad -\n620 pad B\n622 pad -\n700 pad D\n702 pad -\n710 pad A\n712 pad -\n720 pad A\n722 pad -\n1330 pad B\n1332 pad -\n1400 pad B\n1402 pad -\n1410 pad A\n1412 pad -\n1420 pad D\n1422 pad -\n1424 pad D\n1426 pad -\n1430 pad A\n1432 pad -\n1440 pad A\n1442 pad -\n2200 end\n' > "$T/relaunch.txt"
rm -f "$T/sd/_odemenu.cfg" "$T/disc/nvram_ODEMenu"
out=$(run "$T/relaunch.txt")
l=$(echo "$out" | grep "C5 launch" | sed 's/.*C5 launch: //')
n=$(echo "$l" | grep -c .)
first=$(echo "$l" | sed -n 1p); third=$(echo "$l" | sed -n 3p)
[ "$n" = 3 ] && [ "$first" = "$third" ] && ! echo "$l" | grep -q "^ *$" && ok "after launches that do not boot, the menu stays in step (3 launches, back and in again)" || { bad "relaunch"; echo "$l"; }

# favorites: A on a game in a subfolder, ADD TO FAVS; the list is written in the drive root through "../"
rm -f "$T/sd/_odemenu.cfg" "$T/disc/nvram_ODEMenu" "$T/sd/_odemenu.fav"
printf '20 pad A\n22 pad -\n30 pad D\n32 pad -\n34 pad A\n36 pad -\n40 pad A\n42 pad -\n50 pad D\n52 pad -\n60 pad A\n62 pad -\n120 end\n' > "$T/fav.txt"
out=$(run "$T/fav.txt")
grep -q "^Games/Sub/Deep (USA).iso$" "$T/sd/_odemenu.fav" 2>/dev/null && ok "favorite written to the root's _odemenu.fav from a subfolder" || { bad "favorite file"; cat "$T/sd/_odemenu.fav"; echo "$out" | grep E1; }

# FAVS tab from the root: shoulder R, A, A starts the favorite, going down into its folder first
printf '20 pad r\n22 pad -\n30 pad A\n32 pad -\n40 pad A\n42 pad -\n600 end\n' > "$T/favplay.txt"
out=$(run "$T/favplay.txt")
echo "$out" | grep -q "C5 launch: .*Games/Sub/Deep (USA).iso" && ok "a favorite starts from the FAVS tab" || { bad "favorite start"; echo "$out" | grep -E "C5|C4|invalid"; }
echo "$out" | awk '/C3|C4/{c=1} /E1 open .* write/{if(c)w=1} /C5 launch/{exit} END{exit w}' && ok "no card write between building the queue and C5" || bad "card write right before launch"
printf '20 end\n' > "$T/boot.txt"
run "$T/boot.txt" > /dev/null
head -2 "$T/sd/_odemenu.rec" 2>/dev/null | grep -q "^Games/Sub/Deep (USA).iso$" && ok "the launch goes into RECENT at the next start" || { bad "recent file"; cat "$T/sd/_odemenu.rec"; }
grep -q "Deep" "$T/disc/nvram_ODEMenu" 2>/dev/null && bad "the NVRAM note stays after it went into RECENT (a removed game would come back)" || ok "the NVRAM note is emptied once it is in RECENT"

# artwork: the info pane opens _art/<name>.img beside the game once the selection rests
mkdir -p "$T/sd/Games/_art"
python3 -c "import struct;open('$T/sd/Games/_art/Game 2 (USA).img','wb').write(b'OART'+struct.pack('>HH',88,66)+b'\x7c\x00'*(88*66))"
rm -f "$T/sd/_odemenu.cfg" "$T/disc/nvram_ODEMenu" "$T/sd/Games/_odemenu.idx"
printf '20 pad A\n22 pad -\n80 pad D\n82 pad -\n84 pad D\n86 pad -\n88 pad D\n90 pad -\n160 shot art\n170 end\n' > "$T/art.txt"
out=$(run "$T/art.txt")
echo "$out" | grep -q "E1 open _art/Game 2 (USA).img read -> 1" && ok "artwork read from _art/ for the selected game" || { bad "artwork"; echo "$out" | grep E1; }
python3 -c "
d=open('$T/art.ppm','rb').read().split(b'\n',3)[3]
p=(90*320+260)*3
import sys; sys.exit(0 if d[p:p+3]==bytes((255,0,0)) else 1)" && ok "artwork drawn in the info pane" || bad "artwork drawn"

# PNG and JPG artwork: never decoded while browsing; the card scan (TOOLS) turns them into _art/<name>.img
pane_rgb() { python3 -c "
d=open('$1','rb').read().split(b'\n',3)[3]
p=(90*320+260)*3
r,g,b=d[p:p+3]
import sys; sys.exit(0 if eval('$2') else 1)"; }
# keys "A D D A" [gap] [tail]: one press per step, gap frames apart; "w" waits a step; "s<name>" takes a screenshot
keys() { python3 -c "
import sys
f=40; out=[]
for k in sys.argv[1].split():
    if k=='w': f+=$2; continue
    if k.startswith('s'): out.append('%d shot %s'%(f,k[1:])); f+=2; continue
    out.append('%d pad %s'%(f,k)); out.append('%d pad -'%(f+2)); f+=$2
out.append('%d end'%(f+$3))
print('\n'.join(out))" "$1"; }
# (clone modes: the card scan indexes folders but looks for no pictures, see CLONE at the top)
SCAN="r r r D A D A w w w w w w w w w w w w w w w w w w w w A l l l"     # TOOLS > SCAN CARD > YES, wait, close, back
rm -f "$T/sd/Games/_art/"*
python3 -c "from PIL import Image; Image.new('RGB',(320,240),(255,0,0)).save('$T/sd/Games/_art/Game 2 (USA).png')"
keys "A w D D D w w w w sartx" 30 30 > "$T/artx.txt"
out=$(run "$T/artx.txt")
pane_rgb "$T/artx.ppm" "not (r==255 and g==0 and b==0)" && ! echo "$out" | grep -q "E1 open _art/Game 2 (USA).png" && \
    ! echo "$out" | grep -v "_odemenu" | grep -q "E1 open .* write" && ok "browsing only opens the .img: no decoding, no writing" || { bad "browsing decodes"; echo "$out" | grep E1; }
art_try() {   # $1 file to write (made by PIL), $2 PIL command, $3 test on r,g,b, $4 label, $5 set: progressive JPG
    rm -f "$T/sd/Games/_art/"*
    python3 -c "from PIL import Image; $2.save('$T/sd/Games/_art/$1'${5:+, progressive=True})"
    rm -f "$T/disc/nvram_ODEMenu"
    keys "$SCAN w A w D D D w w w w sartx" 30 30 > "$T/artx.txt"
    out=$(run "$T/artx.txt")
    if [ -n "$CLONE" ]; then
        [ ! -s "$T/sd/Games/_art/Game 2 (USA).img" ] && ! echo "$out" | grep -q "E1 open _art/Game 2 (USA).png" && ok "$4 (clone: not looked for)" || { bad "$4"; echo "$out" | grep "E1 open _art"; }
        return
    fi
    pane_rgb "$T/artx.ppm" "$3" && head -c 4 "$T/sd/Games/_art/Game 2 (USA).img" 2>/dev/null | grep -q OAR8 && ok "$4" || { bad "$4"; echo "$out" | grep "E1 open _art"; }
}
art_try "Game 2 (USA).png" "Image.new('RGB',(320,240),(255,0,0))" "r==255 and g==0 and b==0" "scan: PNG -> .img (RGB 320x240)"
art_try "Game 2 (USA).png" "Image.new('RGB',(40,30),(0,0,255)).convert('P')" "b==255 and r==0" "scan: PNG (palette, smaller than the frame)"
art_try "Game 2 (USA).png" "Image.new('RGBA',(400,200),(0,255,0,255))" "g==255 and r==0" "scan: PNG (RGBA, wide: trimmed)"
art_try "Game 2 (USA).jpg" "Image.new('RGB',(320,240),(255,0,0))" "r>200 and g<60 and b<60" "scan: JPG (320x240)"
art_try "Game 2 (USA).jpg" "Image.new('L',(1024,768),200)" "abs(r-200)<12 and r==g==b" "scan: JPG (grayscale, large: 1/8 decode)"
art_try "Game 2 (USA).jpg" "Image.new('RGB',(600,450),(0,200,0)).convert('RGB')" "g>180 and r<40 and b<40" "scan: progressive JPG (600x450)" progressive
art_try "Game 2 (USA).jpg" "Image.new('L',(97,61),90)" "abs(r-90)<10 and r==g==b" "scan: progressive JPG (grayscale, small)" progressive
# a damaged file: no picture, no crash
rm -f "$T/sd/Games/_art/"*; head -c 3000 /dev/urandom > "$T/junk"; printf '\xff\xd8\xff\xc2' | cat - "$T/junk" > "$T/sd/Games/_art/Game 2 (USA).jpg"
out=$(run "$T/artx.txt")
echo "$out" | grep -q "script end" && [ ! -s "$T/sd/Games/_art/Game 2 (USA).img" ] && ok "scan: a damaged JPG: no crash, no .img" || bad "damaged JPG"
rm -f "$T/sd/Games/_art/"*

# settings: C hides the info pane and the choice is kept in the root's _odemenu.opt
printf '20 pad C\n22 pad -\n40 end\n' > "$T/opt.txt"
run "$T/opt.txt" > /dev/null
grep -q "^pane=0$" "$T/sd/_odemenu.opt" 2>/dev/null && ok "settings saved (C toggles the info pane)" || { bad "settings"; cat "$T/sd/_odemenu.opt"; }
printf '20 pad C\n22 pad -\n40 end\n' > "$T/opt.txt"
run "$T/opt.txt" > /dev/null

# artwork of a favorite in another folder: opened from the drive root, then back into the folder
mkdir -p "$T/sd/Games/Sub/_art"
python3 -c "import struct;open('$T/sd/Games/Sub/_art/Deep (USA).img','wb').write(b'OART'+struct.pack('>HH',88,66)+b'\x03\xe0'*(88*66))"
rm -f "$T/sd/_odemenu.cfg" "$T/disc/nvram_ODEMenu"
printf '20 pad A\n22 pad -\n90 pad r\n92 pad -\n150 shot favart\n160 pad l\n162 pad -\n170 pad D\n172 pad -\n180 pad A\n182 pad -\n190 pad A\n192 pad -\n200 pad A\n202 pad -\n760 end\n' > "$T/favart.txt"
out=$(run "$T/favart.txt")
echo "$out" | grep -q "E1 open Games/Sub/_art/Deep (USA).img read -> 1" && ok "a favorite's artwork in another folder is read from the root" || { bad "favorite artwork"; echo "$out" | grep -E "E1|C0"; }
echo "$out" | grep -q "C5 launch: .*Games/Sub/Deep (USA).iso" && ok "after the root trip the menu is still in step (enter Sub, play)" || { bad "after root trip"; echo "$out" | grep -E "C5|invalid"; }

# ---- games in their own folders
G="$T/sd/Games2"
mkdir -p "$G/Doom" "$G/Wing" "$G/Multi" "$G/Nested/Inner" "$T/sd/Games3/Night (Disc 1)" "$T/sd/Games3/Night (Disc 2)"
touch "$G/Doom/doom_1994_ID_Software.cue" "$G/Doom/doom_1994_ID_Software.bin" "$G/Wing/wing.iso" \
      "$G/Multi/a.iso" "$G/Multi/b.iso" "$G/Nested/x.iso" "$G/readme.jpeg" \
      "$T/sd/Games3/Night (Disc 1)/night1.cue" "$T/sd/Games3/Night (Disc 2)/night2.cue" \
      "$T/sd/Games3/Myst (Disc 1).iso" "$T/sd/Games3/Myst (Disc 2).iso" "$T/sd/Games3/Myst (Disc 3).iso"
python3 -c "from PIL import Image; Image.new('RGB',(320,240),(255,0,0)).save('$G/Doom/doom_1994_ID_Software.png'); Image.new('RGB',(320,240),(0,0,255)).save('$G/Wing/wing.jpg')"
rm -f "$T/disc/nvram_ODEMenu"
# root: Games, Games2, Games3 -> Games2: .., Multi, Nested, Doom, Wing (readme.jpeg hidden)
keys "D A w sg2 D D w w w w sdoom A A" 30 700 > "$T/gf.txt"
out=$(run "$T/gf.txt")
echo "$out" | grep -q "C5 launch: .*Games2/Doom/doom_1994_ID_Software.cue" && ok "a game folder starts its game (Games2/Doom)" || { bad "game folder start"; echo "$out" | grep -E "C0|C4|C5"; }
grep -q "^Games2/Doom/$" "$T/disc/nvram_ODEMenu" && ok "a game folder's launch goes into RECENT as the folder" || { bad "game folder RECENT note"; cat "$T/disc/nvram_ODEMenu"; }
# (a SataTo3DO-based clone creates any file the menu opens, so an empty one is fine there)
[ ! -s "$G/Doom/_cover.img" ] && ok "browsing a game folder makes no _cover.img" || bad "browsing wrote _cover.img"
python3 -c "
d=open('$T/g2.ppm','rb').read().split(b'\n',3)[3]
# 5 rows expected (.., Multi, Nested, Doom, Wing): row 6 (y=96) must be empty panel, row 5 (y=88) text
import sys
row=lambda y: set(bytes(d[(y*320+x)*3:(y*320+x)*3+3]) for x in range(40,200))
sys.exit(0 if len(row(105))==1 and len(row(97))>1 else 1)" && ok "only folders and games listed (readme.jpeg hidden)" || bad "hidden files (see $T/g2.ppm)"
grep -q "^OMC3" "$G/_odemenu.idx" 2>/dev/null; python3 -c "
d=open('$G/_odemenu.idx','rb').read()
import sys; sys.exit(0 if b'doom_1994_ID_Software.cue' in d else 1)" && ok "the folder index keeps the game folder" || bad "index game folder"
echo "$out" | awk '/C0/{c++} END{exit 0}'

# after the card scan: Doom's picture (named after the game file) and Wing's (wing.jpg) are covers
keys "$SCAN w D A w D D w w w w sdoom D w w w w swing" 30 30 > "$T/gf2.txt"
out=$(run "$T/gf2.txt")
if [ -n "$CLONE" ]; then
    [ ! -s "$G/Doom/_cover.img" ] && ! echo "$out" | grep -q "E1 open .*\.\(png\|jpg\)" && ok "clone: the scan opens no pictures (no empty .jpg/.png left behind)" || { bad "clone scan pictures"; echo "$out" | grep "E1 open"; }
    pane_rgb "$T/doom.ppm" "not (r==255 and g==0 and b==0)" && ok "clone: no cover made by the console" || bad "clone cover"
else
[ -f "$G/Doom/_cover.img" ] && ok "scan: doom_1994_ID_Software.png kept as Doom/_cover.img" || { bad "_cover.img"; echo "$out" | grep "E1 open Doom"; }
pane_rgb "$T/doom.ppm" "r==255 and g==0 and b==0" && ok "game folder cover shown in the info pane" || bad "game folder cover shown"
pane_rgb "$T/wing.ppm" "b>200 and r<60" && ok "a cover named after the game file (wing.jpg)" || { bad "game-name cover"; echo "$out" | grep "E1 open Wing"; }
fi

# favorite of a game folder, started from FAVS
rm -f "$T/sd/_odemenu.fav"
keys "D A w D D A D A r A A" 30 700 > "$T/gff.txt"
out=$(run "$T/gff.txt")
grep -qx "Games2/Doom/" "$T/sd/_odemenu.fav" && ok "a game folder is kept in favorites as Games2/Doom/" || { bad "fav of game folder"; cat "$T/sd/_odemenu.fav"; }
echo "$out" | grep -q "C5 launch: .*Games2/Doom/doom_1994_ID_Software.cue" && ok "a game folder starts from FAVS" || { bad "game folder from FAVS"; echo "$out" | grep -E "C4|C5"; }
rm -f "$T/sd/_odemenu.fav"

# several discs: Night (Disc 1/2) folders and Myst (Disc 1/2/3) files; START, then START WITH ALL DISCS
rm -f "$T/disc/nvram_ODEMenu"
keys "D D A w D D D A A A" 30 700 > "$T/md.txt"
out=$(run "$T/md.txt")
echo "$out" | grep -q "C4 queued: night1.cue" && echo "$out" | grep -q "C4 queued: night2.cue" && echo "$out" | grep -q "C5 launch: .*night1.cue (queue 2)" && ok "disc 1 of a game in folders starts with disc 2 queued" || { bad "multi-disc folders"; echo "$out" | grep -E "C4|C5"; }
keys "D D A w A A A" 30 700 > "$T/md2.txt"
out=$(run "$T/md2.txt")
echo "$out" | grep -q "C5 launch: .*Myst (Disc 1).iso (queue 3)" && ok "disc 1 of a three-disc game file starts with discs 2 and 3 queued" || { bad "multi-disc files"; echo "$out" | grep -E "C4|C5"; }
keys "D D A w A A D A" 30 700 > "$T/md3.txt"
out=$(run "$T/md3.txt")
echo "$out" | grep -q "C5 launch: .*Myst (Disc 1).iso (queue 1)" && ok "DISC 1 ONLY starts just disc 1" || { bad "disc 1 only"; echo "$out" | grep -E "C4|C5"; }

# the card scan indexes folders it has not seen; a picture named after the game folder works too
rm -f "$G/Wing/_cover.img" "$G/_odemenu.idx" "$T/sd/Games3/_odemenu.idx"
mkdir -p "$T/sd/Games3/Night (Disc 1)"; python3 -c "from PIL import Image; Image.new('RGB',(64,48),(0,255,0)).save('$T/sd/Games3/Night (Disc 1)/Night (Disc 1).jpg')"
keys "$SCAN" 30 30 > "$T/scan.txt"
out=$(run "$T/scan.txt")
if [ -n "$CLONE" ]; then
    [ -s "$T/sd/Games3/_odemenu.idx" ] && ok "clone: the card scan indexes every folder" || { bad "card scan"; ls -la "$T/sd/Games3"; }
else
[ -f "$G/Wing/_cover.img" ] && [ -f "$T/sd/Games3/_odemenu.idx" ] && [ -f "$T/sd/Games3/Night (Disc 1)/_cover.img" ] && ok "the card scan indexes every folder and makes covers (game or folder name)" || { bad "card scan"; echo "$out" | grep -E "E1 open .*(cover|idx|Night)"; ls "$G/Wing"; }
fi

# first start on a card without settings: the scan is offered once, and the offer is remembered
mv "$T/sd/_odemenu.opt" "$T/opt.keep"
keys "A" 30 30 > "$T/first.txt"
run "$T/first.txt" > /dev/null
grep -q "^scanned=1$" "$T/sd/_odemenu.opt" 2>/dev/null && ok "first start offers the scan once (scanned=1 kept)" || { bad "first-start offer"; cat "$T/sd/_odemenu.opt"; }
mv "$T/opt.keep" "$T/sd/_odemenu.opt"

# SataTo3DO-based clone (FWSIM_CLONE=7): its firmware ignores C0 to the id it already holds, so a folder whose only
# entry is a folder with the same number (entry 1 in entry 1) can not be opened: the menu must say so and stay in
# step (it did not, and wrote one folder's listing as the root's index)
if [ "${FWSIM_CLONE:-0}" = 7 ]; then
    C="$T/c7"; mkdir -p "$C/sd/A/X" "$C/disc/assets"; cp "$T/disc/assets/logo.img" "$C/disc/assets/"; touch "$C/sd/boot.iso" "$C/sd/A/X/Inner (USA).iso"
    printf 'OMO1\npane=1\nbar=1\nshine=0\nindex=1\nscanned=1\n' > "$C/sd/_odemenu.opt"
    printf '40 pad A\n42 pad -\n120 pad A\n122 pad -\n200 shot stuck\n210 end\n' > "$C/s.txt"
    for start in "" A; do
        rm -f "$C/sd/_odemenu.idx" "$C/sd/A/_odemenu.idx"
        $M --sd "$C/sd" --out "$C" --disc "$C/disc" --script "$C/s.txt" ${start:+--start $start} > "$C/out.txt" 2>&1
        python3 -c "import sys; d=open('$C/sd/_odemenu.idx','rb').read(); sys.exit(0 if b'boot.iso' in d and b'Inner' not in d else 1)" 2>/dev/null &&
            ok "clone: a folder holding one same-numbered folder: the menu stays in step${start:+ (after a reset inside A)}" ||
            { bad "clone single subfolder${start:+ after reset}"; grep -E "C0" "$C/out.txt" | head; }
    done
fi

[ -s "$T/dotdot.log" ] && { bad "file names with '..' (exFAT cannot resolve them)"; sort -u "$T/dotdot.log" | head -3; } || ok "no file name relies on '..' (exFAT)"
[ -f "$T/invalid.log" ] && bad "firmware path went above the drive root ($(wc -l < "$T/invalid.log") runs)" || ok "firmware path never went above the drive root"

echo "work dir: $T"
exit $FAIL
