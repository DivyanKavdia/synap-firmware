# Synap Firmware

Production firmware for the Synap pendant.

The current source tree keeps explicit application targets for:

- `synap_esp32s3/` — ESP32-S3 SuperMini / ESP32-S3FH4R2
- `synap_esp32c3/` — ESP32-C3 SuperMini

Both targets share the same BLE application protocol, device identity model and resumable OTA protocol. Target-specific source is kept explicit so initial USB flashing, troubleshooting and manufacturing do not depend on build-time source rewriting.

## Current board wiring

### ESP32-S3 SuperMini

| Function | GPIO |
| --- | ---: |
| RGB status LED | 48 |
| I2S BCLK | 4 |
| I2S WS | 5 |
| I2S microphone data | 6 |

The sketch defaults to deterministic 440 Hz test audio unless `USE_REAL_I2S_MIC=1` is supplied at compile time.

### ESP32-C3 SuperMini

The C3 target is generated from the audited S3 application source by `tools/materialize-target.cjs`, which applies only the target-specific board identity and hardware pin mapping required for the C3 build. The generated C3 source is synchronized back into `main` after successful production publication.

## BLE service

Primary service UUID:

`4fa12345-0000-1000-8000-00805f9b34fb`

Characteristics include audio, control/status, stable public device identity, diagnostics, OTA write and OTA status.

The public device identity is derived from the ESP factory MAC and survives firmware updates. It is intended to let the PWA recognize the same physical pendant across reconnects and OTA releases without requiring users to manage an OTA key.

## Audio protocol

- PCM: 16 kHz, signed 16-bit, mono
- Frame duration: 50 ms
- Samples per frame: 800
- PCM bytes per frame: 1600
- BLE application packet header: 8 bytes
- Audio frame split across 10–20 BLE notifications depending on negotiated ATT capacity

The PWA starts and stops streaming through the control characteristic using protocol version 2.

## Status LED

ESP32-S3 uses the onboard GPIO 48 NeoPixel:

- red — disconnected
- blue — BLE connected / idle
- green — streaming
- error state — firmware error indication

## OTA model

Synap OTA is application-level BLE OTA and does not require Wi-Fi credentials on the pendant.

The PWA downloads the correct signed production firmware, validates the release metadata and sends the image over BLE. OTA state and byte offset are maintained by the pendant during a short BLE interruption so the PWA can reconnect and resume instead of restarting the transfer from byte zero.

The firmware verifies:

- target/device compatibility
- ESP image structure
- product and target markers
- SHA-256 digest
- target OTA partition

The public OTA release feed is maintained on the `ota-releases` branch by GitHub Actions.

See [`OTA_RELEASES.md`](OTA_RELEASES.md) for release/feed details.

## Build and release

Production releases are built by `.github/workflows/firmware.yml` with Arduino-ESP32 `3.3.5` and pinned dependencies.

The workflow:

1. runs firmware protocol/release tests
2. prepares audited target sources
3. compiles both ESP32-S3 and ESP32-C3 with one build number
4. creates OTA and factory binaries
5. attests production firmware provenance
6. atomically publishes the OTA feed
7. verifies the public manifests, binaries, provenance and CORS
8. synchronizes the explicit generated ESP32-C3 source back to `main`

## Initial USB flashing

For the first flash, use the target-specific sketch directory and the corresponding board target in Arduino IDE/Arduino CLI. After a compatible Synap OTA-enabled build is installed once, normal updates are intended to happen through the Synap PWA over BLE.

## Development notes

Do not reuse an ESP32-S3 firmware binary on an ESP32-C3 or vice versa. Although the product protocol is shared, the MCU targets and firmware images are different.

When changing the BLE protocol, OTA format, hardware mapping or partition assumptions, update the associated tests and release validation in the same change.