# Firmware runtime review

## Changes

| Area | Finding | Implemented correction |
| --- | --- | --- |
| Microphone lifecycle | Capture/recovery and the control task could call the same I2S driver concurrently. A fixed 90 ms STOP delay did not establish ownership during recovery. | Serialize reads, initialization and shutdown with a statically allocated recursive mutex. STOP waits for actual driver ownership. |
| STOP acknowledgement | Removing a fixed wait also requires explicit coordination with the transmitter. | Mark transmitter activity before checking the recording generation, and wait for an in-flight send to return before acknowledging STOP. Stale frames cannot start a new send afterward. |
| BLE reconnects | Connection events shared a bounded command queue and could be dropped when it filled. Recovery covered disconnects but not missed connections. | Publish link changes through an atomic pending flag and reconcile them before handling queued commands. Rapid changes converge on the latest link state. |
| Control-loop responsiveness | Discarding a stale command used `continue`, skipping touch, OTA, power and advertising maintenance for that iteration. | Ignore the stale command while still running maintenance. |
| Recording status | OTA capability initialization or an MTU-driven refresh could replace STREAMING with CONNECTED_IDLE while capture continued. | OTA state changes cannot overwrite an active recording's device state. |
| Diagnostics | A GATT read inspected the OTA engine while another task mutated it. Boot validation also read a microphone flag written by the capture task. | Read an atomic OTA busy snapshot in diagnostics; make microphone-validation state atomic. |
| START admission | A transport with an insufficient MTU could power up the microphone before START was rejected. | Check transport capacity before activating the CPU profile and microphone. |

The pinned [Arduino-ESP32 I2S implementation](https://github.com/espressif/arduino-esp32/blob/3.3.5/libraries/ESP_I2S/src/ESP_I2S.cpp) shares mutable channel/lifecycle state across `readBytes`, `begin` and `end`. The application mutex protects that wrapper, including nested recovery calls. The [BLE implementation](https://github.com/espressif/arduino-esp32/blob/3.3.5/libraries/BLE/src/BLECharacteristic.cpp) returns from notification submission without a peer acknowledgement; firmware STOP coordination waits for submission to finish, and does not claim that the app has persisted audio.

These changes preserve the microphone format, encoder output, packet pacing, active protocol versions, characteristic UUIDs, hardware pins and touch gestures. Recording still stops on BLE disconnection. No offline storage or reconnect recording buffer is introduced.

## Verification

Run `node --test tests/*.cjs` and both pinned production board builds. The native suite compiles the actual firmware functions, rather than a separate model of their logic.

New tests cover concurrent STOP during a blocked microphone read and during recovery initialization; STOP acknowledgement while a transmitter is active; stale frames after STOP; connection/disconnection coalescing without relying on command queue space; stale-command maintenance; standby and advertising recovery; START rejection before microphone initialization; idempotent START; OTA MTU refresh and rejected BEGIN during recording; and diagnostic snapshot updates.

Existing checks retain codec golden output, frame packing and pacing across MTUs and timer rollover, audio filtering, partial I2S reads, wake gestures, battery guards, resumable OTA and target identity. Native concurrency fixtures use deterministic synchronization, warnings as errors and undefined-behavior sanitization.

Physical S3/C3 checks remain necessary for STOP latency, I2S recovery, sustained audio/drop counters, repeated disconnect/reconnect, touch gestures, sleep/wake, OTA and power consumption. The review establishes firmware fixes and build/test results; it does not measure radio-range improvements or guarantee that a phone will keep BLE connected in the background.
