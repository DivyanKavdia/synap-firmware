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

## 2026-10-16 field update: build 2206 SD read failure (CMD17)

**Hardware:** Synap Odyssey C3 / ESP32-C3 SuperMini 4 MiB, SPI2 SDSPI CS=GPIO0, SCK=GPIO10, MOSI=GPIO21, MISO=GPIO20, SD powered at 3.3 V. The **assembled** battery divider is 1 MΩ / 470 kΩ (the archived fabrication BOM's values are historical).

**Evidence:** mounted and catalogued, media-v3 BLE (MTU 517/ATT 514, op31), then `Rxxxxxxx.WAV` download failed after 69,632 of 3,084,288 bytes. A 4,096-byte cache fill read only 3,072 bytes; **CMD17 timed out (ESP_ERR_TIMEOUT=263) after 1,000 ms**. An earlier offline take had already reported **CMD25 timeout after 5,045 ms** at byte 368,640. These are *different native SD read and write failures*. Missing BLE notifications (6/12 etc.) were observed too, but the fatal failure is native SD I/O. No proof of FAT corruption is provided by these readings.

### Shipped software guard (post-2206)

- Intercept timeout results from **CMD17/CMD18 only after a previously verified mount**, with no extra SD command. Latch a volatile **read-timeout quarantine**, alongside existing write-timeout quarantine.
- Refuse **same-powered-card remount/recovery**, including explicit BLE op14 and touch recovery, after a timed-out read. Preserve all existing audio, do **not** automatically format, clear, delete, reset the SD host, or attempt CMD0/CMD12. Keep deep-sleep veto. The firmware cannot electrically reset the card because its 3.3 V rail is not software-switchable.
- Expose **`readQ` / `writeQ`** in existing op29 RAM-only transfer diagnostics without touching storage or changing the BLE ABI. Historical op27 recorder faults remain distinct from live read errors.
- Do not change recorder buffering, 60-second sync interval, native 800-kHz SD clock, FAT layout, partitions, or the read cache in response to one failed sector: none would be a demonstrated fix for an electrical/data-response timeout.

### Physical ECO / acceptance, not a firmware change

1. **Preserve data first.** Before opening the enclosure or removing the card, completely power off, disconnect USB, and confirm the SD rail has discharged. Do not hot-remove the mounted card. Back up readable WAV files via a trusted external reader; avoid reformatting the original.
2. **Measure SD VCC at the microSD module, not at the battery ADC.** Scope VCC-to-SD-GND at boot/mount, first FAT metadata write, sustained PCM recording, and live BLE SD sync. Record both DC voltage and short transient minimum/ground bounce. The ESP32-C3's cell reading near 3.9 V proves nothing about the SD's local 3.3 V rail.
3. **Check fitted BOM first.** Verify C2 (10 µF local SD bulk) and the appropriate 100 nF ceramic decoupler are present and truly connected to SD VCC/GND. On a prototype, an additional **47–100 µF low-ESR bulk capacitor** and **100 nF ceramic** placed very close to the SD socket can be evaluated for transient droop. Observe polarity on polarized capacitors, minimum 6.3-V rating (10-V preferred for margin), regulator stability, startup inrush and connector clearance. Extra capacitance does not fix a weak 3.3 V regulator, an unreliable socket, or a defective card.
4. **Inspect SPI electrical path.** Check short wires/traces, solder joints, shared ground and the actual module's CS/CMD/DAT pull-ups (typically ~10 kΩ where required by SD SPI design). The C3 internal MISO pull-up alone is not an adequate substitute for verifying the module. Measure CS, CLK, MOSI, MISO and SD rail together if possible. Do not use S3 pins for this C3 board.
5. **Cross-test** using a genuine known-good SD card (FAT32, compatible capacity) after securing the original data. On separate disposable media, run one-minute and ten-minute recording, full readback/verified BLE sync, then an immediate second recording **without an MCU reset**. Repeat at different LiPo state of charge and under USB vs battery power. Never use production recordings for destructive testing.
6. **Future PCB revision:** add a properly sized, switchable SD 3.3 V rail/load switch and a defined off/discharge interval. Ensure firmware stops FatFs/SPI operations, unmounts cleanly and closes files before switching power; never cut power during a write or after an unresolved program-timeout just to force a retry. Add SD VCC/GND test points and measured local decoupling. A hardware rail redesign is not achievable via OTA.

**Acceptance gate:** 30-minute offline recording, verified BLE import, another recording with no restart, multiple stop/start cycles, and no native CMD17/CMD24/CMD25 timeout on at least two known-good cards. Firmware safety quarantine is not a remedy for faulty media or supply power.
