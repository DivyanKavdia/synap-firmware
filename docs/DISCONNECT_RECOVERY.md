# Short disconnect recovery

The feature is negotiated by a PWA session, and never enabled by an old client.
Existing gestures, battery calibration, target pins, OTA and sleep behavior are retained.

Read/notify value (16 bytes, little endian): `52 01 flags 00 capacity:u16 pending:u16 generation:u32 tokenHash:u32`.
Flags: bit 0 buffer available, bit 1 armed, bit 2 awaiting resume, bit 3 stopping/draining.
- ARM write: `01 token[8]` while idle. The full token is not returned by reads; tokenHash is FNV-1a of the eight bytes, acknowledging which ARM was applied. This is session binding, not BLE authentication.
- RESUME write: `02 token[8] lastCompleteSequence:u16`. The token must match; audio notifications and a valid MTU must already be configured. Writes are applied by the control task, not inside the BLE callback. The app waits for acknowledgement via the read value.

The ring retains independently encoded ADPCM frames with their original uint16 sequence numbers. Resume starts after the last complete frame observed by the same app journal; if that frame aged out, it starts at the oldest available frame and the PWA retains the sequence gap as silence. Normal frame pacing remains 45 ms; catch-up uses 30 ms only on transports with at most five fragments per frame. Memory pressure, client suspension, RF loss and platform limits still require device measurement. Notification submission is not a delivery acknowledgement; the app's last complete frame is the recovery boundary.

STOP freezes microphone capture, drains the remaining buffer and then acknowledges idle. Disconnection recovery is bounded at 60 seconds; stop drain is bounded at 35 seconds and reports a transport error if it expires. Audio is volatile: nothing survives reboot, power removal or sleep. This is not standalone recording or background iOS support.

Native tests run the production ring/request code for rollover, bounded overflow, token mismatch, stale connection writes, subscription/MTU refusal, stop drain and allocation fallback. Existing codec golden bytes, all ATT capacities, capture/STOP concurrency, C3/S3 gestures and OTA tests still run. PWA browser tests exercise recovery into one journal and stopping before catch-up finishes. Real S3/C3 free heap, long recordings, RF interruptions, Bluefy and battery life must be checked on devices before claiming a measured reliability improvement.
