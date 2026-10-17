# Odyssey C3 + SD: unresolved reliability issues and next validation

**As of:** 2026-10-17 (IST). **Hardware:** ESP32-C3 SuperMini 4 MiB, new C3+SD enhanced dual-OTA image, native ESP-IDF SDSPI + FatFs, 3.3 V microSD module. **Transport:** BLE only, no Wi-Fi. **Last known on-device firmware:** build 2235. **Scope:** C3+SD, never the separate Chakshu S3.

> **Release classification:** the proposed 256 KiB first checkpoint is a *recoverability experiment*, NOT a fix for CMD25/SD controller timeout. Software CI, successful OTA, and a positive FAT mount are not evidence of stable extended offline recording. Do not declare resolved without the real-hardware acceptance gate.

## Evidence and root-cause boundaries

### Follow-up implementation status

The 256 KiB checkpoint is already present on firmware main at `2241dc2`.
The follow-up correction makes a zero-PCM take a failure before the saved/failed
log is emitted. No write retry, timeout extension, unsafe close, automatic
deletion, Wi-Fi, pin change or partition change is introduced.

Companion PWA follow-up: shell227 reads op34 before explicit recovery, refuses
op14 when either quarantine bit is set or offline finalization is active, and
scopes cached health to the exact BLE connection. Invalid/unavailable health
cannot authorize a C3 remount. Discovery also reads health without recovering.
Settings disable recovery/capacity/raw probes/format when quarantine is known;
RAM diagnostics remain available. Library deletion and post-import deletion
offers are disabled for C3. A zero-length entry is described as lacking a
committed audio length, not proof that no PCM sectors were written.

| Issue | Closure status |
| --- | --- |
| CMD25 timeout | OPEN: no demonstrated root-cause fix; card/board measurements required |
| Power-cycle versus deletion | OPEN: controlled test below required |
| Metadata durability | IMPLEMENTED EXPERIMENT: early checkpoint already on main; interrupted-card validation outstanding |
| Finalization | Software fail-closed safeguards retained; empty-take success log corrected. Unsynced-tail recovery remains unproven |
| PWA maintenance UX | Follow-up implementation and regression tests prepared; real-phone validation outstanding |
| Endurance/back-to-back | OPEN: no physical results obtained in this software session |
| Bluefy background/throughput | OPEN: OS suspension cannot be prevented by a web app; existing bounded transfer and integrity protections retained |
| Power/mechanical | OPEN: SD-pad scope traces, contact/pull-up/module checks required |

For each field run retain: exact firmware build, PWA shell, card make/model and
capacity, power source, start/stop times, LED count, op34 before/after, complete
op27 first-fault report, imported byte count and integrity result, and whether
the next recording worked without reset. Use one variable change per trial.
Never label a test passed solely because a mount or BLE import succeeded.

For the 0 B discriminator, first export diagnostics; then use a disposable card
and leave the file intact. Disconnect both USB and battery and verify SD VCC
has fallen to 0 V before reconnecting. Compare recording after that cold start
with the prior failure; do not delete/format as part of this trial. A successful
cold restart supports a controller-state explanation but does not prove the
card or board can sustain the endurance workload.

| Observation | Evidence | Interpretation | Confidence |
| --- | --- | --- | --- |
| Recorder stops during purple LED cycle, then multiple red pulses | Field reports on 2026-10-17. Current LED code maps 2 red pulses to SD/write; 3 to mic; 4 to controller unsafe/start refused. | Record task is failing; LED count and fresh recorder diagnostic are still needed to classify the *latest* event. | High for failure, unconfirmed exact stage |
| Native multi-block write stalls | Build 2235 traces: `wireCmd=25`, `wireErr=263 (ESP_ERR_TIMEOUT)`, ~5044–5045 ms elapsed, 5000 ms budget, `sdClockKhz=800`, batch length 4096. Faults after `recordBytes=16340` and `401364` (prior episodes at several other offsets). | Card/SDSPI transaction failed to complete. A FAT software error alone does not explain the native write timeout. Could be card-internal busy, signal integrity, supply transient, or driver/card interoperability. | High for native timeout; cause unresolved |
| Card appears as a 0 B `Rxxxxxxx.WAV` after abort | `fopen("wb")` creates FAT entry; data writes use 4096-byte batches; previous `f_sync` interval 60 s. Firmware intentionally skips `fclose` after an uncertain write to avoid further FAT writes on a busy card. | Directory `st_size` may stay at zero even after PCM was accepted. Not proof data blocks are empty or that the empty file *caused* the timeout. | High for software mechanism |
| Recording works after user deletes zero-byte file | User observation; whether deletion was via mobile-card reader or PWA has not been confirmed. Firmware build 2235 explicitly rejects BLE op17 (delete) and op18 (clear). | If the SD was removed or the pendant power-cycled for deletion, the controller's power reset is a strong confounder. A 0 B file should not collide with random new 8.3 WAV names. | Unresolved |
| Capacitors did not prevent failure | Extra 100 µF electrolytic and 100 nF ceramic; user measured no visible 3.3 V fluctuation at ESP board at LED transition with multimeter. | Sustained rail sag less likely; multimeter and ESP-side measurement do not exclude short droop at SD pads, regulator transients, or ringing on SPI signals. | Medium |
| FAT /synap missing in diagnostics | `vfsStep=13`, `errno=ENOENT` with otherwise mounted root. | Valid for new/root-only FAT layout, **not** evidence of corruption. | High |

## Open engineering issues — prioritized

### P0 — Native CMD25 5-second timeout (blocks reliable recording)
**Owner:** hardware + embedded. Existing 4 KiB CMD25 transaction fails despite slow 800 kHz SPI clock, stable battery telemetry, and added passive capacitors. Preserve first-fault trace (`wireCmd,wireErr,wireArg,wireMs,wireBudget,wireLen,wireWriteOk,recordStage,recordBytes,failureOffset`). Do not “fix” by ignoring `ESP_ERR_TIMEOUT`, retrying a potentially in-flight write, forcing an unsafe `fclose`, or blind `SD.end()/SD.begin()` on an unchanged power rail.

**Next discriminators:** repeat on *two independently sourced, genuine* FAT32 cards; compare USB/battery; capture an oscilloscope trace at **SD VCC/GND and SCK/MOSI/MISO/CS** during a failure, include ground-reference quality and pull-up verification. If failure follows one card, investigate card controller; if both, investigate board/module and SDSPI compatibility.

### P0 — Real card power-cycle versus deleting 0 B file
**Owner:** hardware test. Use **only a disposable card**. When a 0 B WAV is found, do not delete; completely isolate battery and USB so the SD rail reaches 0 V, then restart and attempt recording with the file intact. This separates FAT entry effects from a genuine controller reset. Log the before/after RAM health op34 and retained recorder details op27. No automatic deletion in production; a 0 B directory entry can coexist with recoverable orphan sectors.

### P1 — Metadata durability and interrupted WAV recovery
**Owner:** firmware. Before candidate: `f_sync` at 60 seconds; an interruption at ~20 seconds can leave a 0 B entry. Proposed branch: **one** initial `fflush()+fsync()` after 262144 PCM bytes (~8.2 s), then every 60 seconds thereafter; keep existing quarantine, append-only WAV, synthesized RIFF header on BLE transfer, read-back verification after successful STOP.

**Known trade-off:** early checkpoint adds a FAT metadata CMD24 and may *increase* failures on a marginal card. Treat it as a monitored field trial, not a guaranteed fix. If it triggers an earlier CMD24 timeout, roll back; investigate alternatives (verified reserve strategy, a card with different internal program latency, hardware power sequencing) with explicit recoverability tests.

### P1 — Recorder finalization and stranded FILE* after unsafe write
**Owner:** firmware. Current policy deliberately avoids calling `fclose()` or `fsync()` after an uncertain CMD24/CMD25 because further FAT writes could compound damage. This fails closed, retains recordings but cannot guarantee discoverability of the unsynced tail. Independent SD power gating is **not available** on current board. Do not mark recording as “saved” until close plus size/readback succeed. Expose exact failure stage and persistent first-fault diagnostics.

### P1 — PWA media maintenance / recovery UX mismatch
**Owner:** PWA. Build 2235 blocks BLE op17/op18 despite visible historical “Remove from SD” affordances. Firmware also refuses remount when `writeQuarantine` or `readQuarantine` is active. PWA should display “protected—power-cycle required,” disable inapplicable Delete/Clear/Remount controls, distinguish unsyncable 0 B files from corrupt FAT and show RAM-only op34 diagnostics. Never tell users that a failed delete succeeded. `devices/chakshu` source paths are shared legacy modules and not evidence an S3 camera is in the C3.

### P1 — No validated endurance / back-to-back recording gate
**Owner:** field QA. CI source tests, a mounted FAT root, and a single audio sync do not prove data integrity or repeated SD use. Require:
1. Known-good disposable FAT32 card, cold boot and healthy op34 state.
2. Ten short stop/start captures with no MCU reset; verify on-card sizes and audio samples.
3. Three **30-minute** offline captures, clean STOP, no missing segments, BLE import with checksum/integrity checks, retain originals.
4. Immediately start another offline recording after each BLE sync without rebooting.
5. Repeat the sequence on a second microSD model and both USB/battery conditions.
6. Zero unexplained LED faults, native CMD17/18/24/25 timeout, FAT error, 0 B save, or missing on-card WAV.

### P2 — iOS Bluefy BLE background / transfer throughput
**Owner:** PWA + phone test. Live BLE recording can lose frames while the iPhone browser backgrounds; this is *separate* from disconnected SD recording. Prior diagnostics showed an approximately 15 s clean stream with 915 missing frames across suspension and another stream without losses. MTU 517, ATT capacity 514, payload ~404. Transfer should retain bounded windows, retries, checksum verification and originals until explicit cleanup. Do not reintroduce Wi-Fi.

### P2 — Power and mechanical characterization
**Owner:** hardware. Archived Rev K BOM lists C2 10 µF and 100 nF decoupling; user added 100 µF electrolytic and “104” ceramic. ESP pin 3.3 V was constant on a multimeter when LED changed, but this is not a high-bandwidth SD-side supply measurement. Verify actual installed capacitors, SD contact tension, solder, module regulator/level shifter (if present), common ground, and CS pull-ups. Pin contract: CS=GPIO0, SCK=GPIO10, MOSI=GPIO21, MISO=GPIO20. Battery ADC readings **must not** gate actual SD I/O.

## What is already protected

- BLE-only firmware path, no SD Wi-Fi AP.
- Serialised storage mutex between offline recording and BLE operations.
- New 8.3 `Rxxxxxxx.WAV` files in FAT root; no `mkdir /synap` for new takes; legacy `/synap` indexed read-only.
- 4 KiB direct append writes on native ESP-IDF SDSPI + FatFs; SD SPI 800 kHz.
- Native first-fault transaction ancestry preserved across post-write CMD13 checks.
- Any uncertain write quarantines the same-powered SD controller; no follow-on `fclose` or automatic format/retry.
- BLE transfer does not delete SD originals; recorder health op34 reads RAM without further SD traffic.
- User's physical capacitor changes and measurement do **not** prove the card fault resolved.

## Release/rollout note

The associated early-checkpoint PR is a **controlled experiment**. Production publishing must pass GitHub Actions and the C3+SD target check, and the candidate must not be represented as a proven reliability fix before physical acceptance. Firmware OTA updates the **application** only; it does not repartition the existing enhanced C3+SD OTA table or deploy itself to an offline pendant. Archive diagnostic logs and card images before destructive testing.
