# Odyssey C3 SD audio — production architecture and recovery reference

**Reviewed: 10 October 2026 (assembled C3+SD divider correction).**

**Current field-board profile:** Assembled Odyssey C3+SD has R1=1 MΩ
(SW_BAT to GPIO1), R2=470 kΩ (GPIO1 to GND); firmware reconstructs
**cellMv = round(adcMv × 1470/470)** after confirming SD mount. Standard
C3 without SD remains x2. The archived Rev K PCB BOM/NETLIST separately
specify 470k/470k: do not infer field resistor values from manufacturing
files or from ADC measurements. A mounted SD triggers a fresh 1470/470
sample before any FAT write probe. At 4.30V battery the ideal GPIO1 is
~1.375V. Measured GPIO1=1.34V versus firmware ADC=1.60V remains a
separate physical measurement discrepancy; firmware must mark ~5.0V
reconstructions untrusted and keep SD writes blocked until verified.
No arbitrary ADC gain, SD safety bypass or OTA partition change.


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

### C3-only CMD25 stop/busy completion fix (10 October 2026)

The pinned Arduino-ESP32 **3.3.5** SD/SPI driver previously used a 500 ms
busy wait before a multi-sector `CMD25` write STOP token, sent `0xFD`,
**then immediately raised chip-select without waiting for the card to finish
programming**. In its partial-write failure path, it attempted
`CMD12` (the read-multiple STOP command) instead of a multi-write STOP token.
Both are bad states for a continuously powered microSD device.

The C3 build-time core patch in `tools/patch-arduino-sd.cjs` now:

- Keeps the 4 KiB (8-sector) append-only PCM recorder, FAT filesystem,
  1 MHz SPI clock, and existing partition/OTA layout unchanged.
- Waits for card-ready, with a **bounded 5,000 ms timeout**, before each
  `CMD25` data block and before sending the multi-write `0xFD` stop token.
- Waits again **after** `0xFD` for final programming completion while CS is
  still asserted; only then deselects and checks `CMD13` status.
- On a rejected data token, attempts the proper multi-write stop once when
  the card becomes ready, then **returns a write error**. It does not
  continue with `CMD12` or blindly retry partially accepted blocks.
- Returns failure without issuing additional write/stop commands if the card
  stays busy through the deadline. No software patch can repair a card held
  LOW electrically or after a persistent voltage fault.

The change is fail-closed and limited to C3: the pinned source must match
exactly, and the patcher is idempotent. Unit and native-C++ state simulations
cover stop-token ordering, blocked programming, rejected tokens, and timeouts.
S3/Chakshu compile before the driver patch.

**Important:** Stage 70 / `wrE=9` on build 1908 does not prove the
`CMD25` stop timing is the sole root cause. Those are persisted diagnostics
from a failed `fwrite`; a power-rail issue, faulty SD module, or card may
produce the same symptom. OTA cannot revive an already hung, still-powered
SD card. Confirm a genuine SD 3V3 power cycle and sync existing recordings
before acceptance testing. Qualification still requires real-device long
offline recordings, repeated start/stop cycles, recovery, and BLE sync.

### Committed OTA and failed SD: distinct restart safety (10 October 2026)

Field evidence on build 1908: a correctly transferred build 1913 remained
`COMMITTED` (OTA state 5) and continued advertising build 1908. An existing
`odysseySdUnsafeToSleep` latch from a failed offline write prevented the
ordinary SD power-transition function from allowing reboot.

The committed-update handler is now **separate from deep sleep/restart**:
it acquires the shared SD mutex, refuses restart with an active recording,
and requires the existing idle/quiesce proof if SD is mounted. But if the
SD mount already failed and no application file is open, an old SD failure
flag alone cannot prevent an already-committed firmware from booting. No
partition table, WAV format, SD contents or sleep safety contract changes.
The firmware prints the running/selected partition and OTA image status on
boot and the partition selection result on commit.

C3 mount VFS diagnostics now identify the failing operation:
`vfsStep=7` means `fputc` failed and `vfsStep=12` means `fflush` failed
(after a successful `fputc`). Existing 8/9 remain close/unlink failures.
Neither operation is automatically retried or formatted.

**Bootstrap limitation:** this fix is not present in build 1908. If an older
firmware commits a newer OTA image but blocks its restart, power must
actually be cycled, and successful boot of the new image must be confirmed.
ESP-IDF rollback can still occur if the candidate image does not validate.
PWA shell198 recognizes OTA state 5 and stops offering a second transfer.

A device with invalid battery voltage telemetry, media I/O failures or
possible power instability still needs physical validation. Do not erase SD
recordings to repair the OTA flow.

### Rev K BOM versus assembled field C3+SD and SD write telemetry (10 October 2026)

**Hardware distinction:** Archived Rev K BOM specifies
**R1/R2 = 470kΩ / 470kΩ** (2:1) with R1 SW_BAT to BAT_ADC, R2 BAT_ADC
to GND, and C1=100nF; this is a manufacturing design, not an actual
measurement of the tested field C3+SD device. The latter is physically
assembled as **1MΩ / 470kΩ** (1470/470), with ~320kΩ Thevenin source
impedance. Verify physical GPIO1, switched battery, and SD 3V3 rail;
neither force-correct ADC samples nor bypass write safety.

**SD write failure localization:** the C3-only pinned Arduino core patch
now retains one compact, first-error 32-bit value `wrD` in the existing
SD catalogue error JSON. It persists alongside the legacy-compatible NVS
`sd-recdiag/last` and the additional `sd-recdiag/write` fields. Decode:
`op=(wrD>>24)&255`, `phase=(wrD>>16)&255`, `block=(wrD>>8)&255`,
`response=wrD&255`. A value of zero means that the error did not pass
the instrumented low-level SD write paths, or no trace was captured.

| Operation | Phase | Failure |
| --- | --- | --- |
| 24 | 1/2/3/4/5 | select/CMD24/data-token/post-write busy/CMD13 status |
| 25 | 1/2/3/4/5/6/7/8/9 | ACMD23/select/CMD25/per-sector busy/data-token/pre-stop busy/post-stop busy/CMD13/invalid count |

The trace is captured at `fwrite` failure **before `fclose`** so a
subsequent FAT metadata write cannot replace the original evidence.
The C3 core's inner `sdWriteBytes` busy timeout also now matches its
outer 5,000 ms bounded wait (previously it still used 500 ms). As before,
an unsuccessful write is not replayed automatically, SD recordings are
never auto-formatted or deleted, and the only physical source of an SD
controller power-cycle on the shipped Rev K carrier is a genuine rail-off
transition (power switch OFF and USB absent).

**Qualification required:** test an actual Rev K assembled board for
(1) battery voltage against multimeter on battery pads, (2) 3V3 sag at
SD1 VCC/GND at the first offline write, (3) 10 short offline captures,
(4) 5/15-minute takes, (5) read/verified sync before user-confirmed
deletion, (6) real power-switch and USB backfeed behavior. If `wrD`
indicates driver failure or MISO remains stuck LOW, adding OTA
retries cannot prove recovery.

### Power-loss-tolerant C3 WAV checkpoints (10 October 2026)

Odyssey C3 **SD-equipped firmware only** now checkpoints an open WAV every
10 seconds, **after** a complete successful 4 KiB PCM batch, while holding
the existing full-take SD mutex. It calls `fflush(FILE*)` followed by
`fsync(fileno(FILE*))`. On the pinned ESP-IDF FatFs VFS, `fsync` maps to
FatFs `f_sync()`, committing allocation and file-size/directory metadata
without closing/reopening or rewriting audio sectors.

The on-disk WAV still starts with the provisional 44-byte header and
appends only the original 4 KiB batches. After a sudden rail disconnect
or MCU reset, provided the FAT volume is readable and the file has a
committed length, **existing media-v1 catalogue/read** lists the WAV and
synthesizes a correct 44-byte RIFF header during transfer, in memory.
It does not format, truncate, rename, remove or edit the source file.
Recoverability is best-effort, not guaranteed if power disappears during
FAT metadata programming or the SD controller remains stuck LOW.

If a checkpoint fails, the recorder **stops**; it retains the original
file for later recovery and saves these stages in the existing failure
journal:
* `71` — C stdio `fflush` failed.
* `72` — FatFs-backed POSIX `fsync` failed.
* `73` — the VFS `fileno` was invalid.

The first filesystem errno and any low-level `wrD` trace are preserved
in the existing diagnostic response; no partial sector is replayed.
A normal stop still calls `fclose` for finalization. No OTA partition
changes, journal compatibility changes or PWA protocol changes.

**Limits and validation:** a checkpoint helps make the last successful
~10 seconds of data *durable at file-system level* but cannot provide
atomic FAT transactions or electrically complete a microSD write.
Hardware Rev K has no software-controlled SD rail. Before testing on
sealed units: sync recordings while accessible. Qualify on a spare card
by interrupting power after different checkpoints, remounting, verifying
WAV readability/content and checking that no existing recordings are
deleted. Test battery and USB supply separately because Rev K routes
the single-cell battery through an SS14 diode to the SuperMini 5V input.

### C3 low-voltage write admission and read-only SD rescue (10 October 2026)

**Evidence motivating mitigation:** production build 1922 revealed
`wrD=0x18040000` = `CMD24 / phase 4 / post-write busy timeout` after an
accepted data token; `wrE=5`, followed by unresponsive SD MISO. The
contemporaneous calibrated battery was ~3.49–3.51 V, 14–15%. This
supports, but does **not prove**, instability of the Rev K
LiPo → Schottky → SuperMini 5V/VIN → 3V3 power path. The SD supply has
not been measured directly. A card/controller defect remains possible.

This patch follows the safety pattern of read-only recovery and
conservative power-admission common in battery-operated data loggers:

* **Boot:** sample calibrated battery before SD initialization. If weak
  or untrustworthy, mount and validate the existing /synap directory
  **read-only** rather than creating, flushing and deleting the temporary
  FAT write-probe file. The VFS diagnostic `vfsStep=11` means “write
  probe deliberately skipped”; it is **not** a mount error.
  Existing recordings can still be listed and downloaded for recovery.
  If /synap does not exist, firmware refuses to create it on weak power.
* **Offline START:** require two fresh, valid battery samples at least
  **3900 mV** each, separated by 80 ms, before any card recovery or
  filename/file creation. This is a conservative, experimental limit,
  not a verified SD VCC power-good threshold.
* **While recording:** sample every ~5 s after a completed 4 KiB batch;
  two consecutive measurements below **3800 mV** or untrustworthy
  readings request ordinary STOP/close/checkpoint instead of another
  uncontrolled write. Existing ten-second `fflush+fsync` checkpoints,
  WAV header synthesis, append-only batches and SD mutex remain.
* **Mutating media requests:** user-requested SD deletion/clear return
  `BUSY` under the low-voltage guard rather than silently reporting
  success; catalogue/download operations remain allowed.
* **SD busy timeout:** after confirmed `CMD24/phase 4` or
  `CMD25/phase 4,6,7`, skip the *automatic* rearm/reset command
  sequence. Preserve the fault and SD sleep veto. An explicit manual
  SD recovery, or a genuine power-off with USB absent, remains available.
* **No format or reallocation:** data/metadata writes and electrical
  reset cannot be guaranteed safe if an already-powered card remains
  stuck busy. The patch does not erase files, enlarge OTA slots, or
  change S3/Chakshu behavior.

**Potential limitation:** while powered by USB, the battery voltage may
remain below the conservative threshold even if SD 3V3 is stable.
Because Rev K has neither a separate SD power-good sensor nor reliable
USB-present rail sensing, the firmware intentionally remains cautious.
This can block new offline writes/deletes until the battery is charged.
It does **not** prove the supply is stable at 3900 mV; qualify against
actual SD pin 3V3 under load and adjust only with measured headroom.
A low-voltage read-only mount is still subject to FatFs mount/read errors.

**Acceptance checks:** bench-test a spare Rev K board, known-good card,
battery states 3.5/3.8/3.9/4.15 V, both with and without charger input.
At low battery, catalogue and SD download must remain usable without
creating a write probe or allowing deletion. At high battery, short
and 15-minute offline recording must stop correctly and sync after
cold restart. Capture SD1 VCC and CMD24 timing on an oscilloscope.

### Hardware-unresponsive SD: touch-recovery veto (10 October 2026)

Firmware build 1935 already contains Rev K battery-admission safeguards:
calibrated battery sampling before writable mount checks, two fresh >=3900 mV
samples before offline recording, continued checks below 3800 mV,
and read-first SD catalogue/sync access when the writable probe is skipped.
The current patch adds **one further C3-only guard**: if the card is
unmounted and the bit-bang probe confirms deselected MISO remains LOW
(`odysseySdBusStuckLow()`, at least 900 of 1024 bytes zero), a hardware
double-tap does not attempt another `odysseyRecoverSdCard("touch")`.
Instead it records a visible fault, stops the task, releases recording
state, and waits for an actual card-power recovery. This prevents
repeating CMD0/CMD12/SPI reset against an electrically nonresponsive,
still-powered card. A normal available card and a card with *non*
stuck-low mount errors retain the previous one-gesture recovery path.

No change to recording bytes, WAV transfer, 10s `f_sync`, PWA media
protocol, SD deletion policy, OTA partitions, S3/Chakshu or native
battery thresholds. This **does not** repair a card held LOW, and must
not be described as a substitute for regulator headroom or switchable
SD power. Prefer read/sync of intact recordings after a true power-off
(USB disconnected) and confirmation of a healthy mount.

### 10 October 2026: build 1937 SD read-rescue findings
On installed firmware **1937** the PWA reported `wrD=0x18040000` (a
historical accepted CMD24 write followed by busy timeout), `lastRecordKiB=2040`,
and `sdState=2`. Later it also reported `vfsStep=2,vfsErrno=2`, meaning
the SD driver reached FAT/VFS but `/synap` was not present or could not
be created. Low-level `CMD0=1,CMD8=1,R7=426` responses indicate the
card answered SPI commands in that session; they do not prove intact FAT
metadata or readable WAV files. The reported `mountAttempts=44` came
from repeated attempted recovery; do not continue automatic mount cycling.

The guarded fix adds `vfsStep=13,errno=ENOENT` when `/synap` is
missing on a card with a **persisted prior WAV byte count**. In that
case firmware intentionally **does not create the directory**, format
the card, rewrite FAT, or claim that the old recordings were deleted.
It prints up to eight root directory entries over serial for diagnostics.
A genuinely fresh, never-recorded SD can still create its directory
only when power measurements pass the existing safety gate.

Automatic SD remount before idle sleep is now limited to a single
attempt per boot and is suppressed entirely for an untrusted/weak ADC.
Explicit user-initiated recovery is unchanged. This does **not** rebuild
a missing FAT directory or recover orphaned data; such recovery
requires a read-only sector-level image or validated offline repair on
a spare card. Never automatically format or clear a card in this state.

### Build 1941 regression fix: a formatted SD has no /synap (10 October 2026)

The C3 host mounted the FAT card and verified its root but refused to publish
SD readiness when `/synap` was absent and persisted NVS history reported an
older WAV write failure (build 1941 `vfsStep=13`). The retained 2040 KiB was
not evidence about the identity/content of the **currently inserted** SD:
the user had physically formatted the card. Independently, an untrusted
battery measurement blocked automatic `mkdir`. Neither condition means the
mounted FAT volume is unreadable.

Corrected behavior: after validating the FAT root, `/synap` absent
(ENOENT) with historical recorder bytes or untrusted battery returns a
readable, successfully mounted **root-only mode** (diagnostic
`vfsStep=13`); catalogue returns `[]` after rechecking the FAT root.
Other directory I/O errors continue to fail. No background, catalogue,
or boot-time FAT metadata write occurs in this mode.

A disconnected explicit double-tap can initialize `/synap` under the
recording mutex after two fresh safe battery samples, then create the
append-only WAV. With the currently anomalous ADC (reported 2.2–2.7 V
at GPIO1 instead of multimeter ~1.34 V), write admission remains blocked.
Do not bypass the 3900/3800 mV gates to claim recording fixed. No SD
format, no NVS reset, no partition change, no changes to S3/Chakshu.

### 1943 field ADC saturation investigation (10 October 2026)

After formatted SD fix, `sdDetectionState=1` and `sdLiveProbeState=6`.
Offline double-tap fails battery preflight: GPIO1 raw=4095, ADC=2949mV,
field ratio gives cellMv=9223, batteryAvailable=false. These are not a
physical 1S cell voltage; user previously measured ~1.34V on BAT_ADC.

For a 1M/470k divider, Rth≈320kOhm and C1=100nF gives tau≈32ms.
An accidentally enabled weak GPIO1 pull-up could saturate ADC readings.
In an isolated **hypothesis test**, clear both internal pulls via
`gpio_set_pull_mode(GPIO_FLOATING)` after Arduino attaches the ADC,
then wait 175ms (over five RC constants) on the C3 SD profile.
Reject near-full-scale raw>=4090 even if calibration changes.
No guess-based voltage adjustment, ratio change, lower write gate,
automatic formatting, or S3/Chakshu behavior change.

This is not proof of the exact hardware fault. If saturated readings
persist, measure the physical pin during operation, verify population
and power/ground, and inspect for unintended electrical bias.

BLE disconnected while app visible after ~66s; next diagnostics reported
POWERON reset. That is separate from ADC recovery and should be
investigated for physical switch/supply resets and peripheral instability.

### Explicit offline SD writing diagnostic: post-1948 (10 October 2026)

For this field C3+SD board, 1948 reports a successfully mounted FAT volume
(`sdDetectionState=1`, `sdLiveProbeState=6`) but an explicit disconnected
double-tap aborts before attempting `fopen` or `fwrite`. The C3-local
recorder now executes the actual write path for **only an explicit
disconnected double-tap**. This isolates whether card + SPI + FAT + microphone
can save user audio, independently of battery percentage/ADC telemetry.

After mount proof and mutex ownership, a fresh FAT card gets `/synap`
initialized, the take appends 4KiB WAV batches, performs 10s `fsync`
checkpoints, closes on STOP, and reads back the closed WAV header and size.
A readback/stat failure is a persisted stage 74/75; full success logs
`[SD] WAV VERIFIED` and clears the prior NVS fault. The PWA retains
its existing catalogue, chunked BLE reads, sync, and deletion safeguards.
Other tasks and boot must not create files or clear media automatically.
CMD24/25 busy errors continue to suppress unsafe immediate recovery and
deep sleep, and recording owns the SD mutex.

**Power/data-integrity limitation:** a valid FAT mount is not a measurement
of the SD regulator's 3.3V rail. This is an explicit-user experimental
recording path, not automatic background writes. Confirm the device has
a stable external power source, start with a short test, and verify the WAV
on the PWA before relying on long recordings. Voltage-based recorder gating
is intentionally not exercised in this diagnostic branch. The normal
PWA battery telemetry and unrelated firmware power features are unchanged.

### C3 SD write path fault localization after build 1951 (10 October 2026)

Field observation on build 1951: `sdDetectionState=1, sdLiveProbeState=6,
sdProbeState=49, lastRecordKiB=0`. Stage 49 maps to an `EIO` during
directory setup *or* WAV creation (both reused the same mapping).
This only proves that FAT was readable enough to mount; boot avoids write
probes with untrusted ADC, so it **does not** validate CMD24 write capability.

A deliberate offline double tap now records *the first* Arduino SD driver's
`CMD24/CMD25` failure from `mkdir`, `fopen` and close. New persisted
stage 76 = `mkdir('/synap')` failure; 77 = directory stat failure;
78 = path not a directory. Stage 49 remains WAV `fopen` with `EIO`.
The first fault code is `(command<<24)|(phase<<16)|(block<<8)|token`.
`wrD` decodes command 24/25 and phase 1–9 as defined in
`tools/patch-arduino-sd.cjs`; zero means the SD driver did not log a
CMD24/25 failure. `wrE` stores the POSIX error (5 for EIO).

BLE media operation 27 is a *read-only* report that returns
`recordStage,recordBytes,wrE,wrD,wrN,wrX,wrF,sdState,sdLiveProbe,
vfsStep,vfsErrno`. The PWA may inspect it after a failed recording.
It never triggers remount, file creation, formatting or media reset.
The report supports comparing SD command phase against the exact pinned
Arduino ESP32 3.3.5 driver to select a verified remedy; do not
speculatively increase retries or change FAT content on an unproven bus.

### Build 1955 driver review: delayed programming-busy handshake

The published C3 build 1955 maps to source
`df36623a16ef77621f1a6e3ded4fb9374dc77519`. Its five-second CMD24/CMD25
waits still use Arduino's first-nonzero-byte readiness check. Immediately
after the data-response token, the card can return an idle byte before
asserting programming busy. Without consuming that interval, the wait can
return early: CMD24 releases CS, or CMD25 sends the next data/STOP token,
before observing programming completion. The same gap exists after the
multi-block STOP token.

The C3-only core patch now clocks one idle byte after the data response
and after `0xFD`, before the existing bounded busy waits. ESP-IDF v5.5's
`components/esp_driver_sdspi/src/sdspi_host.c` provides the reference:
`poll_busy` waits for two nonzero observations after block data, and the
STOP transaction explicitly sends `{0xFD, 0xFF}` before polling.
Source: https://github.com/espressif/esp-idf/blob/v5.5/components/esp_driver_sdspi/src/sdspi_host.c

The executable mock-SPI regression runs the generated CMD24 and CMD25
driver against immediate busy and delayed `0xFF,0x00,...,0xFF` busy,
including all eight blocks of a 4 KiB write, rejected data responses,
permanent data busy and permanent STOP busy. It asserts that no next token,
status command or successful deselection occurs while programming remains
pending. Timeout and first-fault diagnostics remain intact. Applying the
patch to the pinned upstream 3.3.5 source is idempotent.

The supplied build-1955 field log verifies installation at 17:35:42 IST.
After the offline attempt, 17:37:31 reports stage 76 (mkdir failure), zero
PCM bytes and live mount stage 2. At 17:37:33, `wrE=5` and
`wrD=402915328=0x18040000` identify CMD24 phase 4: a sector was accepted,
but programming busy did not clear within five seconds. `raw0=1023`,
`rawFF=0`, `bbHigh=0` show the subsequently held-low bus. This is distinct
from the stage 49 history carried over immediately after OTA. The log
also reports a POWERON reset; its physical cause is not established.

This fixes a reproducible driver handshake defect in build 1955, but the
field phase-4 timeout is NOT itself evidence of premature ready detection.
The patch cannot promise recovery of an already held-low card, nor prove
the card or its supply healthy. Hardware validation must
check recording start, sustained writes, STOP, catalogue/readback and a
second recording, retaining operation-27 diagnostics on any failure.
The change does not format media or alter the filesystem, pinout, recorder
batch size, clock, power policy or OTA partition layout.

### Build 1957 follow-up: metadata through one-block CMD25

The 18:10 IST field test still fails before capture: stage 76, errno 5,
`wrD=0x18040000` (accepted CMD24 sector, five-second programming timeout).
The boot probe succeeded with `/synap` absent (`vfsStep=13`, ENOENT).
Its idle-high raw samples are cached boot/recovery observations, not a
post-failure bus test. The earlier response-latency fix did not resolve
the device's write failure.

For SD/SDSC/SDHC cards the C3 core patch now routes single-sector writes
through CMD25 with count=1, the same bounded implementation used for audio.
This applies to mkdir, file creation, FAT updates and close, without
changing the logical sector or its 512 data bytes. CMD25 transfers blocks
until the SPI Stop Tran token; a one-block transaction requires no padding
or neighbouring-sector rewrite (SD Physical Layer Simplified Specification,
section 7.2.4, SPI Data Write). MMC retains the previous CMD24 path.

The transaction sends one data block, waits for programming, sends 0xFD,
waits for STOP completion, then checks CMD13. A failure returns immediately
without CMD24 fallback or replay of uncertain metadata. Existing bounded
busy-failure recovery/sleep vetoes cover CMD25 phases 4, 6 and 7.
New single-sector timeouts report `0x19060100` (data programming) or
`0x19070100` (STOP programming), rather than CMD24 phase 4.

Executable tests verify one-sector payload equality, SDSC byte vs SDHC
block addressing, command rejection, data rejection, data/STOP timeout,
status errors, zero-count rejection, MMC compatibility, and eight-sector
audio writes. This is a protocol compatibility remedy for the observed
CMD24 failure, not proof that the card or power rail is healthy. Real-device
mkdir, recording, STOP and transfer verification remain necessary.

### Stage-72 investigation: two-sample CMD25 busy polling (candidate)

The October 10 offline take progressed for roughly two minutes, then stage 72
identified an `fsync` checkpoint failure (`errno=EIO`,
`wrD=0x19070100`: CMD25 one-sector STOP programming timeout). The later
recovery snapshot reported MISO low even while CS was high, 1023 raw-zero
samples, and unavailable storage. These diagnostics do not prove whether the
SD card firmware, supply integrity, SPI timing, or the host's STOP timing was
responsible; the recorded `lastRecordKiB=2040` is a *saturated* progress
counter, not the full byte count. Use read-only operation 27 for exact
`recordBytes` following PWA PR #183.

Arduino-ESP32 3.3.5 `sdWait()` accepts one nonzero byte. The ESP-IDF v5.5
SDSPI host instead requires two nonzero busy-poll observations before it
proceeds. Candidate firmware PR #180 adds this two-sample readiness poll only
inside the C3 CMD25 write path, before blocks, before STOP, and after STOP,
without lengthening the five-second deadline, replaying uncertain writes,
altering MMC/CMD24, the 4 KiB recording batch, SPI pinout, power policy, or
OTA partitions. Native tests inject two transient 0xFF samples before the
program-busy period. A card that remains busy returns an explicit fault and
must not be hammered with retries. This is a protocol hardening hypothesis,
not field-verified recovery; keep PR #180 separate from production until
recovery/readback and physical long-duration recording tests pass.


### October 10 field reliability follow-up: one early FAT checkpoint

The first field recording after release 1959 advanced past mkdir/file opening
and wrote microphone PCM with a purple recording indication, then turned red
after approximately two minutes. A following BLE catalogue returned failure,
`sdProbeState=72`, `wrE=5`, and `wrD=0x19070100`: the last failed operation
was `fsync` writing a single FAT metadata sector via CMD25, and STOP never
completed within the five-second busy budget. The card then appeared held-low
on MISO even with CS HIGH. This is not a microphone fault.

The 10-second `f_sync` loop, introduced after the historically successful
append-only STOP path, repeatedly forces the same high-risk FAT metadata write.
Firmware PR #180 now performs **one checkpoint** after the first complete 4KiB
audio batch at or after 10 seconds, making a prefix of the take recoverable.
After that, all audio remains sequential CMD25 writes and further directory
metadata is deferred until normal `fclose` on STOP. Abrupt power loss may lose
the appended data after the first checkpoint, even if PCM sectors were accepted.
The existing PWA read-only virtual WAV header and SD source retention continue.

PR #180 also requires two nonzero ready samples for CMD25 phase 4/6/7 while
CS remains asserted, following ESP-IDF's SDSPI polling strategy. The five-second
timeout and first-fault codes remain; uncertain sectors are never replayed.
Neither change can release a card/controller physically held BUSY or fix a
defective SD supply; physical validation of boot, 10-minute capture, STOP,
catalogue/download and second take remains required.


### October 10 C3-specific recovery experiment after build 1968 (PR #181)

The sealed Odyssey C3 field device confirmed OTA build 1968. New, *fresh*
offline write failures were observed in the Arduino SD SPI CMD25 path:

- `wrD=0x190800FF` — CMD25 phase 8, CMD13/status response 0xFF after
  transferring ~1,056,724 bytes of PCM (~33 seconds mono PCM16).
- `wrD=0x19070800` — CMD25 phase 7, eight accepted sectors then STOP/busy
  timeout, after 16,340 PCM bytes (~0.51 seconds). On a later cold boot the SD
  successfully initialized and mounted (`sdLiveProbeState=6`) before another
  recording failed. Captured `bbCmd0=1 bbCmd8=1 bbR7=0x1AA` values may reflect
  boot-time recovery, **not** the post-failure card state.

**C3-only remediation:** avoid CMD25 altogether: convert FatFs multi-sector
writes to consecutive verified CMD24 writes, each 512 bytes, with card-selected
busy completion and CMD13 before starting the next sector. 4 KiB microphone
batches still append to the same file; no file format, SPI wiring, recorder
checkpoint frequency or OTA partition is changed. 16 consecutive 0xFF ready
bytes are required after each accepted block before releasing CS, to reject
short high gaps before a later 0x00 busy period. One-sector metadata writes
also use CMD24. On any unknown or failed sector status, return EIO immediately;
**never replay an uncertain SD write.** Driver first-fault codes now report
CMD24 (`0x18` high byte) with phases 3=data token rejected, 4=program busy
timeout, 5=CMD13 R1, 6=CMD13 R2. Native tests exercise 8-sector batches and
partial failures.

The independent BLE disconnect at 21:47:18 was followed by
`ESP_RST_POWERON` and uptime=8s on reconnect, consistent with a real reset.
This does not prove a voltage sag, but argues for testing the switched battery
path, EN pin and SD 3.3V regulator **in addition** to software fixes. The
current battery-derived `cellMv` is marked unavailable, so neither a safe
deletion voltage nor real supply voltage is established. The C3 deletion guard
deliberately returns BUSY when the battery reading is untrusted. Do not bypass
it or delete original recordings until physical supply is verified.

PR #181 is an **untested-on-device** write-path change. Publishing a build
confirms compilation/tests only. Before declaring the issue resolved,
safely back up recoverable files, confirm SD mount, exercise short and
10-minute disconnected double-tap recording, STOP/readback and a second take
on a known-stable supply. Do not assume success from purple LEDs alone.
