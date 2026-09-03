# Synap ESP32-S3 firmware

Release **1.0.0** for ESP32-S3FH4R2 / SuperMini. Audio uses protocol 2; BLE OTA uses protocol 3 with permanent device-ID targeting.

This firmware deliberately does **not** store recordings on the pendant. Audio is captured and streamed to the Synap PWA. Any recording persistence lives in the browser.

## Hardware

- ESP32-S3FH4R2
- 4 MB flash
- 2 MB QSPI PSRAM
- Arduino-ESP32 3.3.5
- Adafruit NeoPixel 1.15.2
- default two-slot OTA partition layout, 0x140000 bytes per application slot
- RGB GPIO48: red disconnected, blue idle, green recording, purple error, amber during OTA

Default source mode generates a 440 Hz test tone. For a real I2S microphone compile with `USE_REAL_I2S_MIC=1`; current pins are BCLK 4, WS 5, DIN 6.

## BLE interface

Service: `4fa12345-0000-1000-8000-00805f9b34fb`

- audio notify: `...346`
- control/status: `...347`
- OTA write: `...348`
- OTA status: `...349`
- firmware target/build identity: `...34b`
- permanent board ID: `...34c`
- runtime diagnostics: `...34d`

Permanent ID is exactly `SYNAP-` plus 12 uppercase hex digits derived from the factory eFuse MAC. It is independent of NVS and firmware version.

## Audio transport

PCM16 mono is captured at 16 kHz in 800-sample / 50 ms frames. Each 1600-byte frame is split into 10–20 BLE notifications depending on negotiated MTU. Sequence numbers are assigned at capture time, so acquisition-queue drops remain observable as gaps on the wire.

The acquisition and transmitter tasks are separated. START is idempotent and does not reset an active stream. STOP invalidates queued/in-flight work through a stream-generation counter.

## Runtime diagnostics

Read `4fa1234d-0000-1000-8000-00805f9b34fb` for a 32-byte little-endian diagnostics value:

| Offset | Value |
| --- | --- |
| 0 | magic `0xD6` |
| 1 | diagnostics version `1` |
| 2 | flags: bit0 real mic, bit1 connected, bit2 streaming, bit3 OTA busy |
| 3 | ESP reset reason |
| 4 | captured frames u32 |
| 8 | capture queue drops u32 |
| 12 | BLE notification rejects u32 |
| 16 | control queue drops u32 |
| 20 | free heap u32 |
| 24 | minimum free heap u32 |
| 28 | uptime seconds u32 |

The reset reason makes brownout/watchdog/panic resets visible without persistent pendant logging.

## OTA protocol 3

The device exposes an inactive OTA slot only when both standard OTA application slots and OTA metadata exist. The PWA supplies a nonzero transfer ID, image length, SHA-256 and 18-byte permanent device ID.

Commands:

1. BEGIN
2. DATA with exact offset
3. VERIFY
4. COMMIT
5. ABORT
6. RESUME

The session keeps the flash handle and rolling SHA-256 state in RAM across a short BLE interruption. A matching RESUME can rebind a new BLE connection for up to two minutes. Power loss intentionally loses this session; there is no local OTA-session storage on the pendant.

The control loop drains up to four queued OTA DATA messages per iteration. The PWA can therefore use a conservative four-packet write-response window and one cumulative persisted-offset acknowledgement, reducing BLE round trips while keeping strict ordered flash writes.

The firmware verifies ESP32-S3 image structure, exact product/target markers and SHA-256 before selecting the new boot partition.

## Publisher trust

The device ID is targeting, not authentication. Firmware publisher authentication is enforced in the official PWA/release pipeline using a signed production manifest.

Production manifests are ES256 / P-256 signed with key ID `prod-2026-01`. Only the public verification key is committed at `tools/release-public-key.pem`. The private key must never be committed to Git.

The first signed production build remains installable from deployed build 1008 because the existing protocol-3 wire format is unchanged.

For a stronger hostile-client threat model, future manufacturing can additionally enable ESP-IDF Secure Boot / signed-app verification, flash encryption and BLE security. Those require a deliberate factory provisioning design and are not silently enabled by this Arduino application update.

## Rollback behavior

When the selected bootloader configuration has `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, a newly booted OTA image is no longer marked valid immediately in `setup()`.

The application waits at least five seconds and checks that BLE/server queues exist and, when a real microphone build is used, that the microphone initialized successfully. Only then does it call `esp_ota_mark_app_valid_cancel_rollback()`. This leaves an early-crashing image in `PENDING_VERIFY` long enough for the bootloader's rollback mechanism to protect the previous slot.

The stock Arduino board package/partition/bootloader used in production must itself be qualified for rollback; application code cannot add a missing rollback-capable bootloader remotely.

## Release channels

`main` pushes compile and publish to **`ota-test` only**. Normal customers do not see this feed.

Production promotion is a manual GitHub Actions dispatch with `channel=production`. The protected production environment must provide:

`SYNAP_RELEASE_PRIVATE_KEY_PEM`

The workflow then:

1. runs host regression tests;
2. compiles with the pinned board/toolchain configuration;
3. validates target/build markers and slot size;
4. creates a schema-2 production manifest;
5. signs its canonical form with ES256;
6. publishes binary + manifest atomically to `ota-releases`;
7. verifies the public binary digest, production signature policy and CORS response.

If the signing secret is missing, production publication fails closed.

Test and production manifests have different channel fields and immutable branch-specific binary URLs, so a test build cannot be consumed by the production PWA feed accidentally.

## Factory provisioning

New units should be provisioned with:

- this application family;
- 4 MB/default dual-OTA partition layout;
- a production-qualified bootloader/partition table;
- the required PSRAM configuration;
- the correct microphone build flag and pins.

Older updater generations may need a one-time developer/factory USB migration. After protocol 3 is installed, customer firmware updates are intended to be PWA-only with no OTA key entry or physical button.

## Build/test

GitHub Actions compiles with:

`esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=default,PSRAM=enabled,USBMode=hwcdc,CDCOnBoot=cdc`

Run host tests with:

```bash
node --test tests/*.cjs
```

The CI compile plus host tests validate the release format and OTA session state machine. Physical release qualification should additionally cover record/stop, long audio streaming, BLE interruption, OTA interruption/resume, reboot verification and power/brownout behavior.
