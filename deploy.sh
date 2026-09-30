#!/bin/bash
set -euo pipefail

DEVICE=${CROW_DEVICE:-root@10.31.2.1}
CAMERA=/workspace/crow/build/crow_camera
WEB=/workspace/crow/build/crow_web

printf '%s\n' "=== Deploying Crow Camera v0.9 Wireless API + Robust Shutdown ==="

if [ ! -x "$CAMERA" ] || [ ! -x "$WEB" ]; then
    echo "Missing build/crow_camera or build/crow_web. Run ./build.sh first."
    exit 1
fi

echo "Stopping any running Crow processes before replacing binaries..."
ssh "$DEVICE" '
    web_pid=$(pidof crow_web 2>/dev/null || true)
    if [ -n "$web_pid" ]; then
        kill $web_pid 2>/dev/null || true
    fi

    camera_pid=$(pidof crow_camera 2>/dev/null || true)
    if [ -n "$camera_pid" ]; then
        echo "Requesting clean Crow camera shutdown (PID $camera_pid)..."
        kill -INT $camera_pid 2>/dev/null || true

        for wait_step in 1 2 3 4 5 6 7 8; do
            [ -z "$(pidof crow_camera 2>/dev/null || true)" ] && break
            sleep 1
        done

        camera_pid=$(pidof crow_camera 2>/dev/null || true)
        if [ -n "$camera_pid" ]; then
            echo "Crow camera did not exit after 8 sec; sending TERM..."
            kill $camera_pid 2>/dev/null || true
            sleep 1
        fi

        camera_pid=$(pidof crow_camera 2>/dev/null || true)
        if [ -n "$camera_pid" ]; then
            echo "Crow camera still busy; forcing final stop..."
            kill -9 $camera_pid 2>/dev/null || true
            sleep 0.2
        fi
    fi

    # A forced camera kill can leave its arecord child behind. Stop only the
    # PID Crow recorded for its own microphone process.
    if [ -f /root/crow_audio.pid ]; then
        audio_pid=$(cat /root/crow_audio.pid 2>/dev/null || true)
        if [ -n "$audio_pid" ]; then
            kill "$audio_pid" 2>/dev/null || true
            sleep 0.2
            kill -9 "$audio_pid" 2>/dev/null || true
        fi
        rm -f /root/crow_audio.pid
    fi
'

scp "$CAMERA" "$WEB" "$DEVICE:/root/"

ssh -t "$DEVICE" '
    chmod +x /root/crow_camera /root/crow_web

    if command -v nohup >/dev/null 2>&1; then
        nohup /root/crow_web >/root/crow_web.log 2>&1 </dev/null &
    else
        /root/crow_web >/root/crow_web.log 2>&1 </dev/null &
    fi
    sleep 0.3
    web_pid=$(pidof crow_web || true)
    if [ -n "$web_pid" ]; then
        echo "Crow web server running as PID $web_pid"
    else
        echo "WARNING: Crow web server did not stay running. See /root/crow_web.log"
    fi

    echo
    echo "=== Starting Crow v0.9 ==="
    echo "Capture: 2560x1440 H.265, 25 fps, 20 Mbps CBR"
    echo "Audio: onboard analog microphone, 48 kHz mono PCM ring"
    echo "Rolling rewind: ~60 sec"
    echo "Completed video/audio media is published atomically after assembly"
    echo "Web/media UI over USB: http://10.31.2.1:8080"
    echo
    echo "In another Docker terminal:"
    echo "  ./crowctl.sh hindsight"
    echo "  ./crowctl.sh record"
    echo "  ./crowctl.sh status"
    echo "  ./wifi-scan.sh"
    echo "  ./wifi-connect.sh \"SSID\" \"PASSWORD\""
    echo
    echo "On your Mac terminal:"
    echo "  ./crow-media.sh list"
    echo "  ./crow-media.sh event 2"
    echo "  ./crow-media.sh recording 1"
    echo

    /root/crow_camera
    status=$?

    echo
    echo "=== Crow v0.9 result ==="
    if [ -d /root/crow_segments ]; then
        count=$(find /root/crow_segments -maxdepth 1 -type f -name "segment_*.h265" | wc -l)
        echo "Rolling segments currently retained: $count"
        du -sh /root/crow_segments 2>/dev/null || true
    fi

    if [ -d /root/crow_audio ]; then
        audio_count=$(find /root/crow_audio -maxdepth 1 -type f -name "audio_*.pcm" | wc -l)
        echo "Rolling audio segments currently retained: $audio_count"
        du -sh /root/crow_audio 2>/dev/null || true
    fi

    if [ -d /root/crow_events ]; then
        echo "Hindsight events:"
        find /root/crow_events -maxdepth 1 -mindepth 1 -type d -name "event_*" | sort || true
        du -sh /root/crow_events 2>/dev/null || true
    fi

    if [ -d /root/crow_recordings ]; then
        echo "Normal recordings:"
        find /root/crow_recordings -maxdepth 1 -mindepth 1 -type d -name "recording_*" | sort || true
        du -sh /root/crow_recordings 2>/dev/null || true
    fi

    exit "$status"
'
