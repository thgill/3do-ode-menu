#!/bin/sh
# The Cover Maker page's folder indexes (tools/getart.html, IDXLIB) against the ones the menu itself writes.
# A card is indexed by the menu (TOOLS > SCAN CARD, with the firmware simulator); the page's code then indexes the
# same card from the order the file system lists it in, and every index must come out byte for byte the same.
# Then the menu runs on the page's indexes alone: it must use them (no folder listings) and start the right game.
set -u
cd "$(dirname "$0")/.."
ROOT=$(pwd)
M=$ROOT/build/host/menu_host
T=$(mktemp -d)
FAIL=0
ok()  { echo "PASS  $1"; }
bad() { echo "FAIL  $1"; FAIL=1; }
run() { $M --sd "$T/sd" --out "$T" --disc "$T/disc" --script "$@" 2>&1; }
keys() { python3 -c "
import sys
f=40; out=[]
for k in sys.argv[1].split():
    if k=='w': f+=$2; continue
    out.append('%d pad %s'%(f,k)); out.append('%d pad -'%(f+2)); f+=$2
out.append('%d end'%(f+$3))
print('\n'.join(out))" "$1"; }

mkdir -p "$T/disc/assets"; cp assets_gen/logo.img "$T/disc/assets/"
S=$T/sd
mkdir -p "$S/Games" "$S/Doom" "$S/Two" "$S/Big" "$S/Nested/Inner" "$S/Empty" "$S/_art" "$S/.Trashes" "$S/Saves" \
         "$S/Myst (Disc 1)" "$S/Myst (Disc 2)" "$S/Ünïcödé Gämé (Japan)"
touch "$S/boot.iso" "$S/readme" "$S/notes.txt" "$S/cover.jpeg" "$S/Loose Game (USA).iso" "$S/save.nvm" "$S/thing.ode" "$S/._junk.iso"
for i in 12 3 1 25 7 100 2; do touch "$S/Games/Game $i (USA).iso"; done
touch "$S/Games/game 05 (europe).ISO" "$S/Games/Zeta.cue" "$S/Games/Zeta.bin" "$S/Games/$(python3 -c "print('L'*140)").iso"
touch "$S/Doom/Doom (USA).cue" "$S/Doom/Doom (USA).bin" "$S/Doom/Doom (USA).jpg" "$S/Doom/doom.nvm"
touch "$S/Two/A.iso" "$S/Two/B.iso"
for i in $(seq 1 40); do touch "$S/Big/file$i.txt" "$S/Big/x$i.bin" "$S/Big/plain$i"; done; touch "$S/Big/One.iso"   # >32 listed: not a game folder
touch "$S/Nested/Inner/Deep (USA).iso"
touch "$S/Myst (Disc 1)/Myst (Disc 1).iso" "$S/Myst (Disc 2)/Myst (Disc 2).iso" "$S/Ünïcödé Gämé (Japan)/Ünïcödé Gämé (Japan).cue"
touch "$S/Saves/s1.nvm"
printf 'OMO1\npane=1\nbar=1\nshine=0\nindex=1\nscanned=1\n' > "$S/_odemenu.opt"

# 1. the menu indexes the card
SCAN="r r r D A D A w w w w w w w w w w w w w w w w w w w w A l l l"
keys "$SCAN" 30 30 > "$T/scan.txt"
run "$T/scan.txt" > "$T/scan.log"
mkdir "$T/console"
( cd "$S" && find . -name _odemenu.idx ) | while read -r f; do mkdir -p "$T/console/$(dirname "$f")"; mv "$S/$f" "$T/console/$f"; done
n=$(cd "$T/console" && find . -name _odemenu.idx | wc -l)
[ "$n" -ge 6 ] && ok "the menu indexed $n folders" || bad "the menu indexed only $n folders"

# 2. the page's code indexes it from the file system's order (os.listdir does not sort)
python3 - "$S" > "$T/order.json" <<'PY'
import json, os, sys
root = sys.argv[1]; out = {}
for d, dirs, files in os.walk(root):
    out[os.path.relpath(d, root)] = [[n, os.path.isdir(os.path.join(d, n))] for n in os.listdir(d)]
print(json.dumps(out))
PY
sed -n '/IDXLIB BEGIN/,/IDXLIB END/p' tools/getart.html | sed '1s/^/\/\* /' > "$T/idxlib.js"
cat >> "$T/idxlib.js" <<'JS'
const fs = require("fs"), path = require("path");
const [order, sd] = [JSON.parse(fs.readFileSync(process.argv[2], "utf8")), process.argv[3]];
function listing(rel) {
  const listed = order[rel].map(([n, d]) => fwEntry(n, d)).filter(Boolean);
  for (const e of listed) if (e.isDir) setFolder(e, listing(rel === "." ? e.name : rel + "/" + e.name));
  const b = buildIndex(listed);
  if (b) fs.writeFileSync(path.join(sd, rel, IDX), b);
  return listed;
}
listing(".");
JS
node "$T/idxlib.js" "$T/order.json" "$S"
same=0; diff=0
for f in $(cd "$T/console" && find . -name _odemenu.idx | tr ' ' '\001'); do
  f=$(echo "$f" | tr '\001' ' ')
  if cmp -s "$T/console/$f" "$S/$f"; then same=$((same+1)); else diff=$((diff+1)); echo "  differs: $f"; fi
done
[ "$diff" -eq 0 ] && ok "page indexes match the menu's byte for byte ($same folders)" || bad "$diff page indexes differ from the menu's"
extra=$(cd "$S" && find . -name _odemenu.idx | while read -r f; do [ -f "$T/console/$f" ] || echo "$f"; done)
[ -n "$extra" ] && echo "  (page also indexed: $(echo $extra))"

# 2b. the same with the Python Cover Maker (tools/covermaker.py), from the same order (os.scandir does not sort)
find "$S" -name _odemenu.idx -delete
python3 -c "
import sys; sys.path.insert(0, 'tools'); import covermaker as c
jobs, idx = [], []
c.walk(sys.argv[1], True, '', jobs, idx)
for x in idx: open(x['path'], 'wb').write(x['bytes'])
" "$S"
same=0; diff=0
for f in $(cd "$T/console" && find . -name _odemenu.idx | tr ' ' '\001'); do
  f=$(echo "$f" | tr '\001' ' ')
  if cmp -s "$T/console/$f" "$S/$f"; then same=$((same+1)); else diff=$((diff+1)); echo "  differs: $f"; fi
done
[ "$diff" -eq 0 ] && ok "covermaker.py indexes match the menu's byte for byte ($same folders)" || bad "$diff covermaker.py indexes differ"

# 3. the menu on the page's indexes alone: no listing of Games, and the right game starts
keys "D D A w w D D A A" 30 600 > "$T/play.txt"      # root: Games is row 2 (Big, Games, ...)? found below
out=$(run "$T/play.txt")
echo "$out" | grep -q "C5 launch: .*/Games/Game 3 (USA).iso" && ok "the right game starts from the page's indexes" || bad "wrong or no launch"
r1=$(echo "$out" | sed -n 's/.* \([0-9]*\) directory reads.*/\1/p')
find "$S" -name _odemenu.idx -delete
out=$(run "$T/play.txt")
r0=$(echo "$out" | sed -n 's/.* \([0-9]*\) directory reads.*/\1/p')
[ "$r1" -lt $((r0 / 3)) ] && ok "the menu used the page's indexes ($r0 directory reads without them, $r1 with)" || bad "indexes not used ($r0 -> $r1)"
echo "work dir: $T"
exit $FAIL
