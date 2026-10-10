# Odyssey C3 SD recording I/O — implementation notes

**Reviewed: 7 October 2026.** This is the low-level companion to [ODYSSEY_C3_SD_AUDIO.md](ODYSSEY_C3_SD_AUDIO.md). If the two ever disagree, the canonical architecture document and current production source win.

## Current production path

The active C3 SD implementation is:

- `firmware/shared/odyssey-sd-1631-detect.cpp`
- `firmware/shared/odyssey-sd-1631-recording.cpp`
- `firmware/shared/odyssey-sd-1631-transfer.cpp`
- `tools/patch-arduino-sd.cjs`

The recorder uses the mounted Arduino-ESP32 3.3.5 SD/FatFs VFS at a retained runtime clock of **1 MHz**.

## File creation

A disconnected double tap starts a new file under `/synap/`.

The recorder:

1. acquires the SD storage guard;
2. creates a unique path;
3. opens it with `fopen(...,"wb")`;
4. disables stdio buffering with `setvbuf(...,_IONBF,...)`;
5. stages a provisional 44-byte WAV header in the first 4 KiB batch;
6. starts I2S only after the file is open and ready.

There is no contiguous preallocation and no journal sidecar in the current production design.

## Audio writes

PCM is captured as 16 kHz mono PCM16.

The write accumulator is:

```cpp
alignas(4) static uint8_t batch[4096];
```

PCM is copied into this batch and every full batch is written with:

```cpp
fwrite(batch, 1, sizeof(batch), file);
```

This deliberately gives FatFs eight contiguous 512-byte sectors and keeps the Arduino SD layer on its multi-block path.

Do not replace this with a loop of 512-byte writes. Physical testing showed that forcing `count == 1` repeatedly changed the SD transaction behavior and failed reproducibly around the same data volume.

## Stop path

STOP remains sequential.

If the final batch is partial, the unwritten tail is zero-filled and the whole 4 KiB batch is written. The file is then closed.

The current recorder does **not**:

- seek back to offset zero;
- rewrite the 44-byte header;
- truncate a preallocated extent;
- write a journal;
- perform a periodic WAV checkpoint.

That is intentional. Earlier validation proved sustained audio could be written successfully while small random writes/finalization operations still failed.

## WAV validity

The on-card file keeps a provisional header.

During BLE media read, `odyssey-sd-1631-transfer.cpp` derives the real PCM length from the file size and overlays a correct 44-byte WAV header into the outgoing byte stream.

The SD source remains append-only; the PWA receives a conventional WAV.

Any future transfer implementation must preserve this property unless the SD card is requalified for in-place random header updates.

## Single-sector metadata writes

FatFs still performs single-sector metadata operations when creating/closing/updating directory state.

The C3 production build therefore patches Arduino-ESP32 3.3.5 `sdWriteSector()` before compiling the C3 target.

`tools/patch-arduino-sd.cjs` changes the CMD24 completion sequence so that:

- `0x05` is required as the accepted data-response token;
- CS remains asserted while the card is program-busy;
- firmware waits up to 5 seconds for ready;
- the card is deselected only after that busy period clears;
- status is checked after completion.

This was the difference required to move from stage 47 on `fclose()` to a healthy completed take.

The patch is C3-only in CI. Odyssey S3 and Chakshu compile before it is applied.

## Mount recovery

The mount path is guarded by one SD mutex and one-open-file policy.

If a previous storage failure suggests the continuously powered card may still be in a data/program state, the recovery code can:

- drain/stop an unfinished CMD18 read;
- send the stop token for an unfinished CMD25 write;
- return the card to SPI idle;
- retry the normal Arduino mount once.

Do not remount underneath an open file or active recording.

## Failure persistence

The recorder stores the first completed failure stage and the number of successfully committed PCM bytes.

The PWA exposes that as:

- historical `sdProbeState`;
- current `sdLiveProbeState`;
- `lastRecordKiB`.

This is diagnostic persistence, not proof the current card is still unhealthy.

## Sync

Phase-one media-v1 on C3 exposes request/response catalogue and file reads.

The transfer path supports:

- catalogue;
- explicit path reads;
- per-file delete;
- Clear SD for Synap-owned captures;
- explicit recovery.

The PWA verifies durable import before it calls a delete operation. A successful sync is recorded in the PWA even when the SD original is intentionally retained.

## Validation evidence

The physically useful progression was:

- 512-byte capture writes: repeated stage-55 EIO;
- preallocation did not change that failure point;
- 4 KiB capture writes moved the failure to post-record finalization;
- append-only STOP moved the remaining failure to `fclose()`;
- the CMD24 busy-completion patch removed that final metadata failure;
- fresh build-1836 record/stop cycles returned `sdDetectionState=1`, `sdProbeState=6`, `sdLiveProbeState=6`, `lastRecordKiB=0`;
- build 1838 re-enabled the media-v1 sync surface without changing the proven recorder.

## Rules for future changes

A change to any of these should trigger actual-device SD qualification:

- Arduino core version;
- SD driver patch;
- SPI clock;
- batch size;
- stdio buffering;
- file open mode;
- STOP/finalization behavior;
- mount/recovery order;
- one-open-file policy;
- BLE transfer read semantics.

Do not infer that a filesystem/unit test reproduces the SD controller's real busy/program timing. The final acceptance gate is always physical hardware.


## October 10, 2026 — checkpoint policy correction

This older implementation note describes the 7 October path. Subsequent
builds introduced periodic `fsync` every 10 seconds, and the field log after
build 1959 identified a stage-72 `fsync` metadata failure: CMD25 STOP busy
timeout, `0x19070100`, after roughly two minutes of offline recording.

The candidate implementation in firmware PR #180 uses a single 10-second
checkpoint per take; later PCM remains sequential 4KiB appends, with the
next FAT metadata commit deferred until normal STOP/fclose. That returns
to a much lower metadata-write rate while retaining one recoverable prefix
after an unexpected shutdown. A sudden power interruption may still lose
the tail recorded after the checkpoint. Read the **current source** and
`ODYSSEY_C3_SD_AUDIO.md` for the up-to-date policy. Physical qualification
is required before treating the candidate as field-proven.
