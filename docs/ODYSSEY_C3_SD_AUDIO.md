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

Current optional C3 media-v1 transport (9 October 2026):

- **Feature bit 0:** fast BLE notification windows on characteristic `4fa1235a-0000-1000-8000-00805f9b34fb`. A request (op 12) names the WAV and offset. The worker sends up to six paced 480-byte chunks with explicit offsets, then an end-of-window marker. The browser accepts only contiguous chunks, and can resume from the first missing byte. Small MTU connections return an empty completion and fall back to the original op 4 request/response path; cancellation op 16 remains supported.
- **Feature bit 1:** direct Wi-Fi upload via BLE control, only advertised if the optional Wi-Fi component is available. Wi-Fi is a separate user-selected transfer route, not an automatic replacement for BLE sync.
- **Feature bit 2:** full-card format remains disabled.

This changes only *transfer*, not the validated 4 KiB offline recorder, appended WAV bytes, SD power guards, OTA partition layout, or delete-after-verification policy. Notification delivery is not treated as proof of durability; the PWA verifies the complete transfer before importing Memories. Physical iPhone/Bluefy throughput and integrity tests are still required before making performance claims.

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

## C3 + SD BLE audio: high-MTU congestion protection (9 October 2026)

Firmware build 1894 and Bluefy shell195 revealed live audio after successful
PCM16 START could exhaust the NimBLE transmit mbuf pool:
`notifyStatus=4` (ERROR_GATT), `notifyError=6` (ENOMEM),
`notifyRejects=173`, `captureDrops=28`, plus a 12-second foreground stall.
With `MTU=517`, PCM16 16 kHz mono requires 1600 bytes every 50 ms
(four 400-byte notifications, 80/sec, 32 KB/sec). MTU capacity by itself is
**not** proof that the controller or a given iOS BLE browser can sustain
that amount of notification traffic.

The C3 live BLE profile now uses the already-supported independent-frame
IMA-ADPCM v3 format: 404 bytes every 50 ms (one notification at MTU=517,
20/sec, approximately 8.08 KB/sec). The Synap PWA's
`audio-codec-v3.js` decompresses every complete ADPCM frame to the
original 16 kHz / 800-sample PCM *shape* for recording and transcription.
ADPCM is **lossy**: BLE live audio is lower bitrate, not bit-for-bit
identical to raw microphone PCM. This tradeoff applies to C3 live BLE only.
The **offline SD recorder continues to save uncompressed PCM16** in its
append-only WAV capture path, with identical 1 MHz SPI and 4 KiB writes.

S3 and Chakshu retain the existing high-MTU PCM-first protocol. C3's
transport selection is fixed at START/RESUME and included in the status
characteristic; do not change encoding mid-frame or mid-session. When even
C3 ADPCM repeatedly encounters transmit allocation errors for four
consecutive frames, firmware emits a stream error and stops capture rather
than recording indefinitely with zero delivered audio. A new recording
resets that congestion counter.

Battery telemetry error `cellMv=5561` with `adcMv=1778` is **separate**
from NimBLE exhaustion: 1 MΩ/470 kΩ reconstruction correctly produces that
implausible value, and firmware must continue marking it unavailable.
Physical BAT+/GND and GPIO1/GND measurements are required for voltage
calibration, especially during charging. Do not force the percentage to
100 or lower the actual cell voltage without a reference instrument.

### C3 BLE connection stability and reconnect policy (October 2026)

After the 1897 ADPCM update, the remaining connection symptoms are distinct
from media congestion: Bluefy sometimes takes multiple attempts to discover
GATT services, an established link may later disconnect, and iOS may suspend
or disconnect the native BLE link when the PWA is backgrounded. The latter
is a Bluefy/iOS lifecycle constraint; firmware cannot force an iOS browser
to keep an active connection in the background.

For ESP32-C3 targets only, `applyCpuPowerProfile()` now holds the active
160 MHz CPU clock while BLE is connected, during SD transfers, and for a
bounded 12-second recovery window after a link loss. Previously C3 switched
to 80 MHz as soon as audio/OTA activity stopped, even during connected idle
and GATT discovery. When disconnected and idle beyond the recovery window
it returns to 80 MHz, protecting battery life; active offline SD capture
and Wi-Fi keep their existing full-speed requirements. **Tradeoff:** the
connected-idle current is higher.

The pinned Arduino NimBLE C3 GAP connect callback no longer immediately
calls `updateConnParams()`. The iPhone is the BLE central and can negotiate
suitable parameters; the existing C3 ADPCM live transport sends only one
404-byte packet per 50 ms at MTU 517. S3's existing connection negotiation
is unchanged. Keep `advertiseOnDisconnect(true)` so the pinned library
restarts connectable advertising after an unintentional link drop.

Firmware `lastDisconnectReason=65535` still means that the pinned NimBLE
Arduino callback does not expose the GAP disconnection reason. PWA logs
must distinguish user/app-initiated disconnect from browser/peripheral loss.
Do not label generic Bluefy native code 2 as a definite peripheral bug;
permission-wrapper failures may need manual reselection.

Hardware validation: leave the PWA foregrounded for 3 minutes connected
and idle, then record 2 minutes over ADPCM, deliberately disconnect and
reconnect, then repeat after deep sleep. Log `firmwareBuild`, `GATT
disconnected` (including `visibility` and `origin`), `Connection
setup` stages, `Audio delivery stalled`, `notifyRejects`, and
`Battery`. Check offline SD recording separately. These software changes
cannot rule out cell-voltage instability or iOS radio-layer limitations.

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

**Sleep reliability revision (9 October 2026):** The SD shutdown helper
first looks for 64 consecutive idle bytes under asserted chip-select and
confirms the card responds with clean CMD13/R2 status. A healthy SD card
therefore does not receive an unnecessary CMD12 read-stop or CMD25 write-stop
token while idle. If CMD13 cannot confirm idle, the existing bounded
CMD12/CMD25 recovery sequence remains in place. The host is still guarded
by the SD mutex and no offline WAV may be open.

After a failed quiesce, the C3 retains `odysseySdUnsafeToSleep`. When a
later explicit touch-hold or disconnected-timeout/critical-battery request
calls `enterDeepSleep()`, firmware attempts one non-destructive validated
SD recovery at most once per **30 seconds**; only a successful remount can
clear that latch. Failure leaves the C3 awake. Failed VFS *reads*, as well
as failed writes, now also latch unsafe-to-sleep because an unfinished
CMD18 session is possible.

A long-hold while offline recording or Wi-Fi is active remains pending
until capture/upload finishes. After the five-second post-WAV settle, a
failed sleep transition is retried every five seconds rather than dropping
the original user request; a fresh touch cancels the request. A BLE
`POWER_STATE_DEEP_SLEEP` event is published **only after** SD has reached
confirmed safe idle, so a refused sleep does not falsely tell the PWA
that the pendant powered off. The existing OTA partition, recorder batching,
and WAV close sequence do not change.

Do not add a forced-timeout sleep or remove the SD safety latch merely
to improve battery life. Battery brownout or external power loss cannot
be prevented by software; that case still requires filesystem recovery
on the next boot.

### Failed-SD sleep veto across boot (9 October 2026)

At normal C3 boot, a persisted recorder failure stage >=44 (except stage 48,
empty audio) restores the in-memory unsafe-to-sleep latch before any SD
recovery or mount attempt. A successful card mount and VFS validation clears
the latch. A failed mount leaves it set. The old recorder diagnostic may
remain visible after recovery; current live status remains authoritative.

If initialization fails and MISO is driven LOW even while CS is HIGH, with
at least 900 of 1024 sampled bytes equal to zero, the bus is treated as
unsafe rather than safely absent. This vetoes deep sleep until validated
recovery, even without a previous recorder failure. A standard no-SD C3
with pulled-up, undriven MISO keeps its normal no-card sleep behavior.

Observed incident: historical stage 70 at approximately 2040 KiB with
raw0=1023, bbHigh=0 and live probe 2. Removing, formatting and replacing
the card later produced live probe 6 and ready=381. Do not confuse the
historical stage-70 record with a current write failure.

### C3 + SD battery ADC reading and calibration

The assembled C3+SD test board uses a **1 MΩ high-side R1** from switched
battery positive to **GPIO1 (BAT_ADC)** and a **470 kΩ low-side R2** to
GND. Its **100 nF C1** capacitor is across GPIO1 and GND. The canonical Rev K
repository BOM still lists both R1 and R2 as 470 kΩ, so verify the actual
assembled resistor values rather than inferring them from the BOM.

The divider reconstruction is exactly
`cellMv = round(adcMv * 1470 / 470)`. The *standard C3 without SD* is
1 MΩ / 1 MΩ (2:1), while S3 uses its independent calibrated conversion.
The C3 ADC stays on 11 dB attenuation with factory calibration from
`analogReadMilliVolts()`; do **not** apply an arbitrary scale factor to
force a reported 4.457 V down to 4.200 V.

Boot now identifies SD/divider hardware **before** the first C3 battery
reading is published. The C3 + SD reading takes 16 calibrated readings,
discards two extremes at each end, and averages the middle 12. A wide
central spread (>120 mV) invalidates the percent rather than displaying
a false battery voltage; raw ADC diagnostics are still sent. S3 and the
standard C3 retain their prior mean sampling.

Field readings on a nominally fully charged cell:
- USB connected: `adcMv=1607`, reconstructed `cellMv=5026`
- USB disconnected: `adcMv=1425`, reconstructed `cellMv=4457`
- Expected at a verified 4.200 V cell: GPIO1 ≈1343 mV

A consistent 82 mV discrepancy at the ADC pin cannot be safely corrected
from software or from the words "fully charged" alone. Before a
device-specific gain/offset can be fitted, measure **BAT+ to GND** and
**GPIO1 to GND** with a high-impedance multimeter on an accessible
prototype. Check with USB disconnected first. Stop using a charger if
the actual LiPo voltage exceeds its rated full-charge value.

The 1 MΩ/470 kΩ divider has ≈320 kΩ Thevenin source impedance and
≈32 ms RC time constant with C1. Retain C1 and the factory-calibrated ADC
function; do not change voltage cutoffs or bypass invalid readings.

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

### Power-on / BLE / SD / deep-sleep lifecycle contract (9 October 2026)

This applies **only** to Odyssey ESP32-C3 with SD; do not reuse the C3 pin
mapping, recording behavior or touch wake semantics on S3/Chakshu.

| Transition | Required behavior |
| --- | --- |
| External power switch OFF -> ON (ESP_RST_POWERON) | Cold physical power-on overrides a previous NVS sleep lock; initializes SD once before BLE and then starts advertising. |
| Normal deep-sleep GPIO touch wake | Hold the TTP223 touch for 4 seconds to wake intentionally. A short tap remains asleep. |
| Retained sleep lock + non-touch MCU reset | Return to sleep when safe, but a persisted failed SD write instead boots awake for validated recovery. If arming deep sleep fails, recover into BLE advertising rather than strand setup before BLE init. |
| Boot SD bus held LOW even while CS HIGH | A definitive all-zero probe ends futile repeated boot mounts, preserves the unsafe-sleep veto and brings up BLE for explicit card recovery; it does not claim SD is healthy. |
| Connected BLE SD transfer | Transfer worker holds an in-flight lock and a 15-second post-request grace. Long-touch sleep is deferred until the transfer finishes and the grace has elapsed. |
| Accidental BLE disconnect | Restart advertising; normal disconnected auto-sleep remains 5 minutes, not immediate. |
| Deep sleep | Do not announce sleep or stop radio until after SD has been quiesced and the touch wake source is armed. |
| Brownout | Physical supply drop can reset BLE and leave the continuously-powered SD card stuck. Software safeguards cannot guarantee transfer survival or recover an electrically held-low MISO bus. |

**Battery/power validation remains mandatory.** The 1 MOhm/470 kOhm
battery ADC circuit has reported implausible ~0.3-6.3 V reconstructed values.
Do not calibrate by changing the resistor ratio to hide unstable readings.
Validate LiPo terminal voltage, GPIO1 divider midpoint, common ground,
regulator/SD 3.3 V rail under load, and the ADC input capacitor with a
multimeter or oscilloscope. Do not disable the hardware brownout detector.

**Acceptance tests:** Cold-switch power-on with an old sleep lock; SD mounted
and missing; 4-second touch wake from deliberate deep sleep; 3-minute offline
WAV finalized before sleep; at least two SD-to-PWA transfers without touching
power, including one forced BLE disconnect and reconnect; no SD deletion until
journal verification and receipt completion; and brownout/fault simulation.
Check boot reset reason, SD live probe, BLE last-disconnect cause, mount stage,
and source checksum for each transition. Build and hardware tests are required
before considering the lifecycle production-validated.

### C3 held-LOW MISO / stage-70 field diagnosis (9 October 2026)

A connected field unit on build 1901 reported a successful ~2,040 KiB offline PCM
write followed by `lastRecordStage=70`, `sdDetectionState=2`, and
`sdLiveProbeState=2`. The catalogue response then had
`bbHigh=0, raw0=1023/1024, rawFF=0, bbDrain=8192, bbStop=3`, indicating a
strongly held-LOW MISO/DO bus despite deselecting the card. This **does not**
by itself distinguish SD-module power integrity, wiring, a card controller
stuck busy, or a broken/unrecoverable card. The contemporaneous C3 battery ADC
is also invalid and must not be used as proof of a safe supply.

The guarded follow-up keeps the proven Arduino SD mount, append-only 4 KiB
recorder, and 1.25 MiB OTA partition unchanged. It adds:

- A **rollback-compatible NVS recording-failure journal** retaining the
  original three-word `sd-recdiag/last` value so older OTA builds still see
  unsafe SD failures. A separate stage/bytes-matched `sd-recdiag/write`
  record captures `errno`, returned `fwrite` bytes, requested 4096 bytes,
  and `ferror(FILE*)`. Success clears the failure marker; new firmware also
  reads the interim seven-word format if encountered.
- Compact SD catalogue failure diagnostics `wrE`, `wrN`, `wrX`, `wrF`
  carrying those four persisted values, alongside the pre-existing bus probes.
  A value `wrE=0` is **unknown/unset errno**, not proof of a successful write.
- If a **failed** SD mount has measured CS-high MISO LOW and ≥900 zero bytes in
  the 1024-byte raw bus sample, disconnected idle-timeout automatic sleep
  recovery **stops repeatedly remounting** the same non-power-cycled SD card.
  SD unsafe-to-sleep protection stays asserted. Explicit BLE SD retry (op 14),
  disconnected double-tap, and a *genuine SD power cycle* remain possible.

This is a **diagnostic and recovery-loop containment fix**, not a validated SD
repair. Do not format or delete a card with untransferred recordings. Because
SD is wired to the always-on C3 3V3 rail on Rev K, ESP deep sleep/reset does
not reliably power-cycle the card. Confirm real rail removal and measure 3.3 V
under sustained SD-write load if stage 70 recurs.

Before declaring stable on the sealed device: test 10 offline start/stop cycles,
a ≥5-minute offline take, unexpected BLE disconnect during catalogue/read,
the real power switch's effect on SD 3V3, and retained WAV sync after recovery.
