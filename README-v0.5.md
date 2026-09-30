# Crow Camera v0.5 — 60-second Hindsight Alpha

This version deliberately bundles several features at once on top of the validated v0.4 segmented H.265 pipeline.

## Included in v0.5

- Continuous 2560x1440 H.265 at 25 fps using the hardware encoder.
- Independent ~2-second H.265 segments with a 50-frame GOP / IDR boundary.
- A bounded **~60-second rolling rewind buffer**:
  - 30 completed segments x ~2 sec each.
  - Old ring files are pruned automatically.
  - The currently-open segment may temporarily make the directory contain 31 files.
- **Hindsight trigger** using `SIGUSR1` / `./trigger.sh`.
- Each Hindsight event saves:
  - up to ~60 seconds before the trigger, and
  - ~30 seconds after the trigger.
- Event preservation uses hard links when possible, so saving the previous minute is immediate and does not initially duplicate the same blocks on disk.
- Event directories survive restarts and are numbered automatically:
  - `/root/crow_events/event_0001/`
  - `/root/crow_events/event_0002/`
  - ...
- Event manifest with source segment mapping and completion state.
- Live status request using `SIGUSR2` / `./status.sh`.
- Clean Ctrl+C shutdown from the validated v0.3.2/v0.4 stop path.
- If Crow is stopped while an event is still collecting its +30 sec tail, the event is kept and marked partial rather than discarded.

## Event layout

Example:

```text
/root/crow_events/event_0001/
  pre_00.h265
  pre_01.h265
  ...
  pre_29.h265
  post_00.h265
  ...
  post_14.h265
  manifest.txt
```

If Crow has been running for less than one minute before the trigger, the event simply contains however much pre-event history exists.

## Build / deploy

```bash
cd /workspace/crow
./build.sh
./deploy.sh
```

Leave `./deploy.sh` running.

## Trigger Hindsight

Open a second Docker terminal:

```bash
cd /workspace/crow
./trigger.sh
```

Crow waits for the segment containing the trigger to finish, then preserves that segment plus the preceding ring history. This means the pre-event side includes the actual trigger moment with at most ~2 seconds of overlap after the signal.

## Request status

From a second Docker terminal:

```bash
cd /workspace/crow
./status.sh
```

The deploy terminal will print the current segment, approximate rewind depth, and event state.

## List saved events

```bash
cd /workspace/crow
./list-events.sh
```

## First useful test

1. Let Crow run for at least 70 seconds so the ring is full.
2. Run `./status.sh` and confirm rewind is about 60 seconds.
3. Run `./trigger.sh`.
4. Keep Crow running at least another 35 seconds.
5. Run `./list-events.sh`.
6. The event should contain roughly 30 `pre_*.h265` files + 15 `post_*.h265` files.
7. Stop Crow with Ctrl+C.
8. Confirm `/root/crow_segments` stays bounded at about 30 files rather than growing forever.

## Intentionally deferred

Audio/muxing, the physical GPIO button, BLE/app control, battery/power management, and final event remuxing are still separate workstreams. The v0.5 signal trigger is a software stand-in for the future physical Hindsight button.
