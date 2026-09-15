# Device capabilities

The same catalog supplies firmware configuration, release validation, and the PWA's target definitions. Firmware is the canonical owner of `devices/catalog.json`. The PWA checks in an exact copy so it can identify devices and verify releases while offline; no runtime catalog download is required.

| Capability | synap S3 | synap C3 | Chakshu |
| --- | --- | --- | --- |
| Target | `esp32s3-fh4r2-qspi-4m` | `esp32c3-supermini-4m` | `xiao-esp32s3-sense-8m` |
| Module ID | 1 | 2 | 3 |
| Microphone | External I2S | External I2S | Onboard PDM |
| Flash / PSRAM | 4 / 2 MiB | 4 / 0 MiB | 8 / 8 MiB |
| Audio recording, transcription and OTA | Yes | Yes | Yes |
| Touch / automatic sleep | Yes | Yes | No |
| Battery telemetry | Yes, with cutoff policy | Yes, telemetry only | No |
| PWA photo / video capture | No | No | Camera required; SD optional |
| Offline photo, WAV and paired video | No | No | SD required |
| Local voice commands | No | No | Verified model and mic required; embedded model needs no SD |

The catalogue describes implemented support. It does not establish that a microphone, card, camera or model initialized successfully. Voice-service availability is separate from recognition readiness; its status characteristic reports disabled/missing/failed/listening states.

## Descriptor v1

Read `4fa12350-0000-1000-8000-00805f9b34fb`. The 20-byte payload fits the default ATT capacity. Multi-byte fields are little-endian.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | Magic `0xC7` |
| 1 | 1 | Descriptor version `1` |
| 2 | 1 | Module ID |
| 3 | 1 | Capability-layout version `1` |
| 4 | 2 | Supported flags |
| 6 | 2 | Ready flags, always a subset of supported |
| 8 | 2 | Camera sensor PID, or zero |
| 10 | 2 | Audio sample rate |
| 12 / 13 | 1 each | Detected flash / PSRAM MiB |
| 14 / 15 | 1 each | Media / voice protocol version, zero if absent |
| 16–19 | 4 | Reserved |

| Flag | Value |
| --- | --- |
| audio | 1 |
| camera | 2 |
| sd | 4 |
| settings | 8 |
| touch | 16 |
| battery | 32 |
| standby | 64 |
| video | 128 |
| sdAudio | 256 |
| photo | 512 |

Camera readiness makes live photo/video possible. Offline video also requires SD; SD audio requires both mic and SD. Capabilities never grant another device family's features merely because a packet has extra bits. The PWA checks known target identity, supported flags, ready flags and the required protocol before exposing an operation. Account association and connection/recording ownership remain separate checks.

Older C3/S3 firmware without this descriptor can be identified from its exact firmware target. It receives conservative legacy behavior with unknown readiness. Chakshu visual controls require the descriptor; a display name or a saved association alone does not enable capture.

## Change both repositories

After changing this catalog, run in the PWA checkout:

```sh
node tools/device-catalog.cjs --from ../synap-firmware
node tools/device-catalog.cjs --check
npm test
```

Commit the firmware catalog and the PWA copy/generated profiles together in their respective repositories. Keep new PWA readers compatible with deployed descriptor versions. Do not change existing module IDs, flag values, target markers or OTA slot sizes to represent readiness.
