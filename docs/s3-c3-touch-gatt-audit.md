# S3/C3 double-tap and GATT audit

The September 16 report combines failed double-tap control with protocol-v2 command errors, echoed two-byte writes, transport fields stuck at MTU 23, and diagnostics reporting the boot-time disconnected state. The installed build was not available in that log, so these symptoms alone do not prove every reported failure has the same cause.

## Confirmed defects

Arduino ESP32 3.3.5's built-in NimBLE compatibility layer schedules each write callback in a new delayed task, without retaining the write payload. Firmware subsequently reads the characteristic's current value. Control status and battery updates share that value, so the callback can parse telemetry as a command and report PROTOCOL_MISMATCH. Consecutive requests can also replace each other. The library's short-read test uses packet-header length >8, suppressing normal onRead callbacks whose header is exactly eight bytes and leaving diagnostics/recovery stale.

The physical gesture implementation itself queues START/STOP on the control task. START is idempotent and STOP drains negotiated recovery before idle/standby. Existing native gesture tests exercise S3 GPIO13 and C3 GPIO3, including debounce, double-tap, long press, OTA, reconnect and wraparound. These are simulated pin inputs; actual wiring and radio behavior still require device validation.

## Correction

`tools/patch-arduino-ble.cjs` accepts only the exact original or already-patched 3.3.5 characteristic files. It enables normal short-read callbacks and adds a synchronous callback with owned request bytes. S3/C3 control handling copies those two bytes into the existing queue without replacing the readable status. Other callbacks retain their existing queue-based behavior. The Bluedroid control overload also consumes request bytes directly. Control reads explicitly regenerate status. Chakshu keeps its separate NimBLE-Arduino driver.

The PWA adopts physical START through the recorder directly, without a hidden-button click or another START. Fragments received while the journal opens are bounded and retained before close. A quick second double-tap cannot restart a stopped take.

## Validation and release

Native tests execute the patched library's actual GATT handler with normal reads, consecutive START/STOP/status writes, chained writes and invalid lengths. Firmware gesture, recording, recovery, OTA and materialization tests remain release gates. CI compiles all three boards and publishes target-specific OTA binaries only after success. Browser tests cover S3, buffered C3 and audio-only compatibility connections with immediate first packets, duplicate status and slow journal creation.

A PWA refresh changes adoption behavior; the installed S3/C3 firmware must also be updated for the library fixes. Physical-device acceptance: connect after waking, double-tap to record, double-tap to stop, repeat from standby, and confirm live diagnostics report connected with negotiated transport. Do not infer actual hardware stability from host tests.

Pinned library source: https://github.com/espressif/arduino-esp32/blob/3.3.5/libraries/BLE/src/BLECharacteristic.cpp
