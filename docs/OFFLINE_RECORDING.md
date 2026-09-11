# PSRAM reconnect buffer proposal

Status: revised scope; proposed, not implemented. The pendant uses PSRAM only to bridge a momentary disconnection during an already active recording. It does not provide standalone offline recording or persistent audio storage.

## User experience

- Recording starts only while connected to the PWA. Booting or double-tapping while disconnected does not start recording.
- If an active recording loses its connection, continue capture briefly into a bounded PSRAM queue and indicate that reconnection is in progress.
- On reconnection, resume the same recording and immediately start sending queued audio while capturing new audio. Release each queued frame only after the PWA acknowledges receipt into the recording; a BLE connection or successful notification alone is not delivery confirmation.
- Once the backlog is acknowledged, clear the used buffer and return to normal live streaming. Do not retain a recording archive on the pendant.
- If the reconnect grace period expires or the queue fills, stop capture, discard buffered audio, and report the unrecovered gap when the PWA returns. Do not keep recording through a prolonged disconnection or silently overwrite frames.
- A local STOP stops the microphone immediately. Any remaining frames may drain if the app returns within the same grace period. An explicit sleep, reset or power loss discards the temporary buffer.

No audio writes to flash, NVS, a filesystem or SD are part of this feature. There is no deferred recording library or offline start mode.

## Suggested initial limits

Use a **30-second reconnect grace period** and a **512 KiB PSRAM allocation** on the S3. The grace period defines a brief interruption; the allocation also leaves room for live frames arriving while the backlog drains. These are proposed defaults to validate on hardware.

The current source produces 404-byte independent ADPCM frames every 50 ms, or 8,080 audio bytes per second. Allowing 16 bytes of metadata per frame, 30 seconds needs 252,000 bytes; a 512 KiB queue holds approximately 62 seconds total before implementation overhead. Queue capacity is not permission to record disconnected for that long: the 30-second limit still applies.

The configured S3 has 2 MiB PSRAM. Check its actual availability and allocate the queue explicitly in PSRAM. If the allocation fails, report that reconnect buffering is unavailable and retain the current stop-on-disconnect behavior. The configured C3 has no PSRAM and therefore does not offer this feature. Do not silently substitute flash or a large internal-RAM recording buffer.

## Capture and reconnect design

1. Separate recording-session state from BLE-link state. The existing disconnect callback immediately disables capture and the control path clears the live queue; those actions must become conditional on an eligible, explicitly started session with buffering negotiated by the PWA.
2. Encode each frame once. Keep microphone/DMA staging in internal RAM and store the bounded compressed queue in PSRAM. Tag frames with an ephemeral session ID, a 32-bit frame index and sample offset. Keep only a bounded, unacknowledged tail during live streaming so frames in flight at disconnection can also be recovered.
3. Require the returning PWA to resume the matching session before replay. Deduplicate frames by session ID and index, preserve sample order, and advance the queue only on cumulative acknowledgements. A connection without a resumed app session must not clear the queue or reset the grace deadline.
4. Begin backlog transfer as soon as the app resumes. Do not wait for recording to stop. Use bounded batches and application acknowledgements with measured BLE backpressure; new audio must continue to enter the queue without blocking capture.
5. Reset the interruption budget after the backlog is fully acknowledged. Repeated connection flaps must not extend unattended recording indefinitely. If a renewed disconnect arrives after the original grace deadline while backlog remains, stop and clear the queue.
6. Clear session buffers on session replacement, timeout, overflow, explicit sleep or reset. Block OTA while capture or backlog draining is active. Report discarded intervals accurately; never synthesize missing speech or merge different sessions.

The current transmitter deliberately budgets 45 ms per frame. At that pace, its ideal audio throughput is approximately 8,978 bytes/s, only 898 bytes/s above capture. A 30-second backlog would therefore take roughly 270 seconds to drain even before BLE overhead. Fast reconnect recovery requires a separate, flow-controlled catch-up path with sustained throughput above capture rate; simply replaying through the live pacing loop will not meet the intended experience. Slow links must respect the same queue bound and report overflow instead of growing memory use.

## Implementation and acceptance

Deliver the firmware capability and matching PWA session-resume/acknowledgement path together. A PWA without the capability retains the existing live-only behavior.

Validate short disconnects, the 30-second deadline, reconnect during active capture, repeated flaps, lost acknowledgements, duplicate replay, sequence rollover, a mismatched app session, queue exhaustion, PSRAM allocation failure, local STOP, sleep, reset and OTA exclusion. Measure catch-up throughput, time to clear the queue, capture drops and memory/stack headroom on a physical S3.

Acceptance requires no disconnected recording start, no audio writes to persistent storage, no silent frame loss or duplication, timely catch-up on supported links, and an empty session buffer after successful recovery. C3 behavior must remain functional without PSRAM buffering.
