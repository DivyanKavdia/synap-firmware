# Synap firmware variants and build targets

**Reviewed: 21 October 2026.** This is the functional-product mapping. The authoritative build identities and pin contracts are in `devices/catalog.json`; the authoritative installable build for each target is in the `ota-releases` feed. A firmware source commit is not proof that a physical device has been updated.

## Four functional device families; five production OTA targets

| Functional variant | Board and target | Recording when BLE/PWA is connected | Recording when disconnected | Additional behavior |
| --- | --- | --- | --- | --- |
| **Synap Odyssey S3** | ESP32-S3 SuperMini; `esp32s3-fh4r2-qspi-4m`; module 1 | Live 16 kHz audio to the PWA | No standalone SD recording | TTP223, battery, NeoPixel, standby; one-shot SD detection is not an SD recorder |
| **Synap Odyssey C3 (standard, no SD)** | ESP32-C3 SuperMini; `esp32c3-supermini-4m`; module 2 | Live 16 kHz audio to the PWA | No offline recording without a mounted SD card | TTP223, battery telemetry, NeoPixel, standby |
| **Synap Odyssey C3 + SD** | Same C3 board target and module 2, with a wired, mounted SD card | Live PWA audio; any existing offline SD take keeps its destination until stopped or finalized for PWA START | TTP223 double tap toggles local 16 kHz mono WAV capture to SD | Purple pulse for local SD recording; SD catalogue and verified import/sync to PWA |
| **Chakshu** | XIAO ESP32-S3 Sense; `xiao-esp32s3-sense-8m`; module 3 | PWA mic/photo/video controls capture to phone; idle Hey Snap captures visual actions to SD | Touch-initiated SD audio; Hey Snap plain photo, explicitly marked comprehension photo, or timed video | Camera, PDM mic, microSD, experimental local TinyML Hey Snap; voice inference suspended during PWA-owned audio/video capture |

**C3 standard and C3+SD share common application logic but have three supported OTA/partition tracks.** They are `esp32c3-supermini-4m` (legacy 1.25 MiB OTA slot), `esp32c3-supermini-4m-sd` (factory 1.875 MiB OTA slot, distinct marker) and `esp32c3-supermini-4m-sd-ota` (existing preprovisioned 1.875 MiB OTA slot that preserves the legacy firmware identity). A physically mounted healthy SD card makes optional SD capabilities *ready*; supported flags alone are not evidence of writable storage. Cardless C3 still supports BLE audio but not offline SD recording. Preserve these OTA identities, modules and partition sizes.

## Routing and safety contracts

- **Odyssey S3 / standard C3:** connected double tap starts/stops PWA audio. Disconnected recording is unavailable without a supported local recorder.
- **C3 + SD:** disconnected double tap starts an SD WAV; another double tap requests stop/finalization and extinguishes the purple pulse. BLE reconnect alone must not switch the destination of an in-progress take. PWA START must first finalize the offline take, or fail safely if microphone ownership cannot be released within the bounded deadline. Connected recording uses the existing green pulse.
- **C3 + SD synchronization:** SD catalogue appears in the PWA after BLE connection; media-v1 operation 4 includes the requested file path on *every* file chunk (`@catalogue` for catalogue bytes). Catalogue refresh must not replace a foreground file selection. The PWA durably imports and verifies a source, persists a receipt, and marks a retained SD copy as already synced. Deletion is a separate explicit user choice before or after sync; failed transfers retain the SD original. Normal reads do not remount; explicit operation 14 is recovery.
- **Chakshu:** Hey Snap is an independent source and saves to SD while BLE is disconnected or connected *and PWA capture is idle*. A PWA-initiated audio/video START suspends the voice listener and owns conflicting resources. Touch is audio-only: connected to PWA, disconnected to SD. Photo/video via Hey Snap are SD captures.
- **All targets:** active recording/media work, OTA and sleep transitions must not race. No OTA binary may be installed on a different target, even if the products have a similar name.

## Hardware and source ownership

| Variant | Relevant board pins / source |
| --- | --- |
| Odyssey S3 | I2S BCLK/WS/DATA GPIO4/5/6; touch GPIO13; battery GPIO8; NeoPixel GPIO48. Shared runtime and primary generated sketch. |
| Standard C3 and C3 + SD | I2S BCLK/WS/DATA GPIO4/5/6; touch GPIO3; battery GPIO1; NeoPixel GPIO8. Optional SD: CS GPIO0, SCK GPIO10, MOSI GPIO21, MISO GPIO20. Enable USB CDC on boot to keep UART0 off the SD pins. |
| Chakshu | Onboard PDM clock/data GPIO42/41; touch GPIO1/D0; battery GPIO2/D1; NeoPixel GPIO5/D4; Sense SD CS GPIO21. Camera, SD and TinyML implementation live in `firmware/xiao-sense/`. |

C3 SD production code lives in `firmware/shared/odyssey-sd-1631-detect.cpp`, `odyssey-sd-1631-recording.cpp` and `odyssey-sd-1631-transfer.cpp`. It uses ESP-IDF native SDSPI/FatFs under Arduino-ESP32 3.3.5, with a current 2 MHz C3 SD data clock, aligned multi-sector audio writes, append-only STOP behavior and virtual WAV-header synthesis on transfer. The retired Arduino SD CMD24 patch is not in the current workflow. The Rev K carrier PCB source, gerber archive, pin contract, BOM and case interface live in `hardware/odyssey-c3/pcb/final/`.

## Production/release model

The production workflow materializes, tests and compiles **five** target images (Odyssey S3, standard C3, factory C3+SD, existing large-slot C3+SD and Chakshu), then publishes separate OTA manifests. Always read the individual current feed before quoting a released build; installed devices may lag behind.

- [S3 manifest](https://github.com/DivyanKavdia/synap-firmware/blob/ota-releases/latest.json)
- [C3 manifest](https://github.com/DivyanKavdia/synap-firmware/blob/ota-releases/targets/esp32c3-supermini-4m/latest.json)
- [Chakshu manifest](https://github.com/DivyanKavdia/synap-firmware/blob/ota-releases/targets/xiao-esp32s3-sense-8m/latest.json)

## Minimum physical acceptance matrix

Check standard C3 *without a card* and C3 + SD *with a healthy card* separately. In particular: no-card BLE startup/recording; C3 cold SD mount and retry diagnostics; offline start and second-double-tap stop; purple-off after stop; reconnect during an SD take; PWA START handoff; catalogue amid chunked foreground sync; verified import and retained-copy state; explicit delete after sync; failed import leaving originals; restart/OTA only when idle. Chakshu camera and TinyML acceptance must be recorded separately from C3 SD results. A passing CI workflow does not establish physical-device acceptance.
