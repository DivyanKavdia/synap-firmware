# Odyssey C3 hardware generations and OTA safety

Two ESP32-C3 generations share the BLE media protocol but **must never share firmware images**:

| Hardware | Existing shipped C3 (no SD) | New factory C3+SD |
| --- | --- | --- |
| Firmware target | `esp32c3-supermini-4m` | `esp32c3-supermini-4m-sd` |
| Product marker | `SYNAP-ESP32C3-OTA-ID-V3` | `SYNAP-ESP32C3-SD-OTA-ID-V1` |
| Flash | 4MiB | 4MiB |
| Partition scheme | `default` | `c3-sd-dual-ota` |
| app0/app1 capacity | 1,310,720 B | 1,966,080 B |
| Wi-Fi + HTTPS | excluded | included |
| Filesystem | none on board | removable microSD for audio (no SPIFFS partition) |
| Initial provisioning | existing sealed layout, **do not change** | USB factory flash of merged image and custom table before sealing |

## New C3+SD factory layout

```
0x009000  NVS        0x004000
0x00D000  ota data   0x002000
0x00F000  PHY        0x001000
0x010000  ota_0      0x1E0000
0x200000  ota_1      0x1E0000
```

The new partition table is in `partitions/c3-sd-dual-ota.csv`. `tools/verify-c3-sd-layout.cjs` verifies the **compiled partition binary**, including sizes, alignment and non-overlap. The GitHub build packages `factory.bin`, `partitions.bin`, `partitions.csv`, `firmware.bin`, and the target-specific `latest.json` for new production units. For first provisioning, flash the `factory.bin` using the board's proper USB serial bootloader workflow; do not OTA an application image to create a new partition table.

## Factory acceptance checklist

1. Before sealing, provision using USB factory flash and verify identity `SYNAP-FW:esp32c3-supermini-4m-sd:...` via BLE; the older no-SD C3 must retain the old identity.
2. Read device OTA status and verify inactive slot capacity **1,966,080 bytes**. A reported 1,310,720 means the old table is still installed; **do not attempt Wi-Fi image OTA**.
3. Check microphone, touch, battery, SD mount and read-only catalogue. Never auto-format or erase SD. Confirm physical 3.3V supply is stable during SD writes and Wi-Fi TX.
4. Record offline on a spare known-good FAT card, stop gracefully, read with BLE, then Wi-Fi sync over a trusted network. Verify Synap Cloud receipt and unchanged WAV on SD.
5. Verify BLE and Wi-Fi coexist with sufficient free heap, TLS certificate verification, and upload progress; test interrupted upload without SD loss.
6. Run a second **BLE OTA** to prove the new dual-slot layout, reboot selection and rollback behavior. Confirm older C3 firmware feed rejects new target image.

## Non-goals and safety gates

* An application-only OTA **cannot** change a shipped unit's flash partition table; never send `factory.bin` through BLE OTA.
* Existing 4MB no-SD C3s retain default partitions and their current production update track.
* The new C3+SD retains firmware-side blocks on SD delete, Clear SD and Format SD until FAT metadata write reliability is qualified.
* Wi-Fi upload is HTTPS with certificate verification; never downgrade to plaintext Internet upload to reduce image size.
