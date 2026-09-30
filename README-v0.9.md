# Crow v0.9 — Wireless API + robust shutdown

Crow v0.9 keeps the validated v0.8 capture/audio pipeline and focuses on the interface the phone app will use.

## Capture path retained

- GC4653 -> VI/ISP -> hardware H.265
- 2560x1440 @ 25 fps
- 20 Mbps CBR
- 2-second keyframe-aligned video segments
- ~60-second rolling Hindsight buffer
- onboard microphone at 48 kHz mono S16_LE PCM
- atomic publication of completed H.265/PCM media

## Wireless/app API

The Crow web service now exposes:

- `GET /api/ping`
- `GET /api/status`
- `GET /api/media`
- `GET /api/capabilities`
- `POST /api/hindsight`
- `POST /api/record`
- `POST /api/shutdown`

The same endpoints work over the USB gadget address (`10.31.2.1:8080`) or over the Lichee's Wi-Fi IP on port 8080.

`/api/media` returns completed H.265 and PCM URLs suitable for the future React Native app. Media downloads now support HTTP byte ranges (`Range:` / `206 Partial Content`) so downloads can resume and future clients can seek efficiently.

## MP4 direction

v0.9 does **not** add FFmpeg to the hat. The board remains responsible for efficient capture/storage/transfer. The phone app will download H.265 + PCM and create/play a normal MP4. This avoids spending hat CPU, storage bandwidth, and battery on media conversion.

## Shutdown hang fix

Earlier builds could occasionally block inside `CVI_VENC_StopRecvFrame()` after printing:

`Stop requested. Finalizing current segment...`

v0.9 moves that vendor wake call off the control thread and adds a shutdown watchdog:

1. Crow requests a normal stop without blocking.
2. After 0.5 sec, a helper thread wakes the VENC path if needed.
3. Normal VENC/ISP cleanup still gets the first chance to finish.
4. If the encoder is still wedged after 5 sec, Crow stops audio, syncs storage, and exits instead of hanging forever.

The watchdog is a fallback; normal clean shutdown remains the expected path.

## Test

Build/deploy:

```bash
cd /workspace/crow
./build.sh
./deploy.sh
```

Then from Mac or another terminal:

```bash
curl http://10.31.2.1:8080/api/status
curl http://10.31.2.1:8080/api/media
curl http://10.31.2.1:8080/api/capabilities
```

Wi-Fi uses the same URLs with the board's Wi-Fi IP instead of `10.31.2.1`.

Once these endpoints are validated, the existing Crow React Native/Expo app can replace `MockHatDevice` with an HTTP-backed `HatDevice` implementation for status, recording controls, Hindsight, and media lists.
