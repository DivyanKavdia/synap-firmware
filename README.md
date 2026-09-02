# Synap ESP32-S3 BLE OTA — firmware 1.0.0

This firmware repository is intentionally separate from the PWA-only GitHub repository.
It contains the complete existing audio sketch plus a BLE updater. Use with
PWA 1.0.0 at https://divyankavdia.github.io/synap-pwa/.
Updates are approved in the PWA with a per-pendant owner key. No BOOT press is
required once this firmware is installed and the owner key has been retrieved.

## Automatic GitHub updates

Push to `main`: CI compiles and validates this exact board, publishes a release,
and atomically updates the PWA feed. Connect in synap-pwa, tap Update pendant,
and approve. No manual .bin selection or physical button is needed after one-time
owner-key enrollment. See [OTA_RELEASES.md](OTA_RELEASES.md) for setup, build
numbering, authorization storage and safety details.

## Release numbering

The synchronized firmware/PWA baseline remains **1.0.0**. Official firmware
builds use `1000 + github.run_number`, starting at 1001; local source builds
default to 504. The increasing counter, not only the version, identifies an
update. Audio and authenticated OTA protocols remain v2.

## Read before flashing

- Target: **ESP32-S3FH4R2**, 4 MB flash, 2 MB QSPI PSRAM, Arduino-ESP32 **3.3.5**, Adafruit NeoPixel **1.15.2**.
- The inherited audio default is a **440 Hz test tone**, not microphone capture.
  Set `USE_REAL_I2S_MIC` to `1` for INMP441 after checking wiring: BCLK4, WS5,
  DIN6, L/R grounded, 3.3 V power. Keep your existing real-microphone configuration
  when integrating this into another firmware revision.
- RGB LED default GPIO48. The updater does not read BOOT/GPIO0 or require a
  physical unlock. Confirm the LED pin against your actual board schematic.
- CI uses ESP32S3 Dev Module, FlashSize 4M, PSRAM enabled (QSPI), USB hardware
  CDC, and the existing default two-slot OTA layout (0x140000 bytes per app).
- The real sketch is compiled in GitHub Actions, then its size, ESP32-S3 image
  header, compatibility and exact build identity are checked before publication.
  Hardware OTA/reboot/power-loss testing remains required; keep USB recovery
  available during first tests. Precompiled application binaries are in Releases.

## One-time setup — USB key retrieval

An existing 5.1.0 pendant can install this application through the old updater
using its existing BOOT unlock one final time. The PWA supports that migration.
A device without OTA support, or needing a new partition layout, requires USB
installation. An app cannot retrofit OTA into firmware already running without it.
After either installation route, retrieve the new owner key once over USB.

1. Download `synap_esp32s3/synap_esp32s3.ino`. Open it in a fresh sketch folder named
   `synap_esp32s3`, with only this one `.ino` file. All project headers are embedded;
   do not add an older sketch as a second tab.
2. Open that sketch in Arduino IDE / ArduinoDroid. Select the actual ESP32-S3
   board and Arduino-ESP32 3.3.5; install Adafruit NeoPixel if needed.
3. Select an OTA-capable partition scheme containing two application partitions
   (`ota_0`, `ota_1`) and `otadata`. Do not choose Huge APP / No OTA.
   First try the existing Default scheme. The compiled application must fit
   each OTA slot. If it does not, select a larger OTA layout compatible with
   your real flash size and install that layout **by USB**, backing up device
   data first. This package does not silently replace the partition table.
4. Compile and USB-upload. Check Serial at 115200 for the expected build and BLE startup.
   Verify normal recording/stop works before testing firmware transfer.
5. In Serial Monitor at 115200, send `OTAKEY` with a newline. Copy the 64-character
   owner key to a private password manager. It is generated on the device, is not
   a key from this repository, and is never printed in ordinary startup logs.
   The command is available through the sketch's configured USB/UART `Serial`.
6. In PWA Settings → Pendant firmware → Check pendant, confirm the expected build and
   nonzero available OTA space. If characteristics are missing after a USB
   upgrade, disconnect, close other BLE clients and reconnect. If the OS retains
   the old GATT table, forget/reselect the pendant. Do not clear browser audio data.

## Manual file fallback — Bluetooth only

Normally use the automatic GitHub flow above. These instructions are for a
manually compiled application image.

1. Build the next firmware with the same correct hardware configuration and
   keep the OTA code/marker. Set `SYNAP_BUILD` in the standalone sketch
   to a number higher than the running build; update the human-readable version too.
2. Export the **application .bin** (usually `synap_esp32s3.ino.bin`). Do not
   choose `.merged.bin`, `.bootloader.bin` or `.partitions.bin`. No bootloader,
   partition-table, flash-layout or unrelated-board update is supported over BLE.
3. Keep the pendant on stable power, close to the phone. Connect via the PWA,
   stop/save recording, then select the .bin in Settings.
4. Paste that pendant's owner key and confirm the on-screen approval. No button
   press or USB connection is needed. Manual file mode requires the key for each update. The automatic GitHub flow
   can remember authorization when you explicitly opt in. Firmware keeps the key in NVS across normal application OTA.
   Erasing NVS/all flash creates a new key; retrieve it again over USB.
5. Click Update firmware. Recording and processing are blocked while transferring.
   Keep the page visible/unlocked. A screen wake lock is requested where supported.
   Expect minutes rather than seconds; radio/browser performance determines speed.
6. After verification, the pendant commits the boot slot and reboots. Reconnect
   (automatically where supported), click Check pendant, and confirm the intended
   build number. Test audio start/stop twice. Resume the FIFO queue manually.

## Failure and recovery behavior

- SHA-256 is computed by the PWA and independently on the received firmware.
  ESP-IDF validates the complete image; both sides also check ESP32-S3 application
  headers and the `SYNAP-ESP32S3-OTA-AUTH-V2` compatibility marker. The old marker
  remains in this image for migration, but this release rejects images missing the new one.
- Flash writes run in the control task, not synchronous BLE callbacks. A bounded
  queue, transfer ID, connection generation, exact next offset and duplicate-byte
  checks prevent silent chunk loss or reordering. The UI only advances progress
  after a written-byte ACK; lost notifications fall back to reading status.
- Before commit, cancellation, invalid images, hash failures, disconnects and a
  45-second stalled transfer leave the previously selected application bootable.
  Partial inactive-slot data is not activated. Reconnect/authorize/restart from zero.
- After final commit, cancellation is no longer possible. If the ACK is lost,
  the app reports an uncertain outcome and asks you to reconnect/check the build.
  Do not interpret loss of the BLE connection alone as successful installation.
- This is application-only OTA. No partition erases outside the inactive app slot
  or eFuse/security provisioning is performed by the updater.
- Keeping the prior app slot is NOT automatic rollback. Stock Arduino bootloaders
  generally do not enable boot rollback. A valid image with a software bug may
  require USB recovery. With an independently configured rollback-enabled
  bootloader, this sketch marks an app valid after basic BLE/task/microphone startup;
  qualify that policy with your own health tests before production use.

## Security boundary

- A random 256-bit owner key is generated after BLE initialization and stored in
  NVS. It is available through the explicit Serial `OTAKEY` command, never BLE.
  Physical Serial access can disclose the key; NVS encryption is not enabled here.
- The PWA proves possession using HMAC-SHA256 over `SYNAP-OTA-V2` (UTF-8, no NUL),
  a fresh 16-byte challenge, and the first 41 bytes of BEGIN: command, transfer ID,
  image length and SHA-256. Authorization occurs before flash erase/write.
- Challenges change on each connection generation and consumed authorization
  attempt (including a wrong MAC). Attempts are spaced by at least one second;
  five failures trigger 30 seconds of cooldown. Cooldown survives BLE reconnect,
  not power loss. Wrong/malformed approval cannot start flash writes.
- The PWA can remember a non-extractable HMAC CryptoKey per pendant in IndexedDB
  with explicit consent. It never logs/uploads the key or stores plaintext, and
  clears the password input after an attempt/disconnect. Do not paste
  it into untrusted sites or share it in screenshots, issues, logs or GitHub.
- This authenticates the owner's chosen image, **not its publisher**. SHA-256 and
  public markers do not make an untrusted image safe. BLE bonding/encryption,
  publisher signatures, anti-rollback fuses and secure boot are not provisioned.
  Use trusted local binaries. A key holder can authorize arbitrary code; anyone
  with radio access may still disrupt the BLE link. Secure-boot validation, if
  independently provisioned, remains ESP-IDF's responsibility.

## Validation included / still required

The OTA protocol and authentication engine passed host regression tests during
preparation. Those tests are not bundled in this single-file distribution.
A full ESP32-S3 compile and hardware validation are still required.

Before normal use on a test pendant:

- Compile for the actual board/core and both your audio mode and partition scheme.
- USB install; record/stop/record/stop; verify microphone audio if enabled.
- Transfer a known-good incremented build, reconnect and verify its build/audio.
- Confirm no BOOT press is needed; try a wrong key, wait and retry the correct key.
  Verify key retention after OTA/reboot and challenge renewal after reconnect.
- Cancel midway; repeat with BLE disabled and with browser reload midway;
  confirm the old firmware restarts and permits a fresh upload.
- Reject a wrong-chip, merged, oversized and corrupted image. A malicious client
  must not bypass the firmware's own size/header/hash validation.
- Interrupt power during transfer, before commit, and during boot on a test unit;
  confirm recovery appropriate to the actual bootloader/partition configuration.
- Verify long idle operation and repeated OTA updates (both slots alternating),
  GATT cache behavior, and LED/state transitions. Never test on your only copy of data.

## Protocol and references

Audio protocol remains v2. Authenticated OTA protocol v2 uses the same service
`4fa12345-0000-1000-8000-00805f9b34fb`, writes at `4fa12348-...` and read/notify
status at `4fa12349-...`, plus read-only challenge at `4fa1234a-...` (same suffix).
BEGIN is 73 bytes (the former 41 bytes plus a 32-byte HMAC). Status remains 20
bytes but advertises protocol 2. State 1 means awaiting PWA authorization; state
2 is reserved for legacy migration. Minimum data capacity is 64 bytes (MTU 76),
so the authenticated BEGIN fits. Errors 12/13 mean failed/throttled authorization.
The full wire layout is documented
in the PWA README and implemented in the embedded `OtaSession` class.

- [Espressif ESP32-S3 OTA API, IDF 5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/ota.html)
- [Arduino-ESP32 BLE API, 3.3.5](https://github.com/espressif/arduino-esp32/tree/3.3.5/libraries/BLE)

Files: one complete standalone sketch and this guide. No PWA
copy, generated binary or device key is bundled. Preferences and mbedTLS are
provided by the ESP32 core; no new third-party firmware dependency is required.
