#!/bin/bash
set -e

DEVICE=root@10.31.2.1
BINARY=/workspace/crow/build/crow_camera

echo "=== Deploying Crow Camera ==="

scp "$BINARY" "$DEVICE:/root/crow_camera"

ssh -t "$DEVICE" '
    chmod +x /root/crow_camera
    rm -f /root/crow_test.h265 /root/test-0.h265
    /root/crow_camera
    echo
    echo "=== Recording ==="
    ls -lh /root/crow_test.h265
'