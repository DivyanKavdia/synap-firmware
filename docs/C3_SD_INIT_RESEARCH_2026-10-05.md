# C3 offline SD initialization repair

## Evidence and scope

The supplied build-1766 diagnostic reports mount failure (state/probe 2/2),
five SD.begin attempts and a stage-1 ENODEV recording failure with zero bytes.
It does not identify the failing card command or distinguish card initialization
from FAT mounting. The reported free-space value could survive a failed remount.

Current main uses Arduino ESP32 **3.3.5**, not 3.3.7. Its release workflow omitted
the existing SD initialization backport. Git history shows this was deliberately
reverted in 970a0e3 while trying the older known-good baseline. Re-enabling it is
not, by itself, proof that the user's physical failure is fixed.

The 3.3.5 driver has a single CMD0 sequence, a single CRC enable attempt, a one-second
ACMD41 timeout and SPI-mode ACMD41 arguments corrected by Espressif PR 12766.
This release applies that upstream fix to **C3 only**, with native behavioral
regressions and driver diagnostics. S3's initializer body is preserved unchanged.

Primary references:
- https://github.com/espressif/arduino-esp32/pull/12766
- https://github.com/espressif/arduino-esp32/blob/3.3.5/libraries/SD/src/sd_diskio.cpp
- https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c3/api-reference/peripherals/sdspi_host.html
- https://github.com/espressif/esp-idf/blob/v5.5/components/fatfs/vfs/vfs_fat.c

## Behavior and diagnostics

Keep CS=0, SCK=10, MOSI=21, MISO=20 and USB CDC on boot. Retain the 400 kHz
clock, three-second boot settle and existing bounded mount attempts. Never
format as part of boot, double tap or recovery.

Disconnected double tap runs mount/recovery and WAV preparation (amber), then
starts the microphone and pulses purple. The second double tap stops capture
and seals the WAV. A BLE connection alone does not redirect the running take.
The existing five-minute segmentation, inline recovery journal and verified
PWA import-before-delete remain in use. Failure shows red, not purple.

`sdInit` in the catalogue error is `[step, command, R1, fatResult, vfsResult]`:

- step 0: card initialization was not entered (inspect VFS result/resource allocation).
- step 1: initial clocks; 2: CMD0; 3: CRC enable; 4: card version/OCR/ACMD41;
  5: card-detect configuration; 6: block length; 7: capacity/final initialization.
- command and R1 are the last initializer command/result; 255 means no valid response.
- FAT result -1 means no f_mount result; 0 success; 1 disk I/O; 3 not ready;
  13 no valid FAT volume. A successful command handshake with FAT failure is
  materially different from an unresponsive card at CMD0.
- VFS result 0 means registration succeeded; nonzero is the actual ESP error.

Tracking stops before failed SD.begin cleanup sends CMD0, so teardown cannot
replace the initializer's failing command. Snapshots survive application teardown.
Free-space telemetry is cleared when the mount is released or invalidated.
Error JSON falls back to a compact complete object if counters exceed the BLE
payload budget. Firmware linkage requires the patched driver; CI cannot silently
omit it again.

## Single-sector write acceptance

The pinned driver's single-sector writer explicitly rejects CRC/write error
responses, but other responses (including its data-wait timeout value zero)
can fall through to CMD13 and be reported successful. C3 now requires the
accepted data-response token 0x05. The existing 0x0A CRC retry remains. A native
regression executes the patched writer for all 32 masked response tokens and
requires every nonaccepted token to fail, even with a successful CMD13. This
protects WAV headers, recovery sectors and FAT metadata from false success;
it does not make an electrically unresponsive card writable.

## First-checkpoint recovery correction

A separate, reproduced recorder bug affected optional contiguous-allocation
fallback. That path left an empty file; writing only the first inline journal
slot extended it to `journal_offset + 512`, but discovery required
`journal_offset + 1024`. A reset before checkpoint two therefore missed the
valid first checkpoint and could truncate the recording using its zero-length
initial WAV header. Initial commit now materializes/clears the second slot
before committing the first. A regression starts with a non-preallocated WAV,
commits one checkpoint, closes it as on reset, and verifies exact recovered PCM.

## Validation and device acceptance

Native tests execute the actual pinned old and patched initializer bodies against
cards requiring a second CMD0, CRC retry, correct ACMD41 arguments, or 1.6 seconds
to become ready. They also require an absent card to remain failed. Existing
recorder tests exercise real temporary WAVs, PCM contents, stop, rollover,
reconnect, journal recovery and storage failures. LED tests execute actual RGB
logic and verify amber, pulsing purple, immediate stop and red failure.

These are simulations plus compilation, not physical SD validation. After OTA:

1. Disconnect BLE, wait for boot preparation to finish, double tap once.
2. Require purple pulses, speak for 20 seconds, then double tap again.
3. Reconnect, sync the WAV, and play it. Require audible complete audio and the
   SD original to remain until verified import.
4. Repeat three start/stop cycles and one take beyond five minutes. Check both
   segments and the join for lost audio.
5. If red appears, retrieve the catalogue diagnostic including sdInit before
   another format or reset. A card that never responds to CMD0 may require an
   actual SD supply power cycle; this board has no software-controlled SD rail.

At 400 kHz and with synchronous capture/storage, worst-case SD write latency and
rollover gaps remain hardware acceptance items. Do not claim gapless long-term
recording or a repaired electrical/card fault from passing host tests.
