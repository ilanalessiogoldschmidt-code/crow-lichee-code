#!/bin/bash
set -euo pipefail

DEVICE=${CROW_DEVICE:-root@10.31.2.1}

ssh "$DEVICE" '
    if [ ! -d /root/crow_events ]; then
        echo "No Crow events directory yet."
        exit 0
    fi

    found=0
    for event in /root/crow_events/event_*; do
        [ -d "$event" ] || continue
        found=1
        echo
        echo "=== $event ==="
        merged=$(find "$event" -maxdepth 1 -type f -name "event_*.h265" | head -1)
        if [ -n "$merged" ]; then
            ls -lh "$merged"
        fi
        echo "Segment parts: $(find "$event" -maxdepth 1 -type f \( -name "pre_*.h265" -o -name "post_*.h265" \) | wc -l)"
        du -sh "$event" 2>/dev/null || true
        if [ -f "$event/manifest.txt" ]; then
            tail -8 "$event/manifest.txt"
        fi
    done

    [ "$found" -eq 1 ] || echo "No Hindsight events yet."
'
