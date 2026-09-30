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
    echo "Wi-Fi interface: $iface"
    if command -v ip >/dev/null 2>&1; then
        ip -4 addr show dev "$iface" || true
        ip link show dev "$iface" || true
    else
        ifconfig "$iface" || true
    fi
    if command -v iw >/dev/null 2>&1; then
        iw dev "$iface" link || true
    fi
'
