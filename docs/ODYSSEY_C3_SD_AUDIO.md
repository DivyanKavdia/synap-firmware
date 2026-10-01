# Odyssey C3: standard and SD-equipped firmware behavior

**Reviewed: 1 October 2026.** Standard Odyssey C3 and Odyssey C3 + SD run **the same** `esp32c3-supermini-4m` firmware image (module 2). The distinction is whether a usable microSD card is physically installed, mounted and ready. Standard C3 must continue normal BLE/PWA audio even when SD is absent or fails.

## Mount and recording

The C3 owns SD via native ESP-IDF SDSPI/FAT and POSIX/VFS. A bounded boot mount completes at **400 kHz before BLE** and promotes to **4 MHz only after validated FAT/VFS mount**. Normal catalogue/recording requests do not trigger repeated remounts; media operation **14** explicitly requests recovery. A failed mount does not format the card or silently delete unsynced files.

| State/action | Expected result |
| --- | --- |
| Standard C3, no card, BLE connected | PWA audio remains functional; SD recording is unavailable |
| C3 + SD, BLE disconnected, first double tap | Start local 16 kHz mono PCM16 WAV under `/synap/`; dim **purple** status pulse |
| C3 + SD, second double tap | Request stop immediately, extinguish purple pulse, checkpoint/finalize WAV |
| BLE connects during an offline take | Existing take stays on SD until explicitly stopped or required PWA capture handoff |
| PWA START while a local take is active | Finalize SD WAV and release microphone before live BLE capture; otherwise fail safely at the handoff deadline |
| BLE connected, no local take | Existing connected PWA audio start/stop; **green** active-recording pulse |
| Four-second hold during local capture | Finalize the take before entering the normal sleep path |
| Card missing, unmounted or unwritable | Log/diagnose failure without breaking BLE; use explicit SD recovery when needed |
| OTA or conflicting media operation | Deny while the local recorder owns storage/microphone |

The WAV header is checkpointed every two seconds and finalized on stop. A real power loss/removal can still damage FAT metadata. Recording never overwrites or evicts older unsynced source files.

## Pins and variant identity

| Function | C3 GPIO |
| --- | ---: |
| I2S BCLK / WS / DATA | 4 / 5 / 6 |
| TTP223 touch | 3 |
| NeoPixel | 8 |
| Battery ADC | 1 |
| SD CS / SCK / MOSI / MISO | 0 / 10 / 21 / 20 |

USB CDC on boot is required to avoid UART0 ownership of SD GPIO20/21. The same C3 module ID, BLE identity, OTA marker and manifest are used whether SD is fitted or not. A compiled `sd`/ `sdAudio` capability does **not** prove that a card is mounted; UI actions must respect runtime readiness.

## PWA SD catalogue and verified synchronization

After reconnect, the PWA can list local C3 WAV files and offer manual sync to Memories. The media-v1 protocol uses operation **7** for catalogue discovery and operation **4** for chunked reads with the explicit WAV path on **every chunk**; `@catalogue` identifies catalogue-byte reads. This prevents an intervening catalogue refresh from replacing the selected foreground file. Legacy selected-file reads remain for compatible older clients.

The PWA downloads source bytes, imports into durable local storage, verifies the imported copy, and **only then** requests operation **17** to delete that SD original. If download, import or verification fails, the original remains. Operation **18** is an explicit user-facing clear of Synap-owned C3 recordings, guarded against active recording. Routine catalogue reads never clear storage.

## Validation boundary

The native/unit contract suite covers recorder lifecycle, STOP gestures, collision/short-write failures, SD media protocol and ownership transitions. A passing build is not a physical-device pass. Test **both** the standard no-card C3 and the Rev K SD-equipped C3 on hardware: cold mount, failed mount with working BLE, offline start/stop and purple indicator, repeated double tap, reconnect, PWA START handoff, verified sync, failed-sync original retention, clear SD and OTA/restart while idle.

See [Firmware variants](FIRMWARE_VARIANTS.md), `firmware/shared/odyssey-sd-{detect,recording,transfer}.cpp`, and `hardware/odyssey-c3/pcb/final/`.
