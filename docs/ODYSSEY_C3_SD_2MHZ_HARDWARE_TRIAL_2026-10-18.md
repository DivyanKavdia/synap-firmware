# Odyssey C3 SD: 2 MHz data-clock trial after pull-ups and decoupling

Date: 2026-10-18 (IST) | Target: `esp32c3-supermini-4m-sd-ota` | Transport: BLE only.

## Field change and why

The operator has fitted SD signal pull-ups and supply decoupling. Prior builds produced native ESP-IDF SDSPI `CMD25` `ESP_ERR_TIMEOUT=263` with `cmd->timeout_ms=5000` at 800 kHz; build 2248 using four-sector/2048-byte bursts reached 800724 PCM bytes (~25 s) before the same timeout. Additional pull-ups and capacitors are a new hardware condition **not yet verified in a repeat recording**.

In this controlled application-only OTA, the C3+SD DATA clock moves from 800 kHz to **2 MHz**, while card INIT stays on the driver's normal 400 kHz path. Each 2 KiB CMD25 burst should spend less time on SPI transfer, reducing bus duty at 16 kHz PCM16 (32 KiB/s) and allowing more idle time between writes. This is below the SDSPI standard 20 MHz ceiling, but not a proven solution to card-internal 5-second busy stalls. Revert to 800 kHz if the hardware develops read/write errors sooner, fails mount, or audio quality worsens.

## Protected invariants

- Never retry a CMD25 whose completion is uncertain.
- No same-power-rail reinitialization after a native write/read timeout; card may still be programming.
- Do not `fclose`, `fsync` or unmount a FILE* stranded after uncertain SD write; preserve prior data.
- No boot-time FAT mutation, format, deletion, preallocation or speculative repair.
- 2 KiB aligned multi-sector writes, one first metadata checkpoint after 256 KiB and 60-second subsequent checkpoints, verified clean-close and logical WAV header synthesis on BLE import.
- No change to pins, OTA partition scheme, legacy C3 no-SD support, or Wi-Fi removal.
- A **verified fresh FAT mount** restores normal SD operation; historical stage 66 remains available only as a past fault. Warm CPU reset alone is not evidence that SD VCC was power-cycled.
- `op27.sdClockKhz` now reports *failure-time clock* preserved in a backward-compatible separate stage/PCM-byte-matched `wclk` NVS record. Old faults without `wclk` report 0=unknown rather than being relabelled as today's 2 MHz.

## Hardware acceptance — disposable SD only initially

1. Fully remove USB and battery power, allowing SD VCC to fall to 0 V before first test. Confirm firmware build and `sdState=1`, `sdLiveProbe=6`, no quarantine in op34.
2. Record 30 s offline; stop successfully; reconnect over BLE and verify file length, playback, and checksum. Repeat **10 short takes consecutively** without resetting or deleting the card.
3. Record 10 minutes, stop, BLE sync, and immediately record again. Then complete **3 x 30-minute** captures across battery/USB.
4. Repeat with at least two genuine FAT32 cards. Compare before/after with the prior 800 kHz build; changing hardware AND firmware means you cannot isolate which helped without A/B.
5. On a failure, capture live op34 `writeQuarantine`, stage, bytes BEFORE reset and op27 `wireCmd,wireErr,wireLen,wireMs,sdClockKhz,wireWriteOk`. Do not format, retry or delete. A new 2 MHz fault should report `sdClockKhz=2000`.
6. If the card works after a true SD power reset while the failed WAV remains, preserve the file: deletion is not required for card recovery.

## Outstanding root cause

CMD25 timing out at 5s can arise from card controller program-busy, signal integrity, contact/module, power transient, or software/card interoperability. Neither 2 MHz nor FAT preallocation guarantees to cure it. **Production recording remains unqualified until the physical acceptance gate passes.** A future PCB with independently switched SD 3.3 V can restore the card without turning the whole pendant off, but must not be simulated in firmware on a permanently powered card.
