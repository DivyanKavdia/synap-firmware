# Odyssey C3 slider-off SD recovery

Date: 2026-10-18; hardware ESP32-C3 SuperMini plus microSD on common switched battery rail.

## Operator procedure

- Unplug USB (otherwise the ESP and SD 3.3V rail might still be energized), move battery slider OFF for approximately 30 seconds, then ON.
- ESP_RST_POWERON allows one native SDSPI initialization and **read-only FAT mount validation first** after CMD25. Only after a clean mount, a bounded automatic cleanup may unlink provably empty Synap artifacts. No format or WAV data rewrite.
- If mounted and root FAT/directory reads pass, the stored historical recording fault remains for diagnostics while the card becomes ready for normal recording and sync.
- If the one cold mount fails, manual touch/op14 cannot blindly repeat SPI initialization on the same-powered SD controller. Warm/OTA reset with an unresolved recorded fault also refuses any remount.
- Current CMD24/25 or CMD17/18 timeout quarantine remains protected. No uncertain write retries or unsafe FAT close.
- Additive op34 RAM-only fields: coldRecovery 0 normal/no historical gate, 1 cold recovery verified, 2 cold attempt failed/pending, 3 warm reset deferred; resetReason is Espressif reset code. The two fields never issue SD commands.

## Validation and limitation

- After the slider power cycle verify SD state 1, probe 6, both read/write quarantines 0 and coldRecovery 1 if there was a previously retained storage fault.
- Do not delete failed WAV files. Test 30-second offline audio with clean STOP and BLE sync, then successive short and ten-minute sessions. Preserve originals and inspect fault op27 if CMD25 returns.
- Existing 2 MHz SDSPI and 2 KiB CMD25 recording batch stay unchanged. Cold recovery restores accessibility but does not fix the underlying cause of a five-second SD controller timeout.

## New automatic cleanup and battery-independent SD operations

- All C3 SD media actions, including selected-file deletion and explicit format, are admitted on actual FAT readiness, controller quarantine, exclusivity and idle state—not the 3,900 mV battery reading. Battery telemetry and general power safety remain separate.
- Only on real slider OFF/ON with USB unplugged: scan root and /synap for Synap-generated R+7-hex WAV and interrupted .PART filenames. Delete at most 16 entries with FAT length 0–44 bytes. **Never delete a nonempty .PART with >44 bytes or a WAV containing PCM**; those may be recoverable.
- Before the first FAT unlink, persist an SD-cleanup unsafe marker in ESP internal NVS. On an unlink/read/write error, immediately stop cleanup, quarantine SD and require another true rail power cycle. On success after a verified mount, clear the marker.
- An overflow beyond 16 candidate files is reported as cleanupStatus=3, with remaining files eligible on a later cold boot.
- RAM-only media op34 reports cleanupRemoved, cleanupStatus, coldRecovery and resetReason. Cleanup runs only once at startup, not after OTA-only reboot, not during recording, and not in a BLE sync callback.
- If no cleanup was necessary, op34 may report zero removed. A lingering historical CMD25 fault in NVS is not evidence of an active fault when current SD state is 1, probe 6, and both quarantines 0.

These safeguards cannot prevent a genuine SDSPI CMD25 timeout, and FAT writes still require validation with the physical pendant.
