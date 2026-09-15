# Chakshu audio delivery and stopping

Chakshu uses NimBLE; the C3 and standard S3 targets keep their existing BLE implementations. Audio notifications share a finite mbuf pool with command, status and recovery traffic.

The transmitter allocates the actual notification packet and checks the remaining pool before submitting it. It leaves four blocks for control traffic. If the reserve would be crossed, it frees the unsent packet and retries the same fragment later. It does not estimate block use from the application byte count: that estimate previously refused a 408-byte PCM fragment whenever fewer than nine blocks were free, even if the actual allocation fitted with the control reserve intact.

The host owns a submitted packet on success and failure. The transmitter advances its fragment cursor only after a successful enqueue. The recovery ring advances only after all fragments of that frame have been accepted. PCM remains 16 kHz, mono, 16-bit; this change does not compress or modify recorded samples.

Native tests cover subscription ownership, actual allocation accounting, reserve rejection with release, enqueue failure and partial-frame retry. Firmware CI compiles all three board targets with the pinned toolchain. A physical Chakshu test is still required to measure sustained audio delivery and Stop latency on each browser.

For a stalled stream, compare capturedFrames, captureDrops, notifyRejects and lastNotifyError in the PWA diagnostics with received frame and packet counts. A screenshot of a connected badge alone cannot distinguish microphone capture failure from notification loss.
