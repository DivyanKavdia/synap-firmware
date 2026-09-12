# Firmware architecture

## Source ownership

One runtime supplies audio capture, encoding, BLE transport, OTA, optional recovery, battery sampling and task coordination for both boards. It remains in the established Arduino sketch path, `synap_esp32s3/synap_esp32s3.ino`, with S3 defaults and SDK target guards.

`tools/materialize-target.cjs` selects the board. S3 is a byte-for-byte passthrough. The C3 adapter in `tools/boards/esp32c3/index.cjs` applies identity/pin mapping, LED, battery, touch and single-core task overrides in that order. `tools/target-source.cjs` provides shared checked source edits and template loading. Missing or ambiguous single-replacement anchors abort generation.

| Change | Edit location |
| --- | --- |
| Shared audio, transport, recovery or OTA behavior | Production sketch |
| S3 LED, battery or gesture defaults | Production sketch; review C3 integration anchors |
| C3 battery ratio, ADC range, full-charge anchor or cutoff policy | `tools/boards/esp32c3/battery.cjs` |
| C3 LED pulse behavior | `firmware/esp32c3/status-led.cpp` |
| C3 LED setup, off state and deep-sleep hold | `tools/boards/esp32c3/led.cjs` |
| C3 awake gestures / boot wake confirmation | `firmware/esp32c3/touch.cpp` / `wake.cpp` |
| C3 function insertion boundaries | `tools/boards/esp32c3/touch.cjs` |
| C3 pins and task creation | `tools/boards/esp32c3/index.cjs` |
| Binary identity, capacity and release paths | `tools/targets.cjs`; coordinate source identity changes |

The C3 templates inherit runtime types and globals when inserted; compiling them separately is unsupported. The retained name `confirmTouchWakeTripleTap` is the shared insertion/call boundary; its C3 implementation validates a hold, not a triple tap.

Shared changes must be exercised on both generated targets. Target-specific changes must preserve the other target's behavior. Keep release identities, protocol versions and materialization anchors explicit. Comments should explain ownership, timing constraints or hardware reasons.

## Shared runtime contracts

### Audio and concurrency

- Capture: 16 kHz PCM16 mono, 800 samples / 50 ms frame; left I2S slot.
- Conditioning: unity-gain conversion followed by a 70 Hz first-order high-pass. There is no firmware speech-band denoiser, noise gate or automatic gain boost.
- Filter coefficients: a = 31880/32768, b = 32324/32768; y[n] = a*y[n−1] + b*(x[n]−x[n−1]). Q8 state, 64-bit intermediates, symmetric rounding and saturation retain quiet signals without extra audio frames.
- Filter history belongs to capture and resets on recording generation or successful I2S recovery. Synthetic tests verify response and bypass; hardware timing and speech quality require measurement.
- Encoding: independent 404-byte IMA ADPCM frames, so a lost frame does not corrupt the following frame.
- Transport adapts to ATT capacity; normal fragment pacing uses a 45 ms window.
- Capture blocks while idle. Transmit blocks on its queue. A recursive microphone mutex serializes I2S reads, startup, shutdown and recovery.
- STOP waits for capture ownership and in-flight notification submission before acknowledging idle. Submission is not proof of phone persistence.
- BLE transitions use an atomic pending flag independent of command queue capacity. Stale commands do not skip control-loop maintenance.
- OTA state refresh cannot overwrite an active recording state. Diagnostics use atomic snapshots for cross-task state.

### Power and battery

CPU profiles are manual: 80 MHz idle, S3 240 MHz active, C3 160 MHz active. Paused OTA releases its boost after one second without commands and boosts before resumed flash work. The Arduino housekeeping loop waits one second after boot validation; control, capture, transmit and radio retain their own timing.

Standby stops microphone/I2S but keeps BLE available. C3 retains its connected LED heartbeat; S3 stays dark outside OTA. A disconnected, non-recording, non-OTA pendant can sleep after five minutes. Deep sleep is guarded by retained and durable markers; the board-specific wake gesture is checked before BLE starts. Touch gestures are ignored during OTA.

Battery sampling averages 16 readings every 15 seconds, with forced status samples. Notifications are suppressed during recording. Valid reconstructed cell range is 2.8–4.35 V; readings outside it remain diagnostic data and report unavailable. Low/critical thresholds are 3.60/3.40 V. Only S3 enforces confirmed critical-battery sleep/OTA guards. Wiring, calibration and the unresolved C3 charging discrepancy are documented in [hardware](HARDWARE_PINOUT.md).

No measured runtime extension is claimed. Assess complete battery-side current in recording, connected idle, standby, advertising and deep sleep before changing clocks, BLE intervals or supplies.

### Disconnect recovery

A compatible PWA explicitly arms volatile audio recovery for its session. Without that negotiation, disconnect stops recording. S3 can reserve 600 encoded frames (30 seconds) in PSRAM. Without PSRAM, 100 frames (5 seconds) require more than 140 KB free internal heap after BLE initialization; allocation failure leaves ordinary capture available.

Recovery expires after 60 seconds; STOP drain has a 35-second bound. Overflow, reboot, sleep and app reload have explicit limits. No audio is written to flash. See the [recovery protocol](DISCONNECT_RECOVERY.md) for session binding, pacing and acknowledgement semantics.

## BLE contracts

Primary service: `4fa12345-0000-1000-8000-00805f9b34fb`. Characteristic suffixes below replace the last two digits of its first UUID group.

| Suffix | Purpose | Protocol |
| --- | --- | --- |
| 46 | Audio | v3 |
| 47 | Control / status | v2 |
| 48 / 49 | OTA write / status | v3 |
| 4b | Firmware identity | Target and build |
| 4c | Public device ID | Factory eFuse-derived identity |
| 4d | Diagnostics | v1 |
| 4e | Battery, touch and power events | Per-event version |
| 4f | Optional disconnect recovery | v1 |

The public device ID is not a secret. OTA validates target identity, image structure, size and SHA-256, and supports resume. Recording blocks OTA; confirmed critical battery additionally blocks it on S3. The PWA verifies publisher provenance separately; see [release trust](../OTA_RELEASES.md).

## Validation

`node --test tests/*.cjs` compiles actual firmware functions with warnings as errors and undefined-behavior sanitization. Tests cover codec bytes, all MTU values, filtering, partial I2S reads, concurrent STOP/recovery, connection races, battery policies, LED timing, both gesture models, recovery limits, OTA resume and release identity.

Source-generation tests also exercise the CLI outside the repository working directory and reject changed/ambiguous anchors. CI compiles both pinned Arduino targets and validates release artifacts.

Before claiming a runtime improvement, validate both physical boards: long recordings and drop counters, START/STOP latency, repeated RF interruptions, touch gestures, sleep/wake, battery readings while charging/unplugged, OTA resume and battery-side current. The PWA owns enhancement, transcription, summaries and speaker identification; those features are outside this repository.

## Implementation references

- [Pinned Arduino I2S implementation](https://github.com/espressif/arduino-esp32/blob/3.3.5/libraries/ESP_I2S/src/ESP_I2S.cpp)
- [Pinned BLE notification implementation](https://github.com/espressif/arduino-esp32/blob/3.3.5/libraries/BLE/src/BLECharacteristic.cpp)
- [INMP441 format and response](https://product.tdk.com/system/files/dam/doc/product/sw_piezo/mic/mems-mic/data_sheet/inmp441.pdf)
