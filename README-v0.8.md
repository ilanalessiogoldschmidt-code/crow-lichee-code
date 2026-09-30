# Crow v0.8 — Audio + Wi-Fi Media

Crow v0.8 keeps the validated v0.7.2 video path and adds the LicheeRV Nano's onboard analog microphone.

## Video

- GC4653 -> VI/ISP -> hardware H.265
- 2560x1440 @ 25 fps
- 20 Mbps CBR
- 2-second keyframe-aligned H.265 segments
- ~60-second rolling Hindsight buffer
- Hindsight: ~60 sec before + ~30 sec after
- Normal record start/stop
- Atomic final publication (`.part` -> final filename)

## Audio

The LicheeRV Nano has an onboard analog microphone. Crow uses ALSA `arecord`:

- `hw:0,0`
- 48 kHz
- mono
- signed 16-bit little endian
- raw PCM
- 2-second rolling audio segments
- ~90 seconds retained in the temporary audio ring (extra margin around the 60-second video ring)

Crow sends `SIGUSR1` to `arecord` at control boundaries so the current raw PCM segment closes cleanly and a new one starts.

Normal recordings and Hindsight events now produce both:

- `recording_000X.h265` / `event_000X.h265`
- `recording_000X.pcm` / `event_000X.pcm`

Both assembled outputs are hidden behind `.part` files until complete.

If ALSA audio fails to start, Crow intentionally continues video-only and points to `/root/crow_audio.log`.

## Mac media export

`crow-media.sh` now automatically downloads matching PCM audio when present and muxes it to AAC in the exported MP4.

Examples:

```bash
./crow-media.sh recording 5
./crow-media.sh event 3
./crow-media.sh recording 5 wide
```

Default export remains a centered 1920x1440 (4:3) crop. The VideoToolbox export bitrate is now 20 Mbps so the Mac export does not undo the source-quality improvement from v0.7.1.

## First test

Build/deploy normally:

```bash
cd /workspace/crow
./build.sh
./deploy.sh
```

After ~5 seconds, in a second Docker terminal:

```bash
cd /workspace/crow
./crowctl.sh status
```

Expected status includes something like:

```text
Microphone: running, approx audio rewind 4 sec
```

Then make a short normal recording:

```bash
./crowctl.sh record
# speak / clap / move for 15-20 seconds
./crowctl.sh record
```

Wait for both messages:

```text
Assembled recording_XXXX.h265 successfully.
Assembled recording_XXXX.pcm microphone audio (... segments).
```

On the Mac:

```bash
cd ~/crow-lichee-code
./crow-media.sh recording <number>
```

The resulting MP4 should contain video + microphone audio.

## Audio diagnostics

On the board:

```bash
pidof arecord
ls -lh /root/crow_audio | tail
cat /root/crow_audio.log
```

Sipeed's documented microphone capture path uses ALSA device `hw:0,0` at 48 kHz S16_LE and ADC Capture Volume 24.

## Current sync limitation

Audio and video each rotate in roughly 2-second pieces. Crow forces audio boundaries at control/finalization points, so sync should be close, but v0.8 is still the first A/V integration pass. A start offset of up to roughly one segment (~2 sec) is still possible in edge cases. We should measure and tighten this after confirming microphone capture works reliably.
