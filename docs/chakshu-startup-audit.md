# Chakshu startup and connection audit — 15 September 2026

Baseline: firmware `dfc456d` (build 1240), PWA `4ad45c6` (shell 116).
The reported symptom is repeated disconnects, especially just after connecting,
with better previous results on the standard S3. No new build-1240 device log or
radio trace was supplied with this report. Earlier logs establish both
app-requested disconnects and disconnects originating outside the app.

## Findings and changes

| Finding | Evidence | Change | Confidence / limit |
| --- | --- | --- | --- |
| Healthy startup work can cause an app-requested disconnect | The PWA queue budget covered only the active request, not all earlier requests. Simultaneous callers could enqueue before an active owner existed. Two regression cases failed on the baseline: a five-second discovery with a queued subscription, and three healthy six-second discoveries. | Budget queue wait across all preceding requests. Preserve each request's separate native deadline. A queued timeout can disconnect only when the actual native blocker has already timed out. | Reproduced in executable tests. This is a confirmed app defect. |
| Chakshu explicitly requested data length that its host already requests | Chakshu's `onConnect` called `setDataLen(251)`. In pinned NimBLE 2.3.6, `ble_gap_event_connect_call` calls the application callback and then requests maximum data length itself. | Remove the duplicate application request. The host still negotiates maximum data length. | Confirmed call duplication; its contribution to this particular phone's drops requires device validation. |
| Additional connection tuning was bundled into the same startup callback | `onConnect` also requested 15–30 ms / latency 0 / six-second supervision parameters. These values are not inherently invalid. The host can terminate a link on a parameter-procedure timeout. | Retain the central's negotiated parameters instead of requesting another update during startup. | Removes an avoidable negotiation path; no claim that every previous drop was a negotiation timeout. |
| Passive update discovery competed with initial capability/event discovery | The updater ran immediately when the connection became ready and discovered updater/status/identity characteristics and subscribed to progress. | Automatic checks wait five seconds. Manual checks remain immediate. Passive consumers defer during manual camera/video activity as well as audio capture. | Full-app test covers delayed discovery, updater deferral and Start during discovery. |
| The diagnostic text obscured NimBLE HCI reasons | The earlier raw reason 531 is `0x213`. The decoder handled plain HCI codes but not NimBLE's `0x200` namespace. | Preserve the raw code and decode only the HCI namespace. `0x213` is remote-host termination; `0x208` is supervision timeout. | Decoding is definitive. Remote-host termination does not identify why the phone ended the connection. |
| App logs could not separate hardware boot from browser setup time | The boot timing existed only on serial; no retained duration/stage/parameters for the last link were exposed. | Chakshu diagnostics v3 retains boot/media initialization time and the previous connection's duration, stage and negotiated parameters. The PWA takes one deferred idle snapshot after reconnect and logs slow requests with separate queue/native durations. | Enables follow-up with the existing Copy diagnostics workflow. Timing values are measured by the device, not estimated by the app. |

## Comparison with the standard S3

| Area | Standard S3 | Chakshu after this change |
| --- | --- | --- |
| BLE integration | Arduino ESP32 3.3.5 core BLE wrapper | NimBLE-Arduino 2.3.6; no application data-length or connection-parameter request at connect |
| Required setup | Microphone, queues, recovery and BLE | Same owners plus camera/SD workers; all required queues/tasks exist before advertising |
| Microphone | External I2S, 32-bit input converted to PCM16 | Onboard PDM on GPIO42/41, native PCM16 |
| Audio | 16 kHz mono; PCM at sufficient MTU, ADPCM fallback for smaller MTU | Same transport contract; no local voice model or firmware DSP |
| CPU | 80 MHz idle / 240 MHz active | 240 MHz idle and active; no clock transition during connection setup |
| Camera/SD | Absent | Camera uses LCD_CAM, PDM uses I2S0; media work runs outside BLE callbacks and the control loop |
| Advertising | Existing target behavior | Fixed 20 ms interval while advertising; automatic reconnect advertising remains host-owned |
| Diagnostics | Existing 48-byte v2 packet | 72-byte v3 packet, preserving the first 48 bytes' meanings |

No standard-S3 or C3 firmware module/profile/sketch behavior is changed by this
patch. The shared PWA queue correction applies to all pendants. A wrapper change
back to the core BLE library was not made: the previous Chakshu read-handler
and notification-ownership fixes remain necessary and covered by tests.

## Other paths reviewed

- **Boot:** no voice model, SD model upload, recognition task or voice GATT
  characteristics are compiled. Camera/SD initialization still happens before
  advertising. Its actual contribution is now exposed as `mediaBootMs`; the
  audit does not assign an invented boot-time improvement. No external touch,
  battery ADC, LED or deep-sleep operations are restored on the Sense pins.
- **GATT callbacks:** control writes copy/enqueue commands without overwriting
  readable status. Camera/SD work is delegated to workers. Diagnostic reads
  copy retained data; they do not request radio parameters or access SD.
- **Audio:** the transmitter retains partially sent frames, uses the actual
  allocated mbuf count and preserves control headroom. A rejected notification
  is not automatically a lost frame. PCM samples remain unprocessed.
- **Recovery/Stop:** the 30-second PSRAM buffer, connection/session ownership,
  STOP drain, replay cursor and stale-link rejection remain intact. Native work
  that outlives a caller timeout retains the app queue until it settles or the
  connection resets. An expired queued command never executes later.
- **Camera/video:** single framebuffer and resource admission remain in place.
  BLE video requests QVGA; standalone photos and SD capture use VGA. Progress,
  cancellation, complete-image checks and bounded SD takes remain unchanged.
- **Reconnect:** advertisements still respect backoff. No new chooser prompts,
  automatic data deletion, recording retry resets or permission flows are added.

## Diagnostic contract

Magic `0xD6`, version 3, exactly 72 bytes. Integers are little-endian.
Existing v1/v2 readers remain supported by the updated PWA. Only Chakshu emits
v3. Firmware byte 2 still reports streaming/capture state independently of the
link stage; a buffered stream waiting for RESUME is not counted as streaming on
the newly connected link.

| Offset | Type | Meaning |
| --- | --- | --- |
| 0–47 | Existing v2 fields | Reset, capture/drop/heap/uptime counters, raw disconnect reason and count |
| 48 | u32 | Boot-to-ready milliseconds |
| 52 | u32 | Camera/SD initialization milliseconds |
| 56 | u32 | Duration of the last disconnected link, milliseconds |
| 60 | u16 | Last link interval, units of 1.25 ms |
| 62 | u16 | Last link peripheral latency, connection intervals |
| 64 | u16 | Last link supervision timeout, units of 10 ms |
| 66 | u8 | Last link stage |
| 67 | u8 | Current link stage |
| 68 | u32 | Current link duration, milliseconds; zero when disconnected |

Stages: 0 disconnected, 1 connected, 2 audio subscribed, 3 valid GET_STATUS
received, 4 streaming with recovery no longer waiting. Stage 3 records receipt
of the request, not proof that the phone received its reply. Values before the
first disconnect are zero. Last-link evidence survives subsequent recordings
and reconnects; a reboot clears it and supplies a new reset reason/uptime.

## Verification and remaining device work

- `tests/recording-transport.cjs`: baseline regressions reproduced, progressing
  setup, native timeout ownership, stale callbacks and stuck-link recovery.
- `tools/startup-smoke.cjs`: full PWA with slow capability/event/updater
  discovery, Start during discovery, audio save and three reconnect/capture
  cycles. The native fixture rejects overlapping ATT requests.
- `tests/chakshu-link.cjs`: actual production callbacks over 30 links, stranger
  rejection, recovery preservation, diagnostic encoding and clock rollover;
  compiled pinned host code confirms the automatic data-length request.
- Existing complete firmware/PWA suites and all three firmware builds are
  required before release. Camera, storage, recovery and browser workflow CI
  remain release checks.

These are source, native and browser-fixture checks. They do not prove RF
stability, camera throughput, antenna quality, power integrity or iOS background
delivery on the physical pendant. Seeed's board instructions specifically require
attaching the supplied external Wi-Fi/Bluetooth antenna; Bluetooth may not work
without it. The supplied screenshots show the app, not the antenna connector, so
its attachment cannot be established from this report. Check that connection as
part of the physical retest; this is not evidence that it is currently missing.

The next device run should use this firmware
and shell together: cold boot, connect, record audio, take a photo, record a
short video, stop/save, and reconnect several times. If a drop remains, the new
log can distinguish early handshake loss from streaming loss and a fresh boot
from a retained link failure. A missing SD card still prevents SD-only capture;
it is not required for BLE audio or photo transfer.

## Primary implementation references

- [Pinned host connection event and data-length negotiation](https://github.com/h2zero/NimBLE-Arduino/blob/2.3.6/src/nimble/nimble/host/src/ble_gap.c)
- [Pinned server callback dispatch](https://github.com/h2zero/NimBLE-Arduino/blob/2.3.6/src/NimBLEServer.cpp)
- [Pinned host error namespaces](https://github.com/h2zero/NimBLE-Arduino/blob/2.3.6/src/nimble/nimble/host/include/host/ble_hs.h)
- [Apple advertising/connection guidance, QA1931](https://developer.apple.com/library/archive/qa/qa1931/_index.html)
- [Seeed XIAO ESP32S3 Bluetooth antenna installation](https://wiki.seeedstudio.com/xiao_esp32s3_bluetooth/#installation-of-antenna)

QA1931 supports the fixed 20 ms advertising choice. Its older connection-timeout
range is not used to reject modern central-selected parameters; the firmware
now leaves those parameters to the central.
