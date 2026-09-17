# Synap firmware

Device firmware for **synap S3**, **synap C3**, and **Chakshu**. All three use the same audio, control, recovery and OTA contracts. Board profiles select electrical configuration and optional services.

## Start here

- [Device capabilities](docs/DEVICE_CAPABILITIES.md): supported features, readiness and the PWA contract.
- [Architecture](docs/ARCHITECTURE.md): source ownership and how targets are assembled.
- [Pinout](docs/HARDWARE_PINOUT.md): wiring and battery calibration.
- [Chakshu](docs/CHAKSHU.md): audio, camera, SD and first installation.
- [OTA releases](OTA_RELEASES.md): build identities, artifacts and publication.
- [Disconnect recovery](docs/DISCONNECT_RECOVERY.md): buffering and session ownership.

## Edit and validate

`devices/catalog.json` owns target identity, pins, CPU clocks, battery policy, supported capabilities and release limits. Shared runtime code lives in `firmware/shared/`; optional Chakshu drivers live in `firmware/xiao-sense/`.

```sh
node tools/assemble-source.cjs
node tools/assemble-source.cjs --check
node --test tests/*.cjs
node tools/materialize-target.cjs esp32c3-supermini-4m synap_esp32s3/synap_esp32s3.ino prepared/synap_esp32c3/synap_esp32c3.ino
node tools/materialize-target.cjs xiao-esp32s3-sense-8m synap_esp32s3/synap_esp32s3.ino prepared/synap_chakshu/synap_chakshu.ino
```

The checked-in S3 `.ino` is generated. Edit its owners, then regenerate it. Keep each complete Arduino sketch in its own folder; two complete `.ino` files in one folder are compiled together and cause duplicate definitions.

Before a local Arduino build, run `node tools/patch-arduino-ble.cjs <esp32-3.3.5/libraries/BLE/src>`. CI runs the same checksum-verified patch. It fixes short reads, reports notification-buffer allocation failures, and passes incoming S3/C3 commands directly to the command queue before status or battery telemetry can overwrite them. Callbacks enqueue work without waiting for capture or OTA. See [S3/C3 touch and GATT audit](docs/s3-c3-touch-gatt-audit.md).

CI pins Arduino ESP32 **3.3.5**, Adafruit NeoPixel **1.15.2**, and NimBLE-Arduino **2.3.6**. It compiles every target without local speech recognition, and retains prepared source plus application and factory binaries. Eligible `main` builds publish OTA releases as `synap-os1-build#`; unpublished local builds identify as build0. First installation uses USB; OTA subsequently writes the inactive application slot. Never interchange target binaries.

Audio capture is 16 kHz mono PCM16 without software filtering or gain. BLE prefers PCM at MTU185 or above and uses ADPCM on smaller supported links. Ordinary recording remains available when optional camera, SD or voice setup fails. Host tests and successful compilation do not establish microphone quality, radio endurance or battery accuracy on physical boards.
