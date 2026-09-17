# Synap firmware

Device firmware for **synap S3**, **synap C3**, and **Chakshu**. All three use the same audio, control, recovery and OTA contracts. Board profiles select electrical configuration and optional services.

## Current capabilities

- **16 kHz mono audio capture** with a shared streaming contract across S3, C3 and Chakshu.
- **Adaptive BLE audio transport**: PCM is preferred on sufficiently large MTUs and ADPCM is used on smaller supported links.
- **Congestion-safe PCM delivery**: the transmitter retains progress through the current frame when the Bluetooth controller temporarily rejects a notification instead of repeatedly restarting at fragment zero.
- **Disconnect recovery**: a bounded volatile recovery ring can replay recent audio after a short interruption. Recovery retains the frame being transmitted even if newer capture evicts its original ring slot.
- **Generation-safe replay**: reconnects, stream changes and explicit replay requests invalidate stale fragment cursors so bytes from different sessions or frames are not mixed.
- **Asynchronous controls**: S3/C3 BLE commands are queued independently of capture and OTA work so telemetry cannot overwrite incoming commands.
- **Battery, standby and deep-sleep support** according to the selected board profile.
- **Target-aware BLE OTA** with image structure, size and integrity checks and inactive-slot installation.
- **Chakshu extensions** for XIAO ESP32S3 Sense hardware while preserving the common audio/control contract.

A locally accepted BLE notification only proves that the controller queued it; it does not prove that the phone persisted the bytes. Native regressions cover partial sends, replay, ring overflow and byte continuity, but real Bluefy throughput and recording quality still require physical pendant testing.

## Start here

- [Device capabilities](docs/DEVICE_CAPABILITIES.md): supported features, readiness and the PWA contract.
- [Architecture](docs/ARCHITECTURE.md): source ownership and how targets are assembled.
- [Pinout](docs/HARDWARE_PINOUT.md): wiring and battery calibration.
- [Chakshu](docs/CHAKSHU.md): audio, camera, SD and first installation.
- [OTA releases](OTA_RELEASES.md): build identities, artifacts and publication.
- [Disconnect recovery](docs/DISCONNECT_RECOVERY.md): buffering and session ownership.
- [S3/C3 touch and GATT audit](docs/s3-c3-touch-gatt-audit.md): command handling and BLE-library requirements.

## Audio transport and recovery

Audio capture is **16 kHz mono PCM16** without software filtering or gain. BLE prefers PCM at MTU185 or above and uses ADPCM on smaller supported links.

During congestion, the transmitter keeps a cursor for the current generation, connection, replay generation, sequence and payload layout. Successfully accepted fragments advance that cursor; rejected fragments are retried without resending already accepted fragments. A stream change, reconnect or explicit recovery replay invalidates the cursor.

The recovery sender also holds the PCM bytes for the frame currently being drained. This matters when capture continues while Bluetooth is slow: even if the recovery ring evicts the slot that originally contained the frame, the transmitter can finish that exact frame rather than restarting at chunk zero or mixing it with newer capture.

The pinned S3/C3 Arduino BLE patch treats notification-buffer allocation exhaustion as a rejection rather than allowing a null notification buffer to continue into the BLE stack. Ordinary recording remains available when optional camera, SD or voice setup fails.

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

Before a local Arduino build, run:

```sh
node tools/patch-arduino-ble.cjs <esp32-3.3.5/libraries/BLE/src>
```

CI runs the same checksum-verified patch. It fixes short reads, reports notification-buffer allocation failures, and passes incoming S3/C3 commands directly to the command queue before status or battery telemetry can overwrite them. Callbacks enqueue work without waiting for capture or OTA.

CI pins Arduino ESP32 **3.3.5**, Adafruit NeoPixel **1.15.2**, and NimBLE-Arduino **2.3.6**. It compiles every target without local speech recognition and retains prepared source plus application and factory binaries. Eligible `main` builds publish OTA releases as `synap-os1-build#`; unpublished local builds identify as build0. First installation uses USB; OTA subsequently writes the inactive application slot. Never interchange target binaries.

## Validation boundary

Host tests and successful compilation validate software invariants but do not establish microphone quality, radio endurance, sustained phone-side BLE delivery or battery accuracy on physical boards. In particular, congestion regressions verify that firmware preserves fragment progress and PCM bytes; a real recording after OTA remains the acceptance test for Bluefy throughput.
