# Odyssey C3 1.75 MiB dual-OTA layout: one-time wired migration

> **NOT A BLE OTA UPGRADE.** This is an isolated experimental factory image.
> Do not replace the production `ota-releases` feed for older 1.25 MiB C3
> devices. Keep this image off the public PWA OTA upgrade path until a
> partition-aware update channel and on-device verification are ready.

## Purpose

The existing Odyssey C3 4 MiB flash uses Arduino ESP32 3.3.5 default:
`ota_0` at 0x10000 for 0x140000 bytes, `ota_1` at 0x150000 for
0x140000 bytes, SPIFFS at 0x290000 for 0x160000 bytes, and a coredump
area at 0x3F0000. The new layout preserves NVS, OTA metadata and
coredump offsets while reallocating unused internal SPIFFS to firmware:

| Partition | Offset | Size |
| --- | ---: | ---: |
| NVS | 0x9000 | 0x5000 |
| OTA metadata | 0xE000 | 0x2000 |
| OTA app 0 | 0x10000 | 0x1C0000 (1.75 MiB) |
| OTA app 1 | 0x1D0000 | 0x1C0000 (1.75 MiB) |
| SPIFFS | 0x390000 | 0x60000 (384 KiB) |
| Coredump | 0x3F0000 | 0x10000 |

The external microSD remains the audio storage. The production firmware
does not mount SPIFFS and stores durable state in NVS (Preferences).

## Obtain an image

The experimental `c3-large-dual-ota-migration` GitHub Actions artifact
is compiled with the same pinned Arduino-ESP32 core 3.3.5, the Odyssey C3
pin/identity map, the SD CMD24 fix and
`SYNAP_C3_WIFI_UPLOAD_ENABLED=1`. The branch's standalone migration
workflow **never publishes** to the existing OTA production feed.

Use these two individual files from the same successful action artifact:

- `partitions.bin`: compiled 4 MiB C3 partition table.
- `firmware.bin`: app compiled for the two larger OTA slots.

The artifact also includes `factory.bin`, a merged image intended only
for a full factory-flash operation. **Do not flash it to preserve NVS.**

Do **not** use the build 1859 `factory.bin`, which contains the old table.

## One-time migration while C3 USB is accessible

**Preflight:** Back up/sync valuable recordings from the external SD,
confirm a reliable USB programming connection to the ESP32-C3
SuperMini, and keep the board powered through the entire operation.
For USB-hidden enclosures, open the housing or use prewired access to
the original C3 USB data pins; GPIO20/21 are assigned to SD on Odyssey
C3 and should not be repurposed for UART without isolation.

Install an ESP32-C3-capable `esptool` and substitute your actual
serial port for `PORT` (for example `COM7` on Windows or
`/dev/ttyACM0` on Linux). First back up the *entire* 4 MiB flash:

```sh
python -m esptool --chip esp32c3 --port PORT read-flash 0x0 0x400000 c3-before-migration.bin
```

Store this backup privately: it contains NVS/device-specific data.

Enter ROM download mode as required by your specific C3 board and
write the new partition table and app to their fixed positions.
The bootloader remains at 0x0 because we retain the pinned core.

```sh
python -m esptool --chip esp32c3 --port PORT write-flash 0x8000 partitions.bin 0x10000 firmware.bin
python -m esptool --chip esp32c3 --port PORT erase-region 0xE000 0x2000
```

Erasing **only** the OTA-selection metadata at 0xE000 makes the
bootloader select the new app0. It does not erase NVS at 0x9000.
Do not issue `erase-flash` unless you explicitly intend to wipe all
internal state. Do not remove power or disconnect until both commands
finish. Reboot after leaving download mode.

If anything fails and USB is still available, restore the full
device-specific backup using a wired flash:

```sh
python -m esptool --chip esp32c3 --port PORT write-flash 0x0 c3-before-migration.bin
```

Restore only to the same backed-up board; the backup contains secrets
and device data. Prefer diagnosing a failure before using it.

## Mandatory functional checks before closing the enclosure

1. Serial boots as ESP32-C3. BLE service, module identity, battery and
   touch still work; no boot-loop or firmware rejection.
2. The runtime partition table reports `ota_0` size 0x1C0000,
   `ota_1` size 0x1C0000 and `ota` data partition at 0xE000.
3. Verify microphone audio and offline SD double-tap recording, then
   record-stop/catalogue/read and sync to the PWA.
4. Verify BLE-controlled Wi-Fi upload via an explicitly configured
   HTTPS destination, including authentication and post-upload SD
   retention. This implementation has not been validated on physical
   C3 merely by compiling.
5. Before relying on OTA for future updates, implement and test a
   partition-aware large-slot release feed / PWA update selector.
   The existing default production feed targets older 1.25 MiB slots.
6. Exercise an A/B firmware update and rollback once on accessible
   hardware, using the larger partition scheme.

**Never attempt to migrate the table from the normal Synap BLE OTA
path.** ESP-IDF treats partition-table rewrites as non-power-fail-safe.

## References

- Espressif Arduino custom partitions:
  https://docs.espressif.com/projects/arduino-esp32/en/latest/tutorials/partition_table.html
- Espressif ESP-IDF OTA partition safety:
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/system/ota.html
- Espressif esptool flashing:
  https://docs.espressif.com/projects/esptool/en/latest/esp32c3/esptool/flashing-firmware.html
