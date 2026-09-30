#!/bin/bash
set -euo pipefail

DEVICE=${CROW_DEVICE:-root@10.31.2.1}

ssh "$DEVICE" '
    if [ ! -d /root/crow_recordings ]; then
        echo "No Crow recordings directory yet."
        exit 0
    fi

    found=0
    for recording in /root/crow_recordings/recording_*; do
        [ -d "$recording" ] || continue
        found=1
        echo
        echo "=== $recording ==="
        merged=$(find "$recording" -maxdepth 1 -type f -name "recording_*.h265" | head -1)
        if [ -n "$merged" ]; then
            ls -lh "$merged"
        fi
        echo "Segment parts: $(find "$recording" -maxdepth 1 -type f -name "part_*.h265" | wc -l)"
        du -sh "$recording" 2>/dev/null || true
        if [ -f "$recording/manifest.txt" ]; then
            tail -8 "$recording/manifest.txt"
        fi
    done

    [ "$found" -eq 1 ] || echo "No normal recordings yet."
'
