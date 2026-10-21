# Synap cross-device parity audit — 21 October 2026

## Reference and scope
Reference: Odyssey C3 with SD hardware (module 2), implemented by the active `odyssey-sd-1631-*` sources in `firmware/shared/sources.json`. Compare four functional device families across five distinct production OTA/partition targets.

Reviewed graph: `devices/catalog.json`, `tools/device-profile.cjs`, `tools/materialize-target.cjs`, `tools/boards/*`, `firmware/shared/*.cpp` in the active source graph, `firmware/xiao-sense/*`, target release validation and the companion PWA's device/capability modules. This is a static code review and CI contract audit, **not** physical-device acceptance.

## Compatibility matrix

| Requirement | C3+SD | C3 without SD | Odyssey S3 | Chakshu |
| --- | --- | --- | --- | --- |
| Connected PWA audio | Shared 16 kHz PCM16/ADPCM transport | Same | Same | Same protocol, PDM hardware adapter |
| Connect/reconnect, journal recovery | Shared BLE callbacks and audio session | Same | Same | Same common path + board BLE overrides |
| Touch double tap while connected | Start / stop PWA take | Same | Same | Same |
| Touch double tap while disconnected | Local SD WAV with purple indication | No SD capture; safe refusal | No SD capture | Local SD WAV, distinct writer |
| Long hold / deep-sleep wake | Shared four-second gesture; C3 GPIO wake | Same | S3 RTC GPIO wake | S3 RTC GPIO wake |
| Battery and power telemetry | Shared module; per-board calibration | Same firmware; card presence changes C3 SD calibration | Shared module and own divider | Shared module and divider |
| SD catalogue, import, quarantine | Native SDSPI, media-v3 credit windows, append-only PCM, virtual WAV header | Capabilities supported but **not ready** without mounted card | Detection only | Native SDSPI, media-v1, photo/video/WAV, separate camera/voice worker |
| OTA and diagnostics | Distinct partition tracks; C3 controller trace retained | Legacy small OTA slot | S3 OTA | 8MB target-specific OTA; media status |
| SD delete / format | Unsafe file-delete and bulk clear disabled; explicit guarded format only | Not applicable | Not applicable | Different delete/format policy — **do not unify unsafely** |
| Voice/camera | Not present | Not present | Not present | Experimental Hey Snap for plain photo, marked vision photo, bounded video |

## Findings

1. **HIGH, corrected in source — Chakshu offline after PWA standby.** The PWA can issue `CMD_STANDBY` during connected idle. The C3+SD physical gesture remains accepted after disconnection, whereas `ChakshuVoice::touchAudioToggle()` rejected `remoteStandby`. Disconnected touch now releases that stale standby latch before queueing an otherwise healthy local SD job. It does not clear real SD quarantine or bypass OTA/sleep ownership.
2. **HIGH, corrected in source — stale Chakshu capability readiness.** The C3 descriptor reports live SD readiness via `odysseySdDetectionState()`. Chakshu previously combined stale `ChakshuMedia::Snapshot.ready` bits with current readiness, making `SD` and `sdAudio` appear available after an actual write fault. It now derives camera/SD from live `ChakshuCamera::ready` and `ChakshuStorage::ready`, and audio from `microphoneValidated`.
3. **HIGH, unresolved in hardware — SD program timeouts.** C3+SD maintains explicit CMD24/25 uncertainty quarantine and retains FAT data across failures. Chakshu separately quarantines uncertain CMD24 writes. Both can still time out electrically. Firmware source consistency is not evidence that both SD rails/cards are healthy.
4. **MEDIUM, intentionally different — recording finalization.** C3+SD uses multi-sector append, no in-place WAV-header rewrite on STOP, and a virtual RIFF header during transfer. Chakshu rewrites the actual 44-byte WAV header and uses camera-linked media files. Migrating C3's strategy would require versioned media-format and import tests; do not blindly copy the implementation.
5. **MEDIUM, intentionally different — BLE media.** C3+SD advertises newer credit-window media-v3 for efficient read-only sync; Chakshu media-v1 supports photo, video, marked comprehension and Wi-Fi download. The companion PWA must route by `moduleId`, feature flags and negotiated `mediaVersion`, not assume all have the same opcode or transfer rate.
6. **MEDIUM, documentation drift — OTA identities.** The catalog and build workflow contain five OTA/partition tracks: S3, C3 legacy, C3+SD factory, C3+SD existing large-slot, Chakshu. Older text claiming just three OTA targets, Arduino SD code or 1 MHz C3 data clock is obsolete. Keep C3's preinstalled partition/marker distinctions.
7. **MEDIUM, intentional hardware policy — C3 no-card idle sleep.** Cardless C3 can enter disconnected-timeout sleep, whereas SD-ready C3 remains armed for offline double-tap. The lack of offline capture on non-SD C3 and S3 is not a parity defect.
8. **MEDIUM, release quality — test scope.** Unit tests and five target compiles verify source consistency. Physical devices must separately pass BLE onboarding, STOP during dropouts, touch timing, SD mount/write/close/readback, power-rail reset, OTA regression and verified PWA import. Real card power is not reset by every MCU reboot.

## Required validation checklist
1. Every one of five OTA tracks: correct chip/marker/partition, BLE advertising and 20-byte capabilities, 16 kHz transport, START/STOP with full PWA receipt, reconnect continuity, 4 s sleep hold and 4 s touch-to-wake.
2. C3+SD on both large-slot tracks: disconnected double tap → sustained WAV/purple, double tap STOP → full verified file, reconnect and import, safe preserved source, no auto FAT delete, uncertain SD timeout → no same-rail remount.
3. C3 no SD: normal BLE audio + OTA remains usable; connected double tap works, disconnected tap cannot create a phantom SD recording.
4. S3: normal I2S/BLE audio and power gestures; no SD media catalog or camera controls.
5. Chakshu: connected audio, PWA standby then disconnect then offline touch recording, instant withdrawal of SD readiness on fault, photo vs marked vision photo, 5/10/15/30-second video, live picture/SD discovery and BLE OTA. Monitor CMD24 timeout even at 1 MHz.

## Release policy
Keep existing OTA target IDs, product markers, BLE protocol and NVS/diagnostic layouts intact. Do not alter C3 SD power/format/delete timing merely to share implementation with Chakshu. Prefer common contracts with board-specific drivers. Treat CI as validation of compilation and invariants, **not** proof of hardware operation.
