# Odyssey C3+SD read/write reliability audit — 13 October 2026

**Audited production:** firmware 2144, ESP32-C3 SuperMini (4 MiB flash), native ESP-IDF SDSPI/FatFs; enhanced C3+SD OTA target. Scope is **only C3+SD**, not S3/Chakshu. Prioritize retention of existing recordings over availability.

## Confirmed field failure

- Build 2139: card mounted successfully, but disconnected recording failed at stage **76** before audio. `mkdir /synap` needed FAT metadata I/O; native **CMD24** returned **ESP_ERR_TIMEOUT (263)** after 5006 ms. The failure happened at different sector arguments on successive attempts.
- C3+SD assembly has the 1 MΩ / 470 kΩ ADC divider; reported battery ~3.922 V cannot validate the **SD 3.3 V supply** under load.
- Read-only raw op28 reported **status 2 / ESP_ERR_INVALID_STATE 259** because mounted driver/SPI was not safely released. No sector or FAT table was read. Do **not** interpret all-zero diagnostic fields as an empty/damaged filesystem.

## Existing correct engineering controls

1. Native SPI2 SDSPI + FatFs, `format_if_mount_failed=false`, no mount-time write test.
2. One SD mutex serializes recording, BLE transfer, Wi-Fi AP, mount and file maintenance.
3. Recorder writes aligned 4096-byte PCM batches; each physical SD write and finalization error has a distinct diagnostic stage.
4. Explicit `f_sync` every 60 seconds, `fclose` at STOP; no unsafe in-place WAV header rewrite. BLE and Wi-Fi synthesize correct WAV lengths from saved file length.
5. On CMD24/CMD25 or following CMD13 program timeout, quarantine the controller; no remount, formatting or more FAT activity until genuine rail power reset.
6. BLE operations 17/18/19 (delete/clear/format) are **disabled** before FAT code; original recordings are preserved after sync.
7. Read-only BLE transfers use a bounded 4 KiB cache; Wi-Fi AP uses one card lock, read-only `fopen/fread/fclose` and a virtual WAV header.
8. New recordings use short `Rxxxxxxx.WAV` filenames physically in FAT root while media-v1 logical paths remain `/synap/...`. Legacy directory recordings stay indexed in BLE/Wi-Fi catalogues.

## Critical edge defect corrected by linked PR

After the *final padded 4096-byte* `fwrite`, the previous code unconditionally invoked `fclose` even if that **final write** had just timed out. It had checked quarantine only *before* writing the tail. PR #218 adds a **second quarantine check between final fwrite and fclose**, preventing a further potentially dangerous FAT metadata operation while SD is still program-busy.

## Residual risks and design improvements

| Priority | Area | Evidence / limitation | Action |
|---|---|---|---|
| P0 | SD electrical power | 3.922 V cell does not establish stable 3.3 V SD rail through Rev K Schottky-to-VIN and regulator | Scope or fast meter at **SD VCC/GND**, capture minimum voltage during first FAT entry and sustained writes; inspect ground and connector soldering |
| P0 | SD card itself | Five-second CMD24 timeout on writable media, observed at different LBAs | Test on a known-good card and validate a full-file write/read-back on a PC/phone before reinsertion; preserve data before changing filesystem |
| P1 | Metadata frequency | FatFs still needs **CMD24** for new directory entries, cluster allocation, FAT/FSInfo even with 8.3 names and 4 KiB batches | On **new empty** test cards only, compare FAT32 cluster sizes (e.g. 16/32 KiB where supported); inspect actual card BPB, never autoformat field cards |
| P1 | Brownout / rail restart | Warm ESP reset cannot power-cycle a card powered from always-on 3.3 V | New hardware revision: switchable SD rail with suitable regulator/load switch, bulk + ceramic capacitors beside module and verified discharge/settle time |
| P1 | Long PCM recording | 16 kHz × 16-bit mono produces 32,000 bytes/s; 5 s write stall accumulates ~160 KB audio beyond available C3 headroom | Do not promise lossless capture across real 5-second SD stalls; fix hardware/card or redesign codec+storage buffer |
| P1 | End-to-end content integrity | WAV header verification + size check only verify first 44 bytes/length; PWA SHA256 of imported bytes verifies transport/storage copy, not captured audio content | Consider rolling CRC32 of audio written, save completion digest after clean STOP without adding FAT writes during recording |
| P1 | Transfer length | BLE and direct AP-to-PWA WAV imports cap at **64 MiB** (~35 min PCM); AP session max **15 min** even for active downloads | Implement incremental IndexedDB journal writes, rolling digest, resume token/offset and active-transfer session extension; do NOT just raise the in-memory blob limit |
| P2 | SD read scalability | BLE catalogue caps at 100 entries; AP UI allows 250 entries | Add cursor-based paginated catalogue in future media protocol; never silently drop excess entries |
| P2 | STOP diagnostics | Touch stop, trusted critical battery and connected capture handoff can request stop | Add a RAM/NVS stop-cause counter; distinguish voluntary STOP from I/O fault in PWA diagnostics |
| P2 | Throughput | 800 kHz SPI data ceiling ~100 KB/s before protocol overhead, vs ~32 KB/s PCM production | Keep transfer/recording mutually exclusive; avoid Wi-Fi radio during SD recording, measure actual write stalls |

**Important:** 4 KiB `fwrite` may trigger multi-sector CMD25 for contiguous data, but it does **not** guarantee CMD25 for **all** writes. FAT metadata and cluster boundaries still create CMD24 transactions. Increasing the SD write timeout or automatically remounting after a programming timeout is not an evidence-based fix.

## Hardware checks (Rev K / assembled variant)

- SD: CS GPIO0, SCK GPIO10, MOSI GPIO21, MISO GPIO20; use USB CDC on boot to avoid UART0 pin conflicts. Verify real pin mapping rather than S3/Chakshu values.
- Board BOM lists **C2 = 10 µF SD bulk** and 100 nF decoupling. Verify C2 and nearby grounds are genuinely populated and connected. A local low-ESR 47–100 µF bulk and 100 nF ceramic may reduce *transient* droop, but cannot cure insufficient regulator headroom or an internally defective card.
- Espressif recommends appropriate SD signal pull-ups (typically 10 kΩ). Confirm the module’s actual pull-ups and short wiring/low contact resistance; firmware's MISO internal pull-up is not a substitute for verified hardware termination.
- If separate USB-powered and LiPo-powered tests differ, investigate the regulator/Schottky power path and simultaneous radio load; do not infer SD VCC from battery ADC percent.
- Never hot-pull the SD card or connect/disconnect its VCC while firmware has the volume mounted.

## Acceptance gates before claiming the storage defect solved

1. Fresh **true SD power cycle**. Read-only catalogue before any new writes; record initial live SD state, build number, reset reason and battery.
2. On a known-good blank FAT32 test card, double-tap offline 1 minute, manual STOP, verify file and play audio; repeat 10 min, 30 min, 60 min.
3. Repeat 20 start/STOP cycles, each verified for preserved file and consistent WAV size. Check no partial catalogue errors.
4. BLE read every generated recording, confirm hash and journal receipt; separately transfer via private AP without editing SD.
5. Remove and test card on PC *only when powered off*, verify actual root-level `Rxxxxxxx.WAV` files and prior legacy `/synap` WAVs.
6. Reproduce under fully charged and lower LiPo voltages, and compare at least two genuine card brands/capacities; record min VCC at module during writes.
7. On any real CMD24/25 timeout, stop without retry/format/delete. Capture RAM-only op27/op29 logs. Treat post-timeout “unavailable” as protective quarantine, not evidence of FAT erasure.
8. Do not enable field SD deletion, formatting, file repair or automatic remount without successful physical qualification.

**References:** Espressif ESP-IDF SDSPI host, SD pull-up requirements, and FATFS mount documentation. Hardware references: `hardware/odyssey-c3/pcb/final/BOM.csv` and `ASSEMBLED_C3_SD_FIELD_VARIANT.md`.
