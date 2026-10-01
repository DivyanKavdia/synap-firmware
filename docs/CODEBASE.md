# Synap firmware codebase guide

**Reviewed: 1 October 2026**

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

Production CI additionally installs the pinned ESP32 toolchain/libraries, applies the required Arduino BLE patch, materializes all three targets, compiles them with one build number and verifies published OTA artifacts/provenance.

## Release truth

The authoritative installable version is the `ota-releases` feed. As checked on 1 October 2026, it reports **build 1546** for all three compiled targets, source commit `21bb5488ecdf7b128e560d59b30b583bcd634feb`.

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

The standard C3 and C3 + SD run the same compiled firmware. The C3 initializes native ESP-IDF SDSPI/FAT at probing speed **400 kHz** before BLE, then validates promotion to **4 MHz** after a successful FAT/VFS mount. The wired pins are CS GPIO0, SCK GPIO10, MOSI GPIO21 and MISO GPIO20; USB CDC on boot keeps UART0 off GPIO20/21.

Mounted SD readiness gates offline local recording. Disconnected double tap toggles a WAV recording and purple NeoPixel pulse; a failed/absent card does not disable ordinary BLE audio. BLE reconnect does not silently change an active SD take's destination, while a subsequent PWA START first finalizes any active SD take.

The media-v1 SD API supplies an explicit path on every operation-4 chunk read and `@catalogue` for catalogue bytes. Normal reads are non-remounting; operation 14 is explicit recovery. The PWA owns verified source import followed by deletion, never deletion before verification. See [C3 SD audio](ODYSSEY_C3_SD_AUDIO.md) and [Firmware variants](FIRMWARE_VARIANTS.md).

The Odyssey module-descriptor startup probe extension (version at byte 17, result at byte 18) is diagnostic information, not a fourth target ID and not a substitute for live SD capability/readiness checks. S3's optional SD check remains detection-only.
