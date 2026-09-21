# Synap Firmware

**Current firmware baseline — 21 September 2026**

This repository owns production firmware for the Synap wearable family.

## Release and source status

- **Current production release:** Synap OS build **1400** (`synap-os1-build1400`).
- **Production source:** `cfe7859c64469ef8672ecb6fcfe548b6262b1bc6`.
- **Current development baseline:** `main`.
- **Release channel:** `ota-releases`.
- **Production targets:** Synap Odyssey S3, Synap Odyssey C3 and Chakshu.
- Build 1400 retains the hardened Chakshu SD boot/re-detection and BLE-exclusive Hey Snap ownership path, and adds the explicitly experimental 8-class personalized TinyML field model plus the offline Describe capture lifecycle. The OTA feed remains authoritative for what is installable on a physical device.

Every production release is compiled in CI, published atomically, attested with GitHub OIDC provenance and checked through the public firmware feed for digests, provenance and browser CORS.

## Supported targets

| Target id | Product | Board | Core capabilities |
| --- | --- | --- | --- |
| `esp32s3-fh4r2-qspi-4m` | **Synap Odyssey S3** | ESP32-S3 SuperMini | audio, settings, touch, battery, standby |
| `esp32c3-supermini-4m` | **Synap Odyssey C3** | ESP32-C3 SuperMini | audio, settings, touch, battery, standby |
| `xiao-esp32s3-sense-8m` | **Chakshu** | XIAO ESP32-S3 Sense | audio, camera, SD, settings, video, SD audio, photo |

The Odyssey naming is a **display/product-name change only**. Existing target ids, BLE advertising identities, OTA product markers, manifest paths and update compatibility identifiers must remain stable so devices already in the field are not orphaned.

The canonical hardware/release definition is `devices/catalog.json`. Hardware mapping, capability flags, OTA identity and release limits should be changed there first.

## Firmware architecture

Shared runtime behavior lives under `firmware/shared/`. Target adapters materialize the common source for the individual boards.

Chakshu-specific camera, SD, media-transfer and local-voice behavior lives under `firmware/xiao-sense/` without changing the common Odyssey S3/C3 audio contract.

The checked-in/generated target sketches are derived from the owned source components. Do not create or revive parallel firmware implementations for the same production target.

## Audio and BLE baseline

- 16 kHz mono capture.
- Adaptive BLE audio transport.
- Congestion-safe fragment progress.
- Bounded volatile disconnect recovery.
- Session/generation-safe replay.
- Asynchronous device controls.
- Target-aware BLE OTA with inactive-slot installation.
- Device capabilities reported independently from transient readiness.

A successful BLE notification enqueue is not proof that the browser persisted the corresponding bytes. CI verifies byte continuity and transport invariants; sustained real-device throughput remains a physical acceptance test.

## Chakshu baseline

Chakshu is the camera/SD member of the Synap family. Its current firmware path includes BLE audio, camera, SD media, OTA and a lightweight local TinyML wake/command runtime.

### Hey Snap

The current local runtime is the lightweight Synap TinyML implementation, not the older ESP-SR / WakeNet / MultiNet startup path.

Current locally classified commands are:

- **Hey Snap** — wake / open the short command window.
- **Take a snap / photo** — save a full-resolution photo to SD.
- **Record a video** — record a 10-second default video + soundtrack bundle to SD.
- **Record audio** — record a bounded 60-second WAV to SD.
- **Explain what you see** — save a tagged full-resolution photo to SD for description after verified PWA sync.
- **Stop** — stop/cancel the active local operation when the classifier owns the microphone.

The build-1400 classifier is intentionally an **experimental field baseline**. Its 8-class weights combine synthetic English augmentation with 22 user-supplied 16 kHz mono utterances: Record audio (8), Record video (6), and Explain what you see (8). Synthetic held-out accuracy was about 94% and fit on the available real utterances about 95.5%, but the limited independent real holdout was weak at roughly 30%. These figures are observations for continuity, not a production-accuracy claim. The next training pass should use more independently recorded, clearly separated real-device utterances plus negative/confusable examples and a true speaker/session holdout.

The reproducible eight-class candidate workflow and real-session split requirements are documented in [Voice training](docs/VOICE_TRAINING.md).

The model uses a small embedded learned-weight payload and an adaptive AC-noise/VAD gate. It removes microphone DC offset from level detection so low-level board/microphone bias does not look like permanent speech.

Voice protocol remains version **2**. Media protocol remains version **1**.

### One command owner at a time

The companion PWA now treats Chakshu as having one operational owner:

- **Disconnected from the PWA:** firmware owns Hey Snap and offline media actions.
- **Connected to the PWA:** the PWA disables the wake engine and owns media/BLE operations.

This avoids voice/media work racing the same serialized BLE, microphone, camera and SD resources.

Firmware now enforces the ownership boundary itself: any BLE connection stands the local wake engine down, and every BLE disconnect re-arms it, including unexpected out-of-range/browser drops where the PWA cannot send a release opcode. The PWA's voice on/off writes are session handoff signals rather than a persisted user preference. Physical verification of this reconnect path remains required.

### SD and recording indicator pin ownership

GPIO21 is both the onboard user LED and the Sense expansion-board SD chip-select. Firmware must not drive it from a status-light task, even to keep the LED off: that can interrupt an SD transaction. A separately controllable recording blink requires an external LED on a verified unused GPIO. The proposed onboard blink was withdrawn before release.

Build **1400** preserves the BLE ownership/SD safeguards and adds the experimental audio/describe voice routes. Queued standalone actions are invalidated on BLE connection and rejected while connected. Already running captures retain safe file finalization.

The experimental personalized model now includes Record audio and Explain what you see classes. Recognition quality remains under field observation and must be retrained with a larger independent real-device dataset before being treated as production-accurate.

### Offline media

Chakshu supports Synap-owned offline SD capture and recovery:

- Synap-owned SD FIFO cleanup when reserve space is required.
- App-triggered clearing of Synap capture files only.
- Verified move-to-app deletion semantics.
- Offline audio stored entirely on SD until explicitly moved into the companion app.
- Offline video stored on SD.
- Full-resolution still capture through the hardened saved-photo path.
- Two SD video profiles used by standalone firmware capture.
- Connected BLE clients cannot start SD audio/video recording; connected capture belongs to the PWA.
- The local spoken **Record a video** path currently uses a **10-second default**.
- The local spoken **Record audio** path is bounded to **60 seconds** because the recorder owns the microphone while active.
- **Explain what you see** saves a tagged JPG offline; visual inference occurs only after a later verified PWA sync.
- Imported offline audio enters the normal transcription and memory pipeline after transfer to the app.

User/model files outside the narrow Synap capture naming convention are not part of FIFO cleanup or Clear SD.

## Chakshu SD recovery

Production build **1400** retains the SD boot/re-detection and post-mount recovery hardening, corrected mount diagnostics, non-destructive recovery probes, and unsynced-capture protection.

### Mount behavior

Cold boot and card re-detection use the field-proven initialization sequence:

1. end any filesystem session,
2. initialize the Chakshu SPI pins once,
3. try the SD card handshake at **10 MHz**, then **4 MHz**, then **1 MHz** on that SPI session,
4. verify the `/synap` directory,
5. verify that filesystem capacity is readable,
6. only then mark the card ready.

If a card never mounts, no clock is treated as proven. The bus is left clean and a later **Check SD card** or catalogue request repeats the full 10→4→1 MHz detection sequence.

A different path is used after a card was mounted and then suffers a real I/O fault: firmware explicitly resets the SPI bus, retries at **1 MHz**, and keeps that conservative recovery clock for the remainder of the boot. Normal catalogue retries cannot raise it again.

No detection or recovery path formats the card or silently replays a failed capture.

### Failure diagnostics

Failed SD media responses may include bounded diagnostics. In build 1368, `sdClockHz` reports the **clock attempted by the current mount try**, not a previously successful/default value; `sdMountStage` distinguishes low-level `bus` failure from `no-card` when the bus responds but no card is detected:

- `sdReady`
- `sdClockHz`
- `sdMountStage`
- `sdMountAttempts`
- `sdRecoveryLocked`
- `freeHeap`

These fields are intended to distinguish an absent card from a card that mounted previously but later became unusable.

A mount alone is not considered proof of a healthy card; directory and capacity checks are part of readiness.

### Media safety

Current Chakshu media handling also:

- avoids starting local photo/video work while live BLE audio is using the conflicting runtime path,
- closes/releases the full-resolution camera path before SD writes where required,
- quarantines SD readiness after exhausted I/O failures,
- retries bounded file-selection/read/catalogue operations through the conservative recovery path,
- preserves the current capture from FIFO deletion while it is active.

The remaining field question is physical: whether the expansion-board/card path can sustain the required camera/video workload at the conservative clock without recurring I/O faults.

## Companion PWA contract

The companion PWA owns:

- connection/session control,
- all new audio/photo/video capture while BLE is connected; connected captures save directly to the PWA,
- disabling Hey Snap while it owns the live BLE link,
- SD catalogue discovery after reconnect,
- Library representation of unsynced SD-only audio, photo and video,
- explicit **Sync to app** for one item or all pending offline captures,
- byte/digest verification before deleting the SD original,
- imported-audio transcription,
- memory creation and downstream inference.

For the detailed offline/Hey Snap application contract, see the companion repository document:

`DivyanKavdia/synap-pwa/docs/CHAKSHU_OFFLINE_AND_HEY_SNAP.md`

## Build and validation

CI pins the ESP32 toolchain and board libraries and validates all three targets.

Core local checks:

```sh
node tools/assemble-source.cjs --check
node --test tests/*.cjs
```

Production CI additionally:

1. installs the pinned ESP32/Arduino dependencies,
2. applies required compatibility patches,
3. materializes Odyssey S3, Odyssey C3 and Chakshu production sources,
4. verifies embedded Chakshu voice assets/runtime constraints,
5. compiles all three targets with one build number,
6. creates OTA and factory artifacts,
7. attests production binaries,
8. publishes the release,
9. verifies public manifests, binaries, digests, provenance and CORS.

## OTA rules

- First installation can use USB.
- Routine compatible updates use the production OTA feed.
- Never interchange target binaries.
- Target, slot size, image structure and digest checks remain mandatory.
- Existing OTA product markers and manifest paths are compatibility identifiers; do not rename them as part of product branding.
- A source commit is not itself a release; the authoritative production feed determines the installable build.

## Physical acceptance boundary

Software CI validates source generation, protocol contracts, storage/recovery logic and board compilation. It does **not** establish real-world hardware quality.

The current hardware acceptance list is:

- sustained Odyssey S3/C3 microphone + BLE recording,
- Chakshu BLE reconnect and recovery,
- Hey Snap wake recognition after a clean standalone boot,
- Hey Snap automatic re-arm after an unexpected BLE disconnect,
- Take a snap recognition and saved-photo completion,
- Record a video recognition and durable SD completion,
- photo quality,
- timed SD video capture,
- long/offline SD audio recording,
- SD I/O recovery at conservative clocks,
- FIFO space reclamation,
- Sync to app followed by verified source deletion,
- complete device → PWA → transcript → memory flow.

Build **1400** is the current production hardware baseline. Physical testing should record the installed build explicitly and compare device logs against this README before attributing behavior to current source. A later `main` commit is not a device behavior until it is published through the OTA feed and installed.

## Live-source cleanup policy

Production firmware is materialized only from the shared source graph plus the current Chakshu TinyML components listed by `tools/boards/xiao-sense/index.cjs`. Retired external ESP-SR/MultiNet flash-model files and tooling are intentionally absent from `main`; do not restore them unless the production architecture is explicitly changed and the materialization/CI path is updated together.

## Working convention

`main` is the only current development baseline.

New work should branch from current `main`; do not revive superseded audit branches, abandoned voice implementations or older parallel architecture paths.

Keep this README focused on current product truth. Detailed implementation history belongs in Git commits, merged pull requests and published releases.
