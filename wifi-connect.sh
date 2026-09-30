#!/bin/bash
set -euo pipefail

DEVICE=${CROW_DEVICE:-root@10.31.2.1}
SSID=${1:-}
PASS=${2:-}

if [ -z "$SSID" ] || [ -z "$PASS" ]; then
    echo 'Usage: ./wifi-connect.sh "SSID" "PASSWORD"'
    echo 'This first-pass helper supports normal WPA/WPA2 personal networks and phone hotspots.'
    echo 'Enterprise networks such as eduroam need a different configuration.'
    exit 1
fi

escape_wpa() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

ssid_escaped=$(escape_wpa "$SSID")
pass_escaped=$(escape_wpa "$PASS")
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

cat > "$tmp" <<CFG
ctrl_interface=/var/run/wpa_supplicant
update_config=1
network={
    ssid="$ssid_escaped"
    psk="$pass_escaped"
    key_mgmt=WPA-PSK
}
CFG

scp "$tmp" "$DEVICE:/root/crow_wifi.conf" >/dev/null

ssh "$DEVICE" '
    set -e
    iface=""
    for x in wlan0 wlan1; do
        if [ -d "/sys/class/net/$x" ]; then iface="$x"; break; fi
    done
    if [ -z "$iface" ]; then
        echo "No wlan interface found."
        exit 1
    fi
    if ! command -v wpa_supplicant >/dev/null 2>&1; then
        echo "wpa_supplicant is not installed on this image."
        exit 1
    fi
    if command -v ip >/dev/null 2>&1; then ip link set "$iface" up; fi
    killall wpa_supplicant 2>/dev/null || true
    rm -f /var/run/wpa_supplicant/$iface 2>/dev/null || true
    wpa_supplicant -B -i "$iface" -c /root/crow_wifi.conf
    sleep 3
    if command -v udhcpc >/dev/null 2>&1; then
        udhcpc -i "$iface" -q -n || true
    elif command -v busybox >/dev/null 2>&1; then
        busybox udhcpc -i "$iface" -q -n || true
    fi
    sleep 1
    ipaddr=""
    if command -v ip >/dev/null 2>&1; then
        ipaddr=$(ip -4 -o addr show dev "$iface" | awk "{print \\$4}" | cut -d/ -f1 | head -1)
    else
        ipaddr=$(ifconfig "$iface" 2>/dev/null | sed -n "s/.*inet addr:\([^ ]*\).*/\1/p" | head -1)
    fi
    echo "Wi-Fi interface: $iface"
    if [ -n "$ipaddr" ]; then
        echo "Wi-Fi IP: $ipaddr"
        echo "Crow web: http://$ipaddr:8080"
    else
        echo "No IPv4 address yet. Run ./wifi-status.sh in a few seconds."
    fi
'
