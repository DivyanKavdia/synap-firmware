# Synap Firmware

Production firmware for the Synap pendant. Product version is **1.0.0**. The GitHub-attested `ota-releases` feed is the source of truth for the latest numeric build.

## Targets

| Target | MCU | Flash | PSRAM | Audio |
| --- | --- | ---: | ---: | --- |
| `esp32s3-fh4r2-qspi-4m` | ESP32-S3FH4R2 SuperMini | 4 MB | 2 MB | real I2S microphone |
| `esp32c3-supermini-4m` | ESP32-C3 SuperMini | 4 MB | none | real I2S microphone |

The S3 target is the primary physically validated pendant. The C3 target is built from the same production source contract; battery telemetry is enabled for a 1 MΩ / 1 MΩ divider on GPIO1 with a 100 nF capacitor to ground. C3 voltage calibration is being validated against hardware.

## Wiring

| Function | ESP32-S3 SuperMini | ESP32-C3 SuperMini |
| --- | --- | --- |
| I2S BCLK / SCK | GPIO4 | GPIO4 |
| I2S WS / LRCLK | GPIO5 | GPIO5 |
| I2S microphone DATA / SD | GPIO6 | GPIO6 |
| TTP223 OUT / SIG | GPIO13 | GPIO3 |
| Status LED | GPIO48, onboard RGB | GPIO8, onboard blue (active-low) |
| Battery ADC sense | GPIO8, 1 MΩ / 470 kΩ | GPIO1, 1 MΩ / 1 MΩ |
| INMP44x / INMP441 L/R | GND / left channel | GND / left channel |
| Microphone VDD / TTP223 VCC | 3V3 | 3V3 |
| Peripheral ground | GND | GND |

The microphone pins are shared. Touch, battery sense and status LED require different pins to preserve the S3 wiring and C3 hardware compatibility. C3 uses only its onboard blue LED; no external NeoPixel is needed. See [hardware pinout](docs/HARDWARE_PINOUT.md) for the constraints and wiring details.

## Touch and power

TTP223 is active-HIGH and momentary. The two hardware targets intentionally use different power gestures. S3 keeps the existing deployed interaction model; C3 uses a simpler long-press power gesture while retaining double tap for recording.

### ESP32-S3 SuperMini

| State | Gesture | Result |
| --- | --- | --- |
| Connected idle | Double tap | Start recording |
| Recording | Double tap | Stop recording, then enter BLE standby |
| Idle, recording or BLE standby | Triple tap | Enter deep sleep; active recording stops first |
| BLE standby | Double tap | Wake and start recording |
| Deep sleep | Triple tap | Wake and continue normal boot |
| Deep sleep | One or two taps | Return to deep sleep without starting BLE |

On S3, double-tap actions are confirmed after a short wait for a possible third tap. This keeps triple tap reserved as the power gesture without confusing it with Start/Stop.

### ESP32-C3 SuperMini

| State | Gesture | Result |
| --- | --- | --- |
| Connected idle | Double tap | Start recording immediately on the second valid tap |
| Recording | Double tap | Stop recording, then enter BLE standby |
| BLE standby | Double tap | Wake and start recording |
| Any awake non-OTA state | Hold at least 4 seconds, then release | Enter deep sleep; active recording stops first |
| Deep sleep | Hold at least 4 seconds through wake validation, then release | Wake and continue normal boot |
| Deep sleep | Short touch | Return to deep sleep without starting BLE |
| Any awake state | Single tap | No action |

On C3, triple tap is not used. The second valid tap acts immediately because there is no need to wait for a possible third tap. A four-second hold is used for both sleep and wake. The firmware waits for release before completing sleep so the GPIO3 HIGH-level wake source cannot immediately wake the device again. Wake validation measures four continuous seconds after firmware starts, so allow brief boot overhead. Release is consumed before normal boot, and waking does not automatically start recording. A press interrupted by OTA is discarded.

For both targets, the first touch electrically wakes the MCU from deep sleep, but firmware validates the target-specific wake gesture before allowing BLE initialization. A retained deep-sleep marker and durable sleep lock prevent an unexpected reset from reconnecting to the PWA without a valid wake gesture.

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

Transport adapts to the negotiated ATT capacity. The firmware requests a large MTU where supported and uses a bounded notification payload. Audio capture and transmission are isolated so transient I2S or BLE issues do not unnecessarily terminate a recording. Microphone access is synchronized across tasks, and STOP waits for capture and notification submission to finish before reporting idle. BLE link changes are handled independently of command-queue capacity. See the [runtime review](docs/RUNTIME_REVIEW.md) for the fixes and validation.

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

S3 calibration reference: **4.13 V cell / 1.32 V ADC / raw 1544**.

C3 wiring: battery positive through 1 MΩ to GPIO1, then 1 MΩ from GPIO1 to ground, with the 104 capacitor (100 nF) between GPIO1 and ground. The owner's latest meter reference is 3.99 V at the cell and 1.36 V at the junction. C3 provisionally uses calibrated ADC millivolts × 3990 / 1360, rounded to the nearest millivolt. The full-charge anchor stays 4.15 V = 100%; 3.99 V maps to approximately 84%. This measured ratio differs from a nominal equal-resistor divider. It assumes the ADC reports the same junction voltage: the earlier 340 mV ADC reading would still be invalid and must be investigated rather than forced into a percentage. ADC attenuation stays at 11 dB. S3 calibration is unchanged. The app shows percentage for valid readings, with electrical readings retained for diagnostics.

Firmware averages ADC readings, publishes battery telemetry and estimates percentage from LiPo discharge anchors.

- low: 3.60 V
- critical: 3.40 V

On S3, confirmed critical battery blocks a new OTA and aborts an active OTA before further flash writes. The C3 trial reports voltage, estimated percentage and low-battery status, while automatic battery-triggered sleep and OTA lockout remain inactive pending voltage validation. C3 retains its four-second touch sleep/wake and disconnected-timeout sleep.

## Runtime power behavior

- microphone/I2S starts only when needed;
- S3 CPU: 80 MHz idle / 240 MHz active;
- C3 CPU: 80 MHz idle / 160 MHz active;
- paused OTA returns to 80 MHz after one second without commands and boosts before resumed flash work;
- disconnected devices can enter deep sleep automatically;
- status LED uses low-duty indication;
- battery notification traffic is reduced while audio is streaming.

S3 non-OTA standby stays dark, including at low battery. C3 connected standby retains two 80 ms blue flashes every 3 seconds; disconnected C3 uses one 100 ms flash every 6 seconds, and recording keeps one 100 ms blink per second. Deep sleep stays dark on both targets. The housekeeping loop wakes once per second after boot validation; audio and touch tasks keep their own timing. See the [power review](docs/POWER_REVIEW.md) for further CPU/BLE experiments and battery-life measurements.

## OTA

Synap uses application-level BLE OTA; Wi-Fi credentials are not required.

Firmware validates device identity, hardware target, image structure, image size, SHA-256 and OTA partition state.

OTA v3 supports BEGIN, DATA, VERIFY, COMMIT, ABORT and RESUME. A short GATT interruption can resume the active OTA session from the reported offset. OTA is blocked while recording and when battery is critically low.

After commit, firmware reboots into the updated application.

## Build and release

`synap_esp32s3/synap_esp32s3.ino` is the production source of truth. CI copies it byte for byte. C3 source is generated from that sketch; both exact target sources are retained with build artifacts and releases. C3-specific pin, single-core and touch/power behavior is applied only during C3 target materialization, so the S3 source remains unchanged.

`.github/workflows/firmware.yml` runs regression tests, compiles both targets, creates OTA/factory artifacts, adds GitHub provenance and verifies the production feed. Successful eligible main builds publish automatically. See [release details](OTA_RELEASES.md).

```sh
node --test tests/*.cjs
mkdir -p prepared/synap_esp32s3
cp synap_esp32s3/synap_esp32s3.ino prepared/synap_esp32s3/synap_esp32s3.ino
node tools/materialize-target.cjs esp32c3-supermini-4m prepared/synap_esp32s3/synap_esp32s3.ino prepared/synap_esp32c3/synap_esp32c3.ino
```

Capture uses real I2S by default. `-DUSE_REAL_I2S_MIC=0` selects a diagnostic test tone. Local USB builds identify as build 0; CI supplies the release build number. The capture task blocks while idle and wakes on START. The transmitter blocks until a frame is queued.

## Initial flash

The first installation is performed over USB with the target-specific board configuration. After an OTA-capable Synap firmware is installed, routine updates are delivered from the PWA over BLE.

## Release rules

- Use the production feed to determine the latest deployed build.
- Never flash an S3 binary onto a C3 or vice versa.
- Changes to BLE protocols, OTA format, pins, gestures, battery hardware, partitions or the PWA transport contract must update the corresponding regression tests.

## Validation

Before production release, validate BLE connect/reconnect, real microphone capture, double-tap start/stop, battery guards, long recording stability, OTA update/resume/reboot, and reconnect after OTA. Additionally validate S3 triple-tap sleep/wake behavior and C3 long-press sleep/wake behavior independently so a change to one target cannot silently alter the other.
