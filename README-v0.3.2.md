# Crow Camera v0.3.2

This version fixes the v0.3.1 shutdown deadlock.

The key change is that a Crow stop request now calls `CVI_VENC_StopRecvFrame(0)`
in addition to setting the stop flag. The worker can be blocked inside the vendor
stream retrieval call, so the flag alone cannot wake it. Stopping receive wakes
the blocked VENC operation, allowing the worker to exit, be joined, and then run
normal VENC/VI/ISP cleanup.

It also prints explicit diagnostics around the worker join.
