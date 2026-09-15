# Chakshu core capture review — 15 September 2026

The release removes local voice recognition and concentrates the active runtime on manual audio, photos, video, SD and OTA. Reintroducing a recognizer needs a separate measured change after those paths work reliably on the device.

## Evidence and changes

| Area | Finding | Release action |
| --- | --- | --- |
| Firmware startup | The target adapter initialized the embedded model and recognizer before BLE advertising. Recognition allocated AFE/model state, additional queues and tasks. | Remove model code, boot initialization, PCM copying, listener tasks, voice/model characteristics and build downloads. Record media and total boot times plus heap/PSRAM in the serial log. |
| Latest audio | The shell115 log records 465 and 394 complete PCM frames, zero missing/incomplete frames, normal journal seals and successful transcription. | Keep the PCM transport and 30-second recovery implementation. No added firmware audio DSP. Native tests continue to cover exact PCM samples, partial reads, backpressure, replay and Stop. |
| Camera transfer | Every JPEG takes one command/read pair per 480 bytes, in addition to capture setup. VGA frames can take many Bluefy round trips. | Request QVGA for online video; retain VGA for standalone photos and SD. Show transfer progress. Preserve one response/selected buffer owner, request IDs and byte validation. |
| Camera abort | The 13:55:50 AbortError occurs immediately before Stop while still connected. PWA Stop explicitly aborts the pending snapshot. | Treat that boundary as inconclusive evidence of a camera defect. Keep complete-frame-only saving. The separate screenshots of connection loss still require a physical run with the new image. |
| Microphone ownership | The retired idle recognizer kept the PDM driver open; SD and overlapping camera/Stop cleanup could leave it running. | Release the PDM driver at media completion when no stream owns it. START remains excluded by the media admission gate. |
| Reconnection | Restored Bluefy handle fails with code 2; manual selection succeeds. Backoff progresses 1.2/2.6/5.2/10 seconds. | Retain backoff and core-first PWA discovery. Do not infer a firmware reset from the browser error. Removal also eliminates periodic voice lease writes. |
| SD | Latest capabilities report microphone/camera ready but no SD. | Keep SD-independent online capture and explicit hardware refresh. Never format, delete or replace existing recordings/models. |
| Older recordings | Seventeen older recordings are still marked for retry; the latest two transcriptions succeed. | Do not erase recordings or claim firmware removal repairs already unreadable saved data. Existing PWA recovery remains available. |
| Wake lock | Bluefy screen-awake request and browser wake lock are denied/timed out. Audio still succeeds while visible. | No claim of continuous background capture. Existing finite recovery remains. |

## Boundaries checked

The firmware keeps the XIAO PDM pins 42/41, LCD_CAM camera pins, PSRAM JPEG buffer and separate SPI SD bus. The camera/SD worker owns frame and file buffers; GATT callbacks only copy bounded commands/results. Audio allocation leaves a host buffer reserve for ATT control. Recovery retains an in-flight PCM frame through ring eviction. OTA owns the same admission gate and targets the inactive partition. S3 and C3 build profiles and source behavior remain separate.

The new optional preview hint is operation 1, offset 1. Offset 0 remains a VGA photo. Media protocol remains version 1 because old firmware ignores that field during exposure and old clients continue to request VGA. Voice protocol is zero, and UUIDs 56–59 are absent. Existing inactive OTA-slot bytes and SD model files need no destructive cleanup.

Native checks cover sensor mode switching, allocation/sensor/capture failures, returned frame ownership, descriptor values, exact PCM, ATT callback behavior, SD remount/ownership and OTA. Browser checks exercise manual photo/video, complete transfer bytes/progress, cancellation, account ownership, saved audio and playback against a simulated pendant, including older voice-capable firmware. Firmware CI compiles and publishes all three targets with source hashes and provenance.

## Physical acceptance still needed

Update firmware and reconnect. Read the serial `ready_ms`/`media_ms` boot line if USB is available. Then record audio, take a standalone photo, record video until at least one frame appears, and Stop/save. Confirm the saved photo, video frames and separate audio are playable; repeat with audio already recording, SD absent and after a reconnect. Compare capture/receive counts and first-frame latency. A mock or successful link cannot establish radio throughput, camera output, SD card health or boot duration on the actual board.
