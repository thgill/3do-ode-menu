#!/bin/sh
# Make a new menu disc from Fixel's official boot.iso (V5) plus the files built here.
#   ./make_disc.sh /path/to/original/boot.iso out/boot.iso
# Needs 3dt (trapexit's 3DO Disc Tool, ships with the 3do-devkit) on PATH, build/3do/LaunchMe and assets_gen/.
# The 3DO system files stay as they are; the old menu's files are removed (see below); 3dt re-signs the image.
set -e
cd "$(dirname "$0")"
SRC=$1
OUT=$2
[ -f "$SRC" ] && [ -n "$OUT" ] || { echo "usage: $0 original_boot.iso output.iso"; exit 1; }
[ -f build/3do/LaunchMe ] || { echo "build/3do/LaunchMe missing: run build_3do.sh"; exit 1; }
[ -f assets_gen/logo.img ] || { echo "assets_gen/logo.img missing: run tools/mkui.py"; exit 1; }
TMP=$(mktemp -d)
3dt unpack "$SRC" -o "$TMP/disc" >/dev/null
rm -f "$TMP/disc/layout.json"
cp "${LAUNCHME:-build/3do/LaunchMe}" "$TMP/disc/LaunchMe"
# nothing of the original menu but what the ODE needs: its driver (System/Drivers/ode) and the Storage Tuner
# (an SDK sample the TOOLS tab runs, with its art: assets/*.cel and assets/sans_serif_14.font). Replaced: the boot banner. Gone: the music player and its music, the old menu's
# background, the About picture, and the three hidden pictures.
cp assets_gen/BannerScreen "$TMP/disc/BannerScreen"     # our own boot banner (the system needs one)
[ -n "$KEEP_OLD" ] || rm -f "$TMP/disc/mplay" \
      "$TMP/disc/assets/"*.IMAG "$TMP/disc/assets/bgm44.aifc" \
      "$TMP/disc/System/Programs/MAUS" "$TMP/disc/System/Programs/WALKER" \
      "$TMP/disc/System/Drivers/TUNERS/access.tuner"
rmdir "$TMP/disc/System/Drivers/TUNERS" 2>/dev/null || true
# AppStartup ends with $boot/LaunchMe, and on an FZ-1 the system starts $boot/LaunchMe again after the script (two
# copies; the later one stays idle, see main.c). The line stays: on an XPort the script's copy is the only one, and
# without it the menu never started (splash, then a black screen). STRIP_SCRIPT_LAUNCH=1 takes it out (test only).
[ -z "$STRIP_SCRIPT_LAUNCH" ] || python3 - "$TMP/disc/AppStartup" <<'PY'
import sys
p = sys.argv[1]
d = open(p, 'rb').read()
lines = d.split(b'\r')
keep = [l for l in lines if l.strip().lower() not in (b'$boot/launchme', b'$boot/launchme&')]
assert len(keep) == len(lines) - 1, 'AppStartup: expected one $boot/LaunchMe line'
open(p, 'wb').write(b'\r'.join(keep))
PY
cp assets_gen/logo.img "$TMP/disc/assets/"
cp assets_gen/OFL.txt "$TMP/disc/OFL.txt"
mkdir -p "$(dirname "$OUT")"
3dt pack "$TMP/disc" -o "$OUT" --volume-label xpboot --billstuff-romtag 2>&1 | grep -v "development signature" || true
3dt verify "$OUT"
rm -rf "$TMP"
