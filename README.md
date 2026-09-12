# Synap Firmware

Firmware for ESP32-S3 SuperMini and ESP32-C3 SuperMini pendants. Product version is **1.0.0**; the [production target index](https://raw.githubusercontent.com/DivyanKavdia/synap-firmware/ota-releases/targets.json) identifies the latest numeric build.

## Shared and board-specific code

The shared runtime and S3 defaults live in `synap_esp32s3/synap_esp32s3.ino`. S3 compiles that file directly. The C3 build applies explicit feature overrides; there is no separately maintained copy of the shared runtime.

| Responsibility | Source |
| --- | --- |
| Audio, BLE, OTA, recovery, task coordination and common power logic | `synap_esp32s3/synap_esp32s3.ino` |
| Target identity and release metadata | `tools/targets.cjs` |
| Target selection and source-generation CLI | `tools/materialize-target.cjs` |
| C3 pins, image validation and single-core task creation | `tools/boards/esp32c3/index.cjs` |
| C3 battery, LED and touch integration | `tools/boards/esp32c3/{battery,led,touch}.cjs` |
| C3 LED and gesture implementations | `firmware/esp32c3/{status-led,touch,wake}.cpp` |

The C3 C++ files are function templates inserted into the generated sketch, not standalone compilation units. Do not edit generated sketches. See [architecture and feature boundaries](docs/ARCHITECTURE.md) for the extension rules and runtime contracts.

## Board differences

| Feature | S3 SuperMini | C3 SuperMini |
| --- | --- | --- |
| Flash / PSRAM | 4 MB / 2 MB | 4 MB / none |
| Touch input | GPIO13 | GPIO3 |
| Recording gesture | Double tap; waits for possible third tap | Double tap; acts on second tap |
| Sleep / wake gesture | Triple tap | Hold 4 seconds, then release |
| LED | Onboard RGB, GPIO48 | Onboard blue, GPIO8 |
| Battery sense | GPIO8 | GPIO1 |
| Battery protection | Confirmed critical battery blocks OTA and requests idle sleep | Telemetry only; automatic cutoff inactive |
| CPU idle / active | 80 / 240 MHz | 80 / 160 MHz |

Both use microphone GPIO4/5/6, 16 kHz mono audio, the same BLE protocols and optional negotiated disconnect recovery.

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

Real I2S capture is the default. `-DUSE_REAL_I2S_MIC=0` enables a diagnostic tone; `-DSYNAP_MIC_HPF_ENABLE=0` bypasses the high-pass filter for comparison. Unpublished USB builds identify as build 0.

Native tests and successful board builds do not establish physical battery accuracy, audio quality or battery life. Hardware acceptance must cover recording, disconnect recovery, gestures, sleep/wake and OTA on both boards.
