# Battery and power review

The main opportunities are the CPU and radio during recording/connected idle, and the board's remaining electrical load during deep sleep. Their relative contribution requires battery-side current measurements; no percentage improvement or runtime extension has been measured.

## Implemented improvements

| Area | Change | Scope of benefit |
| --- | --- | --- |
| Housekeeping | The Arduino loop waits 1 second after successful boot validation, or when rollback validation is disabled. Its 20 ms validation retry remains until boot is accepted. | Reduces this task's idle wakeups from 50/s to 1/s. Other tasks and RTOS/radio wakeups remain. |
| Paused OTA | CPU boost expires after 1 second without OTA commands, or immediately on disconnect. The CPU is boosted before processing resumed commands and flash work. | Uses the existing 80 MHz idle profile while preserving the OTA session/hash/offset and 15-minute resume window. Recording retains its active CPU profile. |
| Standby LED | Standby takes precedence over battery/disconnect pulses; OTA indication retains precedence. | Makes non-OTA standby consistently dark. This does not disconnect the LED's power supply. |

Native tests cover the exact boot loop with rollback enabled/disabled and microphone enabled/disabled; dark standby and retained active indications; OTA pause/resume, CPU boost before flash operations, timeout/overflow, and timer rollover. Existing tests retain audio bytes/pacing, capture ownership, STOP, battery guards and touch/sleep behavior. Both board builds remain release gates.

## Next experiments, in priority order

| Use pattern | Candidate | Validation before deployment |
| --- | --- | --- |
| Long recordings | Benchmark S3 at 160 MHz against 240 MHz; consider 80 MHz only after establishing headroom. | Measure battery current, sustained frame/drop counters, recovered I2S reads, decoded audio and reconnect/OTA transitions. C3 already uses 160 MHz active. |
| Long connected idle | Request a separate BLE idle profile; initially test 30–45 ms intervals with peripheral latency 4. Restore a recording/OTA profile when work starts. | Log the parameters actually accepted by each phone, then test START latency, background behavior, supervision timeout and repeated reconnects. |
| Disconnected waiting | Keep fast discovery initially, then test 250–500 ms advertising after 30 seconds. | Verify discovery/reconnect latency and return to fast advertising on the next disconnect. Preserve the existing five-minute sleep policy. |
| Connected idle CPU | Evaluate ESP-IDF frequency scaling and Bluetooth modem sleep using an audited SDK configuration. | Inspect the exact packaged SDK flags first; rebuild the SDK if required. Check I2S/USB/BLE power locks and timing on both targets. |
| Long deep sleep | Measure regulator, charger, LED, microphone and touch-sensor supply currents on the assembled board. | Identify the actual fitted parts and isolate dominant loads before selecting low-quiescent-current replacements or adding switched supplies. Preserve touch wake. |

These are proposed experiments, not enabled production settings. The priorities depend on time spent in each state. Lowering transmit power is not a first choice given the requirement for stable connections.

The firmware's `setMinPreferred`/`setMaxPreferred` calls advertise connection preferences; they do not establish the phone's actual interval. The pinned Bluedroid advertising defaults are 0x20–0x40 (20–40 ms), distinct from those connection preferences. See the [Arduino-ESP32 3.3.5 advertising implementation](https://github.com/espressif/arduino-esp32/blob/3.3.5/libraries/BLE/src/BLEAdvertising.cpp). Longer connection intervals and peripheral latency can reduce idle connection events, subject to the central's accepted parameters and latency tradeoff. See [Espressif's BLE connection guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/ble/get-started/ble-connection.html).

Frequency scaling requires `CONFIG_PM_ENABLE`; automatic light sleep additionally requires tickless idle. Bluetooth and I2S locks can constrain achievable sleep. Calling a sleep API alone does not establish a working connected low-power mode. The current sketch uses manual CPU profiles and does not configure ESP-IDF automatic power management. See [Espressif power management](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/power_management.html).

## Measurement and runtime estimate

Measure the complete pendant at its battery input with USB disconnected, using a current profiler that captures radio bursts and deep-sleep current without excessive voltage drop. Compare the same board, battery voltage, phone, signal strength and firmware settings in recording, connected idle, standby, advertising, deep sleep and paused/resumed OTA. Run at least ten minutes of recording/idle measurements and a long recording stability check before adopting lower clocks. Module datasheet currents exclude development-board overhead; [Espressif's measurement guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/current-consumption-measurement-modules.html) explains that distinction.

The existing 1 MOhm + 470 kOhm divider draws approximately 2.8 microamps at 4.13 V, calculated from V/R. It is a lower-priority target than unmeasured CPU/radio and board loads. Battery sampling cadence and calibration remain intact.

Estimate hours as usable battery capacity (mAh) divided by measured average battery current (mA). For mixed use, average current is the sum of each state's current multiplied by its fraction of time. Use battery-side current so regulator losses are included. Required inputs are the fitted battery capacity, usable capacity to shutdown, and time spent recording versus idle/sleep.
