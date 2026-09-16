# Short disconnect recovery

The feature is negotiated by a PWA session, and never enabled by an old client.
Existing gestures, battery calibration, target pins, OTA and sleep behavior are retained.

Read/notify value (16 bytes, little endian): `52 01 flags replayAck:u8 capacity:u16 pending:u16 generation:u32 tokenHash:u32`.
Flags: bit 0 buffer available, bit 1 armed, bit 2 awaiting resume, bit 3 stopping/draining, bit 4 connected replay supported, bit 5 Stop requested. Older firmware leaves unsupported flags zero; older apps ignore them.
- ARM write: `01 token[8]` while idle. The full token is not returned by reads; tokenHash is FNV-1a of the eight bytes, acknowledging which ARM was applied. This is session binding, not BLE authentication.
- RESUME write: `02 token[8] lastCompleteSequence:u16`. The token must match; audio notifications and a valid MTU must already be configured. Writes are applied by the control task, not inside the BLE callback. The app waits for acknowledgement via the read value.
- REPLAY write: `03 token[8] lastCompleteSequence:u16`, only when bit 4 is present. This handles a web page losing callbacks while BLE stays connected. It rewinds the volatile ring after the supplied boundary without restarting capture, changing format or changing the connection. It requires the matching armed token, active capture, an audio subscription and no pending disconnect/Stop drain. Successful application increments `replayAck` modulo 256. The app reads the previous acknowledgement and waits for it to change in the same generation; an unchanged streaming status is not acknowledgement. The original buffer capacity and overflow behavior still apply.

The ring retains uncompressed PCM16 frames with their original uint16 sequence numbers. S3 PSRAM reserves up to 600 frames (30 seconds); the C3/no-PSRAM fallback uses 25 frames (1.25 seconds) within about 40 KB. It does not encode audio while retaining it. Sending uses uncompressed protocol v2 at MTU >=185, otherwise the existing ADPCM v3 fallback. The same token/status protocol works with both formats. Resume starts after the last complete frame observed by the same app journal; if that frame aged out, it starts at the oldest available frame and the PWA retains the sequence gap as silence. Normal frame pacing remains 45 ms; catch-up uses 30 ms only on transports with at most five fragments per frame. Memory pressure, client suspension, RF loss and platform limits still require device measurement. Notification submission is not a delivery acknowledgement; the app's last complete frame is the recovery boundary.

STOP freezes microphone capture, drains the remaining buffer and then acknowledges idle. Disconnection recovery is bounded at 60 seconds; stop drain is bounded at 35 seconds and reports a transport error if it expires. Audio is volatile: nothing survives reboot, power removal or sleep. This is not standalone recording or background iOS support.

A physical STOP during disconnection follows the same drain path when recovery
is armed. Capture ends immediately; the matching RESUME can still transfer the
retained frames. Bit 5 and the token hash remain after the ring expires, including
through idle connection reconciliation. This receipt prevents the owning app
from restarting a stopped take when it finds the pendant idle. It retains no
audio and is cleared by a new ARM, START or reboot. The app applies it only to an
already-confirmed interrupted take, before rotating its token; an undelivered new
START must not inherit an earlier take's Stop. Bit 3 still means an active drain.

Chakshu consumes control writes through its characteristic's `writeEvent` override,
without storing the two command bytes in the readable status value. Status
notifications carry the explicit 16-byte snapshot, so a later write cannot change
an already requested notification. This fixes build1231's command echoes during
STOP drain. C3/S3 consume owned command bytes through the pinned Arduino BLE
library patch, independently of readable status snapshots.

Chakshu audio checks host mbuf headroom before allocating a notification, leaving
room for ATT control/recovery traffic. When congestion exhausts a fragment's retry
budget, the transmitter retains the first unsent fragment index. The recovery
ring retries from that index rather than resending the accepted prefix forever.
A different stream, connection, frame or packet layout resets the cursor; a
completed frame can still be replayed in full. These are local enqueue guarantees,
not acknowledgements that the phone saved audio.

If the link drops during STOP drain, a restored connection must still complete RESUME before the buffer can be considered drained. Waiting for that handshake is not an empty buffer. The existing absolute drain deadline continues to apply during the interruption.

Native tests run the production ring/request code for rollover, bounded overflow, token mismatch, stale connection writes, subscription/MTU refusal, stop drain and allocation fallback. Existing codec golden bytes, all ATT capacities, capture/STOP concurrency, C3/S3 gestures and OTA tests still run. PWA browser tests exercise recovery into one journal and stopping before catch-up finishes. Real S3/C3 free heap, long recordings, RF interruptions, Bluefy and battery life must be checked on devices before claiming a measured reliability improvement.

Capture/transmit errors are latched outside the command queue. The control task
consumes the first error for the active stream generation, even if the command
queue is full or the connection epoch changes during recovery. A stopped or newer
take ignores stale producer errors. The native control-task test injects queue
saturation, reconnect and stale-generation faults; both board builds remain CI
gates. These checks do not replace the physical acceptance runs above.
