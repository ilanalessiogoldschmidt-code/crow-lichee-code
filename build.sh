#!/bin/bash
set -e

SDK=/workspace/sdk
CROW=/workspace/crow
VENC=$SDK/middleware/v2/sample/venc

echo "=== Building Crow Camera ==="

cd "$SDK"
source build/cvisetup.sh >/dev/null
defconfig sg2002_licheervnano_sd >/dev/null

mkdir -p "$CROW/build"

# Sync Crow's modified encoder code into the SDK build tree.
cp "$CROW/vendor/venc/src/sample_venc_lib.c" \
   "$VENC/src/sample_venc_lib.c"

# Put our Crow main beside Sipeed's VENC sources for compilation.
cp "$CROW/src/main.c" "$VENC/crow_main.c"

cd "$VENC"

make \
    KERNEL_INC="$SDK/linux_5.10/build/sg2002_licheervnano_sd/riscv/usr/include" \
    VENC_SRCS="$VENC/crow_main.c" \
    TARGET_VENC="$CROW/build/crow_camera" \
    "$CROW/build/crow_camera"

echo
echo "=== Crow build complete ==="
file "$CROW/build/crow_camera"
ls -lh "$CROW/build/crow_camera"