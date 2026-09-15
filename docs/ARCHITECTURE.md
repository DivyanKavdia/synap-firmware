# Firmware architecture

## Source ownership

`devices/catalog.json` is the device catalog shared with the PWA. It defines each target's module ID, release identity and limits, supported features, pins and power policy. `tools/targets.cjs` exposes the catalog to build and release tools. `tools/device-profile.cjs` renders the complete profile block, so board adapters do not rewrite individual pin or battery constants.

| Responsibility | Owner |
| --- | --- |
| Device catalog and release limits | `devices/catalog.json` |
| Profile rendering and target selection | `tools/device-profile.cjs`, `tools/materialize-target.cjs` |
| Assembly order and generated S3 sketch | `firmware/shared/sources.json`, `tools/assemble-source.cjs` |
| Types, queues, shared state and prototypes | `firmware/shared/runtime.cpp` |
| Capability descriptor and readiness | `firmware/shared/module-capabilities.cpp` |
| Microphone lifecycle and recursive driver lock | `firmware/shared/microphone.cpp` |
| Battery conversion, availability and cutoff | `firmware/shared/battery.cpp` |
| Status LED and CPU clocks | `firmware/shared/status-led.cpp`, `cpu-power.cpp` |
| Touch, standby and durable sleep/wake gates | `firmware/shared/power.cpp` |
| Recording sessions, buffering and MTU negotiation | `firmware/shared/audio-session.cpp` |
| BLE callbacks and serialized control task | `firmware/shared/ble-control.cpp` |
| PCM capture and packet transmission | `firmware/shared/audio-capture.cpp`, `audio-transport.cpp` |
| OTA verification and inactive-slot writes | `firmware/shared/ota.cpp` |
| Initialization and boot validation | `firmware/shared/boot.cpp` |
| C3 single-core tasks and discrete LED | `tools/boards/esp32c3/`, `firmware/esp32c3/` |
| Chakshu PDM, hardware exclusions and NimBLE adapter | `tools/boards/xiao-sense/` |
| Chakshu resource admission and initialization barrier | `firmware/xiao-sense/ownership.cpp` |
| Chakshu camera, SD, media and voice/model services | `firmware/xiao-sense/` |

The shared fragments are assembled in order into one Arduino translation unit; they inherit the runtime's types and prototypes. `synap_esp32s3/synap_esp32s3.ino` is generated for portable downloads and native tests. CI checks byte equality. C3 and Chakshu are materialized from that sketch after release preparation, preserving the selected build identity.

Board adapters retain checked transformations where library APIs differ. C3 replaces the RGB implementation with an active-low discrete LED and creates tasks on one core. Chakshu selects native PDM capture, omits absent GPIO peripherals and sleep, and adds camera/SD/voice services. Its NimBLE adapter owns subscription state, synchronous command snapshots and direct notification acceptance. A missing or ambiguous transformation boundary fails the build.

## Runtime contracts

- **Audio:** 800 PCM16 samples per 50 ms frame. INMP441 converts signed 32-bit slots with `raw >> 16`; PDM already supplies PCM16. No software gain, denoiser, gate or silence trimming is applied to the recording stream. Voice inference consumes a separate copy.
- **Transport:** START/RESUME selects PCM v2 at MTU ≥185 or independent 404-byte IMA ADPCM v3 frames on smaller supported links. Missing frames retain timeline gaps. Negotiated MTU is not a guarantee of radio throughput.
- **Ownership:** the recursive microphone mutex serializes reads/start/stop. Capture blocks while idle; transmit blocks on its queue. STOP waits for capture ownership and in-flight notification submission before acknowledging idle. A notification accepted by the local stack is not proof of phone persistence.
- **Chakshu admission:** a nonblocking resource lease orders START/OTA transitions against camera, SD and model jobs. START publishes streaming state before releasing it; OTA publishes its atomic busy snapshot. Long media jobs retain the lease until file finalization. The control task waits for complete BLE initialization. See [the Chakshu runtime review](CHAKSHU_RUNTIME_REVIEW.md).
- **Connections:** generations invalidate sends from old links. Control transitions use an atomic pending flag independent of command-queue capacity. An abandoned buffered stream may retain STREAMING while a new link still reports MTU23/zero payload; the PWA must issue STOP only when it has no matching recording owner, then require an idle acknowledgement.
- **Recovery:** an explicitly armed session stores volatile PCM, up to 30 seconds in available PSRAM. Without PSRAM, allocation is smaller and conditional on free heap. Recovery expires after 60 seconds; STOP drain is bounded to 35 seconds. See the recovery guide for token and replay semantics.
- **Power:** profiles preserve S3 80/240 MHz, C3 80/160 MHz and Chakshu 240/240 MHz. C3/S3 share double-tap recording control and four-second hold/release sleep/wake. Chakshu does not use external touch, battery-divider or LED pins.
- **Battery:** the catalog selects calibration and full-charge reference. Both SuperMini targets report valid measurements; only S3 enforces confirmed critical-battery sleep/OTA guards. C3 remains telemetry-only. Out-of-range samples remain diagnostic data and report unavailable.
- **OTA:** the updater checks target, public device ID, image structure, size and SHA-256 before committing the inactive slot. Recording blocks updates. Public IDs are association identifiers, not credentials.

## BLE ownership

Primary service: `4fa12345-0000-1000-8000-00805f9b34fb`.

| First UUID group | Purpose |
| --- | --- |
| `4fa12346` / `47` | Audio / control-status |
| `4fa12348` / `49` | OTA command / progress |
| `4fa1234b` / `4c` | Target/build identity / permanent device ID |
| `4fa1234d` / `4e` | Diagnostics / battery, touch and power events |
| `4fa1234f` | Optional disconnect recovery |
| `4fa12350` | Device capabilities |
| `4fa12351`–`59` | Chakshu hardware checks, media, voice and model services |

Diagnostics v2 preserves v1's initial fields and adds disconnect reason/count/time and last notification failure. Flags `0x40` and `0x80` mean unconditioned real-mic capture and selected PCM transport respectively. Counters persist across recording starts/reconnects but not reboot. `0xFFFF` means the stack did not provide a disconnect reason.

## Extending a device

1. Update the catalog with an explicit feature and electrical policy. Preserve existing IDs and wire-bit assignments.
2. Implement the feature in its owning driver. Report support separately from successful initialization.
3. Regenerate the S3 sketch and materialize all targets. Update the PWA catalog using its `tools/device-catalog.cjs --from ../synap-firmware` command.
4. Add a behavior regression at the affected boundary and run native tests plus all board builds. Keep generated outputs out of handwritten source edits.
5. Validate physical boards for affected timing, microphone, camera, storage, power or OTA behavior. CI covers code and simulated protocols, not physical endurance.

Comments should explain electrical constraints, asynchronous ownership or protocol compatibility. Current behavior belongs here and in the feature guides; old incident narratives belong in Git history.
