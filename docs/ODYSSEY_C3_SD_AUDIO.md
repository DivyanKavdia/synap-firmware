# Odyssey C3 SD audio — production architecture and recovery reference

**Reviewed: 7 October 2026.**

This document is the canonical reference for the optional microSD implementation on **Synap Odyssey C3 / ESP32-C3 SuperMini**. It exists specifically to prevent a repeat of the October 2026 debugging cycle.

Standard Odyssey C3 and Odyssey C3 + SD use the **same** firmware target, module identity and OTA feed:

- target: `esp32c3-supermini-4m`
- module: `2`
- production baseline validated on hardware: **Synap OS build 1838**
- build-1838 source: `978b44a8cc8b4c4b270fd15c396c1ab740d92008`
- stable SD recorder first physically validated in build **1836**
- Arduino ESP32 core: **3.3.5**

The OTA feed remains authoritative for the currently installable build. Do not infer installed behavior from a later source commit alone.

## 1. Hardware contract

Do not mix this pinout with Chakshu / XIAO ESP32-S3 Sense.

| Function | Odyssey C3 GPIO |
| --- | ---: |
| INMP441 BCLK | 4 |
| INMP441 WS / LRCLK | 5 |
| INMP441 DATA | 6 |
| TTP223 OUT | 3 |
| Battery ADC | 1 |
| NeoPixel DIN | 8 |
| microSD CS | 0 |
| microSD SCK | 10 |
| microSD MOSI | 21 |
| microSD MISO | 20 |

USB CDC on boot must remain enabled so UART0 does not take ownership of GPIO20/21.

The Rev K carrier hardware source and manufacturing contracts live under `hardware/odyssey-c3/pcb/final/`.

## 2. Active production source graph

The C3 SD runtime intentionally uses the restored 1631-family modules:

- `firmware/shared/odyssey-sd-1631-detect.cpp`
- `firmware/shared/odyssey-sd-1631-recording.cpp`
- `firmware/shared/odyssey-sd-1631-transfer.cpp`

These files are selected by `firmware/shared/sources.json` and are the production source of truth.

The similarly named files below are **not** the active C3 production implementation and must not be substituted casually:

- `odyssey-sd-detect.cpp`
- `odyssey-sd-recording.cpp`
- `odyssey-sd-transfer.cpp`
- `odyssey-sd-io.cpp`
- `odyssey-sd-clean-*.cpp`

Git history is the rollback store. Do not revive an older parallel SD architecture beside the active one.

## 3. Mount lifecycle

The working C3 mount path is based on **Arduino-ESP32 3.3.5 SD/SPI + FatFs/VFS**, not the abandoned direct native-SDSPI mount architecture.

Important details:

1. The card is initialized before BLE setup.
2. Arduino performs its low-speed card initialization internally at approximately **400 kHz**.
3. The retained runtime SD data clock is deliberately conservative at **1 MHz**:
   `ODYSSEY_SD_DATA_FREQ_HZ=1000000u`.
4. Only one open file is allowed:
   `ODYSSEY_SD_MAX_OPEN_FILES=1`.
5. CS is driven HIGH **before** switching it to OUTPUT, preserving the known-good lifecycle and avoiding a startup CS-low glitch.
6. Mount validation does not stop at `SD.begin()`. Firmware verifies:
   - the VFS root,
   - the `/synap` directory,
   - directory open/close,
   - a small create/write/flush/close/unlink probe.
7. A failed mount never formats the card.

Ready state is only published after the filesystem validation succeeds.

### Boot and recovery

The card is continuously powered across many ESP32 software resets, so MCU reset does **not** guarantee an SD protocol reset.

If the previous recorder ended with a storage-related failure, boot performs a bounded protocol re-arm before the first normal mount. Recovery can also stop an unfinished:

- CMD18 multi-block read, or
- CMD25 multi-block write,

then return the card to SPI idle before retrying the normal Arduino mount path.

A disconnected double tap does not require a separate recovery gesture. If storage is stale/unavailable, the recording task first attempts recovery and then continues into recording on that same gesture.

After a failed recording, the recorder releases its file and storage guard, then performs one bounded re-arm before returning idle.

## 4. The C3-only Arduino SD driver patch

This is a required production dependency, not an optional optimization.

The production workflow pins Arduino-ESP32 **3.3.5** and, **only before compiling the C3 target**, applies:

`tools/patch-arduino-sd.cjs`

to:

`libraries/SD/src/sd_diskio.cpp`

The patch is guarded against the exact expected Arduino 3.3.5 source and fails CI if that source no longer matches.

### Why the patch exists

The stable recorder proved that large multi-sector audio writes worked, while `fclose()` still failed during FAT/directory metadata updates.

The remaining distinction was the single-sector CMD24 write path. The production patch aligns that path with native ESP-IDF SDSPI behavior:

- accept only the SD data-response token `0x05`,
- keep CS asserted while the card is internally programming the sector,
- wait for the card to become ready for up to **5 seconds**,
- then deselect the card,
- then issue status checking,
- treat the CRC-response token as retryable.

This fixed the observed stage-47 close failure and was physically validated in build 1836.

### Scope isolation

The GitHub Actions workflow intentionally compiles Odyssey S3 and Chakshu **before** applying this core patch. Only the C3 binary is built against the patched SD library.

Do not move the patch earlier in the workflow without deliberately deciding to change the S3/Chakshu core as well.

## 5. Offline recording architecture

Disconnected double tap starts/stops a local **16 kHz mono PCM16** recording under `/synap/`.

While recording, the status NeoPixel pulses **purple**. Connected PWA recording continues to use the normal BLE path and its connected-status behavior.

### Write path

The current recorder is deliberately simple:

```text
INMP441 / I2S
    ↓
PCM16
    ↓
4 KiB aligned RAM batch
    ↓
fwrite(..., 4096)
    ↓
FatFs multi-sector disk_write
    ↓
Arduino SD multi-block write / CMD25
```

Key invariants:

- the batch is `alignas(4) static uint8_t batch[4096]`;
- it is static rather than placed on the 8 KiB recorder task stack;
- normal capture writes are exactly 4 KiB;
- 4 KiB equals eight 512-byte sectors and keeps FatFs on the multi-sector write path;
- the final partial capture batch is zero-padded to 4 KiB and written through the same multi-block path;
- that can add at most about 128 ms of trailing silence;
- no periodic header checkpoint occurs during capture.

### STOP is append-only

This was a decisive reliability change.

The recorder does **not** perform any of the following at STOP:

- contiguous preallocation,
- `ftruncate()`,
- `fseek()`,
- random 44-byte WAV-header rewrite,
- periodic/final header checkpoint,
- journal-sector rewrite.

STOP performs the final 4 KiB write and then `fclose()`.

The on-card file therefore retains a provisional WAV header. That is intentional.

The BLE transfer path synthesizes the correct 44-byte WAV header from the final file length while returning bytes to the PWA. The app receives a valid WAV without requiring a risky in-place SD header update after capture.

## 6. Why this design must not be “simplified”

The October 2026 hardware tests established the following failure ladder:

| Build / experiment | Result | What it proved |
| --- | --- | --- |
| 1827, 512-byte writes | stage 55 near 136 KiB | forced single-sector capture path was unreliable |
| 1829, contiguous preallocation + 512-byte writes | stage 55 near 136 KiB | FAT allocation during I2S was not the root cause |
| 1830/1833, 4 KiB multi-block writes | >1 MiB PCM written; later finalization failure | sustained CMD25 audio writing worked |
| 1834, append-only recorder | ~1.304 MiB PCM written; stage 47 on `fclose()` | recording path was stable; remaining fault was single-sector metadata commit |
| 1836, C3-only CMD24 busy-completion patch | healthy state after fresh offline record/stop cycle | low-level single-sector metadata write was fixed |
| 1838 | media-v1 catalogue/read/delete/clear re-enabled on the stable recorder | current production sync baseline |

The key lesson is that **512-byte application writes are not “safer” merely because they match the physical sector size**. In this system they force FatFs into repeated single-sector CMD24 operations. The physically proven capture path is the aligned 4 KiB multi-sector path.

Likewise, rewriting a small WAV header after capture looks harmless at application level but reintroduces the fragile random/single-sector metadata path. Keep recording append-only.

## 7. Recorder failure diagnostics

Recorder failures are persisted separately from the current live mount state so a later successful recovery does not erase the evidence.

Important fields exposed through the C3 module capability descriptor and PWA diagnostics:

- `sdDetectionState` — current detected/mounted state snapshot;
- `sdProbeState` — persisted recorder failure stage when one exists, otherwise current probe stage;
- `sdLiveProbeState` — current live mount/probe stage independently of historical recorder failure;
- `lastRecordKiB` — successful PCM progress before the persisted failure, in approximate KiB units.

Healthy state:

```json
{
  "sdDetectionState": 1,
  "sdProbeState": 6,
  "sdLiveProbeState": 6,
  "lastRecordKiB": 0
}
```

A historical failure may remain visible while the live card has already recovered; always read `sdLiveProbeState` before assuming the current mount is bad.

Useful recorder stages include:

- 40 — storage guard / not ready fallback;
- 41 — logical path failure;
- 42 — stream/buffering setup failure;
- 43 — microphone/I2S start/read failure;
- 47 — `fclose()` failure;
- 48 — zero PCM captured;
- 49–54 — file-creation errno classes;
- 66–70 — 4 KiB batch-write errno classes.

Stages from older validation builds may still appear in persisted diagnostics on upgraded devices. Interpret them together with the installed build and live probe state.

## 8. Media-v1 BLE sync

Build 1838 exposes the phase-one C3 media-v1 request/response transport.

Enabled:

- catalogue,
- explicit-path chunk read,
- per-file delete,
- Clear SD for Synap-owned recordings,
- explicit recovery.

Intentionally not advertised in this phase:

- notification-window/high-speed transfer,
- direct Wi-Fi transfer,
- destructive full-card format.

The transfer layer virtualizes the WAV header on reads so the PWA receives a conventional playable WAV even though the on-card recorder remains append-only.

## 9. PWA sync and retention contract

The companion PWA owns import durability and user retention choice.

Current contract:

1. discover the SD file;
2. download it;
3. import it into Memories;
4. verify the durable imported copy;
5. persist a receipt identifying that SD source as already synced;
6. keep the SD original unless the user explicitly chooses deletion.

A retained file is shown as **Synced to Memories · Also on device SD**, not as pending sync.

The PWA must not import the same retained SD file again merely because it still appears in the catalogue.

The user may:

- delete an unsynced SD file, after an explicit destructive warning;
- keep a synced SD copy;
- delete a synced SD copy later using **Delete from SD**;
- choose deletion immediately after successful sync when prompted;
- use **Clear SD** as a separate explicit action for Synap-owned files.

Deleting an SD copy must never delete the corresponding Memory.

## 10. Ownership and concurrency rules

- C3 standard/no-card operation must retain normal BLE audio even if SD is absent or unhealthy.
- Offline SD recording owns microphone + storage for the duration of the take.
- Connected PWA audio uses the normal BLE audio path.
- OTA, sleep/power transition and conflicting storage work must not race an active local take.
- A BLE reconnect must not silently redirect an in-progress local recording.
- Recovery/remount is bounded and explicit; do not add autonomous remount loops during an open recording.
- No recovery path formats the card.

### C3 deep-sleep and SD durability contract (8 October 2026)

The C3 SD card may remain powered when the ESP32-C3 enters deep sleep or
restarts. A successful WAV `fwrite` is **not** enough to release the
sleep veto: the last padded 4 KiB write, `fclose`/FAT metadata commit,
recording diagnostics, and any bounded SD recovery must all finish.

| Trigger | C3 + SD behavior |
| --- | --- |
| Disconnected inactivity (>5 min) | Refuse sleep while local recording, Wi-Fi, SD settlement, OTA, or an unsafe SD bus exists |
| Critical battery | Request local recording STOP first; only consider sleep after finalization and the guard |
| Four-second physical touch hold | During local SD recording, request STOP and remember the pending sleep gesture; wait for the full 5-second post-write guard |
| Connected double-tap | Stop BLE capture and enter remote standby, not SD deep sleep |
| App/OTA restart | Reuse the same guarded SD shutdown; refuse restart if SD is busy or quiesce fails |
| Sleep-locked wake gate | Require the same SD shutdown check before re-entering sleep after an invalid wake/reset |
| No SD installed | Preserve standard C3 sleep behavior; no-card mount does not act as an unresolved write fault |

A normal C3 + SD offline recording keeps `odysseyRecording=true` from
task launch through `fclose`, diagnostic persistence and bounded recovery.
Only after completion does the worker clear the recording flag, set
`odysseySdSleepGuardUntil=millis()+5000`, and reset `disconnectedAt`
to prevent an immediate timeout on long disconnected sessions.

All sleep/restart transitions must acquire the SD mutex and call
`odysseyPrepareSdForPowerTransition()`. A quiesce result other than
confirmed SPI idle (1) **must veto the transition**. The persistent-in-uptime
`odysseySdUnsafeToSleep` latch prevents the next idle-timeout attempt
from treating the now-unmounted card as safely absent. A validated
explicit SD remount clears the latch. An unrecovered SD write/close
failure also sets this latch.

The guard is intentionally conservative: if quiescence cannot be proven,
the ESP32-C3 stays awake rather than risking FAT metadata corruption.
Do not add a forced-timeout sleep or remove this latch to improve battery
life. Battery brownout or external power loss cannot be prevented by
software; that case still requires filesystem recovery on the next boot.

## 11. Production tests that protect this implementation

Relevant firmware contracts include:

- `tests/arduino-sd-patch.cjs`
- `tests/c3-sd-lifecycle.cjs`
- `tests/c3-sustained-write-diagnostic.cjs`
- `tests/odyssey-sd-detect.cjs`
- `tests/odyssey-sd-transfer.cjs`
- `tests/end-to-end-production.cjs`

The workflow also checks target ordering so the Arduino SD patch applies only to C3.

Relevant PWA contracts include:

- `tests/odyssey-sd-status.cjs`
- `tests/odyssey-sd-sync.cjs`
- `tests/c3-sd-transfer.cjs`
- Chakshu/media browser contracts that also exercise the shared Library SD UI.

## 12. Physical acceptance checklist

After any material C3 SD change, test on the actual Rev K SD-equipped device:

1. cold boot with card inserted;
2. connect once and confirm healthy live state;
3. disconnect;
4. double tap to start offline recording;
5. verify purple recording pulse;
6. record at least 30–60 seconds;
7. double tap to stop;
8. reconnect and confirm live state remains healthy;
9. catalogue the file;
10. sync it to Memories;
11. verify the retained SD row changes to **Synced**, not **Sync to Memories**;
12. choose Keep and confirm it does not import twice;
13. use **Delete from SD** and confirm the Memory remains;
14. separately verify Clear SD;
15. repeat across reboot/OTA boundaries while idle.

For release qualification, add long-duration recording, repeated start/stop cycles, low-battery operation, unexpected reset/power interruption, and failed-transfer retention.

## 13. Non-regression rules

Do not change any of the following without repeating physical SD acceptance:

- Arduino ESP32 core version;
- `tools/patch-arduino-sd.cjs`;
- ordering of the C3-only core patch in CI;
- 1 MHz runtime SD clock;
- CS-high-before-output mount sequence;
- one-open-file constraint;
- 4 KiB capture batch size;
- append-only STOP behavior;
- virtual WAV-header transfer;
- storage mutex / guarded ownership;
- protocol re-arm logic;
- sync receipt / explicit-delete semantics.

If a future change appears to make the implementation “cleaner” by returning to 512-byte writes, in-place WAV finalization, direct native-SDSPI mounting, automatic remount loops, or automatic source deletion after sync, treat that as an architecture change requiring hardware requalification—not as a refactor.
