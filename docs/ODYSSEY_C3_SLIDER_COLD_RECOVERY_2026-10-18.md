# Odyssey C3 slider-off SD recovery

Date: 2026-10-18; hardware ESP32-C3 SuperMini plus microSD on common switched battery rail.

## Operator procedure

- Unplug USB (otherwise the ESP and SD 3.3V rail might still be energized), move battery slider OFF for approximately 30 seconds, then ON.
- ESP_RST_POWERON allows a single native SDSPI initialization and read-only FAT mount after any prior CMD25 fault. No formatting, file rewrite, deletion, or write preflight at boot.
- If mounted and root FAT/directory reads pass, the stored historical recording fault remains for diagnostics while the card becomes ready for normal recording and sync.
- If the one cold mount fails, manual touch/op14 cannot blindly repeat SPI initialization on the same-powered SD controller. Warm/OTA reset with an unresolved recorded fault also refuses any remount.
- Current CMD24/25 or CMD17/18 timeout quarantine remains protected. No uncertain write retries or unsafe FAT close.
- Additive op34 RAM-only fields: coldRecovery 0 normal/no historical gate, 1 cold recovery verified, 2 cold attempt failed/pending, 3 warm reset deferred; resetReason is Espressif reset code. The two fields never issue SD commands.

## Validation and limitation

- After the slider power cycle verify SD state 1, probe 6, both read/write quarantines 0 and coldRecovery 1 if there was a previously retained storage fault.
- Do not delete failed WAV files. Test 30-second offline audio with clean STOP and BLE sync, then successive short and ten-minute sessions. Preserve originals and inspect fault op27 if CMD25 returns.
- Existing 2 MHz SDSPI and 2 KiB CMD25 recording batch stay unchanged. Cold recovery restores accessibility but does not fix the underlying cause of a five-second SD controller timeout.
