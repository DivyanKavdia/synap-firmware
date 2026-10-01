# Synap firmware variants and build targets

**Reviewed: 1 October 2026.** This is the functional-product mapping. The authoritative build identities and pin contracts are in `devices/catalog.json`; the authoritative installable build for each target is in the `ota-releases` feed. A firmware source commit is not proof that a physical device has been updated.

## Four functional variants; three production OTA targets

| Functional variant | Board and target | Recording when BLE/PWA is connected | Recording when disconnected | Additional behavior |
| --- | --- | --- | --- | --- |
| **Synap Odyssey S3** | ESP32-S3 SuperMini; `esp32s3-fh4r2-qspi-4m`; module 1 | Live 16 kHz audio to the PWA | No standalone SD recording | TTP223, battery, NeoPixel, standby; one-shot SD detection is not an SD recorder |
| **Synap Odyssey C3 (standard, no SD)** | ESP32-C3 SuperMini; `esp32c3-supermini-4m`; module 2 | Live 16 kHz audio to the PWA | No offline recording without a mounted SD card | TTP223, battery telemetry, NeoPixel, standby |
| **Synap Odyssey C3 + SD** | Same C3 board target and module 2, with a wired, mounted SD card | Live PWA audio; any existing offline SD take keeps its destination until stopped or finalized for PWA START | TTP223 double tap toggles local 16 kHz mono WAV capture to SD | Purple pulse for local SD recording; SD catalogue and verified import/sync to PWA |
| **Chakshu** | XIAO ESP32-S3 Sense; `xiao-esp32s3-sense-8m`; module 3 | PWA mic/photo/video controls capture to phone; idle Hey Snap commands still capture to SD | Hey Snap photo/video/audio and touch-initiated SD audio | Camera, PDM mic, microSD, experimental local TinyML Hey Snap; voice inference suspended during PWA-owned audio/video capture |

**C3 standard and C3 + SD do not use different firmware binaries.** The same C3 OTA image contains both behaviors. A physically mounted, healthy SD card makes the SD features *ready* at runtime; supported capability flags alone do not establish that storage is present or writable. If SD is absent or unmounted, ordinary BLE audio must remain usable and disconnected double tap cannot record to SD. Do not add a fourth `devices/catalog.json` target, renumber modules or create a second C3 OTA feed merely to represent the hardware variant.

## Routing and safety contracts

- **Odyssey S3 / standard C3:** connected double tap starts/stops PWA audio. Disconnected recording is unavailable without a supported local recorder.
- **C3 + SD:** disconnected double tap starts an SD WAV; another double tap requests stop/finalization and extinguishes the purple pulse. BLE reconnect alone must not switch the destination of an in-progress take. PWA START must first finalize the offline take, or fail safely if microphone ownership cannot be released within the bounded deadline. Connected recording uses the existing green pulse.
- **C3 + SD synchronization:** SD catalogue appears in the PWA after BLE connection; media-v1 operation 4 includes the requested file path on *every* file chunk (`@catalogue` for catalogue bytes). Catalogue refresh must not replace a foreground file selection. The PWA durably imports and verifies a source before requesting deletion; failed transfers retain the SD original. Normal reads do not remount; explicit operation 14 is recovery.
- **Chakshu:** Hey Snap is an independent source and saves to SD while BLE is disconnected or connected *and PWA capture is idle*. A PWA-initiated audio/video START suspends the voice listener and owns conflicting resources. Touch is audio-only: connected to PWA, disconnected to SD. Photo/video via Hey Snap are SD captures.
- **All targets:** active recording/media work, OTA and sleep transitions must not race. No OTA binary may be installed on a different target, even if the products have a similar name.

## Hardware and source ownership

| Variant | Relevant board pins / source |
| --- | --- |
| Odyssey S3 | I2S BCLK/WS/DATA GPIO4/5/6; touch GPIO13; battery GPIO8; NeoPixel GPIO48. Shared runtime and primary generated sketch. |
| Standard C3 and C3 + SD | I2S BCLK/WS/DATA GPIO4/5/6; touch GPIO3; battery GPIO1; NeoPixel GPIO8. Optional SD: CS GPIO0, SCK GPIO10, MOSI GPIO21, MISO GPIO20. Enable USB CDC on boot to keep UART0 off the SD pins. |
| Chakshu | Onboard PDM clock/data GPIO42/41; touch GPIO1/D0; battery GPIO2/D1; NeoPixel GPIO5/D4; Sense SD CS GPIO21. Camera, SD and TinyML implementation live in `firmware/xiao-sense/`. |

C3 SD code lives in `firmware/shared/odyssey-sd-detect.cpp`, `odyssey-sd-recording.cpp` and `odyssey-sd-transfer.cpp`. Its SD lifecycle uses native ESP-IDF SDSPI/FAT and VFS, boot-time initialization before BLE and explicit recovery. The Rev K carrier PCB source, gerber archive, pin contract, BOM and case interface live in `hardware/odyssey-c3/pcb/final/`.

## Production/release model

The production workflow materializes and compiles **three** targets (S3, C3, Chakshu), publishes their separate OTA manifests, and attests the binaries. As checked on 1 October 2026, the `ota-releases` manifests publish **build 1546** from firmware source commit `21bb5488ecdf7b128e560d59b30b583bcd634feb` for all three. Always read the current feed before quoting a *current* version; an installed physical device may still be on an older build.

- [S3 manifest](https://github.com/DivyanKavdia/synap-firmware/blob/ota-releases/latest.json)
- [C3 manifest](https://github.com/DivyanKavdia/synap-firmware/blob/ota-releases/targets/esp32c3-supermini-4m/latest.json)
- [Chakshu manifest](https://github.com/DivyanKavdia/synap-firmware/blob/ota-releases/targets/xiao-esp32s3-sense-8m/latest.json)

## Minimum physical acceptance matrix

Check standard C3 *without a card* and C3 + SD *with a healthy card* separately. In particular: no-card BLE startup/recording; C3 cold SD mount and retry diagnostics; offline start and second-double-tap stop; purple-off after stop; reconnect during an SD take; PWA START handoff; catalogue amid chunked foreground sync; verified import before delete; failed import leaving originals; restart/OTA only when idle. Chakshu camera and TinyML acceptance must be recorded separately from C3 SD results. A passing CI workflow does not establish physical-device acceptance.
