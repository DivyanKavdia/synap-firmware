# Synap firmware codebase guide

**Reviewed: 7 October 2026**

## Production source graph

The firmware repository intentionally has one production implementation with target adapters.

```text
devices/catalog.json
      │
      ├─ tools/device-profile.cjs
      ├─ tools/targets.cjs
      └─ tools/materialize-target.cjs
                │
firmware/shared/sources.json
      │
      ├─ firmware/shared/*.cpp
      ├─ firmware/esp32c3/status-led.cpp
      └─ firmware/xiao-sense/*.cpp
                │
                v
generated/prepared target sketch
                │
                v
Arduino compile → OTA/factory artifacts → ota-releases
```

`synap_esp32s3/synap_esp32s3.ino` is the checked-in generated primary source used as an auditable/materialization baseline. It is not a second independent firmware implementation.

## Target ownership

- **Synap Odyssey S3** — target `esp32s3-fh4r2-qspi-4m`, module 1.
- **Standard Synap Odyssey C3 and Odyssey C3 + SD** — one target `esp32c3-supermini-4m`, module 2, one binary; GPIO8 NeoPixel. The SD-equipped functional variant activates local WAV recording/sync only when the optional card mounts and is ready.
- **Chakshu** — target `xiao-esp32s3-sense-8m`, module 3; camera, SD, media transfer, Wi-Fi download and local voice live under `firmware/xiao-sense/`; shared touch/battery/standby/status behavior is enabled on GPIO1/GPIO2/GPIO5.

There are **four functional variants on three compile/OTA targets**: Odyssey S3, standard C3 (no SD), C3 + SD, and Chakshu. The presence/readiness of optional C3 SD is a runtime distinction, not a fourth device identity or OTA binary. See [Firmware variants](FIRMWARE_VARIANTS.md).

Display names are not compatibility IDs. Do not rename target IDs, product markers, manifest paths or BLE advertising identities as part of branding work.

## Shared source

`firmware/shared/sources.json` is the ordered common runtime list. The shared runtime owns:
- boot/power;
- microphone and audio capture/session/transport;
- BLE control;
- battery/status;
- capability publication;
- OTA.

Target adapters should be narrow. If a behavior belongs to both Odyssey targets, prefer the shared runtime over duplicated board code.

## Chakshu source

Chakshu-specific files own:
- BLE server/link/health adapters;
- camera and media buffers;
- SD storage/recording;
- media transfer/catalogue;
- ownership handoff;
- Wi-Fi downloads;
- TinyML voice contract/runtime/model.

GPIO21 is reserved by the Sense SD path and is not a semantic status LED. The board's active-low orange USER_LED is electrically shared with GPIO21, so real microSD chip-select traffic can visibly flash it even though firmware never uses it for status. The hardware status NeoPixel is GPIO5 / D4 and is dark by default in idle states; TTP223 is GPIO1 / D0; and the S3-style 1 MΩ / 470 kΩ battery divider is read on GPIO2 / D1.

## Build and test

Fast contract checks:

```sh
node tools/assemble-source.cjs --check
node --test tests/*.cjs
```

Production CI additionally installs the pinned ESP32 toolchain/libraries, applies required compatibility patches, materializes all three targets, compiles them with one build number and verifies published OTA artifacts/provenance. For C3 specifically, `tools/patch-arduino-sd.cjs` is applied only after Odyssey S3 and Chakshu compile, so the CMD24 busy-completion fix affects the C3 binary alone.

## Release truth

The authoritative installable version is the `ota-releases` feed. As checked on 7 October 2026, the Odyssey C3 manifest reports **build 1838**, source commit `978b44a8cc8b4c4b270fd15c396c1ab740d92008`. Read each target manifest before quoting current S3/Chakshu versions.

A later `main` commit is development source until a successful publish updates that feed.

## Voice model

`firmware/xiao-sense/tiny-voice-model.h` is the currently embedded experimental eight-class model. Training tooling and limitations are documented in [VOICE_TRAINING.md](VOICE_TRAINING.md).

Do not restore retired ESP-SR/WakeNet/MultiNet parallel model paths without deliberately changing the production architecture and source materialization/tests together.

## Cleanup policy

A firmware runtime file is removable only when it is absent from:
- `firmware/shared/sources.json`;
- target adapter materialization;
- generated-source checks;
- production tests/tooling.

The retired C3 discrete-LED implementation has been removed: `firmware/esp32c3/status-led.cpp` and `tools/boards/esp32c3/led.cjs`. C3 uses GPIO8 NeoPixel through the shared status engine. `tests/source-graph.cjs` now rejects unlisted shared source, unused target templates and unreachable board adapters.

The non-runtime tools are also intentional:
- `tools/train-tiny-voice.py` and `tools/test-train-tiny-voice.py` reproduce and validate experimental Chakshu voice candidates;
- release/feed/patch tools are invoked by the production firmware workflow;
- target-source/materialization helpers are required to derive C3 and Chakshu sources from the reviewed shared baseline.

The companion PWA has its own reachability/cleanup policy in [its development guide](https://github.com/DivyanKavdia/synap-pwa/blob/main/docs/DEVELOPMENT.md). Device/PWA responsibilities must stay separated: firmware owns disconnected capture; the PWA owns connected capture, verified SD import/deletion and cloud-derived memory surfaces.

Git history is the rollback store; do not keep retired production implementations beside their replacements.

### Odyssey C3 optional SD lifecycle (current)

The standard C3 and C3 + SD run the same compiled firmware. The wired pins are CS GPIO0, SCK GPIO10, MOSI GPIO21 and MISO GPIO20; USB CDC on boot keeps UART0 off GPIO20/21.

The active C3 storage graph is deliberately the restored 1631-family implementation selected by `firmware/shared/sources.json`:

- `odyssey-sd-1631-detect.cpp`
- `odyssey-sd-1631-recording.cpp`
- `odyssey-sd-1631-transfer.cpp`

It uses the Arduino-ESP32 3.3.5 SD/SPI mount path with a retained **1 MHz** runtime data clock, one-open-file policy, guarded VFS validation and bounded protocol re-arm. Do not replace these active modules with similarly named retired SD files without a deliberate architecture change.

Mounted SD readiness gates offline local recording. Disconnected double tap toggles a 16 kHz PCM16 WAV and purple NeoPixel pulse. Capture uses aligned **4 KiB** writes to keep FatFs on its multi-sector path. STOP remains append-only: no preallocation, seek, truncate or in-place WAV-header rewrite. The transfer layer synthesizes the valid WAV header from file length.

The production workflow applies `tools/patch-arduino-sd.cjs` **only before the C3 compile**. That patch fixes Arduino 3.3.5 single-sector CMD24 completion by keeping CS asserted while the card is program-busy and waiting up to 5 seconds before deselect. This is required for reliable FAT/directory metadata commit on `fclose()`.

The media-v1 SD API supplies an explicit path on every operation-4 chunk read and `@catalogue` for catalogue bytes. Normal reads are non-remounting; operation 14 is explicit recovery. The PWA verifies durable import, persists a sync receipt, and treats SD deletion as a separate explicit user choice. A retained source must show as already synced rather than being imported twice.

The Odyssey module descriptor keeps historical recorder evidence separate from live mount state: `sdProbeState`, `sdLiveProbeState` and `lastRecordKiB` must be interpreted together. See [C3 SD audio](ODYSSEY_C3_SD_AUDIO.md), [C3 recording I/O](odyssey-c3-sd-recording.md) and [Firmware variants](FIRMWARE_VARIANTS.md).
