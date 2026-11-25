#!/bin/bash
# Build voidsw for the Atari GameStation Go (ARMv7 hard-float, Debian stretch
# SDK) with the proven GCC 6.3 cross toolchain. Run from WSL Ubuntu:
#   platform/gamestation-go-build.sh
# Output: ./voidsw (unstripped), ./voidsw.withsymbols and the stripped
# deployable output/gamestation-go-armv7hf/voidsw.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
sdk=${SDK:-/mnt/d/Development/SDKs/gamestation-go-dice-stretch-armhf}
tools=${TOOLS:-/mnt/d/Development/AGSG/eduke32/obj/gamestation-go-tools}
defs="-DEDUKE32_GAMESTATION_GO"
flags="--sysroot=$sdk -I$tools/gcc6-headers -idirafter /usr/arm-linux-gnueabihf/include -march=armv7-a -mfloat-abi=hard -mfpu=neon $defs"

cd "$root"
touch source/build/src/sdlayer.cpp

# The `sw` target also tries to link the wangulator editor, which fails on the
# AGSG (unrelated to the game); voidsw itself is linked before that point.
LD_LIBRARY_PATH="$tools/gcc6-host-libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" PATH="$tools/gcc6:$tools:$PATH" \
make -j4 PLATFORM=LINUX ARCH=armv7l CROSS=arm-linux-gnueabihf- HAVE_GTK2=0 STARTUP_WINDOW=0 USE_OPENGL=0 \
  USE_LIBVPX=0 USE_MIMALLOC=0 obj=obj/gamestation-go LTO=0 STRIP=true \
  "CFLAGS=$flags" "CXXFLAGS=$flags" \
  "LDFLAGS=-fno-use-linker-plugin -B$sdk/usr/arm-linux-gnueabihf/lib --sysroot=$sdk -L$sdk/usr/arm-linux-gnueabihf/lib -L$sdk/usr/lib/arm-linux-gnueabihf -L$sdk/lib/arm-linux-gnueabihf -L$sdk/runtime -Wl,-rpath-link,$sdk/runtime" \
  sw || [ -x voidsw ]

cp voidsw voidsw.withsymbols
arm-linux-gnueabihf-strip --strip-all -o voidsw.stripped voidsw
mv voidsw.stripped voidsw
cp voidsw output/gamestation-go-armv7hf/voidsw

readelf -h voidsw | grep -E 'Class|Machine|Flags'
readelf -d voidsw | grep NEEDED
readelf -p .comment voidsw | grep GCC
sha256sum voidsw output/gamestation-go-armv7hf/voidsw
