#!/bin/bash
set -euo pipefail

DEVICE=${CROW_DEVICE:-root@10.31.2.1}
OUTDIR=${CROW_MEDIA_DIR:-$HOME/Desktop/CrowMedia}
FPS=25
AUDIO_RATE=48000

usage() {
    cat <<'USAGE'
Run this script from your Mac terminal, not inside Docker.

Usage:
  ./crow-media.sh list
  ./crow-media.sh event <number> [4x3|wide]
  ./crow-media.sh recording <number> [4x3|wide]
  ./crow-media.sh focus event <number>
  ./crow-media.sh focus recording <number>

Defaults:
  export shape: 4x3 (center crop from 2560x1440 to 1920x1440)
  output folder: ~/Desktop/CrowMedia

Crow v0.8:
  - downloads the assembled H.265 file
  - downloads matching 48 kHz mono S16_LE PCM when available
  - muxes microphone audio into AAC automatically
  - explicitly creates video timestamps, avoiding raw-H.265 MP4 timestamp warnings
USAGE
}

need_ffmpeg() {
    if ! command -v ffmpeg >/dev/null 2>&1; then
        echo "ffmpeg is required on the Mac."
        exit 1
    fi
}

list_media() {
    ssh "$DEVICE" '
        echo "=== Hindsight events ==="
        find /root/crow_events -maxdepth 2 -type f \( -name "event_*.h265" -o -name "event_*.pcm" \) -print 2>/dev/null | sort || true
        echo
        echo "=== Normal recordings ==="
        find /root/crow_recordings -maxdepth 2 -type f \( -name "recording_*.h265" -o -name "recording_*.pcm" \) -print 2>/dev/null | sort || true
    '
}

remote_for() {
    local kind=$1
    local number=$2
    local id
    printf -v id '%04d' "$number"

    case "$kind" in
        event)
            REMOTE="/root/crow_events/event_${id}/event_${id}.h265"
            AUDIO_REMOTE="/root/crow_events/event_${id}/event_${id}.pcm"
            BASE="event_${id}"
            ;;
        recording)
            REMOTE="/root/crow_recordings/recording_${id}/recording_${id}.h265"
            AUDIO_REMOTE="/root/crow_recordings/recording_${id}/recording_${id}.pcm"
            BASE="recording_${id}"
            ;;
        *)
            echo "Unknown media type: $kind"
            exit 1
            ;;
    esac
}

download_raw() {
    mkdir -p "$OUTDIR"
    RAW="$OUTDIR/${BASE}.h265"
    AUDIO_RAW="$OUTDIR/${BASE}.pcm"

    echo "Downloading $REMOTE ..."
    scp "$DEVICE:$REMOTE" "$RAW"
    echo "Raw video: $RAW"

    if ssh "$DEVICE" "test -s '$AUDIO_REMOTE'"; then
        echo "Downloading microphone audio ..."
        scp "$DEVICE:$AUDIO_REMOTE" "$AUDIO_RAW"
        HAVE_AUDIO=1
        echo "Raw audio: $AUDIO_RAW"
    else
        HAVE_AUDIO=0
        rm -f "$AUDIO_RAW"
        echo "No assembled microphone audio found; exporting video-only."
    fi
}

export_wide() {
    local out="$OUTDIR/${BASE}_wide.mp4"
    echo "Creating timestamped 16:9 MP4 without re-encoding video..."

    if [ "$HAVE_AUDIO" -eq 1 ]; then
        ffmpeg -hide_banner -loglevel warning -y \
            -r "$FPS" -i "$RAW" \
            -f s16le -ar "$AUDIO_RATE" -ac 1 -i "$AUDIO_RAW" \
            -map 0:v:0 -map 1:a:0 \
            -c:v copy \
            -bsf:v "setts=pts=N/(${FPS}*TB):dts=N/(${FPS}*TB)" \
            -c:a aac -b:a 128k \
            -shortest \
            -tag:v hvc1 \
            -movflags +faststart \
            "$out"
    else
        ffmpeg -hide_banner -loglevel warning -y \
            -r "$FPS" -i "$RAW" \
            -c:v copy \
            -bsf:v "setts=pts=N/(${FPS}*TB):dts=N/(${FPS}*TB)" \
            -tag:v hvc1 \
            -movflags +faststart \
            "$out"
    fi

    echo "MP4: $out"
}

export_4x3() {
    local out="$OUTDIR/${BASE}_4x3.mp4"
    local filter="setpts=N/(${FPS}*TB),crop=1920:1440:320:0"

    echo "Creating centered 1920x1440 (4:3) MP4..."

    if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q 'hevc_videotoolbox'; then
        if [ "$HAVE_AUDIO" -eq 1 ]; then
            ffmpeg -hide_banner -loglevel warning -y \
                -r "$FPS" -i "$RAW" \
                -f s16le -ar "$AUDIO_RATE" -ac 1 -i "$AUDIO_RAW" \
                -map 0:v:0 -map 1:a:0 \
                -vf "$filter" \
                -c:v hevc_videotoolbox \
                -b:v 20000k \
                -c:a aac -b:a 128k \
                -shortest \
                -tag:v hvc1 \
                -movflags +faststart \
                "$out"
        else
            ffmpeg -hide_banner -loglevel warning -y \
                -r "$FPS" -i "$RAW" \
                -vf "$filter" \
                -an \
                -c:v hevc_videotoolbox \
                -b:v 20000k \
                -tag:v hvc1 \
                -movflags +faststart \
                "$out"
        fi
    else
        echo "VideoToolbox HEVC encoder not found; using libx265 fallback."
        if [ "$HAVE_AUDIO" -eq 1 ]; then
            ffmpeg -hide_banner -loglevel warning -y \
                -r "$FPS" -i "$RAW" \
                -f s16le -ar "$AUDIO_RATE" -ac 1 -i "$AUDIO_RAW" \
                -map 0:v:0 -map 1:a:0 \
                -vf "$filter" \
                -c:v libx265 \
                -preset medium \
                -crf 16 \
                -c:a aac -b:a 128k \
                -shortest \
                -tag:v hvc1 \
                -movflags +faststart \
                "$out"
        else
            ffmpeg -hide_banner -loglevel warning -y \
                -r "$FPS" -i "$RAW" \
                -vf "$filter" \
                -an \
                -c:v libx265 \
                -preset medium \
                -crf 16 \
                -tag:v hvc1 \
                -movflags +faststart \
                "$out"
        fi
    fi

    echo "MP4: $out"
}

focus_frame() {
    local out="$OUTDIR/${BASE}_focus.png"
    echo "Extracting a full-resolution frame for focus inspection..."
    ffmpeg -hide_banner -loglevel warning -y \
        -r "$FPS" -i "$RAW" \
        -vf "select='eq(n,25)'" \
        -frames:v 1 \
        "$out"
    echo "Focus frame: $out"
}

cmd=${1:-}
case "$cmd" in
    list)
        list_media
        ;;
    event|recording)
        [ $# -ge 2 ] || { usage; exit 1; }
        need_ffmpeg
        remote_for "$cmd" "$2"
        download_raw
        mode=${3:-4x3}
        case "$mode" in
            4x3) export_4x3 ;;
            wide) export_wide ;;
            *) echo "Mode must be 4x3 or wide"; exit 1 ;;
        esac
        ;;
    focus)
        [ $# -ge 3 ] || { usage; exit 1; }
        need_ffmpeg
        remote_for "$2" "$3"
        download_raw
        focus_frame
        ;;
    *)
        usage
        exit 1
        ;;
esac
