# Chakshu runtime review — 15 September 2026

This review covers the XIAO ESP32S3 Sense path from source materialization through boot, microphone capture, BLE delivery/recovery, control, camera/SD, voice and firmware/model updates. The standard S3 and C3 paths were compared at their shared runtime and board adapters. The fixes below are confined to Chakshu; their materialized S3/C3 runtime is unchanged.

## Findings and corrections

| Boundary | Fault | Correction and regression evidence |
| --- | --- | --- |
| START vs offline recording | Checking `mediaBusy()` did not reserve it. A worker could claim SD/microphone ownership before START published streaming state, allowing two recording consumers. | START holds `ChakshuResources::Lease` through microphone initialization and streaming publication. A native test schedules a competing thread during initialization and covers failed/idempotent START. |
| Firmware OTA vs media | Media could start after OTA's busy check but before its flash transaction became visible. Voice/transfer workers also read the control task's mutable OTA engine. | OTA holds the same admission lease until its atomic snapshot is published. Workers use that snapshot. Tests cover media-first and OTA-first ordering, cancellation and retry. |
| Boot vs control task | Chakshu creates tasks before initializing BLE. Control could access partially initialized OTA/services. | Control waits for `runtimeReady`, published after BLE initialization. Commands can queue while startup completes. |
| SD refresh vs file transfer | An open selected file survived between BLE requests, while hardware refresh could unmount/remount the filesystem. | Keep the selected path; open, read and close under each resource lease. Tests remount between chunks and cover short reads, failed seeks, missing card and RAM-only photos. |
| Reconnect vs camera IDs | A repeated ID/operation from a new connection could be mistaken for an earlier completed capture. A BUSY result also prevented a valid retry. | Deduplicate within the connection, and permit retries of requests that were never admitted. Stale queued hardware jobs are cancelled before touching devices. |
| Reconnect vs transfer results | A delayed result could reuse the new connection's transaction ID or overwrite its response. | Responses retain their connection generation; stale results are discarded and old cached results read as pending. |
| BLE buffer admission | The previous packet-size estimate could refuse PCM although the actual packet fitted while preserving the control reserve. | Build 1235 allocates the packet first and checks actual remaining buffers; unsent allocations are released. Native tests cover reserve pressure, fragment progress and host failure ownership. |

## Ownership rules

`firmware/xiao-sense/ownership.cpp` defines the single nonblocking admission gate. `ChakshuMedia::busy` aliases that gate for existing media/model code. A failed lease does not release another task's ownership. START and OTA use short scoped leases; media workers and model uploads retain ownership through completion or abort. A transfer worker checks streaming state after acquiring the gate before starting offline microphone capture.

The recursive microphone mutex remains the driver lock. The idle voice task uses a nonblocking lock attempt and checks recording/media/OTA state again after acquisition. Active recordings provide voice inference with copies of PCM rather than allowing a second reader. Cross-task standby and SD readiness are atomic. Filesystem capacity and file mutation stay inside the resource owner; BLE UI reads use snapshots.

Online frame capture may share BLE audio; offline WAV/video recording cannot compete with BLE microphone capture. Offline video retains separate MJPEG, WAV and timestamp-index files. Model weights bundled in the firmware continue to install via the normal inactive-slot OTA without an SD card. SD remains necessary for standalone offline media storage.

## Other paths checked

| Area | Contract retained |
| --- | --- |
| Device selection | Catalog-defined pins, module IDs, feature/readiness masks and target markers; display names do not select hardware. |
| Microphone | Chakshu PDM clock 42/data 41, mono PCM16 at 16 kHz; S3/C3 INMP441 use their existing I2S slot conversion. Driver reads/init/stop are serialized. |
| Scheduling | Capture/control/transmit remain separate; voice receives bounded copied blocks. Camera/SD work stays outside BLE callbacks. |
| Recovery and Stop | Partial BLE fragments retain progress; replay is connection/session owned. Firmware drain remains bounded at 35 seconds. PWA Stop has independent no-progress and absolute deadlines. |
| Voice | “Hi Chakshu” gates commands; model validation, disabled/missing/error status, queue limits, online lease expiry and offline Stop routing remain explicit. |
| OTA | Target/device identity, inactive-slot size, SHA-256, chunk order and duplicate/resume handling remain enforced. |
| S3/C3 | Shared audio, battery, touch/standby/sleep and board-specific task/pin adapters remain intact. All targets require compilation in release CI. |

## Physical verification still required

Native regressions and board compilation do not measure radio throughput, PDM timing, speech recognition accuracy, SD latency or power stability. In particular, offline video interleaves camera/SD work with PDM reads; sustained sample loss under a slow card must be measured on a device.

After installing the release, verify these on Chakshu:

1. With no SD card, record 30 seconds through the PWA, speak throughout, stop, and play the saved audio. Compare elapsed time, received samples and firmware captured/drop/reject counters.
2. Repeat with local voice disabled and enabled. Confirm “Hi Chakshu” commands in idle, online and offline modes, including Stop while the PWA is suspended.
3. Capture a RAM photo during audio, then stop. With a card installed, record offline video and check that MJPEG, WAV and timestamp-index files all close and import/play correctly.
4. Reconnect and repeat the first photo request. Refresh hardware between SD download chunks; retry busy media requests. No old response should be accepted for the new connection.
5. Exercise disconnect/replay during recording and Stop, then OTA from idle. Audio/media activity must block an update and remain usable after an update is refused.

When reporting a failed run, include the installed target/build, PWA revision, exported diagnostics, SD presence and voice status. A connected badge or a microphone-ready flag establishes neither received audio nor sustained capture.
