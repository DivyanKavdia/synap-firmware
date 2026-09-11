# Synap Firmware

Production firmware for the Synap pendant. Product version is **1.0.0**. The GitHub-attested `ota-releases` feed is the source of truth for the latest numeric build.

## Targets

| Target | MCU | Flash | PSRAM | Audio |
| --- | --- | ---: | ---: | --- |
| `esp32s3-fh4r2-qspi-4m` | ESP32-S3FH4R2 SuperMini | 4 MB | 2 MB | real I2S microphone |
| `esp32c3-supermini-4m` | ESP32-C3 SuperMini | 4 MB | none | real I2S microphone |

The S3 target is the primary physically validated pendant. The C3 target is built from the same production source contract; battery telemetry remains disabled until its hardware divider is validated.

## ESP32-S3 wiring

| Function | GPIO / connection |
| --- | --- |
| RGB status NeoPixel | GPIO48 |
| I2S BCLK / SCK | GPIO4 |
| I2S WS / LRCLK | GPIO5 |
| I2S microphone DATA / SD | GPIO6 |
| TTP223 OUT / SIG | GPIO13 |
| Battery ADC sense | GPIO8 |
| INMP44x / INMP441 L/R | GND / left channel |

## Touch and power

TTP223 is active-HIGH and momentary.

| State | Gesture | Result |
| --- | --- | --- |
| Connected idle | Double tap | Start recording |
| Recording | Double tap | Stop recording, then enter BLE standby |
| Idle, recording or BLE standby | Triple tap | Enter deep sleep; active recording stops first |
| BLE standby | Double tap | Wake and start recording |
| Deep sleep | Triple tap | Wake and continue normal boot |
| Deep sleep | One or two taps | Return to deep sleep without starting BLE |

Double-tap actions are confirmed after a short wait for a possible third tap. This keeps triple tap reserved as the power gesture without confusing it with Start/Stop.

The first touch electrically wakes the MCU from deep sleep, but firmware blocks BLE initialization until the triple-tap wake sequence is complete. A retained deep-sleep marker ensures an unexpected immediate reset cannot reconnect to the PWA without the wake gesture.

Touch is ignored during OTA. Short state-transition lockouts prevent one physical interaction from triggering multiple state changes.

BLE standby keeps the connection available while the microphone/I2S and status LED are off. The PWA can request standby after an idle period.

## BLE service and protocols

Primary service: `4fa12345-0000-1000-8000-00805f9b34fb`

- audio: `4fa12346-0000-1000-8000-00805f9b34fb`
- control/status: `4fa12347-0000-1000-8000-00805f9b34fb`
- OTA write: `4fa12348-0000-1000-8000-00805f9b34fb`
- OTA status: `4fa12349-0000-1000-8000-00805f9b34fb`
- firmware identity: `4fa1234b-0000-1000-8000-00805f9b34fb`
- public device ID: `4fa1234c-0000-1000-8000-00805f9b34fb`
- diagnostics: `4fa1234d-0000-1000-8000-00805f9b34fb`
- asynchronous events: `4fa1234e-0000-1000-8000-00805f9b34fb`

Protocols:

- control/status: **v2**
- audio transport: **v3**
- OTA: **v3**

The public `SYNAP-XXXXXXXXXXXX` identity is derived from the factory eFuse MAC and survives OTA. It is an identifier, not a secret.

## Audio

Capture format is 16 kHz, signed 16-bit, mono. Each 50 ms frame contains 800 PCM samples.

A conservative 70 Hz high-pass filter reduces very low frequency rumble before encoding. The conversion retains unity digital gain, and the filter does not gate quiet speech or change frame timing. It is not a speech-band noise suppression model. See [the audio review](docs/AUDIO_REVIEW.md) for measured filter response, a bypass build and the remaining hardware validation.

For BLE transport, each frame is independently encoded with IMA ADPCM. Independent frames prevent one lost frame from corrupting later audio.

Transport adapts to the negotiated ATT capacity. The firmware requests a large MTU where supported and uses a bounded notification payload. Audio capture and transmission are isolated so transient I2S or BLE issues do not unnecessarily terminate a recording.

The pendant streams audio while connected and does not store recordings locally. If BLE disconnects, capture stops and the queued audio is discarded.

## Battery

S3 battery divider:

```text
Battery + ---- 1 MOhm ----+---- GPIO8
                           |
                         470 kOhm
                           |
Battery - / GND -----------+---- GND

GPIO8 ---- 100 nF ---------- GND
```

Calibration reference: **4.13 V cell / 1.32 V ADC / raw 1544**.

Firmware averages ADC readings, publishes battery telemetry and estimates percentage from LiPo discharge anchors.

- low: 3.60 V
- critical: 3.40 V

Confirmed critical battery blocks a new OTA and aborts an active OTA before further flash writes.

## Runtime power behavior

- microphone/I2S starts only when needed;
- S3 CPU: 80 MHz idle / 240 MHz active;
- C3 CPU: 80 MHz idle / 160 MHz active;
- disconnected devices can enter deep sleep automatically;
- status LED uses low-duty indication;
- battery notification traffic is reduced while audio is streaming.

## OTA

Synap uses application-level BLE OTA; Wi-Fi credentials are not required.

Firmware validates device identity, hardware target, image structure, image size, SHA-256 and OTA partition state.

OTA v3 supports BEGIN, DATA, VERIFY, COMMIT, ABORT and RESUME. A short GATT interruption can resume the active OTA session from the reported offset. OTA is blocked while recording and when battery is critically low.

After commit, firmware reboots into the updated application.

## Build and release

`synap_esp32s3/synap_esp32s3.ino` is the production source of truth. `tools/prepare-production.cjs` copies it byte for byte. C3 source is generated from that sketch; both exact target sources are retained with build artifacts and releases.

`.github/workflows/firmware.yml` runs regression tests, compiles both targets, creates OTA/factory artifacts, adds GitHub provenance and verifies the production feed. Successful eligible main builds publish automatically. See [release details](OTA_RELEASES.md).

```sh
node --test tests/*.cjs
node tools/prepare-production.cjs synap_esp32s3/synap_esp32s3.ino prepared/synap_esp32s3/synap_esp32s3.ino
node tools/materialize-target.cjs esp32c3-supermini-4m prepared/synap_esp32s3/synap_esp32s3.ino prepared/synap_esp32c3/synap_esp32c3.ino
```

Capture uses real I2S by default. `-DUSE_REAL_I2S_MIC=0` selects a diagnostic test tone. Local USB builds identify as build 0; CI supplies the release build number. The capture task blocks while idle and wakes on START, avoiding a periodic 40 ms polling delay.

## Initial flash

The first installation is performed over USB with the target-specific board configuration. After an OTA-capable Synap firmware is installed, routine updates are delivered from the PWA over BLE.

## Release rules

- Use the production feed to determine the latest deployed build.
- Never flash an S3 binary onto a C3 or vice versa.
- Changes to BLE protocols, OTA format, pins, gestures, battery hardware, partitions or the PWA transport contract must update the corresponding regression tests.

## Validation

Before production release, validate BLE connect/reconnect, real microphone capture, double-tap start/stop, triple-tap deep sleep, triple-tap wake without premature BLE reconnect, battery guards, long recording stability, OTA update/resume/reboot, and reconnect after OTA.
