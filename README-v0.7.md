# Crow Camera v0.7 — Quality + Wi-Fi Media

v0.7 keeps the validated v0.6 recording engine and adds a higher-quality video profile, a browser-based media/control UI, Wi-Fi test helpers, and a Mac media/export helper.

## Recording engine

- GC4653 -> VI/ISP -> hardware H.265
- 2560x1440 at 25 fps
- 10 Mbps CBR prototype quality profile
- 2-second keyframe-aligned segments
- ~60-second rolling Hindsight buffer
- ~60 sec before + ~30 sec after Hindsight trigger
- retrigger extends the Hindsight tail
- normal forward recording mode
- assembled raw H.265 event and recording files

## Why raw capture is still 16:9

The sensor/encoder pipeline remains at 2560x1440 so Crow keeps all available image data and does not destabilize the validated VI -> VENC path just to change composition.

For viewing/export, `crow-media.sh` defaults to a centered 1920x1440 (4:3) crop. This produces the taller, less-wide view without throwing away the side pixels at capture time. A later version can move that crop into the hardware VPSS path once the media/network layer is stable.

## Image quality / focus

v0.7 raises the encoder target from the roughly 2.5-3 Mbps observed in earlier tests to 10 Mbps CBR. This should substantially reduce compression softness.

To distinguish compression from optical focus, use:

```bash
./crow-media.sh focus recording 1
```

or:

```bash
./crow-media.sh focus event 2
```

Open the resulting full-resolution PNG at 100% zoom. If fine detail is uniformly soft even at the new 10 Mbps capture rate, the remaining issue is likely lens/sensor focus rather than encoded resolution.

## Browser UI

`deploy.sh` starts `crow_web` on port 8080.

While connected over USB networking, open:

```text
http://10.31.2.1:8080
```

The page can:

- trigger Hindsight
- toggle normal recording
- request terminal status
- list completed Hindsight events
- list completed normal recordings
- download the assembled raw H.265 files

After Wi-Fi is connected, the same page is available at the Lichee's Wi-Fi IP on port 8080.

## Wi-Fi first test

From Docker while USB networking is still connected:

```bash
./wifi-scan.sh
```

Then, for a normal WPA/WPA2 personal network or phone hotspot:

```bash
./wifi-connect.sh "SSID" "PASSWORD"
```

Check it later with:

```bash
./wifi-status.sh
```

The first-pass helper intentionally targets personal WPA/WPA2 networks. Enterprise networks such as eduroam need a different `wpa_supplicant` configuration, so a phone hotspot is a good first Wi-Fi test.

## Viewing / exporting media

Run `crow-media.sh` from the **Mac terminal**, not from Docker.

List media:

```bash
cd ~/crow-lichee-code
./crow-media.sh list
```

Download event 2 and create the default 4:3 MP4:

```bash
./crow-media.sh event 2
```

Download recording 1 and create the default 4:3 MP4:

```bash
./crow-media.sh recording 1
```

Preserve the original 16:9 image instead:

```bash
./crow-media.sh recording 1 wide
```

Outputs go to:

```text
~/Desktop/CrowMedia
```

## Timestamp warning fix

The Crow recording core currently stores raw H.265 elementary streams. Those streams do not carry MP4 packet timestamps by themselves, which is why a simple `ffmpeg -c copy` command produced:

```text
Timestamps are unset in a packet for stream 0
```

`crow-media.sh` fixes this when creating MP4 files. The `wide` remux explicitly generates 25 fps packet PTS/DTS with FFmpeg's `setts` bitstream filter. The 4:3 export re-encodes with an explicit 25 fps timeline.

When audio is added, Crow should graduate from raw H.265 event files to a timestamped A/V container/muxer on-device. That is the long-term fix rather than relying on post-processing.

## Build and deploy

Inside the Docker/VS Code container:

```bash
cd /workspace/crow
./build.sh
./deploy.sh
```

`build.sh` now produces two target binaries:

```text
build/crow_camera
build/crow_web
```

## Existing controls

```bash
./crowctl.sh hindsight
./crowctl.sh record
./crowctl.sh status
./crowctl.sh stop
./crowctl.sh poweroff
```
