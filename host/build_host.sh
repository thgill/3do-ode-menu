#!/bin/sh
# build the menu for the host test harness
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-build/host}; mkdir -p $OUT
CF="$EXTRA -std=gnu89 -O1 -g -Wall -Wno-unused-function -Wno-pointer-sign -Wno-char-subscripts -DHOST_BUILD -Ihost -Isrc"
for f in odecmd dirlist input gfx ui lists tools crc rampatch artdec jpgprog; do gcc $CF -c src/$f.c -o $OUT/$f.o; done
for f in tinflate picojpeg; do gcc -O1 -g -w -c src/third/$f.c -o $OUT/t_$f.o; done
gcc $CF -Dmain=menu_main -c src/main.c -o $OUT/main.o
for f in h3do fwsim fwsim_sata main_host; do gcc $CF -c host/$f.c -o $OUT/h_$f.o; done
gcc -o $OUT/menu_host $OUT/*.o
echo built $OUT/menu_host
