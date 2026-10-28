#!/bin/sh
# Build LaunchMe for the 3DO with trapexit's 3do-devkit (Norcroft ARM C 4.91 / ARM SDT 2.51, Portfolio 2.5 libs).
#   DEVKIT=/path/to/3do-devkit ./build_3do.sh
# armcc/armlink must be on PATH (on Linux they need 32-bit x86 support; see README).
set -e
cd "$(dirname "$0")"
DEVKIT=${DEVKIT:-$HOME/3do-devkit}
INC="-I$DEVKIT/include/3do"
LIB="$DEVKIT/lib/3do"
MENU_DATE=${MENU_DATE:-$(date +%Y-%m-%d)}      # the version shown in About
CFLAGS="$EXTRA -DMENU_DATE=\"$MENU_DATE\" -O2 -zpno_check_stack -bigend -za1 -zi4 -fa -fpu none -arch 3 -apcs 3/32/nofp/swst/wide/softfp -DNDEBUG=1"
B=${B:-build/3do}; mkdir -p $B
OBJS=""
# third/: tinf (zlib license) and picojpeg (public domain), for PNG and JPG pictures; see src/third/README.third
for f in main odeio odecmd dirlist input gfx ui lists tools crc rampatch artdec jpgprog third/tinflate third/picojpeg; do
    o="$B/$(basename "$f").o"
    rm -f "./$o"
    armcc $INC $CFLAGS -c src/$f.c -o "$o" 2>&1 | tee "$o.log" | grep -v "apcs /wide" || true
    if grep -qE "[1-9][0-9]* (serious )?errors|Serious error" "$o.log" || [ ! -s "$o" ]; then
        echo "compile failed: src/$f.c"; exit 1
    fi
    OBJS="$OBJS $o"
done
armlink -o $B/LaunchMe -match 0x1 -nodebug -noscanlib -nozeropad -remove -aif -reloc -dupok -ro-base 0 \
    $LIB/cstartup.o $OBJS \
    $LIB/exampleslib.lib $LIB/lib3do.lib $LIB/graphics.lib $LIB/input.lib $LIB/filesystem.lib \
    $LIB/operamath.lib $LIB/clib.lib $LIB/swi.lib
modbin --debug --name=LaunchMe --stack=16384 $B/LaunchMe $B/LaunchMe
ls -l $B/LaunchMe
