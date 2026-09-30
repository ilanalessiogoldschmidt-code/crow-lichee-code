# Crow Camera v0.6 — Multi-Mode Prototype

This release builds on the validated v0.5 rolling Hindsight buffer and intentionally bundles several product-level features.

## What v0.6 adds

- 60-second rolling rewind buffer (30 x ~2-second independent H.265 segments)
- Hindsight save: ~60 sec before + ~30 sec after
- Retriggerable Hindsight: pressing Hindsight again while an event is active extends the post-event tail ~30 seconds from the new trigger
- Automatic assembly of each Hindsight event into one `event_XXXX.h265`
- Normal forward recording mode while the rolling buffer continues in parallel
- Automatic assembly of each normal recording into one `recording_XXXX.h265`
- Unified `crowctl.sh` command interface
- Live status including ring depth, Hindsight state, normal-recording state, saved counts, and filesystem free space
- Clean remote poweroff helper
- Existing validated clean encoder/ISP shutdown and keyframe-aligned segmentation retained

## Commands (from Docker /workspace/crow)

Start Crow:

```bash
./deploy.sh
```

From a second Docker terminal:

```bash
./crowctl.sh status
./crowctl.sh hindsight
./crowctl.sh record
./crowctl.sh record      # run again to stop normal recording
./crowctl.sh stop
./crowctl.sh poweroff
```

Compatibility wrappers still work:

```bash
./trigger.sh
./status.sh
./record.sh
./poweroff.sh
```

List saved media:

```bash
./list-events.sh
./list-recordings.sh
```

## Suggested validation

1. Let Crow run for >65 seconds so the one-minute ring fills.
2. `./crowctl.sh status` should report about 60 sec rewind.
3. `./crowctl.sh hindsight`.
4. After ~10 sec, trigger Hindsight again. The active event should extend rather than ignore the request.
5. Wait >35 sec after the second trigger. `./list-events.sh` should show an assembled `event_XXXX.h265`.
6. Start normal recording with `./crowctl.sh record`, run 10–20 sec, then run it again. `./list-recordings.sh` should show an assembled `recording_XXXX.h265`.
7. Copy the assembled files to the Mac and verify with `ffprobe` / playback.

## Current prototype limitations

- Audio is not integrated yet.
- Physical GPIO button is not integrated yet; commands currently use Unix signals over SSH.
- H.265 output is still an elementary stream, not MP4/MKV.
- Board clock may not be valid, so naming uses monotonic IDs instead of wall-clock timestamps.
- Normal recording start/stop aligns to ~2-second segment boundaries, so it may include up to roughly one segment around the requested boundary.
