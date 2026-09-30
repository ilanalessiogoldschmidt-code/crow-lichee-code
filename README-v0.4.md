# Crow Camera v0.4 — segmented H.265 recording

This version builds on the validated v0.3.2 stop/finalization path.

## What changes

- Keeps the GC4653 -> VI/ISP -> hardware H.265 pipeline alive continuously.
- Writes a new raw H.265 file every 50 encoded frames (~2 seconds at 25 fps).
- Uses a 50-frame GOP to align segment boundaries with random-access points.
- Requests an IDR at each rotation as an additional guard.
- Stores files under `/root/crow_segments/`:
  - `segment_000000.h265`
  - `segment_000001.h265`
  - ...
- Ctrl+C still uses Crow's clean stop path from v0.3.2.

## First validation target

Run for 10–15 seconds, stop once with Ctrl+C, and confirm several non-empty
segment files exist. Then copy several individual segments to the Mac and test
that each can be independently probed/played. Independent decodability is the
key validation before adding rolling retention/Hindsight.

## Not yet implemented

- 30-second ring deletion/retention
- Hindsight event preservation
- audio
- muxing/container format
