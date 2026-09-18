# Synap Firmware

**Current production baseline — 18 September 2026**

This repository owns production firmware for the Synap wearable family.

## Production release

- **Current production build:** **1262**
- **Source baseline:** `8d760280d47890b762f29283d4e12a249c4e5e9b`
- **Release channel:** `ota-releases`
- **Production targets:** ESP32-S3 SuperMini, ESP32-C3 SuperMini and Chakshu / XIAO ESP32S3 Sense

Every production release is compiled in CI, published atomically, attested with GitHub OIDC provenance and checked through the public firmware feed for digests, provenance and browser CORS.

## Supported targets

| Target | Product | Core capabilities |
| --- | --- | --- |
| `esp32s3-fh4r2-qspi-4m` | Synap S3 | audio, settings, touch, battery, standby |
| `esp32c3-supermini-4m` | Synap C3 | audio, settings, touch, battery, standby |
| `xiao-esp32s3-sense-8m` | Chakshu | audio, camera, SD, settings, video, SD audio, photo |

The canonical device definition is `devices/catalog.json`. Target identity, hardware mapping, release limits and supported capabilities should be changed there first.

## Firmware architecture

Shared runtime behavior lives under `firmware/shared/`. Target adapters materialize the common source for the individual boards. Chakshu-specific camera, SD and local-voice behavior lives in the XIAO Sense path without changing the common S3/C3 audio contract.

The checked-in S3 sketch is generated from the owned source components. Do not hand-maintain parallel copies of shared behavior.

## Audio and BLE baseline

- 16 kHz mono capture.
- Adaptive BLE audio transport.
- Congestion-safe fragment progress.
- Bounded volatile disconnect recovery.
- Session/generation-safe replay.
- Asynchronous device controls.
- Target-aware BLE OTA with inactive-slot installation.

A successful BLE notification enqueue is not proof that the browser persisted the corresponding bytes. CI verifies byte continuity and transport invariants; sustained real-device throughput remains a physical acceptance test.

## Chakshu production baseline

The current Chakshu baseline includes the approved offline media lifecycle and a proper two-stage local voice pipeline: Espressif WakeNet detects `Hi ESP`, then MultiNet listens for one supported command for up to 8 seconds. Chakshu remains on the installed Arduino `default_8MB` dual-OTA partition layout so routine releases stay OTA-compatible with deployed devices. The optimized WakeNet→MultiNet image must remain within the existing 0x330000 inactive application slot.

- Synap-owned SD FIFO cleanup when reserve space is needed.
- App-triggered clear of Synap capture files.
- Verified move-to-app deletion semantics.
- Offline audio stored entirely on SD until explicitly moved to the app.
- Offline video stored on SD.
- Default video duration of 10 seconds.
- Explicit requested video durations, within production limits.
- High-quality/native camera capture profiles for the supported camera.
- Local `Hi ESP` WakeNet detection using the embedded pinned speech model.
- Voice protocol v2; wake events remain compatible with the companion app.
- MultiNet commands are enabled only after WakeNet and time out after 8 seconds.
- Supported photo, video, audio and visual-description commands retain their local/online routing.
- Wake detection remains available while compatible SD recording feeds PCM copies.

After offline audio is moved to the companion app, cloud transcription and memory processing are owned by the PWA/backend repository.

## Build and validation

CI pins the ESP32 toolchain and board libraries and validates all three targets.

Core local checks:

```sh
node tools/assemble-source.cjs --check
node --test tests/*.cjs
```

Production CI additionally:

1. installs the pinned ESP32/Arduino dependencies,
2. applies the verified BLE compatibility patch,
3. materializes S3, C3 and Chakshu production sources,
4. embeds and verifies the pinned Chakshu local voice model,
5. compiles all three targets with one build number,
6. creates OTA and factory artifacts,
7. attests production binaries,
8. publishes the release,
9. verifies public manifests, binaries, digests, provenance and CORS.

## OTA rules

- First installation can use USB.
- Routine compatible updates use the production OTA feed.
- Never interchange target binaries.
- Target, slot size, image structure and digest checks remain mandatory.
- A source commit is not itself a release; the authoritative production feed determines the installable build.

## Physical acceptance boundary

Build/test success validates software contracts but does not establish real-world hardware quality. The current production candidate should be physically accepted for:

- sustained microphone/BLE recording,
- reconnect and recovery,
- `Hi ESP` WakeNet acknowledgement and follow-up MultiNet command recognition,
- photo quality,
- timed video capture,
- long SD recording,
- FIFO space reclamation,
- move-to-app followed by source deletion,
- end-to-end transfer into the Synap memory pipeline.

## Working convention from this baseline

`main` is the only current development baseline. New work should branch from current `main`; do not revive old audit branches or superseded implementation paths.

Detailed history belongs in Git commits, merged pull requests and published releases. Keep this README focused on the current production truth.

<!-- release-trigger: voice-production -->
