# Firmware releases

The product version is **1.0.0**. Numeric 16-bit build counters distinguish releases.

## Build and publication

`.github/workflows/firmware.yml` tests the reviewed S3 sketch and materializes the C3 target from it. Both targets compile with Arduino-ESP32 3.3.5, Adafruit NeoPixel 1.15.2, real I2S capture and the default dual-OTA partition layout.

Pull requests build and validate artifacts. Successful eligible pushes to `main`, or workflow dispatches on `main`, publish to `ota-releases` through the `production` environment. Documentation-only pushes are ignored. Dispatch the workflow to rebuild an unchanged source commit.

The workflow assigns `1000 + github.run_number`, validates target identity and image size, and retains the exact sources, application images and factory images in the `verified-firmware` workflow artifact. GitHub release assets include application binaries, manifests, source sketches and source hashes for both targets.

The publisher verifies that the source commit is still current and that the build advances the existing feed. Both target manifests and `targets.json` move together in one release-branch commit. Feed verification runs only after publication succeeds.

## Trust and delivery

Production manifests use schema 3 and declare GitHub Actions provenance. The workflow attests both binaries with GitHub OIDC. Feed checks validate the target, binary SHA-256, GitHub-verified Actions release commit, attestation availability and browser CORS. The PWA consumes the production feed.

- [Primary S3 manifest](https://raw.githubusercontent.com/DivyanKavdia/synap-firmware/ota-releases/latest.json)
- [Target index](https://raw.githubusercontent.com/DivyanKavdia/synap-firmware/ota-releases/targets.json)
- [C3 manifest](https://raw.githubusercontent.com/DivyanKavdia/synap-firmware/ota-releases/targets/esp32c3-supermini-4m/latest.json)

Release utilities also support the explicit `ota-test` channel for engineering use. The configured workflow publishes production; it does not dispatch by channel.

The firmware OTA engine validates image structure, target identity, size and SHA-256 and supports transfer resume. Device OTA integrity checks and PWA publisher-authenticity checks serve different purposes. Keep target markers and active protocol versions intact.

## Local builds

The checked-in sketch defaults to real microphone capture. USB development builds use build 0 unless `-DSYNAP_BUILD=<number>` is supplied; build 0 cannot be packaged by the release validator. Use `-DUSE_REAL_I2S_MIC=0` only for a transport test tone. Generate C3 source with the command in README rather than editing a second copy.

Before releasing runtime changes, pass native regression tests and both target builds. Physical acceptance covers recording, touch gestures, sleep/wake, reconnect, battery guards and OTA. CI compilation does not measure on-device timing or audio quality.
