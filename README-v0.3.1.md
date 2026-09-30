# Crow Camera v0.3.1

Goal: continuous GC4653 -> VI/ISP -> hardware H.265 recording with a clean,
Crow-owned stop path.

## What changed from v0.3

- `main.c` owns SIGINT/SIGTERM instead of the Sipeed vendor library.
- Stop signals are blocked before the encoder thread is created, so Sipeed/VENC
  threads inherit a blocked signal mask and cannot be interrupted directly.
- Crow's main thread consumes Ctrl+C with `sigtimedwait()` and calls the normal
  `crow_request_stop()` API.
- The encoder stream loop observes the Crow stop flag and exits normally.
- `venc_main()` can then finish VENC/VI/ISP cleanup and return to Crow.
- Crow renames `/root/test-0.h265` to `/root/crow_test.h265` only after cleanup.

## Test

Inside the dev container:

```bash
cd /workspace/crow
./build.sh
./deploy.sh
```

Let it record for about 10 seconds, then press Ctrl+C once.

Expected ending:

```text
Stop requested. Finalizing recording...
...
Recording saved to /root/crow_test.h265

=== Recording result ===
Finalized Crow recording:
-rw------- ... /root/crow_test.h265
```
