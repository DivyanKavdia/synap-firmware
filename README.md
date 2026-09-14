# Synap firmware

Firmware for **synap C3**, **synap S3** and **Chakshu**. Versions use synap-os1-build# with a numeric build counter.

[Chakshu first-flash and hardware checks](docs/CHAKSHU.md) · [Wiring](docs/HARDWARE_PINOUT.md) · [Architecture](docs/ARCHITECTURE.md) · [OTA releases](OTA_RELEASES.md)

## Common code and board differences

Edit shared runtime code in **firmware/shared/**. The assembly tool reproduces the portable S3 Arduino sketch; board adapters produce C3 and Chakshu sketches without maintaining separate BLE/audio engines.

| Responsibility | Source |
| --- | --- |
| Common connection, BLE, audio, recovery, OTA, power and boot | firmware/shared/ |
| Target identity, memory, OTA slots and release paths | tools/targets.cjs |
| Shared sketch assembly | tools/assemble-source.cjs |
| Target generation | tools/materialize-target.cjs |
| C3 pins, battery, LED and single-core overrides | tools/boards/esp32c3/, firmware/esp32c3/ |
| Chakshu PDM, pin exclusions and always-awake profile | tools/boards/xiao-sense/ |
| Chakshu camera, SD filesystem and media-check worker | firmware/xiao-sense/ |

Do not edit the generated sketch directly. CI verifies byte-for-byte synchronization. Fragments are assembled in order and inherit shared types; they are not standalone translation units. Checked adapter anchors fail generation when shared code changes incompatibly.

| Feature | S3 SuperMini | C3 SuperMini | Chakshu / XIAO Sense |
| --- | --- | --- | --- |
| Flash / PSRAM | 4 MB / 2 MB | 4 MB / none | 8 MB / 8 MB OPI |
| Microphone | INMP441 I2S, GPIO4/5/6 | INMP441 I2S, GPIO4/5/6 | Onboard PDM, clock42/data41 |
| Touch | GPIO13 | GPIO3 | Disabled |
| LED | RGB GPIO48 | Blue GPIO8 | External indicators disabled |
| Battery sensing | GPIO8 | GPIO1, telemetry only | Disabled |
| Sleep/wake | Four-second hold | Four-second hold | Always awake during bring-up |
| Camera / SD | None | None | OV3660 probe / installed card |
| Local voice model | None | None | Included in Chakshu OTA; no SD required |
| SD checks | None | None | Photo, 10s WAV, silent 2fps MJPEG |
| CPU idle / active | 80 / 240 MHz | 80 / 160 MHz | 240 / 240 MHz |

C3 and SuperMini S3 use double tap to start recording or stop and enter standby. A single tap does nothing. Sleep waits for recording to stop; wake requires a four-second hold through boot validation and release. Chakshu uses the app controls with no touch hardware.

All targets reuse the same BLE service, control protocol, durable device ID and recording format. PWA detection reads a separate versioned capability descriptor; it does not infer hardware from a display name. OTA checks bind each binary to its exact target, including separate identities for SuperMini S3 and XIAO Sense.

## Build and validate

~~~sh
node tools/assemble-source.cjs
node tools/assemble-source.cjs --check
node --test tests/*.cjs
node tools/materialize-target.cjs esp32c3-supermini-4m synap_esp32s3/synap_esp32s3.ino prepared/synap_esp32c3/synap_esp32c3.ino
node tools/materialize-target.cjs xiao-esp32s3-sense-8m synap_esp32s3/synap_esp32s3.ino prepared/synap_chakshu/synap_chakshu.ino
~~~

CI compiles all three targets with Arduino ESP32 3.3.5 and retains application/factory binaries and exact prepared source. Eligible main builds publish automatically; pull requests only build artifacts. First installation uses USB. Subsequent released builds use PWA BLE OTA. Never interchange board binaries.

Capture is 16 kHz mono PCM16 with no software filtering, gain, gate or denoising. INMP441 capture converts its signed 32-bit I2S slot to PCM16; XIAO PDM already supplies PCM16. BLE prefers uncompressed PCM at MTU185 or above and retains ADPCM fallback on smaller links. This is not raw 24-bit streaming. Diagnostic tone builds remain available on C3/S3 with USE_REAL_I2S_MIC=0; Chakshu hardware checks require the real mic. Unpublished builds identify as build0.

Native tests and board builds do not establish physical audio/image quality, battery accuracy or battery life. Validate each physical target, and follow the dedicated Chakshu checklist before extending continuous SD recording or video.
