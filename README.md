# Synap firmware

This repository contains the production firmware source and OTA release tooling for Synap pendants.

## Supported hardware

- ESP32-S3 SuperMini / ESP32-S3FH4R2 target
- ESP32-C3 SuperMini target

The S3 source is maintained in `synap_esp32s3/synap_esp32s3.ino`. The C3 source is materialized from the same protocol/runtime design and synchronized into `synap_esp32c3/synap_esp32c3.ino` by the release workflow.

## BLE protocol

The firmware exposes the Synap BLE service and characteristics used by `synap-pwa` for:

- audio streaming
- recording control/status
- stable public device identity
- diagnostics
- resumable BLE OTA

Firmware and PWA protocol changes should remain backward-compatible wherever possible.

## ESP32-S3 hardware defaults

Current S3 source defaults:

- 16 kHz mono PCM
- GPIO 48 onboard RGB LED
- I2S BCLK GPIO 4
- I2S WS GPIO 5
- I2S data-in GPIO 6

`USE_REAL_I2S_MIC` can be enabled for physical I2S microphone builds. When disabled, the firmware emits a deterministic 440 Hz test tone for transport validation.

## Releases

`.github/workflows/firmware.yml` builds and publishes signed OTA release artifacts from `main`.

The public OTA feed is published to the `ota-releases` branch. Release publishing includes board identity, build number, SHA-256 integrity data, and provenance checks.

See `OTA_RELEASES.md` for OTA release details.