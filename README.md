# Synap ESP32-S3 BLE OTA — firmware 5.1.0 / build 501

This firmware repository is intentionally separate from the PWA-only GitHub repository.
It contains the complete existing audio sketch plus a BLE updater. Use with
PWA 5.6.1 or newer at https://divyankavdia.github.io/ai-pendant-app/.

## Read before flashing

- Target: ESP32-S3, Arduino-ESP32 core **3.3.5**, Adafruit NeoPixel library.
- The inherited audio default is a **440 Hz test tone**, not microphone capture.
  Set `USE_REAL_I2S_MIC` to `1` for INMP441 after checking wiring: BCLK4, WS5,
  DIN6, L/R grounded, 3.3 V power. Keep your existing real-microphone configuration
  when integrating this into another firmware revision.
- RGB LED default GPIO48. Physical OTA unlock default **BOOT/GPIO0**. Confirm
  both against the actual board schematic; change `OTA_BUTTON_PIN` if required.
  Do not use a pin occupied by the microphone or other hardware.
- Preserve the correct board, flash size, PSRAM and USB settings for YOUR board.
  The previous build log selected an ESP32-S3 DevKit LiPo with 4 MB flash; that
  menu selection alone does not prove the physical pendant is that model.
- Source/protocol tests were run here. An ESP32-S3 toolchain and a physical board
  were not available: **this is not a board-compiled or hardware-qualified release**.
  No precompiled .bin is supplied. Keep USB recovery available during first tests.

## First installation — USB is required once

1. Clone or download this repository. Open the sketch under `firmware/`. Keep `dk_pendant_esp32s3.ino`, `SynapOTA.h`, and
   `OtaSession.h` together in the `dk_pendant_esp32s3` sketch folder.
2. Open that sketch in Arduino IDE / ArduinoDroid. Select the actual ESP32-S3
   board and Arduino-ESP32 3.3.5; install Adafruit NeoPixel if needed.
3. Select an OTA-capable partition scheme containing two application partitions
   (`ota_0`, `ota_1`) and `otadata`. Do not choose Huge APP / No OTA.
   First try the existing Default scheme. The compiled application must fit
   each OTA slot. If it does not, select a larger OTA layout compatible with
   your real flash size and install that layout **by USB**, backing up device
   data first. This package does not silently replace the partition table.
4. Compile and USB-upload. Check Serial at 115200 for build 501 and BLE startup.
   Verify normal recording/stop works before testing firmware transfer.
5. In PWA Settings → Pendant firmware → Check pendant, confirm build 501 and
   nonzero available OTA space. If characteristics are missing after a USB
   upgrade, disconnect, close other BLE clients and reconnect. If the OS retains
   the old GATT table, forget/reselect the pendant. Do not clear browser audio data.

## Future updates — Bluetooth only

1. Build the next firmware with the same correct hardware configuration and
   keep the OTA code/marker. Increment `SYNAP_FIRMWARE_BUILD` in `SynapOTA.h`
   for each release (for example 502); update the human-readable version too.
2. Export the **application .bin** (usually `dk_pendant_esp32s3.ino.bin`). Do not
   choose `.merged.bin`, `.bootloader.bin` or `.partitions.bin`. No bootloader,
   partition-table, flash-layout or unrelated-board update is supported over BLE.
3. Keep the pendant on stable power, close to the phone. Connect via the PWA,
   stop/save recording, then select the .bin in Settings and confirm the checklist.
4. With the pendant already running and connected, hold BOOT for at least
   two seconds and **release**. Do not hold BOOT during reset/reboot: it can enter
   the USB download boot mode. Unlock lasts 90 seconds and is tied to this connection.
5. Click Update firmware. Recording and processing are blocked while transferring.
   Keep the page visible/unlocked. A screen wake lock is requested where supported.
   Expect minutes rather than seconds; radio/browser performance determines speed.
6. After verification, the pendant commits the boot slot and reboots. Reconnect
   (automatically where supported), click Check pendant, and confirm the intended
   build number. Test audio start/stop twice. Resume the FIFO queue manually.

## Failure and recovery behavior

- SHA-256 is computed by the PWA and independently on the received firmware.
  ESP-IDF validates the complete image; both sides also check ESP32-S3 application
  headers and the `SYNAP-ESP32S3-OTA-V1` compatibility marker.
- Flash writes run in the control task, not synchronous BLE callbacks. A bounded
  queue, transfer ID, connection generation, exact next offset and duplicate-byte
  checks prevent silent chunk loss or reordering. The UI only advances progress
  after a written-byte ACK; lost notifications fall back to reading status.
- Before commit, cancellation, invalid images, hash failures, disconnects and a
  45-second stalled transfer leave the previously selected application bootable.
  Partial inactive-slot data is not activated. Reconnect/unlock/restart from zero.
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

Physical approval is required for each transfer and is scoped to the connected
client. This is not a replacement for publisher authentication. SHA-256 and the
public product marker do not make an untrusted firmware image safe. This build
does not enable BLE bonding/encryption or require a signing key. Use trusted
local binaries only. Signed releases and a provisioned trust root are needed
before offering unattended or public firmware delivery. Never publish signing
private keys. Existing secure-boot image checks, if provisioned separately, remain
the ESP-IDF validator's responsibility.

## Validation included / still required

Host protocol tests (no external dependencies):

```sh
g++ -std=c++17 -Wall -Wextra -Werror tests/ota_session.cpp -o /tmp/synap-ota-test
/tmp/synap-ota-test
```

Covered: physical unlock ownership, recording lock, size/chip checks, exact chunk
order, valid/invalid duplicates, partial uploads, hash/image failure paths,
timeouts (including clock wrap), disconnect, abort and isolated final commit.
These use a fake flash backend; they do not prove actual flash writes or boot.

Before normal use on a test pendant:

- Compile for the actual board/core and both your audio mode and partition scheme.
- USB install; record/stop/record/stop; verify microphone audio if enabled.
- Transfer a known-good incremented build, reconnect and verify its build/audio.
- Cancel midway; repeat with BLE disabled and with browser reload midway;
  confirm the old firmware restarts and permits a fresh upload.
- Reject a wrong-chip, merged, oversized and corrupted image. A malicious client
  must not bypass the firmware's own size/header/hash validation.
- Interrupt power during transfer, before commit, and during boot on a test unit;
  confirm recovery appropriate to the actual bootloader/partition configuration.
- Verify long idle operation and repeated OTA updates (both slots alternating),
  GATT cache behavior, and LED/state transitions. Never test on your only copy of data.

## Protocol and references

Audio protocol remains v2. OTA protocol v1 uses the same service
`4fa12345-0000-1000-8000-00805f9b34fb`, writes at `4fa12348-...` and read/notify
status at `4fa12349-...` (same UUID suffix). The full wire layout is documented
in the PWA README and implemented in `OtaSession.h`.

- [Espressif ESP32-S3 OTA API, IDF 5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/ota.html)
- [Arduino-ESP32 BLE API, 3.3.5](https://github.com/espressif/arduino-esp32/tree/3.3.5/libraries/BLE)

Files: one complete sketch, two headers, this guide and one host test. No PWA
copy, generated binary, signing key or extra runtime dependency is bundled.
