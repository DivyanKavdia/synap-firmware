# Chakshu capture and media

Chakshu is the third Synap module: Seeed XIAO ESP32S3 Sense, onboard PDM microphone, OV3660 camera and microSD. The initial build assumes an 8 MB flash / 8 MB OPI PSRAM XIAO and an installed 2 GB card. Capacity and camera sensor ID are read from hardware, not hardcoded as successful.

## Initial behavior

- The module stays awake while powered and advertises as **synap-Chakshu**.
- Microphone, camera and SD are probed at boot. A failed camera/card does not disable BLE audio.
- No TTP223, external LED, switch or battery divider is configured. Standby commands leave it awake.
- The main PWA microphone control records through the existing Synap journal/transcription flow: 16 kHz mono PCM16 preferred over BLE, with existing ADPCM fallback at small MTUs. PDM capture does not apply gain, filtering or noise reduction.
- Device settings show module identity and per-feature readiness. Bluetooth pairing still needs a browser picker; automatic detection happens after connection.
- Photo, ten-second WAV, and ten-second silent MJPEG checks save to SD under unique names in /synap. MJPEG uses VGA JPEG frames at a target of 2 fps; it has no audio track or fixed-rate container.
- A running SD check finishes on the pendant even if the app disconnects. Its result remains readable after reconnect until another check or reboot. These bounded checks are independent of the main PWA recording.
- SD checks and BLE audio/OTA are mutually exclusive. Files are never automatically deleted or overwritten. Failed writes retain partial files and report failure.
- Flash/NVS stores settings and device state. Audio is not written to internal flash.

## First flash

Use the Chakshu build only; a generic Synap S3 binary has different pins and memory settings.

The CI **verified-firmware** artifact includes:
- xiao-esp32s3-sense-8m/factory.bin: merged first-flash image, flash at address 0x0.
- xiao-esp32s3-sense-8m/firmware.bin: application image for the configured OTA slots.
- xiao-esp32s3-sense-8m/synap_chakshu.ino: portable Arduino source with the matching build counter; use this for an Arduino/Arduinodroid first flash.
- source-sync/synap_chakshu.ino: unversioned prepared source for auditing.

For a source build, run:

~~~sh
node tools/assemble-source.cjs
node tools/materialize-target.cjs xiao-esp32s3-sense-8m synap_esp32s3/synap_esp32s3.ino prepared/synap_chakshu/synap_chakshu.ino
arduino-cli compile --fqbn 'esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc' prepared/synap_chakshu
~~~

Use Arduino ESP32 core **3.3.5**, 8 MB flash, **OPI PSRAM**, and **default_8MB dual OTA partitions** (two 0x330000-byte slots). CI uses the generic ESP32S3 board definition with all Sense pins explicitly assigned. In Arduino IDE/Arduinodroid, use XIAO ESP32S3 or equivalent settings; do not choose Huge APP/no OTA. Local builds identify as build 0; versioned artifact source includes its build counter. The current PWA updater expects a release-range build identity, so use the versioned artifact source if you want subsequent BLE updates without another USB flash.

## Test with the 2 GB card

1. Connect the Sense expansion board firmly, insert the card and power by USB.
2. Open Serial at 115200 if available. Do not wait for Serial before boot: headless power works.
3. Connect **synap-Chakshu** in the PWA. Open Settings → Device. Confirm Chakshu, OV3660, microphone and SD readiness.
4. Use the main microphone control to record 20–30 seconds. Stop and play the audio in Synap.
5. Stop live recording, then run **Photo to SD**, **10-second WAV to SD**, and **10-second clip to SD** separately.
6. Wait for each saved result. Note the filename, power down and read the card. A successful WAV has 320,000 PCM data bytes plus its 44-byte header. Open JPEG normally. Play/convert the .mjpeg stream with software that accepts raw MJPEG, specifying 2 fps if required.
7. Test a missing card: BLE audio should remain usable. Reinsert the card and press **Check hardware** to remount it.

Try the installed FAT card as-is. Firmware does not format it. If mounting fails, back up its files before manually preparing a compatible FAT volume. Seeed recommends FAT32. Report the Serial log and Device panel status before changing pin definitions.

Readiness means driver initialization succeeded, not that real-world audio/image quality has been certified. Physical checks are required.

## Photo/video library extension

Capability descriptor byte 14 advertises media extension version 1 when its worker is available. C3/S3 keep byte 14 zero. The PWA associates the permanent Chakshu device ID with the signed-in account; this preference never gives access to another account's files.

- Online photo/video reads fresh JPEG frames through the existing BLE service while ordinary audio notifications continue. The PWA stores silent frame sequences and its audio journal separately. Throughput determines frame rate; this is not a full-frame-rate MP4 stream or Wi-Fi preview.
- Offline capture writes a matching `.mjpeg`, `.wav`, and `.json` under `/synap/`. WAV is mono 16 kHz PCM16; JSON records frame positions on the captured audio sample timeline. Takes stop at 60 seconds of PCM, 65 seconds of wall time, a stop request, or a storage/capture error. Disconnection does not stop an accepted SD take.
- SD offline capture excludes BLE audio, OTA and hardware checks. Photo/file transfers share the app's serialized native queue. The two media workers claim the shared camera/SD busy flag atomically.
- File reads are limited to generated `/synap/xxxxxxxx-xxxxxxxx` names and JPEG/WAV/MJPEG/JSON extensions. Listing exposes up to 100 photo/video entries; larger card archives can be imported with a card reader. Downloads never delete the originals. The PWA rejects files larger than 32 MiB.
- Existing ten-second hardware checks remain independent. Earlier silent video has no audio timing sidecar; it can be imported for playback/manual description, but cannot drive timestamped voice explanations.

### Media extension wire format

UUID suffix is `-0000-1000-8000-00805f9b34fb`.

| Characteristic | Layout |
| --- | --- |
| `4fa12354`, write with or without response | `CA`, operation byte, request ID uint32 LE, offset uint32 LE, optional UTF-8 path (up to 63 bytes) |
| `4fa12355`, read | `CB`, version 1, state (1 success, 2 error), error code, request ID uint32 LE, total bytes uint32 LE, offset uint32 LE, up to 480 payload bytes |

The PWA uses write without response for short commands when advertised, then waits for the matching result ID. Requests containing longer file paths retain write with response so minimum-MTU connections can use long writes. Both paths enqueue the same worker request; Bluetooth callbacks do not capture images or access SD.

| Operation | Action |
| --- | --- |
| 1 / 2 | Capture fresh JPEG / read captured bytes at offset |
| 3 / 4 | Select SD file by path / read selected file or listing bytes |
| 5 / 6 | Start bounded SD audio/video take / request stop |
| 7 / 8 | Build JSON catalogue / read catalogue size |
| 9 | Read offline take status as JSON: active, state, error, progress, path |

The client sends one request at a time, polls for its matching response ID, validates every byte offset/total, and does not automatically repeat a capture. BLE callbacks only copy requests/results; camera and SD work execute in the worker. Stop/status remain available during an offline take. Changing the BLE connection invalidates a selected file or frame.

### Physical validation

CI compiles all three board targets. Simulated browser tests exercise account isolation, native queue transfers, separate audio/video storage and frame-limited inference. They do not establish camera/audio synchronization or throughput on a phone. For device validation, record an audible clap in view, verify the matching WAV/JSON/MJPEG timeline, test a disconnected SD take, and check card-full/removed-card recovery. Cloud descriptions require the matching PWA/backend deployment.

## Core capture release

Local voice recognition is removed for now. There is no model initialization, AFE/MultiNet task, microphone copy queue, NVS voice toggle, voice lease or model upload service. Descriptor byte 15 is zero. CI does not download, embed, publish or require voice-model assets. Previous models on an SD card are left untouched; this firmware never opens them. Git history retains the experimental implementation for a later, separately measured reintroduction.

Use the PWA microphone, photo and video buttons. Bluetooth video requests use operation 1 with offset 1 as a QVGA (320 × 240) hint. Standalone photos and SD captures remain VGA (640 × 480). Older clients send offset 0 and keep VGA; older firmware ignores the hint. Sensor changes happen only inside the camera/SD ownership gate. A single PSRAM JPEG framebuffer remains in use.

Required queues, capture tasks and recovery state are ready before advertising. Camera/SD checks still run during setup; their time is reported as `media_ms` alongside `ready_ms`, free heap and free PSRAM in the serial boot log. No claim of a measured boot speedup is made without a new device boot log. The retired model was loaded on the boot path before advertising.

Chakshu uses NimBLE-Arduino 2.3.6 and Arduino ESP32 3.3.5. The main S3 and C3 adapters retain their existing BLE stack. The new image needs a normal firmware update and reboot; refreshing the PWA alone cannot remove a model from installed firmware.

See [the September 15 core review](chakshu-core-review.md) for the log findings, checks and remaining physical validation.

## References

- [Seeed microphone pins and PDM setup](https://wiki.seeedstudio.com/xiao_esp32s3_sense_mic/)
- [Seeed microSD wiring and preparation](https://wiki.seeedstudio.com/xiao_esp32s3_sense_filesystem/)
- [Espressif XIAO camera pin map, core 3.3.5](https://github.com/espressif/arduino-esp32/blob/3.3.5/libraries/ESP32/examples/Camera/CameraWebServer/camera_pins.h)
- [Espressif camera driver and framebuffer tradeoffs](https://github.com/espressif/esp32-camera)
