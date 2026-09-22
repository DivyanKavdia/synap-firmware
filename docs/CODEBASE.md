# Synap firmware codebase guide

**Reviewed: 22 September 2026**

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
- **Synap Odyssey C3** — target `esp32c3-supermini-4m`, module 2; GPIO8 is a NeoPixel driven through the shared RGB status engine.
- **Chakshu** — target `xiao-esp32s3-sense-8m`, module 3; camera, SD, media transfer, Wi-Fi download and local voice live under `firmware/xiao-sense/`; shared touch/battery/standby/status behavior is enabled on GPIO0/GPIO1/GPIO4.

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

GPIO21 is reserved by the Sense SD path and is not a semantic status LED. The hardware status NeoPixel is GPIO4; TTP223 is GPIO0 and the S3-style 1 MΩ / 470 kΩ battery divider is read on GPIO1. GPIO0 remains a reset-time strapping risk that must be covered by physical boot acceptance.

## Build and test

Fast contract checks:

```sh
node tools/assemble-source.cjs --check
node --test tests/*.cjs
```

Production CI additionally installs the pinned ESP32 toolchain/libraries, applies the required Arduino BLE patch, materializes all three targets, compiles them with one build number and verifies published OTA artifacts/provenance.

## Release truth

The authoritative installable version is the `ota-releases` feed. At this review it reports **build 1406** for all three targets, source commit `975759cf869fc9aff6f9929eea4f6005b5e18aca`.

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

At this review every production runtime module is reachable from the materialization graph, so no firmware runtime file was removed.

The non-runtime tools are also intentional:
- `tools/train-tiny-voice.py` and `tools/test-train-tiny-voice.py` reproduce and validate experimental Chakshu voice candidates;
- release/feed/patch tools are invoked by the production firmware workflow;
- target-source/materialization helpers are required to derive C3 and Chakshu sources from the reviewed shared baseline.

The companion PWA has its own reachability/cleanup policy in [its development guide](https://github.com/DivyanKavdia/synap-pwa/blob/main/docs/DEVELOPMENT.md). Device/PWA responsibilities must stay separated: firmware owns disconnected capture; the PWA owns connected capture, verified SD import/deletion and cloud-derived memory surfaces.

Git history is the rollback store; do not keep retired production implementations beside their replacements.
