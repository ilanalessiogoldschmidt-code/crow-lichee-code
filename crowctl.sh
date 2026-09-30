#!/bin/bash
set -euo pipefail

DEVICE=${CROW_DEVICE:-root@10.31.2.1}
CMD=${1:-}

usage() {
    cat <<'USAGE'
Usage: ./crowctl.sh <command>

Web UI: http://10.31.2.1:8080

Commands:
  hindsight   Save ~60 sec before + ~30 sec after (retrigger extends tail)
  status      Print live Crow status in the deploy terminal
  record      Toggle normal recording start/stop
  stop        Cleanly stop Crow
  poweroff    Cleanly stop Crow, wait for exit, then power off the board
USAGE
}

if [ -z "$CMD" ]; then
    usage
    exit 1
fi

case "$CMD" in
    hindsight)
        SIGNAL=USR1
        ;;
    status)
        SIGNAL=USR2
        ;;
    record)
        SIGNAL=HUP
        ;;
    stop)
        SIGNAL=TERM
        ;;
    poweroff)
        ssh "$DEVICE" '
            pid=$(pidof crow_camera || true)
            if [ -n "$pid" ]; then
                kill -TERM "$pid"
                i=0
                while pidof crow_camera >/dev/null 2>&1 && [ "$i" -lt 40 ]; do
                    sleep 0.25
                    i=$((i + 1))
                done
            fi
            sync
            poweroff
        '
        echo "Clean shutdown requested. Wait ~10 seconds before unplugging USB-C."
        exit 0
        ;;
    *)
        usage
        exit 1
        ;;
esac

ssh "$DEVICE" "
    pid=\$(pidof crow_camera || true)
    if [ -z \"\$pid\" ]; then
        echo 'Crow is not running.'
        exit 1
    fi
    kill -$SIGNAL \"\$pid\"
    echo '$CMD command sent to Crow PID' \"\$pid\"'.'
"
