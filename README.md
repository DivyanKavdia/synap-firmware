# Synap Firmware

Production firmware for the Synap pendant.

## Known-good baseline — 4 September 2026

This document is the engineering baseline for the best-known Synap firmware state as of **4 September 2026**.

- Product version: **1.0.0**
- Latest published ESP32-S3 production OTA build: **1052**
- ESP32-S3 target: `esp32s3-fh4r2-qspi-4m`
- ESP32-C3 target: `esp32c3-supermini-4m`
- Recording BLE protocol: **2**
- OTA protocol: **3**
- Production S3 uses the physical I2S microphone.
- TTP223 touch input on production S3: **GPIO13**.
- Pendant recordings remain stateless: audio is streamed to the PWA rather than stored locally on the pendant.

The source tree keeps explicit application targets for `synap_esp32s3/` and `synap_esp32c3/`. Both share the BLE application protocol, stable device identity and resumable OTA model.

## ESP32-S3 hardware wiring

| Function | GPIO / connection |
| --- | --- |
| RGB status NeoPixel | GPIO48 |
| I2S BCLK / SCK | GPIO4 |
| I2S WS / LRCLK | GPIO5 |
| I2S microphone DATA / SD | GPIO6 |
| TTP223 OUT / SIG | GPIO13 |
| TTP223 VCC | 3.3 V |
| TTP223 GND | GND |
| INMP44/INMP441 VDD | 3.3 V |
| INMP44/INMP441 GND | GND |
| INMP44/INMP441 L/R | GND / left channel |

Production S3 releases compile with `USE_REAL_I2S_MIC=1`. The deterministic 440 Hz source is retained only as a development fallback.

## Touch interaction model

The production interaction layer uses a debounced gesture state machine rather than a simple touch toggle.

- debounce: approximately **35 ms**
- double-tap window: approximately **500 ms**
- long press: approximately **1.2 s**
- sleep hold / very long press: approximately **3 s**

Expected user interactions:

| Pendant state | Gesture | Result |
| --- | --- | --- |
| Connected + idle | Long press | Start recording |
| Recording | Long press | Create **Remember This** marker; recording continues |
| Recording | Double tap | Stop recording |
| Idle | Very long press (~3 s) | Enter deep sleep |
| Deep sleep | Touch/wake input | Wake and resume BLE advertising |

Touch actions are guarded during OTA. The Remember event is sent to the PWA, which associates the marker with the current recording and timeline offset. No recording audio is stored on the pendant.

## BLE service and identity

Primary service UUID: `4fa12345-0000-1000-8000-00805f9b34fb`

Important characteristics:

- audio: `4fa12346-0000-1000-8000-00805f9b34fb`
- control/status: `4fa12347-0000-1000-8000-00805f9b34fb`
- OTA write: `4fa12348-0000-1000-8000-00805f9b34fb`
- OTA status: `4fa12349-0000-1000-8000-00805f9b34fb`
- firmware identity: `4fa1234b-0000-1000-8000-00805f9b34fb`
- permanent public device ID: `4fa1234c-0000-1000-8000-00805f9b34fb`
- diagnostics: `4fa1234d-0000-1000-8000-00805f9b34fb`

The permanent `SYNAP-XXXXXXXXXXXX` identity is derived from the ESP factory MAC and survives OTA. It replaces user-managed OTA keys for normal operation and update targeting.

## Audio transport

- PCM: 16 kHz, signed 16-bit, mono
- I2S capture: 32-bit samples converted to PCM16
- frame: 50 ms / 800 samples / 1600 PCM bytes
- BLE audio header: 8 bytes
- frame split: 10–20 BLE notifications according to negotiated ATT capacity
- requested MTU: 517
- maximum audio payload: 160 bytes

Production S3 has been physically validated with the real microphone path and audio reaching the PWA.

## LED / interaction feedback

GPIO48 is the onboard RGB NeoPixel. Core state semantics remain:

- red: disconnected
- blue: connected / idle
- green: recording / streaming
- error indication for firmware faults

The interaction layer can temporarily pulse feedback for touch actions such as Remember without changing the underlying state.

## Recording and phone-screen behavior

The pendant intentionally has no local recording store. A recording exists in the PWA/browser and the pendant streams live PCM over BLE.

If the browser merely suspends JavaScript while the screen is off but BLE remains connected, the PWA recovery path should not falsely stop the recording when the page resumes. If the phone/browser actually terminates the BLE GATT connection while locked, audio cannot be captured during that disconnected interval because there is no pendant-local audio buffer/storage.

This distinction is important when diagnosing screen-lock failures: inspect Diagnostics for an actual GATT disconnect rather than treating every foreground gap as a firmware stop.

## OTA model

Synap uses application-level BLE OTA; Wi-Fi credentials are not required on the pendant. The PWA selects the release by permanent device identity and hardware target.

OTA verifies target/device compatibility, ESP image structure, product/target markers, SHA-256 digest and the OTA partition. Protocol 3 supports reconnect/resume rather than deliberately restarting from byte zero after a short BLE interruption.

The production release feed is maintained on the `ota-releases` branch by GitHub Actions. As of this baseline the S3 production feed points to build **1052**.

See `OTA_RELEASES.md` for feed details.

## Build and release

`.github/workflows/firmware.yml` builds with Arduino-ESP32 3.3.5 and pinned dependencies. Production S3 compile flags include real-mic capture and `SYNAP_TOUCH_PIN=13`.

The workflow tests release/protocol preparation, prepares S3/C3 production sources, compiles both targets under one build number, creates OTA/factory artifacts, publishes production metadata/binaries, verifies the feed and synchronizes generated C3 source where required.

## Battery/power note

The interaction preparation contains battery/deep-sleep infrastructure, but unaudited battery ADC monitoring is intentionally disabled in the production runtime guard until the final battery sensing hardware/divider is validated. Deep-sleep touch behavior is independent of enabling battery ADC telemetry.

## Initial USB flashing

The first installation uses the target-specific sketch/board configuration over USB. Once a compatible Synap OTA-enabled firmware is installed, normal firmware updates are intended to be one-click BLE OTA from the PWA with no physical boot-button sequence and no user-pasted OTA key.

## Release discipline

Treat **4 September 2026 / build 1052** as the reference baseline when investigating regressions. Do not assume that a later Git commit is a later published firmware: confirm the production `ota-releases/latest.json` build and target first.

Do not flash an S3 binary onto a C3 or vice versa. Any change to BLE protocol, OTA format, pin mapping, interaction gestures or partition assumptions must update the corresponding tests and documentation in the same change.