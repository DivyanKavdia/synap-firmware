# Synap Firmware

Production firmware for the Synap pendant. Product version is **1.0.0**; the signed `ota-releases` feed is the source of truth for the latest numeric build.

## Production targets

| Target | MCU | Flash | PSRAM | Production audio |
| --- | --- | ---: | ---: | --- |
| `esp32s3-fh4r2-qspi-4m` | ESP32-S3FH4R2 SuperMini | 4 MB | 2 MB | real I2S microphone |
| `esp32c3-supermini-4m` | ESP32-C3 SuperMini | 4 MB | none | real I2S microphone |

The S3 target is the physically validated primary pendant. The C3 target is generated from the same final production source and compiled with real I2S capture, but its complete battery/mechanical hardware path is not yet physically validated. C3 battery telemetry therefore remains disabled until that divider is audited.

## ESP32-S3 production wiring

| Function | GPIO / connection |
| --- | --- |
| RGB status NeoPixel | GPIO48 |
| I2S BCLK / SCK | GPIO4 |
| I2S WS / LRCLK | GPIO5 |
| I2S microphone DATA / SD | GPIO6 |
| TTP223 OUT / SIG | GPIO13 |
| Battery ADC sense | GPIO8 |
| INMP44x / INMP441 L/R | GND / left channel |

The final prepared S3 source itself contains GPIO13 for touch. Production no longer depends on a hidden compiler-only pin override, so the source artifact and binary configuration are reproducible from the same file.

## Battery monitor

The audited S3 prototype divider is:

```text
Battery + ---- 1 MOhm ----+---- GPIO8
                           |
                         470 kOhm
                           |
Battery - / GND -----------+---- GND

GPIO8 ---- 100 nF ---------- GND
```

The calibration reference is the measured full-charge point **4.13 V cell / 1.32 V ADC / raw 1544**. Firmware averages 16 readings, publishes voltage/raw ADC telemetry, and estimates percentage from LiPo discharge anchors. Low is 3.60 V; critical is 3.40 V. A confirmed critical condition prevents a new OTA and now also aborts an in-progress OTA before further flash writes.

## Touch interaction

TTP223 is an active-HIGH digital input in momentary mode. The touch contract is intentionally small and state-specific:

| Pendant state | Gesture | Result |
| --- | --- | --- |
| Connected + idle | hold at least 2 s and less than 5 s, then release | start recording |
| Recording | double tap | stop recording |
| Recording | single tap or other hold shorter than 5 s | no action |
| Connected + idle | single tap / double tap | no action |
| Any non-OTA state | hold at least 5 s, then release | enter deep sleep; an active recording is stopped cleanly first |
| Deep sleep | hold touch continuously for 5 s, then release | wake and remain awake |
| Deep sleep | release before 5 s | return to deep sleep |

The 2-second START is evaluated on release, preventing a 5-second sleep gesture from accidentally starting a recording at the 2-second mark. A short state-transition lockout prevents the same physical contact from immediately reversing a state change. **Remember This is no longer assigned to the touch sensor.** Touch actions are ignored during OTA.

## BLE service and protocols

Primary service: `4fa12345-0000-1000-8000-00805f9b34fb`

- audio: `4fa12346-0000-1000-8000-00805f9b34fb`
- control/status: `4fa12347-0000-1000-8000-00805f9b34fb`
- OTA write: `4fa12348-0000-1000-8000-00805f9b34fb`
- OTA status: `4fa12349-0000-1000-8000-00805f9b34fb`
- firmware identity: `4fa1234b-0000-1000-8000-00805f9b34fb`
- public device ID: `4fa1234c-0000-1000-8000-00805f9b34fb`
- diagnostics: `4fa1234d-0000-1000-8000-00805f9b34fb`
- asynchronous event channel: `4fa1234e-0000-1000-8000-00805f9b34fb`

Control/status protocol remains **v2**. Audio transport is **v3**. OTA protocol is **v3**.

The public `SYNAP-XXXXXXXXXXXX` identity is derived from the factory eFuse MAC and survives OTA. It is an identifier, not a secret, and replaces user-managed OTA keys for normal update targeting.

## Audio transport

Capture remains 16 kHz, signed 16-bit, mono. Each 50 ms frame contains 800 PCM samples / 1600 PCM bytes. For BLE transport the frame is independently encoded as **404-byte IMA ADPCM**, so losing one frame cannot corrupt later frames.

The negotiated ATT capacity determines **1–20 notifications per frame**. With a large MTU the complete compressed frame fits in one notification, reducing browser/radio event pressure to about 20 audio notifications per second. Minimum supported ATT MTU is 32; requested MTU is 517; maximum audio payload is 500 bytes.

The transmitter task has an 8 KB stack and codec scratch buffers live in static storage. A single I2S read timeout no longer stops a take; the capture path allows a bounded three-read recovery window. An asynchronous MTU change after START no longer stops recording if the already-selected packet size still fits the new capacity.

## Recording lifecycle and phone suspension

The pendant intentionally does not store recordings locally. Audio exists in the PWA/browser and is streamed live over BLE. If iOS/browser suspension actually breaks the GATT connection, audio during that disconnected interval cannot be recovered without adding pendant-local storage.

The firmware does not deliberately stop a stream because the browser has been backgrounded. The PWA is responsible for resynchronizing status when it returns to the foreground and for preserving already-received audio.

## Power behavior

- microphone I2S is started on demand and stopped when idle
- S3 CPU: 80 MHz idle / 240 MHz active
- C3 CPU: 80 MHz idle / 160 MHz active
- disconnected deep sleep timeout: 5 minutes
- status LED uses short dim pulses rather than remaining continuously illuminated
- battery notification traffic is deferred while audio is streaming

Connected idle remains a BLE-connected state, not deep sleep. Further battery-life work should be driven by measured current in connected idle, recording, disconnected-awake and deep-sleep states.

## OTA

Synap uses application-level BLE OTA; Wi-Fi credentials are not required. The PWA selects a target-specific release and validates the signed manifest/provenance before transfer. Firmware validates device/target identity, ESP image structure, SHA-256 and OTA partition state.

Protocol v3 supports reconnect/resume across mobile suspension. A short GATT interruption does not deliberately restart at byte zero. OTA is blocked while recording and on confirmed critically-low battery.

## Build and release

`.github/workflows/firmware.yml` pins Arduino-ESP32 3.3.5 and Adafruit NeoPixel 1.15.2. The workflow:

1. runs source/unit regressions;
2. constructs the exact final S3 production source through the ordered patch chain;
3. materializes C3 from that final source;
4. compiles both targets with `USE_REAL_I2S_MIC=1` under one build number;
5. creates OTA/factory/source artifacts;
6. attests both binaries with GitHub provenance;
7. atomically publishes the release feed and verifies manifests, digests and CORS;
8. synchronizes the generated C3 source back to `main` without triggering another release.

The final production regression asserts cross-layer invariants after the entire patch chain, preventing a patch-level test from passing while the shipped source has different pins, protocol limits or runtime guards.

## Initial USB flash

The first installation uses the target-specific board configuration over USB. Once a compatible Synap OTA-enabled firmware is installed, normal updates are one-click BLE OTA from the PWA with no physical boot-button sequence and no user-pasted OTA key.

## Release discipline

Do not infer the latest deployed build from a Git commit or this README; inspect the signed production feed. Do not flash an S3 binary onto a C3 or vice versa. Any change to BLE protocol, OTA format, pin mapping, interaction gestures, battery divider, partition assumptions or PWA transport contract must update the corresponding final-pipeline regression in the same change.
