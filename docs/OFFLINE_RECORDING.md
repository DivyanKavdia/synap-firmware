# Offline recording proposal

Status: proposed; this cleanup does not enable offline capture or write audio to flash.

## Recommendation

Add an opt-in, approximately **two-minute persistent recording buffer** using the existing data partition and the current independent-frame IMA ADPCM codec. Keep both OTA application slots. Start with recovery of a recording that was already running when BLE disconnects, then add deliberate double-tap recording while disconnected.

The current firmware stops capture on disconnect, and its 20-frame PCM queue is cleared. That queue is about one second of live transport headroom, not offline storage. S3 PSRAM is configured but is not currently allocated for recordings. Browser storage can retain audio already received by the PWA; it cannot recover microphone audio that never crossed BLE.

## Capacity on the current targets

Both configured targets have 4 MiB flash. S3 has 2 MiB PSRAM; C3 has none. These are repository target settings, not a measurement of a redesigned PCB.

The pinned Arduino-ESP32 3.3.5 [default partition table](https://github.com/espressif/arduino-esp32/blob/3.3.5/tools/partitions/default.csv) reserves two 0x140000-byte OTA slots and a 0x160000-byte data partition named `spiffs` at 0x290000. NVS, OTA metadata and the coredump partition are separate.

The existing codec produces 404 bytes per 50 ms: **8,080 bytes/second** (64.64 kbit/s), versus 32,000 bytes/second for PCM16. Estimates below exclude filesystem/log metadata unless stated.

| Storage allocation | ADPCM capacity | Role |
| --- | ---: | --- |
| Current 20-frame PCM queue | 1 second of PCM | Live transport only; cleared on STOP/disconnect |
| 512 KiB of S3 PSRAM | 64.9 seconds | Optional temporary queue; lost on power loss/deep sleep |
| 1 MiB of S3 PSRAM | 129.8 seconds | Optional temporary queue, subject to runtime memory headroom |
| Entire 0x160000-byte flash data partition | 178.4 seconds theoretical | Upper bound before metadata and free-space reserves |
| 1 MiB flash audio budget | About 120 seconds practical target | Recommended first persistent buffer |

At a design allowance of 16 metadata bytes per frame, 120 seconds needs 1,008,000 bytes. That fits a 1 MiB budget and leaves roughly 384 KiB of the data partition for log headers, spare erase blocks and recovery. Final usable duration must be measured with the implemented record format. Do not treat the whole 4 MiB flash or both OTA slots as recording space.

## Recording and sync behavior

1. The PWA enables offline buffering on the pendant and shows its capacity. An active recording continues into local storage on link loss, with a distinct recording indication. A double tap stops it locally. Do not start capture automatically on an ordinary boot or reconnect.
2. Encode each 800-sample frame once, before deciding whether to send or store it. Keep microphone acquisition independent of flash erases and BLE writes.
3. Store records containing a persistent recording ID, a 32-bit frame index, monotonic sample offset, codec ID, length and CRC. The live transport's 16-bit sequence wraps after about 54.6 minutes; it cannot uniquely identify an offline recording.
4. On reconnect, advertise pending duration and storage state over a negotiated buffer service. The PWA requests missing ranges, persists each verified chunk in IndexedDB, then acknowledges it. Resume after another disconnect and deduplicate by recording ID plus frame index.
5. Reclaim only acknowledged chunks. Prioritize live audio; initially drain the backlog while idle. Simultaneous live capture and replay should follow measured BLE throughput headroom.
6. When full, preserve unsynced audio, stop adding local samples and indicate “buffer full.” Never silently overwrite the oldest unsynced conversation. Record the gap so the PWA does not present it as continuous audio.

Recovered audio should attach to its original recording in the PWA, with clear offline intervals and any unrecoverable gaps. A session clock anchor from the phone can map sample offsets to wall time; after a reboot without an anchor, label time as uncertain instead of inventing a timestamp.

## Storage implementation

Use a bounded append-only log with checksummed records, a commit marker written last, generation numbers, and redundant checkpoints. Recover completed records by scanning after reset. Batch writes and pre-erase reclaimable blocks outside the capture path. Wear must be spread over the reserved region; NVS is for settings and occasional checkpoints, not audio frames.

Use [Espressif partition APIs](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/api-reference/storage/partition.html) with runtime checks for the exact data partition label, type, address and size. The proposed raw log does not mount SPIFFS. Initialize only an erased partition or a recognized Synap log; leave unknown data intact and report storage unavailable. Do not format the data partition merely because a mount/scan failed. App-only OTA can deliver this feature without changing partitions if the installed layout matches and that region is available.

SPIFFS is a less attractive choice for a capture-critical writer: Espressif documents roughly 75% reliable utilization and garbage collection that can block for seconds near capacity. [SPIFFS notes](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/storage/spiffs.html).

Use internal-RAM capture/staging buffers; optional S3 PSRAM supplies extra queue headroom after checking available memory. Flash operations can affect execution and cache access, so a separate writer task alone is not proof that recording will remain continuous. Validate worst-case erase/write stalls and DMA capacity on both chips. PSRAM is temporary storage; [deep sleep powers down ordinary memory domains](https://docs.espressif.com/projects/esp-idf/en/v5.2.6/esp32s3/api-reference/system/sleep_modes.html).

Flush committed audio before normal sleep. Preserve completed records after unexpected power loss; explicitly report any incomplete tail. Block OTA while capturing or writing the log, and retain pending recordings through a normal app update. Set a measured battery cutoff that leaves enough energy to finish a small commit. Provide “delete buffered recordings” in the PWA and document retention; CRC detects corruption, not unauthorized reading of flash.

## Delivery order and acceptance

1. Implement the bounded log and recovery tests with a fake flash backend: partial writes, power cuts, corrupt headers/records, full capacity, rollover, unknown data and interrupted checkpoint updates.
2. Add capability negotiation, pending-duration/storage-health status, resumable acknowledged transfer and PWA deduplication. Devices and PWAs without buffer support must retain the current live transport behavior.
3. Validate active-recording disconnect/reconnect on S3, including a full two-minute interruption, full buffer, low battery, STOP, sleep, reset and OTA with pending audio. Measure frame drops, heap, task stack headroom and battery drain.
4. Validate C3 separately because it lacks PSRAM. Then enable offline double-tap starts and consider concurrent backlog sync. A one-hour buffer needs about 29.1 MB of ADPCM payload alone; that exceeds the current onboard capacity and is a separate hardware/storage decision.

Useful features to ship with the buffer are pending-recording count/duration, automatic resume, explicit gap reporting, storage-full indication and one-command export/delete. More aggressive speech enhancement remains a separate measured audio-quality project; it should not consume the capacity or capture deadline needed for reliable recording.
