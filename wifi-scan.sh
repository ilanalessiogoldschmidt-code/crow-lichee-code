#!/bin/bash
set -euo pipefail
DEVICE=${CROW_DEVICE:-root@10.31.2.1}
ssh "$DEVICE" '
    iface=""
    for x in wlan0 wlan1; do
        if [ -d "/sys/class/net/$x" ]; then iface="$x"; break; fi
    done
    if [ -z "$iface" ]; then
        echo "No wlan interface found."
        exit 1
    fi
    if command -v ip >/dev/null 2>&1; then ip link set "$iface" up || true; fi
    echo "Scanning on $iface..."
    if command -v iw >/dev/null 2>&1; then
        iw dev "$iface" scan 2>/dev/null | sed -n "s/^[[:space:]]*SSID: //p" | sort -u
    elif command -v iwlist >/dev/null 2>&1; then
        iwlist "$iface" scan 2>/dev/null | sed -n "s/.*ESSID:\"\(.*\)\"/\1/p" | sort -u
    else
        echo "Neither iw nor iwlist is available on this image."
        exit 1
    fi
'
