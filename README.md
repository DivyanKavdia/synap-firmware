# Synap Firmware

Third module: **Chakshu / XIAO ESP32S3 Sense** adds onboard PDM audio, camera and SD hardware checks. See [Chakshu setup](docs/CHAKSHU.md). Shared code now lives in `firmware/shared/`; regenerate the portable Arduino sketch with `node tools/assemble-source.cjs` before building.

Firmware for ESP32-S3 SuperMini and ESP32-C3 SuperMini pendants. Firmware versions use **synap-os1-build#**, with `#` replaced by the numeric build counter; the [production target index](https://raw.githubusercontent.com/DivyanKavdia/synap-firmware/ota-releases/targets.json) identifies the latest numeric build.

## Shared and board-specific code

The shared runtime and S3 defaults live in `synap_esp32s3/synap_esp32s3.ino`. S3 compiles that file directly. The C3 build applies explicit feature overrides; there is no separately maintained copy of the shared runtime.

| Responsibility | Source |
| --- | --- |
| Audio, BLE, OTA, recovery, gestures, task coordination and common power logic | `synap_esp32s3/synap_esp32s3.ino` |
| Target identity and release metadata | `tools/targets.cjs` |
| Target selection and source-generation CLI | `tools/materialize-target.cjs` |
| C3 pins, image validation and single-core task creation | `tools/boards/esp32c3/index.cjs` |
| C3 battery and LED integration | `tools/boards/esp32c3/{battery,led}.cjs` |
| C3 LED implementation | `firmware/esp32c3/status-led.cpp` |

The C3 LED C++ file is a function template inserted into the generated sketch, not a standalone compilation unit. Touch and wake gestures use the shared runtime on both boards. Do not edit generated sketches. See [architecture and feature boundaries](docs/ARCHITECTURE.md) for the extension rules and runtime contracts.

## Board differences

| Feature | S3 SuperMini | C3 SuperMini |
| --- | --- | --- |
| Flash / PSRAM | 4 MB / 2 MB | 4 MB / none |
| Touch input | GPIO13 | GPIO3 |
| Recording gesture | Double tap; acts on second tap | Double tap; acts on second tap |
| Sleep / wake gesture | Hold 4 seconds, then release | Hold 4 seconds, then release |
| LED | Onboard RGB, GPIO48 | Onboard blue, GPIO8 |
| Battery sense | GPIO8 | GPIO1 |
| Battery protection | Confirmed critical battery blocks OTA and requests idle sleep | Telemetry only; automatic cutoff inactive |
| CPU idle / active | 80 / 240 MHz | 80 / 160 MHz |

Both use microphone GPIO4/5/6, 16 kHz mono audio, the same BLE protocols and optional negotiated disconnect recovery.

One tap does nothing. Double tap starts recording, or stops an active recording
and enters BLE standby. Sleep waits for active recording to stop. Wake requires
four seconds of hold validation after boot, followed by release; allow a little
extra time for boot. Wake alone does not start recording. Update older S3 firmware
to replace its previous triple-tap sleep/wake gesture.

- [Hardware, wiring, LED patterns and battery calibration](docs/HARDWARE_PINOUT.md)
- [Runtime contracts and validation](docs/ARCHITECTURE.md)
- [Disconnect recovery protocol](docs/DISCONNECT_RECOVERY.md)
- [Build, OTA publication and release trust](OTA_RELEASES.md)

## Build and validate

Run from the repository root:

```sh
node --test tests/*.cjs
mkdir -p prepared/synap_esp32s3
cp synap_esp32s3/synap_esp32s3.ino prepared/synap_esp32s3/synap_esp32s3.ino
node tools/materialize-target.cjs esp32c3-supermini-4m prepared/synap_esp32s3/synap_esp32s3.ino prepared/synap_esp32c3/synap_esp32c3.ino
```

CI compiles both targets with the pinned toolchain and retains exact generated sources. Eligible main builds publish automatically. Initial installation uses USB; subsequent updates use PWA BLE OTA. Never interchange C3 and S3 binaries.

Real I2S capture is the default. Capture converts the signed 32-bit I2S slot to PCM16 without software filtering, gain, gating or denoising. Uncompressed PCM16 is preferred at MTU 185 or above; smaller supported links use the existing IMA ADPCM fallback. The app identifies the actual format. This is not raw 24-bit microphone streaming. `-DUSE_REAL_I2S_MIC=0` enables a diagnostic tone. Unpublished USB builds identify as build 0.

Native tests and successful board builds do not establish physical battery accuracy, audio quality or battery life. Hardware acceptance must cover recording, disconnect recovery, gestures, sleep/wake and OTA on both boards.
