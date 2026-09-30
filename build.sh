#!/bin/bash

set -e
set -o pipefail

SDK=/workspace/sdk
CROW=/workspace/crow
VENC="$SDK/middleware/v2/sample/venc"

echo "=== Building Crow Camera v0.9 Wireless API + Robust Shutdown ==="

cd "$SDK"

# Sipeed's environment script references unset variables internally, so do not
# enable `set -u` in this script.
source build/cvisetup.sh >/dev/null
defconfig sg2002_licheervnano_sd >/dev/null

mkdir -p "$CROW/build"

cp "$CROW/vendor/venc/src/sample_venc_lib.c" \
   "$VENC/src/sample_venc_lib.c"
cp "$CROW/vendor/venc/include/sample_venc_lib.h" \
   "$VENC/include/sample_venc_lib.h"
cp "$CROW/src/main.c" \
   "$VENC/crow_main.c"

rm -f \
   "$VENC/src/sample_venc_lib.o" \
   "$VENC/src/sample_venc_lib.d" \
   "$VENC/crow_main.o" \
   "$VENC/crow_main.d" \
   "$CROW/build/crow_camera" \
   "$CROW/build/crow_web"

cd "$VENC"

make \
    KERNEL_INC="$SDK/linux_5.10/build/sg2002_licheervnano_sd/riscv/usr/include" \
    VENC_SRCS="$VENC/crow_main.c" \
    TARGET_VENC="$CROW/build/crow_camera" \
    "$CROW/build/crow_camera"

# The web UI only needs libc/POSIX sockets, so compile it directly with the
# same RISC-V toolchain used by the SDK.
${CROSS_COMPILE}gcc \
    -O2 -Wall -Wextra \
    "$CROW/src/web.c" \
    -o "$CROW/build/crow_web"

echo
echo "=== Crow v0.9 build complete ==="
file "$CROW/build/crow_camera" "$CROW/build/crow_web"
ls -lh "$CROW/build/crow_camera" "$CROW/build/crow_web"
