# Odyssey C3 SD write-timeout investigation: build 2038

Date: 2026-10-11. Hardware under investigation: **sealed ESP32-C3 SuperMini, native ESP-IDF SDSPI / FatFs, 3.3 V microSD module**. This document records evidence, conservative mitigation and field gates; it does **not** claim the card has passed a sustained write test.

## Established field evidence

- Build 2036: forced single-block CMD24 failed on the first write. Build 2038 reverted that experiment.
- Build 2028: write timeout reported after 3,235,796 audio bytes.
- Build 2038: initial mount healthy (`sdDetectionState=1`, `sdLiveProbeState=6`), later native write failed: `recordStage=66`, `recordBytes=1859540`, `failureOffset=1859584`, `wireCmd=25`, `wireErr=263/ESP_ERR_TIMEOUT`, `wireArg=65530`, `wrE=5`, `wrX=4096`, `wrF=1`. Prior synchronized WAV prefix **may** remain on FAT; do not delete or repair it in firmware.
- `65530 = 0xFFFA` is the **physical command address** (sector LBA for SDHC/SDXC, byte address for SDSC), **not** a WAV byte offset or automatic proof of a 16-bit wrap, 32 MiB boundary defect, or FAT metadata allocation. On an SDHC card its byte location is 33,551,360 (six sectors before 32 MiB); interpret only after obtaining OCR/card type and FAT BPB layout.
- Measured battery ~4.22 V at the C3 ADC does **not** rule out SD 3.3 V transient brownout or SD socket/contact noise during a flash erase/program event.

## Pinned native-driver behavior (ESP-IDF 5.5.1 in Arduino ESP32 core 3.3.5)

The active source uses `esp_vfs_fat_sdspi_mount`, `sdmmc_write_sectors_dma` and `sdspi_host_do_transaction`; it is **not** Arduino `SD.write`. The native write command has a 5000 ms timeout (`SDMMC_WRITE_CMD_TIMEOUT_MS`). For CMD25, the SPI host transmits individual data tokens, data and CRC, checks the card's data-response token, waits for MISO to leave busy state after each sector, sends the `0xFD` stop token and waits for final idle **only if the preceding steps succeed**. A timeout may therefore mean command response, accepted-block busy, or final stop-token busy. A generic `ESP_ERR_TIMEOUT` alone does not distinguish these phases.

Importantly, `sdmmc_write_sectors_dma` attempts CMD13 after CMD25 even when the data command fails; in the multi-block error branch it may query ACMD22. Those commands can reach a card with unknown unfinished programming and may make recovery less reliable. A larger CMD24/CMD25 batch does not address this ambiguity.

Sources (pinned):
- https://github.com/espressif/esp-idf/blob/v5.5.1/components/sdmmc/sdmmc_cmd.c
- https://github.com/espressif/esp-idf/blob/v5.5.1/components/esp_driver_sdspi/src/sdspi_host.c
- https://github.com/espressif/esp-idf/blob/v5.5.1/components/sdmmc/include/esp_private/sdmmc_common.h

## Proposed safety fix (not yet physically qualified)

The native `host.do_transaction` interceptor now latches the **first ambiguous CMD24/CMD25 transaction failure**, or the first failed CMD13 immediately after a write, and refuses **all subsequent card commands in RAM** until a genuine SD rail power reset. The initial command's error, argument, timing, response and prior-command context are still captured. The current session cannot send a second command, run post-error CMD13/ACMD22, retry an unknown write, mount again, or attempt GPIO CMD12/CMD0 recovery. A gesture/Check SD is refused while latched. Per-take `odysseySdClearWireFault()` does **not** reset this safety latch.

This is **damage containment**, not a proven solution for an SD that stops responding in the first place. It changes no FAT geometry, existing recordings, OTA partitions, CS/SCK pins, SPI frequency, or write sizes. No automatic formatting, deletion, rewriting or repair occurs.

## Root-cause decision tree

1. On a **known-good powered** card, retrieve op27 fields **`wireMs`, `wireBudget`, `wireLen`, `wirePrev`, `wireWriteOk`** from the newer telemetry firmware. Build 2038's existing error record does not include a reliable elapsed-time measurement. If CMD25 takes almost exactly 5000 ms, programming-busy timeout becomes plausible, **but** a missing R1 command response can have the same outer budget.
2. If possible, scope the **microSD 3.3 V pin relative to SD GND**, not the battery ADC, during sustained recording. Check rail droop, local bypass capacitor, card insertion/contact resistance, MISO/CS pullups and switching edges at 800 kHz. Inspect voltage at erase-boundary pauses. Do not assume stable battery implies stable card supply. Do not change the sealed PCB without measurements.
3. Read the SD card's OCR/CSD and read-only FAT32 BPB (partition start, reserved sectors, FAT copies, FAT size, data start, cluster size). Compare the failed **absolute LBA** to FAT and data regions; examine whether repeated failures cluster around erase-group boundaries. Avoid modifying card sectors during this analysis.
4. If timeouts occur after confirmed 3.3 V stability and healthy data-response tokens, qualify a write-completion/busy-poll driver fix in an isolated test build. Do **not** replace FatFs or introduce a new raw-sector writer in production unless a controlled A/B test proves the vendor SDSPI implementation is the actual cause. A second filesystem owner could destroy existing recordings.
5. Before any production rollout, validate on a **spare card / sacrificial device** and then on a device with backed-up existing recordings. Never use the only copy of old recordings as a stress-test target.

## Deployment gate

- GitHub source checks, all C3/S3/Chakshu production builds and 1,310,720-byte C3 OTA image size limit must pass.
- Physical qualification: **30 minutes continuous offline SD audio**, 10-second checkpoints, clean double-tap STOP, BLE reconnection, file catalogue, verified WAV length/playback, repeat across cold boots; repeat at lower battery and with card nearly full. At minimum two identical long-run passes without timeouts, data loss, SD busy-low or BLE instability.
- Copy/sync old recordings and confirm playback **before** stressing the card. Do not use the PWA Delete/Clear SD commands in this investigation.
- On a failing run, capture op27 diagnostics and any raw serial traces, then do not force further commands. A firmware/ESP restart is **not** an SD power cycle in this circuit.

The production pipeline (`.github/workflows/firmware.yml`) publishes automatically on `main`; leave this change on a PR branch until physical qualification is reported.
