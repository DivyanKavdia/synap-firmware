# Odyssey C3 recording I/O

The C3 uses the mounted ESP-IDF SDSPI/FatFs VFS at 1 MHz. Hardware pins and the
media-v1 BLE wire format are unchanged. S3/Chakshu storage is unchanged.

## Recording

- Reserve a unique filename exclusively, then preallocate a contiguous five-minute
  WAV extent (9,600,044 bytes at 16 kHz mono PCM16).
- Write sequential PCM through a checked POSIX descriptor in sector-aware chunks
  up to 4096 bytes, with an 8192-byte application buffer. Retry EINTR and positive
  short writes; zero progress and other errors stop the take.
- Every 15 seconds, sync PCM and then update an alternate 512-byte sector in the
  fixed 1024-byte `.wav.jrn` sidecar. Each record contains magic/version, sequence,
  committed PCM length, path identity CRC, sample rate, the first 468 PCM bytes,
  and a CRC32 over bytes 0..507. The first audio bytes back up the sector shared
  with the normal 44-byte WAV header.
- On stop or rollover, drain PCM, commit the final journal record, seek/write the WAV
  header, sync, truncate unused preallocation, sync, and check both close results.
  Remove the journal only after successful sealing. Advance to the next numbered
  WAV automatically for a longer take.
- After a storage error, do not resume writing the failed file object. Keep the
  journal for recovery. Never format or remount automatically during a take.

## Recovery and transfer

Mount recovery selects the newest CRC-valid journal record with a matching path
identity. It repairs the first WAV sector and truncates to the committed byte
count before removing the journal. An invalid journal is retained with the WAV
for diagnosis; that file is not offered for sync. Old segmented WAV files without
journals retain recovery through their RIFF committed length. Empty valid parts
are removed. Data after the last successful checkpoint can be lost on reset.

Transfer uses pread with one cached descriptor scoped to path and BLE connection
generation. Retries can request any offset. EOF, disconnect, inactivity, new
path/connection, recording, deletion, format, remount and power transition close
that descriptor under the storage mutex. Direct path reads validate completed
segments, just as catalogue reads do. PWA verified import/delete semantics stay
unchanged; completed files remain conventional 44-byte-header WAVs.

## Validation and limits

Native sanitizer tests cover exact PCM/segment sizes, rollover, stop, checkpoint
CRC fallback, torn first-sector repair, invalid/truncated journals, sync failure,
short writes/EINTR, repeated recovery, cached reads, offset retries and unfinished
file rejection. They do not emulate SD NAND, FAT metadata torn writes, or rail
brownout. Two journal sectors are logically separate but an SD controller can
still lose or damage both during power failure.

The recorder currently services I2S and storage synchronously. Preallocation and
sync latency can exceed I2S buffering. Automatic segmentation is implemented;
gapless capture across worst-case card stalls remains a hardware validation gate.
A separate capture task/ring buffer should be sized using measured worst-case
stall latency if those tests reveal dropped samples.

Before sealed-device acceptance: run 100 cold-boot/start/stop/reconnect/verified
sync/delete cycles, ten 30-minute takes, one eight-hour take, and at least 50
random power interruptions (including checkpoint and rollover). Require readable
FAT throughout, correct WAV sizes/sample ordering, no deleted unsynced recording,
recovery of the latest valid checkpoint, and no audio gaps on a known test signal.
Measure 3.3 V at the SD socket during writes and log reset reason, recording stage,
SD state, write/sync latency and DMA overflow. Formatting is not a repair test.

## Follow-up after build 1723

The C3 data clock is reduced to 1 MHz for hardware qualification. Contiguous
preallocation is optional on allocation denial only if the reserved file remains
empty. Real I/O errors stop recording. Header/journal writes use checked
seek/write under the storage mutex to avoid the IDF 5.5 FatFs pwrite zero-write
ENOSPC path that leaks the VFS lock. Success restores the original cursor.

A failed take saves its first stage/errno, bytes and firmware build as one NVS
blob, loaded before the next mount. Catalogue error responses include these
lastRecord fields separately from current boot state. This records completed
failure handling, not abrupt loss of power before the NVS write. Stages 30 and
32–35 distinguish initial preallocation, journal creation, header write, seek
and initial checkpoint. PWA discovery requests one catalogue even on mount
failure, exposing this information without an SD-ready UI requirement.
