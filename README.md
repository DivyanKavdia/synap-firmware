# Synap ESP32-S3 — device-ID firmware updates

Release 1.0.0. Use with [synap-pwa](https://divyankavdia.github.io/synap-pwa/).
Audio remains protocol 2. OTA is now **protocol 3**, with public device-ID targeting.
No OTAKEY, signing secret, owner enrollment or physical unlock is needed by this firmware.

## Cleanup test build

Build 1006 removes duplicate sketch introduction text and routine serial logs, including periodic
audio statistics. One short boot line identifies the build/device; error diagnostics remain.
Audio, device identity, OTA transfer/resume, LED behavior and partition layout are unchanged.
Connect a pendant on build 1005, install the update, and confirm build 1006 after reconnecting.

## Customer flow

Connect the pendant in the PWA. Its permanent ID is read and retained in the browser's device association.
When an update is available, approve it. The PWA sends the update to that ID, transfers over BLE,
then reconnects and verifies both the device ID and installed build. Keep stable power and the app foregrounded.

## Security tradeoff

This is intentionally **unauthenticated OTA**. The device ID, firmware markers and SHA-256 are public
metadata/integrity checks, not credentials or signatures. Any nearby BLE client able to connect can read
the ID and initiate a compatible-image update, potentially with malicious firmware carrying matching
markers and a matching hash. PWA confirmation is not a firmware-enforced owner check.
This is not a secure mass-production update trust model.
No Secure Boot, BLE bonding/encryption, flash encryption, anti-rollback eFuses or signing is provisioned.
GitHub build/URL/hash checks protect the normal app workflow, not against another BLE client.
Legacy NVS owner-key data is neither read nor erased.

## Board and build

- ESP32-S3FH4R2 / SuperMini: 4 MB flash, 2 MB QSPI PSRAM; Arduino-ESP32 3.3.5; Adafruit NeoPixel 1.15.2.
- GPIO48 RGB: red disconnected, blue idle, green recording, purple error. Confirm your actual board schematic.
- Default audio is a generated 440 Hz tone. For INMP441, enable USE_REAL_I2S_MIC=1:
  BCLK4, WS5, DIN6, L/R grounded, 3.3 V supply. Preserve the correct microphone configuration when building.
- CI uses ESP32S3 Dev Module, FlashSize 4M, PSRAM enabled, USB hardware CDC, CDC on boot,
  and the default two-slot OTA partition layout (0x140000 bytes per application).
- The standalone sketch embeds all project headers. Put only synap_esp32s3.ino in a matching sketch folder.
- Public version remains 1.0.0; releases use 1000 + github.run_number (maximum 65535).
  Local source defaults to build 1006. Use an appropriately increasing build for manual releases.
  Do not reset the workflow counter without choosing a higher build base.

## Initial factory install and prototype migration

New units must ship with this firmware, its compatible bootloader and two-slot partition layout.
That provisioning is a factory/developer operation, not a customer setup task.

An existing protocol-1/2 pendant cannot become keyless from a PWA-only change. The current PWA
detects these older updaters and explains the one-time USB migration without requesting an owner key.
Flash this standalone sketch over USB with the configuration above. No key retrieval is needed.
Do not change partitions remotely or choose Huge APP / No OTA. Back up device data before any
intentional full-flash erase or partition-layout change.

After flashing, reconnect in the PWA and check the device ID, OTA protocol/build and available slot.
If the OS retains old GATT handles, disconnect/forget/reselect the Bluetooth device; do not clear browser
recordings. Test audio start/stop twice before trying another update.

## Release pipeline and fallback

Every main push runs the host tests, compiles the actual sketch and validates the application.
Only a successful build publishes a GitHub Release and atomically updates ota-releases/latest.json.
PRs validate without publishing. See [OTA_RELEASES.md](OTA_RELEASES.md).

Normal updates require no file selection. The developer fallback accepts a trusted application .bin,
not a bootloader, partition table or merged image. Choose the matching board/audio configuration,
connect the identified pendant, stop/save recording, choose the file, approve and start.
The manual path does not enforce increasing build numbers; the GitHub path does.

## Device identity

Read-only characteristic: 4fa1234c-0000-1000-8000-00805f9b34fb.
Exactly 18 UTF-8 bytes: SYNAP- followed by 12 uppercase hex digits from the factory eFuse MAC.
It remains stable across reboots, app updates and NVS erasure on the same board.
The advertising name remains dk-pendant. The ID is initialized before BLE starts using
esp_efuse_mac_get_default from esp_mac.h.

The separate ...34b characteristic reports firmware hardware/version/build identity.
Do not confuse this shared build identity with the per-board device ID.

## OTA protocol 3

All characteristics use the existing service 4fa12345-0000-1000-8000-00805f9b34fb.
Writes use ...348; read/notify status uses ...349. There is no challenge characteristic.
Integers are little-endian. Every packet starts with command u8 and nonzero transfer ID u32.

| Command | Remaining bytes |
| --- | --- |
| 1 Begin | image length u32, SHA-256 (32 bytes), target device ID (18 bytes, no NUL): 59 total bytes |
| 2 Data | offset u32, payload up to maxData |
| 3 Verify | none; valid only after the full declared image |
| 4 Commit | none; valid only after Verify |
| 5 Abort | none; unavailable after Commit |
| 6 Resume | image length u32, SHA-256 (32 bytes), target device ID (18 bytes); rebinds an interrupted session |

Status: magic D7, protocol 03, state u8, error u8, transfer ID u32, next offset u32,
slot capacity u32, maxData u16, build u16 (20 total bytes).
States: 0 unavailable, 1 available, 2 reserved, 3 receiving, 4 verified, 5 committed, 6 failed.
Error 12 means device-ID mismatch. Existing packet/size/flash/hash/connection/timeout errors retain their codes.
Writes are capped at 512 bytes; maxData is up to 503 bytes and must be at least 64.

The device compares the target ID before opening the inactive slot.
It checks image headers/chip, flashed SHA-256, the SYNAP-ESP32S3-OTA-ID-V3 marker and
the SYNAP-FW:esp32s3-fh4r2-qspi-4m: hardware prefix. These checks are not signatures.

## Failure behavior and validation

The control task owns flash writes; BLE callbacks only queue bounded packets.
The session and connection generation bind subsequent packets to the current transfer. A matching
Resume packet can bind a new BLE connection to the same in-memory flash handle, hash, image and offset.
Exact next offsets are required; only an identical repeat of the previous data packet is idempotent.
The PWA sends short write-without-response windows and waits for a written-byte ACK after each window;
it falls back to status reads if notifications are lost.

Cancellation, invalid images, hash failure or a 45-second connected stall before Commit leave the
previous boot selection intact. A BLE disconnect keeps the session resumable in RAM for two minutes.
Power loss, reboot or expiration of that window requires a new transfer. Partial inactive-slot bytes
are never selected.
After Commit, cancellation is impossible. A lost acknowledgement requires reconnect verification,
not an automatic second flash.

Keeping the previous slot is not automatic boot rollback. Stock Arduino bootloaders generally lack
rollback; a valid but broken new app can require USB recovery. Independently configured rollback
builds mark the app valid after basic startup only. No bootloader, partition or eFuse update is performed.

Run node --test tests/*.cjs. Tests include release validation and a native C++ test compiled from
the actual embedded OtaSession engine using a fake flash backend. CI also compiles the real ESP32 sketch.
Physical testing still needs: repeated updates across both slots, wrong/corrupt/oversized images,
cancellation, BLE loss, GATT caching, power loss before/during commit, boot health, LED/audio operation,
and the actual hardware's recovery behavior. Do not claim hardware qualification from host tests.

References:
- [Espressif ESP32-S3 OTA](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/ota.html)
- [Arduino-ESP32 BLE 3.3.5](https://github.com/espressif/arduino-esp32/tree/3.3.5/libraries/BLE)
