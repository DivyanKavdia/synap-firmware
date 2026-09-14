# Firmware architecture

## Source ownership

All three modules share one runtime. Edit canonical fragments in `firmware/shared/`, then run `node tools/assemble-source.cjs`. The generated `synap_esp32s3/synap_esp32s3.ino` remains checked in for portable Arduino downloads and existing native tests; CI verifies it matches the canonical source exactly.

`tools/materialize-target.cjs` selects the hardware adapter. S3 is a byte-for-byte passthrough. C3 adapts pins, LED, battery and single-core tasks. Chakshu selects native PDM PCM16, camera and SD drivers, removes touch/LED/battery GPIO initialization and sleep, and uses a separate 8 MB OTA target. Checked replacement anchors fail generation if shared code changes incompatibly.

| Responsibility | Edit location |
| --- | --- |
| Runtime types, public BLE IDs and shared state | `firmware/shared/runtime.cpp` |
| Device-bound OTA protocol and flash backend | `firmware/shared/ota.cpp` |
| Hardware feature descriptor | `firmware/shared/module-capabilities.cpp` |
| Power, mic lifecycle, battery and touch | `firmware/shared/power.cpp` |
| Recording session, transport negotiation and recovery | `firmware/shared/audio-session.cpp` |
| BLE callbacks and serialized control task | `firmware/shared/ble-control.cpp` |
| PCM capture and frame acquisition | `firmware/shared/audio-capture.cpp` |
| Codec, packetization and transmitter | `firmware/shared/audio-transport.cpp` |
| GATT service and boot | `firmware/shared/boot.cpp` |
| C3 differential behavior | `tools/boards/esp32c3/`, `firmware/esp32c3/` |
| Chakshu PDM, always-awake profile and pin exclusions | `tools/boards/xiao-sense/index.cjs` |
| Chakshu camera and filesystem drivers | `firmware/xiao-sense/camera.cpp`, `sd-storage.cpp` |
| SD hardware check worker and request/status protocol | `firmware/xiao-sense/media.cpp` |
| Target, image capacity and release paths | `tools/targets.cjs` |

The fragments inherit runtime types when assembled; they are not separate translation units. New board features belong behind a board adapter or feature driver, not a copied audio/BLE engine. See [Chakshu](CHAKSHU.md) for current feature boundaries and hardware checks.

The C3 LED template inherits runtime types and globals when inserted; compiling it
separately is unsupported. Both boards share the former C3 gesture behavior:
immediate double-tap recording control and a four-second hold with release for
sleep/wake. Only electrical wake arming and wake-cause validation vary: EXT0 on
S3 GPIO13, GPIO wake on C3 GPIO3. The C3 touch/wake templates and integration
adapter were removed so gestures cannot drift between targets.

Shared changes must be exercised on all three generated targets. Target-specific changes must preserve the other targets' behavior. Keep release identities, protocol versions and materialization anchors explicit. Comments should explain ownership, timing constraints or hardware reasons.

## Shared runtime contracts

### Audio and concurrency

- Capture: 16 kHz PCM16 mono, 800 samples / 50 ms frame; left I2S slot.
- Capture performs only signed I2S slot conversion (`raw >> 16`). The firmware high-pass filter and its state are removed; there is no software gain, gate, denoiser or voice activity suppression. First samples, DC and quiet PCM values survive conversion.
- INMP441 outputs 24 significant bits in a 32-bit slot. PCM16 drops the lower eight significant bits; the microphone's built-in ADC/filtering remains. Capture is unconditioned PCM16, not raw 24-bit microphone data.
- Transport prefers uncompressed protocol-v2 PCM16 (1,600 bytes/frame, 256 kb/s) at negotiated MTU >=185. Lower supported MTUs use independent 404-byte IMA ADPCM protocol-v3 frames (64.64 kb/s), so a lost frame cannot corrupt the next frame. MTU eligibility limits fragment count; it does not prove sustained radio throughput. START/RESUME selects the format; there is no hidden mid-connection codec switch.
- Transport adapts to ATT capacity; normal fragment pacing uses a 45 ms window.
- Connection preferences and the one-time request on each connection use 15–30 ms intervals, zero peripheral latency and a 6-second supervision timeout. These are requests; the phone controls the negotiated parameters.
- Audio notifications retry local stack rejection up to four attempts with 15/30/45 ms yielding backoff. A congested recovery frame retains its cursor for a later retry; a legacy session counts the lost frame and keeps capturing. Pacing restarts after each accepted notification, so a delayed callback cannot burst overdue fragments. Connection generations invalidate old sends even when a reconnect preserves the recording generation.
- Capture blocks while idle. Transmit blocks on its queue. A recursive microphone mutex serializes I2S reads, startup, shutdown and recovery.
- STOP waits for capture ownership and in-flight notification submission before acknowledging idle. Submission is not proof of phone persistence.
- BLE transitions use an atomic pending flag independent of command queue capacity. Stale commands do not skip control-loop maintenance.
- OTA state refresh cannot overwrite an active recording state. Diagnostics use atomic snapshots for cross-task state.

### Power and battery

CPU profiles are manual: 80 MHz idle, S3 240 MHz active, C3 160 MHz active. Paused OTA releases its boost after one second without commands and boosts before resumed flash work. The Arduino housekeeping loop waits one second after boot validation; control, capture, transmit and radio retain their own timing.

Standby stops microphone/I2S but keeps BLE available. C3 retains its connected LED heartbeat; S3 stays dark outside OTA. A disconnected, non-recording, non-OTA pendant can sleep after five minutes. Deep sleep is guarded by retained and durable markers; the shared four-second wake hold and stable release are checked before BLE starts. Touch gestures are ignored during OTA.

Battery sampling averages 16 readings every 15 seconds, with forced status samples. Notifications are suppressed during recording. Valid reconstructed cell range is 2.8–4.35 V; readings outside it remain diagnostic data and report unavailable. Low/critical thresholds are 3.60/3.40 V. Only S3 enforces confirmed critical-battery sleep/OTA guards. Wiring, calibration and the unresolved C3 charging discrepancy are documented in [hardware](HARDWARE_PINOUT.md).

No measured runtime extension is claimed. Assess complete battery-side current in recording, connected idle, standby, advertising and deep sleep before changing clocks, BLE intervals or supplies.

### Disconnect recovery

A compatible PWA explicitly arms volatile audio recovery for its session. Without that negotiation, disconnect stops recording. The ring always stores PCM, including when its eventual transmission uses ADPCM. S3 can reserve 600 PCM frames (30 seconds, about 965 KB) in PSRAM. Without PSRAM, 25 PCM frames (1.25 seconds, about 40 KB) require more than 140 KB free internal heap after BLE initialization; allocation failure leaves ordinary capture available.

Recovery expires after 60 seconds; STOP drain has a 35-second bound. Overflow, reboot, sleep and app reload have explicit limits. No audio is written to flash. See the [recovery protocol](DISCONNECT_RECOVERY.md) for session binding, pacing and acknowledgement semantics.

## BLE contracts

Primary service: `4fa12345-0000-1000-8000-00805f9b34fb`. Characteristic suffixes below replace the last two digits of its first UUID group.

| Suffix | Purpose | Protocol |
| --- | --- | --- |
| 46 | Audio | v2 PCM preferred; v3 ADPCM fallback |
| 47 | Control / status | v2 |
| 48 / 49 | OTA write / status | v3 |
| 4b | Firmware identity | Target and build |
| 4c | Public device ID | Factory eFuse-derived identity |
| 4d | Diagnostics | v2 (48 bytes; v1 fields retain their offsets) |
| 4e | Battery, touch and power events | Per-event version |
| 4f | Optional disconnect recovery | v1 |

The public device ID is not a secret. OTA validates target identity, image structure, size and SHA-256, and supports resume. Recording blocks OTA; confirmed critical battery additionally blocks it on S3. The PWA verifies publisher provenance separately; see [release trust](../OTA_RELEASES.md).

Diagnostics v2 retains the first 32-byte layout from v1, with version byte 2. Flags byte 2 adds `0x40` for real-microphone capture without firmware DSP. `0x80` identifies selected PCM transport. Its absence with `0x40` present identifies ADPCM fallback. Absence of `0x40` means unknown on older builds; existing consumers can ignore it. This flag does not claim lossless transport. Additional little-endian fields are disconnect reason (`u16`, offset 32), last rejected notification status (`u16`, 34), disconnect count since boot (`u32`, 36), last disconnect uptime in milliseconds (`u32`, 40), and last notification error (`u32`, 44). Link/error evidence survives START and reconnect. `0xFFFF` means the reason was unavailable; the pinned NimBLE server callback omits it. Bluedroid supplies the raw reason (for example, `0x08` for supervision timeout). Reboot is distinguishable through reset reason, uptime and reset counters; these diagnostics are volatile.

For a C3 disconnect retest, update firmware and the PWA, keep the app foregrounded, record for 10–15 minutes, then stop and inspect Settings diagnostics. Include the `GATT disconnected` and `Pendant diagnostics` entries. Repeat on battery and USB power if disconnects persist. Software regression checks cannot establish radio or power stability on physical hardware.

## Validation

`node --test tests/*.cjs` compiles actual firmware functions with warnings as errors and undefined-behavior sanitization. Tests cover codec bytes, all MTU values, all 65,536 PCM16 values, partial I2S reads, concurrent STOP/recovery, connection races, battery policies, LED timing, shared gestures under both board configurations, recovery limits, OTA resume and release identity. Gesture regressions include delayed STOP completion, short holds, stable wake release, OTA interruption, reconnect, timer wrap and rejection of the wrong board's wake cause.

Source-generation tests also exercise the CLI outside the repository working directory and reject changed/ambiguous anchors. CI compiles all three pinned Arduino targets and validates release artifacts.

Before claiming a runtime improvement, validate all affected physical boards (and the Chakshu checklist): long recordings and drop counters, START/STOP latency, repeated RF interruptions, touch gestures, sleep/wake, battery readings while charging/unplugged, OTA resume and battery-side current. The PWA preserves decoded PCM for playback and new cloud uploads. Enhancement is an explicit preview/export action; cloud transcription receives the stored upload without automatic trimming. Summaries and speaker identification remain outside this repository.

## Implementation references

- [Pinned Arduino I2S implementation](https://github.com/espressif/arduino-esp32/blob/3.3.5/libraries/ESP_I2S/src/ESP_I2S.cpp)
- [Pinned BLE notification implementation](https://github.com/espressif/arduino-esp32/blob/3.3.5/libraries/BLE/src/BLECharacteristic.cpp)
- [Apple connection parameter guidance](https://developer.apple.com/library/archive/qa/qa1931/_index.html)
- [Pinned BLE server callbacks and connection requests](https://github.com/espressif/arduino-esp32/blob/3.3.5/libraries/BLE/src/BLEServer.cpp)
- [INMP441 format and response](https://product.tdk.com/system/files/dam/doc/product/sw_piezo/mic/mems-mic/data_sheet/inmp441.pdf)
